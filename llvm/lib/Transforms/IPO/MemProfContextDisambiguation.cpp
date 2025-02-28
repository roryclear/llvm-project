//==-- MemProfContextDisambiguation.cpp - Disambiguate contexts -------------=//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements support for context disambiguation of allocation
// calls for profile guided heap optimization. Specifically, it uses Memprof
// profiles which indicate context specific allocation behavior (currently
// distinguishing cold vs hot memory allocations). Cloning is performed to
// expose the cold allocation call contexts, and the allocation calls are
// subsequently annotated with an attribute for later transformation.
//
// The transformations can be performed either directly on IR (regular LTO), or
// on a ThinLTO index (and later applied to the IR during the ThinLTO backend).
// Both types of LTO operate on a the same base graph representation, which
// uses CRTP to support either IR or Index formats.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/IPO/MemProfContextDisambiguation.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SetOperations.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/MemoryProfileInfo.h"
#include "llvm/Analysis/ModuleSummaryAnalysis.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ModuleSummaryIndex.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/GraphWriter.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <sstream>
#include <vector>
using namespace llvm;
using namespace llvm::memprof;

#define DEBUG_TYPE "memprof-context-disambiguation"

STATISTIC(FunctionClonesAnalysis,
          "Number of function clones created during whole program analysis");
STATISTIC(FunctionClonesThinBackend,
          "Number of function clones created during ThinLTO backend");
STATISTIC(FunctionsClonedThinBackend,
          "Number of functions that had clones created during ThinLTO backend");
STATISTIC(AllocTypeNotCold, "Number of not cold static allocations (possibly "
                            "cloned) during whole program analysis");
STATISTIC(AllocTypeCold, "Number of cold static allocations (possibly cloned) "
                         "during whole program analysis");
STATISTIC(AllocTypeNotColdThinBackend,
          "Number of not cold static allocations (possibly cloned) during "
          "ThinLTO backend");
STATISTIC(AllocTypeColdThinBackend, "Number of cold static allocations "
                                    "(possibly cloned) during ThinLTO backend");
STATISTIC(OrigAllocsThinBackend,
          "Number of original (not cloned) allocations with memprof profiles "
          "during ThinLTO backend");
STATISTIC(
    AllocVersionsThinBackend,
    "Number of allocation versions (including clones) during ThinLTO backend");
STATISTIC(MaxAllocVersionsThinBackend,
          "Maximum number of allocation versions created for an original "
          "allocation during ThinLTO backend");
STATISTIC(UnclonableAllocsThinBackend,
          "Number of unclonable ambigous allocations during ThinLTO backend");

static cl::opt<std::string> DotFilePathPrefix(
    "memprof-dot-file-path-prefix", cl::init(""), cl::Hidden,
    cl::value_desc("filename"),
    cl::desc("Specify the path prefix of the MemProf dot files."));

static cl::opt<bool> ExportToDot("memprof-export-to-dot", cl::init(false),
                                 cl::Hidden,
                                 cl::desc("Export graph to dot files."));

static cl::opt<bool>
    DumpCCG("memprof-dump-ccg", cl::init(false), cl::Hidden,
            cl::desc("Dump CallingContextGraph to stdout after each stage."));

static cl::opt<bool>
    VerifyCCG("memprof-verify-ccg", cl::init(false), cl::Hidden,
              cl::desc("Perform verification checks on CallingContextGraph."));

static cl::opt<bool>
    VerifyNodes("memprof-verify-nodes", cl::init(false), cl::Hidden,
                cl::desc("Perform frequent verification checks on nodes."));

static cl::opt<std::string> MemProfImportSummary(
    "memprof-import-summary",
    cl::desc("Import summary to use for testing the ThinLTO backend via opt"),
    cl::Hidden);

/// CRTP base for graphs built from either IR or ThinLTO summary index.
///
/// The graph represents the call contexts in all memprof metadata on allocation
/// calls, with nodes for the allocations themselves, as well as for the calls
/// in each context. The graph is initially built from the allocation memprof
/// metadata (or summary) MIBs. It is then updated to match calls with callsite
/// metadata onto the nodes, updating it to reflect any inlining performed on
/// those calls.
///
/// Each MIB (representing an allocation's call context with allocation
/// behavior) is assigned a unique context id during the graph build. The edges
/// and nodes in the graph are decorated with the context ids they carry. This
/// is used to correctly update the graph when cloning is performed so that we
/// can uniquify the context for a single (possibly cloned) allocation.
template <typename DerivedCCG, typename FuncTy, typename CallTy>
class CallsiteContextGraph {
public:
  CallsiteContextGraph() = default;
  CallsiteContextGraph(const CallsiteContextGraph &) = default;
  CallsiteContextGraph(CallsiteContextGraph &&) = default;

  /// Main entry point to perform analysis and transformations on graph.
  bool process();

  /// Perform cloning on the graph necessary to uniquely identify the allocation
  /// behavior of an allocation based on its context.
  void identifyClones();

  /// Assign callsite clones to functions, cloning functions as needed to
  /// accommodate the combinations of their callsite clones reached by callers.
  /// For regular LTO this clones functions and callsites in the IR, but for
  /// ThinLTO the cloning decisions are noted in the summaries and later applied
  /// in applyImport.
  bool assignFunctions();

  void dump() const;
  void print(raw_ostream &OS) const;

  friend raw_ostream &operator<<(raw_ostream &OS,
                                 const CallsiteContextGraph &CCG) {
    CCG.print(OS);
    return OS;
  }

  friend struct GraphTraits<
      const CallsiteContextGraph<DerivedCCG, FuncTy, CallTy> *>;
  friend struct DOTGraphTraits<
      const CallsiteContextGraph<DerivedCCG, FuncTy, CallTy> *>;

  void exportToDot(std::string Label) const;

  /// Represents a function clone via FuncTy pointer and clone number pair.
  struct FuncInfo final
      : public std::pair<FuncTy *, unsigned /*Clone number*/> {
    using Base = std::pair<FuncTy *, unsigned>;
    FuncInfo(const Base &B) : Base(B) {}
    FuncInfo(FuncTy *F = nullptr, unsigned CloneNo = 0) : Base(F, CloneNo) {}
    explicit operator bool() const { return this->first != nullptr; }
    FuncTy *func() const { return this->first; }
    unsigned cloneNo() const { return this->second; }
  };

  /// Represents a callsite clone via CallTy and clone number pair.
  struct CallInfo final : public std::pair<CallTy, unsigned /*Clone number*/> {
    using Base = std::pair<CallTy, unsigned>;
    CallInfo(const Base &B) : Base(B) {}
    CallInfo(CallTy Call = nullptr, unsigned CloneNo = 0)
        : Base(Call, CloneNo) {}
    explicit operator bool() const { return (bool)this->first; }
    CallTy call() const { return this->first; }
    unsigned cloneNo() const { return this->second; }
    void setCloneNo(unsigned N) { this->second = N; }
    void print(raw_ostream &OS) const {
      if (!operator bool()) {
        assert(!cloneNo());
        OS << "null Call";
        return;
      }
      call()->print(OS);
      OS << "\t(clone " << cloneNo() << ")";
    }
    void dump() const {
      print(dbgs());
      dbgs() << "\n";
    }
    friend raw_ostream &operator<<(raw_ostream &OS, const CallInfo &Call) {
      Call.print(OS);
      return OS;
    }
  };

  struct ContextEdge;

  /// Node in the Callsite Context Graph
  struct ContextNode {
    // Keep this for now since in the IR case where we have an Instruction* it
    // is not as immediately discoverable. Used for printing richer information
    // when dumping graph.
    bool IsAllocation;

    // Keeps track of when the Call was reset to null because there was
    // recursion.
    bool Recursive = false;

    // The corresponding allocation or interior call.
    CallInfo Call;

    // For alloc nodes this is a unique id assigned when constructed, and for
    // callsite stack nodes it is the original stack id when the node is
    // constructed from the memprof MIB metadata on the alloc nodes. Note that
    // this is only used when matching callsite metadata onto the stack nodes
    // created when processing the allocation memprof MIBs, and for labeling
    // nodes in the dot graph. Therefore we don't bother to assign a value for
    // clones.
    uint64_t OrigStackOrAllocId = 0;

    // This will be formed by ORing together the AllocationType enum values
    // for contexts including this node.
    uint8_t AllocTypes = 0;

    // Edges to all callees in the profiled call stacks.
    // TODO: Should this be a map (from Callee node) for more efficient lookup?
    std::vector<std::shared_ptr<ContextEdge>> CalleeEdges;

    // Edges to all callers in the profiled call stacks.
    // TODO: Should this be a map (from Caller node) for more efficient lookup?
    std::vector<std::shared_ptr<ContextEdge>> CallerEdges;

    // The set of IDs for contexts including this node.
    DenseSet<uint32_t> ContextIds;

    // List of clones of this ContextNode, initially empty.
    std::vector<ContextNode *> Clones;

    // If a clone, points to the original uncloned node.
    ContextNode *CloneOf = nullptr;

    ContextNode(bool IsAllocation) : IsAllocation(IsAllocation), Call() {}

    ContextNode(bool IsAllocation, CallInfo C)
        : IsAllocation(IsAllocation), Call(C) {}

    void addClone(ContextNode *Clone) {
      if (CloneOf) {
        CloneOf->Clones.push_back(Clone);
        Clone->CloneOf = CloneOf;
      } else {
        Clones.push_back(Clone);
        assert(!Clone->CloneOf);
        Clone->CloneOf = this;
      }
    }

    ContextNode *getOrigNode() {
      if (!CloneOf)
        return this;
      return CloneOf;
    }

    void addOrUpdateCallerEdge(ContextNode *Caller, AllocationType AllocType,
                               unsigned int ContextId);

    ContextEdge *findEdgeFromCallee(const ContextNode *Callee);
    ContextEdge *findEdgeFromCaller(const ContextNode *Caller);
    void eraseCalleeEdge(const ContextEdge *Edge);
    void eraseCallerEdge(const ContextEdge *Edge);

    void setCall(CallInfo C) { Call = C; }

    bool hasCall() const { return (bool)Call.call(); }

    void printCall(raw_ostream &OS) const { Call.print(OS); }

    // True if this node was effectively removed from the graph, in which case
    // its context id set, caller edges, and callee edges should all be empty.
    bool isRemoved() const {
      assert(ContextIds.empty() ==
             (CalleeEdges.empty() && CallerEdges.empty()));
      return ContextIds.empty();
    }

    void dump() const;
    void print(raw_ostream &OS) const;

    friend raw_ostream &operator<<(raw_ostream &OS, const ContextNode &Node) {
      Node.print(OS);
      return OS;
    }
  };

  /// Edge in the Callsite Context Graph from a ContextNode N to a caller or
  /// callee.
  struct ContextEdge {
    ContextNode *Callee;
    ContextNode *Caller;

    // This will be formed by ORing together the AllocationType enum values
    // for contexts including this edge.
    uint8_t AllocTypes = 0;

    // The set of IDs for contexts including this edge.
    DenseSet<uint32_t> ContextIds;

    ContextEdge(ContextNode *Callee, ContextNode *Caller, uint8_t AllocType,
                DenseSet<uint32_t> ContextIds)
        : Callee(Callee), Caller(Caller), AllocTypes(AllocType),
          ContextIds(ContextIds) {}

    DenseSet<uint32_t> &getContextIds() { return ContextIds; }

    void dump() const;
    void print(raw_ostream &OS) const;

    friend raw_ostream &operator<<(raw_ostream &OS, const ContextEdge &Edge) {
      Edge.print(OS);
      return OS;
    }
  };

  /// Helper to remove callee edges that have allocation type None (due to not
  /// carrying any context ids) after transformations.
  void removeNoneTypeCalleeEdges(ContextNode *Node);

protected:
  /// Get a list of nodes corresponding to the stack ids in the given callsite
  /// context.
  template <class NodeT, class IteratorT>
  std::vector<uint64_t>
  getStackIdsWithContextNodes(CallStack<NodeT, IteratorT> &CallsiteContext);

  /// Adds nodes for the given allocation and any stack ids on its memprof MIB
  /// metadata (or summary).
  ContextNode *addAllocNode(CallInfo Call, const FuncTy *F);

  /// Adds nodes for the given MIB stack ids.
  template <class NodeT, class IteratorT>
  void addStackNodesForMIB(ContextNode *AllocNode,
                           CallStack<NodeT, IteratorT> &StackContext,
                           CallStack<NodeT, IteratorT> &CallsiteContext,
                           AllocationType AllocType);

  /// Matches all callsite metadata (or summary) to the nodes created for
  /// allocation memprof MIB metadata, synthesizing new nodes to reflect any
  /// inlining performed on those callsite instructions.
  void updateStackNodes();

  /// Update graph to conservatively handle any callsite stack nodes that target
  /// multiple different callee target functions.
  void handleCallsitesWithMultipleTargets();

  /// Save lists of calls with MemProf metadata in each function, for faster
  /// iteration.
  std::vector<std::pair<FuncTy *, std::vector<CallInfo>>>
      FuncToCallsWithMetadata;

  /// Map from callsite node to the enclosing caller function.
  std::map<const ContextNode *, const FuncTy *> NodeToCallingFunc;

private:
  using EdgeIter = typename std::vector<std::shared_ptr<ContextEdge>>::iterator;

  using CallContextInfo = std::tuple<CallTy, std::vector<uint64_t>,
                                     const FuncTy *, DenseSet<uint32_t>>;

  /// Assigns the given Node to calls at or inlined into the location with
  /// the Node's stack id, after post order traversing and processing its
  /// caller nodes. Uses the call information recorded in the given
  /// StackIdToMatchingCalls map, and creates new nodes for inlined sequences
  /// as needed. Called by updateStackNodes which sets up the given
  /// StackIdToMatchingCalls map.
  void assignStackNodesPostOrder(
      ContextNode *Node, DenseSet<const ContextNode *> &Visited,
      DenseMap<uint64_t, std::vector<CallContextInfo>> &StackIdToMatchingCalls);

  /// Duplicates the given set of context ids, updating the provided
  /// map from each original id with the newly generated context ids,
  /// and returning the new duplicated id set.
  DenseSet<uint32_t> duplicateContextIds(
      const DenseSet<uint32_t> &StackSequenceContextIds,
      DenseMap<uint32_t, DenseSet<uint32_t>> &OldToNewContextIds);

  /// Propagates all duplicated context ids across the graph.
  void propagateDuplicateContextIds(
      const DenseMap<uint32_t, DenseSet<uint32_t>> &OldToNewContextIds);

  /// Connect the NewNode to OrigNode's callees if TowardsCallee is true,
  /// else to its callers. Also updates OrigNode's edges to remove any context
  /// ids moved to the newly created edge.
  void connectNewNode(ContextNode *NewNode, ContextNode *OrigNode,
                      bool TowardsCallee);

  /// Get the stack id corresponding to the given Id or Index (for IR this will
  /// return itself, for a summary index this will return the id recorded in the
  /// index for that stack id index value).
  uint64_t getStackId(uint64_t IdOrIndex) const {
    return static_cast<const DerivedCCG *>(this)->getStackId(IdOrIndex);
  }

  /// Returns true if the given call targets the given function.
  bool calleeMatchesFunc(CallTy Call, const FuncTy *Func) {
    return static_cast<DerivedCCG *>(this)->calleeMatchesFunc(Call, Func);
  }

  /// Get a list of nodes corresponding to the stack ids in the given
  /// callsite's context.
  std::vector<uint64_t> getStackIdsWithContextNodesForCall(CallTy Call) {
    return static_cast<DerivedCCG *>(this)->getStackIdsWithContextNodesForCall(
        Call);
  }

  /// Get the last stack id in the context for callsite.
  uint64_t getLastStackId(CallTy Call) {
    return static_cast<DerivedCCG *>(this)->getLastStackId(Call);
  }

  /// Update the allocation call to record type of allocated memory.
  void updateAllocationCall(CallInfo &Call, AllocationType AllocType) {
    AllocType == AllocationType::Cold ? AllocTypeCold++ : AllocTypeNotCold++;
    static_cast<DerivedCCG *>(this)->updateAllocationCall(Call, AllocType);
  }

  /// Update non-allocation call to invoke (possibly cloned) function
  /// CalleeFunc.
  void updateCall(CallInfo &CallerCall, FuncInfo CalleeFunc) {
    static_cast<DerivedCCG *>(this)->updateCall(CallerCall, CalleeFunc);
  }

  /// Clone the given function for the given callsite, recording mapping of all
  /// of the functions tracked calls to their new versions in the CallMap.
  /// Assigns new clones to clone number CloneNo.
  FuncInfo cloneFunctionForCallsite(
      FuncInfo &Func, CallInfo &Call, std::map<CallInfo, CallInfo> &CallMap,
      std::vector<CallInfo> &CallsWithMetadataInFunc, unsigned CloneNo) {
    return static_cast<DerivedCCG *>(this)->cloneFunctionForCallsite(
        Func, Call, CallMap, CallsWithMetadataInFunc, CloneNo);
  }

  /// Gets a label to use in the dot graph for the given call clone in the given
  /// function.
  std::string getLabel(const FuncTy *Func, const CallTy Call,
                       unsigned CloneNo) const {
    return static_cast<const DerivedCCG *>(this)->getLabel(Func, Call, CloneNo);
  }

  /// Helpers to find the node corresponding to the given call or stackid.
  ContextNode *getNodeForInst(const CallInfo &C);
  ContextNode *getNodeForAlloc(const CallInfo &C);
  ContextNode *getNodeForStackId(uint64_t StackId);

  /// Removes the node information recorded for the given call.
  void unsetNodeForInst(const CallInfo &C);

  /// Computes the alloc type corresponding to the given context ids, by
  /// unioning their recorded alloc types.
  uint8_t computeAllocType(DenseSet<uint32_t> &ContextIds);

  /// Returns the alloction type of the intersection of the contexts of two
  /// nodes (based on their provided context id sets), optimized for the case
  /// when Node1Ids is smaller than Node2Ids.
  uint8_t intersectAllocTypesImpl(const DenseSet<uint32_t> &Node1Ids,
                                  const DenseSet<uint32_t> &Node2Ids);

  /// Returns the alloction type of the intersection of the contexts of two
  /// nodes (based on their provided context id sets).
  uint8_t intersectAllocTypes(const DenseSet<uint32_t> &Node1Ids,
                              const DenseSet<uint32_t> &Node2Ids);

  /// Create a clone of Edge's callee and move Edge to that new callee node,
  /// performing the necessary context id and allocation type updates.
  /// If callee's caller edge iterator is supplied, it is updated when removing
  /// the edge from that list.
  ContextNode *
  moveEdgeToNewCalleeClone(const std::shared_ptr<ContextEdge> &Edge,
                           EdgeIter *CallerEdgeI = nullptr);

  /// Change the callee of Edge to existing callee clone NewCallee, performing
  /// the necessary context id and allocation type updates.
  /// If callee's caller edge iterator is supplied, it is updated when removing
  /// the edge from that list.
  void moveEdgeToExistingCalleeClone(const std::shared_ptr<ContextEdge> &Edge,
                                     ContextNode *NewCallee,
                                     EdgeIter *CallerEdgeI = nullptr,
                                     bool NewClone = false);

  /// Recursively perform cloning on the graph for the given Node and its
  /// callers, in order to uniquely identify the allocation behavior of an
  /// allocation given its context.
  void identifyClones(ContextNode *Node,
                      DenseSet<const ContextNode *> &Visited);

  /// Map from each context ID to the AllocationType assigned to that context.
  std::map<uint32_t, AllocationType> ContextIdToAllocationType;

  /// Identifies the context node created for a stack id when adding the MIB
  /// contexts to the graph. This is used to locate the context nodes when
  /// trying to assign the corresponding callsites with those stack ids to these
  /// nodes.
  std::map<uint64_t, ContextNode *> StackEntryIdToContextNodeMap;

  /// Maps to track the calls to their corresponding nodes in the graph.
  MapVector<CallInfo, ContextNode *> AllocationCallToContextNodeMap;
  MapVector<CallInfo, ContextNode *> NonAllocationCallToContextNodeMap;

  /// Owner of all ContextNode unique_ptrs.
  std::vector<std::unique_ptr<ContextNode>> NodeOwner;

  /// Perform sanity checks on graph when requested.
  void check() const;

  /// Keeps track of the last unique context id assigned.
  unsigned int LastContextId = 0;
};

template <typename DerivedCCG, typename FuncTy, typename CallTy>
using ContextNode =
    typename CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::ContextNode;
template <typename DerivedCCG, typename FuncTy, typename CallTy>
using ContextEdge =
    typename CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::ContextEdge;
template <typename DerivedCCG, typename FuncTy, typename CallTy>
using FuncInfo =
    typename CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::FuncInfo;
template <typename DerivedCCG, typename FuncTy, typename CallTy>
using CallInfo =
    typename CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::CallInfo;

/// CRTP derived class for graphs built from IR (regular LTO).
class ModuleCallsiteContextGraph
    : public CallsiteContextGraph<ModuleCallsiteContextGraph, Function,
                                  Instruction *> {
public:
  ModuleCallsiteContextGraph(
      Module &M,
      function_ref<OptimizationRemarkEmitter &(Function *)> OREGetter);

private:
  friend CallsiteContextGraph<ModuleCallsiteContextGraph, Function,
                              Instruction *>;

  uint64_t getStackId(uint64_t IdOrIndex) const;
  bool calleeMatchesFunc(Instruction *Call, const Function *Func);
  uint64_t getLastStackId(Instruction *Call);
  std::vector<uint64_t> getStackIdsWithContextNodesForCall(Instruction *Call);
  void updateAllocationCall(CallInfo &Call, AllocationType AllocType);
  void updateCall(CallInfo &CallerCall, FuncInfo CalleeFunc);
  CallsiteContextGraph<ModuleCallsiteContextGraph, Function,
                       Instruction *>::FuncInfo
  cloneFunctionForCallsite(FuncInfo &Func, CallInfo &Call,
                           std::map<CallInfo, CallInfo> &CallMap,
                           std::vector<CallInfo> &CallsWithMetadataInFunc,
                           unsigned CloneNo);
  std::string getLabel(const Function *Func, const Instruction *Call,
                       unsigned CloneNo) const;

  const Module &Mod;
  function_ref<OptimizationRemarkEmitter &(Function *)> OREGetter;
};

/// Represents a call in the summary index graph, which can either be an
/// allocation or an interior callsite node in an allocation's context.
/// Holds a pointer to the corresponding data structure in the index.
struct IndexCall : public PointerUnion<CallsiteInfo *, AllocInfo *> {
  IndexCall() : PointerUnion() {}
  IndexCall(std::nullptr_t) : IndexCall() {}
  IndexCall(CallsiteInfo *StackNode) : PointerUnion(StackNode) {}
  IndexCall(AllocInfo *AllocNode) : PointerUnion(AllocNode) {}
  IndexCall(PointerUnion PT) : PointerUnion(PT) {}

  IndexCall *operator->() { return this; }

  PointerUnion<CallsiteInfo *, AllocInfo *> getBase() const { return *this; }

  void print(raw_ostream &OS) const {
    if (auto *AI = llvm::dyn_cast_if_present<AllocInfo *>(getBase())) {
      OS << *AI;
    } else {
      auto *CI = llvm::dyn_cast_if_present<CallsiteInfo *>(getBase());
      assert(CI);
      OS << *CI;
    }
  }
};

/// CRTP derived class for graphs built from summary index (ThinLTO).
class IndexCallsiteContextGraph
    : public CallsiteContextGraph<IndexCallsiteContextGraph, FunctionSummary,
                                  IndexCall> {
public:
  IndexCallsiteContextGraph(
      ModuleSummaryIndex &Index,
      function_ref<bool(GlobalValue::GUID, const GlobalValueSummary *)>
          isPrevailing);

private:
  friend CallsiteContextGraph<IndexCallsiteContextGraph, FunctionSummary,
                              IndexCall>;

  uint64_t getStackId(uint64_t IdOrIndex) const;
  bool calleeMatchesFunc(IndexCall &Call, const FunctionSummary *Func);
  uint64_t getLastStackId(IndexCall &Call);
  std::vector<uint64_t> getStackIdsWithContextNodesForCall(IndexCall &Call);
  void updateAllocationCall(CallInfo &Call, AllocationType AllocType);
  void updateCall(CallInfo &CallerCall, FuncInfo CalleeFunc);
  CallsiteContextGraph<IndexCallsiteContextGraph, FunctionSummary,
                       IndexCall>::FuncInfo
  cloneFunctionForCallsite(FuncInfo &Func, CallInfo &Call,
                           std::map<CallInfo, CallInfo> &CallMap,
                           std::vector<CallInfo> &CallsWithMetadataInFunc,
                           unsigned CloneNo);
  std::string getLabel(const FunctionSummary *Func, const IndexCall &Call,
                       unsigned CloneNo) const;

  // Saves mapping from function summaries containing memprof records back to
  // its VI, for use in checking and debugging.
  std::map<const FunctionSummary *, ValueInfo> FSToVIMap;

  const ModuleSummaryIndex &Index;
};

namespace llvm {
template <>
struct DenseMapInfo<typename CallsiteContextGraph<
    ModuleCallsiteContextGraph, Function, Instruction *>::CallInfo>
    : public DenseMapInfo<std::pair<Instruction *, unsigned>> {};
template <>
struct DenseMapInfo<typename CallsiteContextGraph<
    IndexCallsiteContextGraph, FunctionSummary, IndexCall>::CallInfo>
    : public DenseMapInfo<std::pair<IndexCall, unsigned>> {};
template <>
struct DenseMapInfo<IndexCall>
    : public DenseMapInfo<PointerUnion<CallsiteInfo *, AllocInfo *>> {};
} // end namespace llvm

namespace {

struct FieldSeparator {
  bool Skip = true;
  const char *Sep;

  FieldSeparator(const char *Sep = ", ") : Sep(Sep) {}
};

raw_ostream &operator<<(raw_ostream &OS, FieldSeparator &FS) {
  if (FS.Skip) {
    FS.Skip = false;
    return OS;
  }
  return OS << FS.Sep;
}

// Map the uint8_t alloc types (which may contain NotCold|Cold) to the alloc
// type we should actually use on the corresponding allocation.
// If we can't clone a node that has NotCold+Cold alloc type, we will fall
// back to using NotCold. So don't bother cloning to distinguish NotCold+Cold
// from NotCold.
AllocationType allocTypeToUse(uint8_t AllocTypes) {
  assert(AllocTypes != (uint8_t)AllocationType::None);
  if (AllocTypes ==
      ((uint8_t)AllocationType::NotCold | (uint8_t)AllocationType::Cold))
    return AllocationType::NotCold;
  else
    return (AllocationType)AllocTypes;
}

// Helper to check if the alloc types for all edges recorded in the
// InAllocTypes vector match the alloc types for all edges in the Edges
// vector.
template <typename DerivedCCG, typename FuncTy, typename CallTy>
bool allocTypesMatch(
    const std::vector<uint8_t> &InAllocTypes,
    const std::vector<std::shared_ptr<ContextEdge<DerivedCCG, FuncTy, CallTy>>>
        &Edges) {
  return std::equal(
      InAllocTypes.begin(), InAllocTypes.end(), Edges.begin(),
      [](const uint8_t &l,
         const std::shared_ptr<ContextEdge<DerivedCCG, FuncTy, CallTy>> &r) {
        // Can share if one of the edges is None type - don't
        // care about the type along that edge as it doesn't
        // exist for those context ids.
        if (l == (uint8_t)AllocationType::None ||
            r->AllocTypes == (uint8_t)AllocationType::None)
          return true;
        return allocTypeToUse(l) == allocTypeToUse(r->AllocTypes);
      });
}

} // end anonymous namespace

template <typename DerivedCCG, typename FuncTy, typename CallTy>
typename CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::ContextNode *
CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::getNodeForInst(
    const CallInfo &C) {
  ContextNode *Node = getNodeForAlloc(C);
  if (Node)
    return Node;

  auto NonAllocCallNode = NonAllocationCallToContextNodeMap.find(C);
  if (NonAllocCallNode != NonAllocationCallToContextNodeMap.end()) {
    return NonAllocCallNode->second;
  }
  return nullptr;
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
typename CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::ContextNode *
CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::getNodeForAlloc(
    const CallInfo &C) {
  auto AllocCallNode = AllocationCallToContextNodeMap.find(C);
  if (AllocCallNode != AllocationCallToContextNodeMap.end()) {
    return AllocCallNode->second;
  }
  return nullptr;
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
typename CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::ContextNode *
CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::getNodeForStackId(
    uint64_t StackId) {
  auto StackEntryNode = StackEntryIdToContextNodeMap.find(StackId);
  if (StackEntryNode != StackEntryIdToContextNodeMap.end())
    return StackEntryNode->second;
  return nullptr;
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::unsetNodeForInst(
    const CallInfo &C) {
  AllocationCallToContextNodeMap.erase(C) ||
      NonAllocationCallToContextNodeMap.erase(C);
  assert(!AllocationCallToContextNodeMap.count(C) &&
         !NonAllocationCallToContextNodeMap.count(C));
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::ContextNode::
    addOrUpdateCallerEdge(ContextNode *Caller, AllocationType AllocType,
                          unsigned int ContextId) {
  for (auto &Edge : CallerEdges) {
    if (Edge->Caller == Caller) {
      Edge->AllocTypes |= (uint8_t)AllocType;
      Edge->getContextIds().insert(ContextId);
      return;
    }
  }
  std::shared_ptr<ContextEdge> Edge = std::make_shared<ContextEdge>(
      this, Caller, (uint8_t)AllocType, DenseSet<uint32_t>({ContextId}));
  CallerEdges.push_back(Edge);
  Caller->CalleeEdges.push_back(Edge);
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<
    DerivedCCG, FuncTy, CallTy>::removeNoneTypeCalleeEdges(ContextNode *Node) {
  for (auto EI = Node->CalleeEdges.begin(); EI != Node->CalleeEdges.end();) {
    auto Edge = *EI;
    if (Edge->AllocTypes == (uint8_t)AllocationType::None) {
      assert(Edge->ContextIds.empty());
      Edge->Callee->eraseCallerEdge(Edge.get());
      EI = Node->CalleeEdges.erase(EI);
    } else
      ++EI;
  }
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
typename CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::ContextEdge *
CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::ContextNode::
    findEdgeFromCallee(const ContextNode *Callee) {
  for (const auto &Edge : CalleeEdges)
    if (Edge->Callee == Callee)
      return Edge.get();
  return nullptr;
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
typename CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::ContextEdge *
CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::ContextNode::
    findEdgeFromCaller(const ContextNode *Caller) {
  for (const auto &Edge : CallerEdges)
    if (Edge->Caller == Caller)
      return Edge.get();
  return nullptr;
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::ContextNode::
    eraseCalleeEdge(const ContextEdge *Edge) {
  auto EI =
      std::find_if(CalleeEdges.begin(), CalleeEdges.end(),
                   [Edge](const std::shared_ptr<ContextEdge> &CalleeEdge) {
                     return CalleeEdge.get() == Edge;
                   });
  assert(EI != CalleeEdges.end());
  CalleeEdges.erase(EI);
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::ContextNode::
    eraseCallerEdge(const ContextEdge *Edge) {
  auto EI =
      std::find_if(CallerEdges.begin(), CallerEdges.end(),
                   [Edge](const std::shared_ptr<ContextEdge> &CallerEdge) {
                     return CallerEdge.get() == Edge;
                   });
  assert(EI != CallerEdges.end());
  CallerEdges.erase(EI);
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
uint8_t CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::computeAllocType(
    DenseSet<uint32_t> &ContextIds) {
  uint8_t BothTypes =
      (uint8_t)AllocationType::Cold | (uint8_t)AllocationType::NotCold;
  uint8_t AllocType = (uint8_t)AllocationType::None;
  for (auto Id : ContextIds) {
    AllocType |= (uint8_t)ContextIdToAllocationType[Id];
    // Bail early if alloc type reached both, no further refinement.
    if (AllocType == BothTypes)
      return AllocType;
  }
  return AllocType;
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
uint8_t
CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::intersectAllocTypesImpl(
    const DenseSet<uint32_t> &Node1Ids, const DenseSet<uint32_t> &Node2Ids) {
  uint8_t BothTypes =
      (uint8_t)AllocationType::Cold | (uint8_t)AllocationType::NotCold;
  uint8_t AllocType = (uint8_t)AllocationType::None;
  for (auto Id : Node1Ids) {
    if (!Node2Ids.count(Id))
      continue;
    AllocType |= (uint8_t)ContextIdToAllocationType[Id];
    // Bail early if alloc type reached both, no further refinement.
    if (AllocType == BothTypes)
      return AllocType;
  }
  return AllocType;
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
uint8_t CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::intersectAllocTypes(
    const DenseSet<uint32_t> &Node1Ids, const DenseSet<uint32_t> &Node2Ids) {
  if (Node1Ids.size() < Node2Ids.size())
    return intersectAllocTypesImpl(Node1Ids, Node2Ids);
  else
    return intersectAllocTypesImpl(Node2Ids, Node1Ids);
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
typename CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::ContextNode *
CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::addAllocNode(
    CallInfo Call, const FuncTy *F) {
  assert(!getNodeForAlloc(Call));
  NodeOwner.push_back(
      std::make_unique<ContextNode>(/*IsAllocation=*/true, Call));
  ContextNode *AllocNode = NodeOwner.back().get();
  AllocationCallToContextNodeMap[Call] = AllocNode;
  NodeToCallingFunc[AllocNode] = F;
  // Use LastContextId as a uniq id for MIB allocation nodes.
  AllocNode->OrigStackOrAllocId = LastContextId;
  // Alloc type should be updated as we add in the MIBs. We should assert
  // afterwards that it is not still None.
  AllocNode->AllocTypes = (uint8_t)AllocationType::None;

  return AllocNode;
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
template <class NodeT, class IteratorT>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::addStackNodesForMIB(
    ContextNode *AllocNode, CallStack<NodeT, IteratorT> &StackContext,
    CallStack<NodeT, IteratorT> &CallsiteContext, AllocationType AllocType) {
  ContextIdToAllocationType[++LastContextId] = AllocType;

  // Update alloc type and context ids for this MIB.
  AllocNode->AllocTypes |= (uint8_t)AllocType;
  AllocNode->ContextIds.insert(LastContextId);

  // Now add or update nodes for each stack id in alloc's context.
  // Later when processing the stack ids on non-alloc callsites we will adjust
  // for any inlining in the context.
  ContextNode *PrevNode = AllocNode;
  // Look for recursion (direct recursion should have been collapsed by
  // module summary analysis, here we should just be detecting mutual
  // recursion). Mark these nodes so we don't try to clone.
  SmallSet<uint64_t, 8> StackIdSet;
  // Skip any on the allocation call (inlining).
  for (auto ContextIter = StackContext.beginAfterSharedPrefix(CallsiteContext);
       ContextIter != StackContext.end(); ++ContextIter) {
    auto StackId = getStackId(*ContextIter);
    ContextNode *StackNode = getNodeForStackId(StackId);
    if (!StackNode) {
      NodeOwner.push_back(
          std::make_unique<ContextNode>(/*IsAllocation=*/false));
      StackNode = NodeOwner.back().get();
      StackEntryIdToContextNodeMap[StackId] = StackNode;
      StackNode->OrigStackOrAllocId = StackId;
    }
    auto Ins = StackIdSet.insert(StackId);
    if (!Ins.second)
      StackNode->Recursive = true;
    StackNode->ContextIds.insert(LastContextId);
    StackNode->AllocTypes |= (uint8_t)AllocType;
    PrevNode->addOrUpdateCallerEdge(StackNode, AllocType, LastContextId);
    PrevNode = StackNode;
  }
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
DenseSet<uint32_t>
CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::duplicateContextIds(
    const DenseSet<uint32_t> &StackSequenceContextIds,
    DenseMap<uint32_t, DenseSet<uint32_t>> &OldToNewContextIds) {
  DenseSet<uint32_t> NewContextIds;
  for (auto OldId : StackSequenceContextIds) {
    NewContextIds.insert(++LastContextId);
    OldToNewContextIds[OldId].insert(LastContextId);
    assert(ContextIdToAllocationType.count(OldId));
    // The new context has the same allocation type as original.
    ContextIdToAllocationType[LastContextId] = ContextIdToAllocationType[OldId];
  }
  return NewContextIds;
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::
    propagateDuplicateContextIds(
        const DenseMap<uint32_t, DenseSet<uint32_t>> &OldToNewContextIds) {
  // Build a set of duplicated context ids corresponding to the input id set.
  auto GetNewIds = [&OldToNewContextIds](const DenseSet<uint32_t> &ContextIds) {
    DenseSet<uint32_t> NewIds;
    for (auto Id : ContextIds)
      if (auto NewId = OldToNewContextIds.find(Id);
          NewId != OldToNewContextIds.end())
        NewIds.insert(NewId->second.begin(), NewId->second.end());
    return NewIds;
  };

  // Recursively update context ids sets along caller edges.
  auto UpdateCallers = [&](ContextNode *Node,
                           DenseSet<const ContextEdge *> &Visited,
                           auto &&UpdateCallers) -> void {
    for (const auto &Edge : Node->CallerEdges) {
      auto Inserted = Visited.insert(Edge.get());
      if (!Inserted.second)
        continue;
      ContextNode *NextNode = Edge->Caller;
      DenseSet<uint32_t> NewIdsToAdd = GetNewIds(Edge->getContextIds());
      // Only need to recursively iterate to NextNode via this caller edge if
      // it resulted in any added ids to NextNode.
      if (!NewIdsToAdd.empty()) {
        Edge->getContextIds().insert(NewIdsToAdd.begin(), NewIdsToAdd.end());
        NextNode->ContextIds.insert(NewIdsToAdd.begin(), NewIdsToAdd.end());
        UpdateCallers(NextNode, Visited, UpdateCallers);
      }
    }
  };

  DenseSet<const ContextEdge *> Visited;
  for (auto &Entry : AllocationCallToContextNodeMap) {
    auto *Node = Entry.second;
    // Update ids on the allocation nodes before calling the recursive
    // update along caller edges, since this simplifies the logic during
    // that traversal.
    DenseSet<uint32_t> NewIdsToAdd = GetNewIds(Node->ContextIds);
    Node->ContextIds.insert(NewIdsToAdd.begin(), NewIdsToAdd.end());
    UpdateCallers(Node, Visited, UpdateCallers);
  }
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::connectNewNode(
    ContextNode *NewNode, ContextNode *OrigNode, bool TowardsCallee) {
  // Make a copy of the context ids, since this will be adjusted below as they
  // are moved.
  DenseSet<uint32_t> RemainingContextIds = NewNode->ContextIds;
  auto &OrigEdges =
      TowardsCallee ? OrigNode->CalleeEdges : OrigNode->CallerEdges;
  // Increment iterator in loop so that we can remove edges as needed.
  for (auto EI = OrigEdges.begin(); EI != OrigEdges.end();) {
    auto Edge = *EI;
    // Remove any matching context ids from Edge, return set that were found and
    // removed, these are the new edge's context ids. Also update the remaining
    // (not found ids).
    DenseSet<uint32_t> NewEdgeContextIds, NotFoundContextIds;
    set_subtract(Edge->getContextIds(), RemainingContextIds, NewEdgeContextIds,
                 NotFoundContextIds);
    RemainingContextIds.swap(NotFoundContextIds);
    // If no matching context ids for this edge, skip it.
    if (NewEdgeContextIds.empty()) {
      ++EI;
      continue;
    }
    if (TowardsCallee) {
      auto NewEdge = std::make_shared<ContextEdge>(
          Edge->Callee, NewNode, computeAllocType(NewEdgeContextIds),
          NewEdgeContextIds);
      NewNode->CalleeEdges.push_back(NewEdge);
      NewEdge->Callee->CallerEdges.push_back(NewEdge);
    } else {
      auto NewEdge = std::make_shared<ContextEdge>(
          NewNode, Edge->Caller, computeAllocType(NewEdgeContextIds),
          NewEdgeContextIds);
      NewNode->CallerEdges.push_back(NewEdge);
      NewEdge->Caller->CalleeEdges.push_back(NewEdge);
    }
    // Remove old edge if context ids empty.
    if (Edge->getContextIds().empty()) {
      if (TowardsCallee) {
        Edge->Callee->eraseCallerEdge(Edge.get());
        EI = OrigNode->CalleeEdges.erase(EI);
      } else {
        Edge->Caller->eraseCalleeEdge(Edge.get());
        EI = OrigNode->CallerEdges.erase(EI);
      }
      continue;
    }
    ++EI;
  }
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::
    assignStackNodesPostOrder(ContextNode *Node,
                              DenseSet<const ContextNode *> &Visited,
                              DenseMap<uint64_t, std::vector<CallContextInfo>>
                                  &StackIdToMatchingCalls) {
  auto Inserted = Visited.insert(Node);
  if (!Inserted.second)
    return;
  // Post order traversal. Iterate over a copy since we may add nodes and
  // therefore new callers during the recursive call, invalidating any
  // iterator over the original edge vector. We don't need to process these
  // new nodes as they were already processed on creation.
  auto CallerEdges = Node->CallerEdges;
  for (auto &Edge : CallerEdges) {
    // Skip any that have been removed during the recursion.
    if (!Edge)
      continue;
    assignStackNodesPostOrder(Edge->Caller, Visited, StackIdToMatchingCalls);
  }

  // If this node's stack id is in the map, update the graph to contain new
  // nodes representing any inlining at interior callsites. Note we move the
  // associated context ids over to the new nodes.

  // Ignore this node if it is for an allocation or we didn't record any
  // stack id lists ending at it.
  if (Node->IsAllocation ||
      !StackIdToMatchingCalls.count(Node->OrigStackOrAllocId))
    return;

  auto &Calls = StackIdToMatchingCalls[Node->OrigStackOrAllocId];
  // Handle the simple case first. A single call with a single stack id.
  // In this case there is no need to create any new context nodes, simply
  // assign the context node for stack id to this Call.
  if (Calls.size() == 1) {
    auto &[Call, Ids, Func, SavedContextIds] = Calls[0];
    if (Ids.size() == 1) {
      assert(SavedContextIds.empty());
      // It should be this Node
      assert(Node == getNodeForStackId(Ids[0]));
      if (Node->Recursive)
        return;
      Node->setCall(Call);
      NonAllocationCallToContextNodeMap[Call] = Node;
      NodeToCallingFunc[Node] = Func;
      return;
    }
  }

  // Find the node for the last stack id, which should be the same
  // across all calls recorded for this id, and is this node's id.
  uint64_t LastId = Node->OrigStackOrAllocId;
  ContextNode *LastNode = getNodeForStackId(LastId);
  // We should only have kept stack ids that had nodes.
  assert(LastNode);

  for (unsigned I = 0; I < Calls.size(); I++) {
    auto &[Call, Ids, Func, SavedContextIds] = Calls[I];
    // Skip any for which we didn't assign any ids, these don't get a node in
    // the graph.
    if (SavedContextIds.empty())
      continue;

    assert(LastId == Ids.back());

    ContextNode *FirstNode = getNodeForStackId(Ids[0]);
    assert(FirstNode);

    // Recompute the context ids for this stack id sequence (the
    // intersection of the context ids of the corresponding nodes).
    // Start with the ids we saved in the map for this call, which could be
    // duplicated context ids. We have to recompute as we might have overlap
    // overlap between the saved context ids for different last nodes, and
    // removed them already during the post order traversal.
    set_intersect(SavedContextIds, FirstNode->ContextIds);
    ContextNode *PrevNode = nullptr;
    for (auto Id : Ids) {
      ContextNode *CurNode = getNodeForStackId(Id);
      // We should only have kept stack ids that had nodes and weren't
      // recursive.
      assert(CurNode);
      assert(!CurNode->Recursive);
      if (!PrevNode) {
        PrevNode = CurNode;
        continue;
      }
      auto *Edge = CurNode->findEdgeFromCallee(PrevNode);
      if (!Edge) {
        SavedContextIds.clear();
        break;
      }
      PrevNode = CurNode;
      set_intersect(SavedContextIds, Edge->getContextIds());

      // If we now have no context ids for clone, skip this call.
      if (SavedContextIds.empty())
        break;
    }
    if (SavedContextIds.empty())
      continue;

    // Create new context node.
    NodeOwner.push_back(
        std::make_unique<ContextNode>(/*IsAllocation=*/false, Call));
    ContextNode *NewNode = NodeOwner.back().get();
    NodeToCallingFunc[NewNode] = Func;
    NonAllocationCallToContextNodeMap[Call] = NewNode;
    NewNode->ContextIds = SavedContextIds;
    NewNode->AllocTypes = computeAllocType(NewNode->ContextIds);

    // Connect to callees of innermost stack frame in inlined call chain.
    // This updates context ids for FirstNode's callee's to reflect those
    // moved to NewNode.
    connectNewNode(NewNode, FirstNode, /*TowardsCallee=*/true);

    // Connect to callers of outermost stack frame in inlined call chain.
    // This updates context ids for FirstNode's caller's to reflect those
    // moved to NewNode.
    connectNewNode(NewNode, LastNode, /*TowardsCallee=*/false);

    // Now we need to remove context ids from edges/nodes between First and
    // Last Node.
    PrevNode = nullptr;
    for (auto Id : Ids) {
      ContextNode *CurNode = getNodeForStackId(Id);
      // We should only have kept stack ids that had nodes.
      assert(CurNode);

      // Remove the context ids moved to NewNode from CurNode, and the
      // edge from the prior node.
      set_subtract(CurNode->ContextIds, NewNode->ContextIds);
      if (PrevNode) {
        auto *PrevEdge = CurNode->findEdgeFromCallee(PrevNode);
        assert(PrevEdge);
        set_subtract(PrevEdge->getContextIds(), NewNode->ContextIds);
        if (PrevEdge->getContextIds().empty()) {
          PrevNode->eraseCallerEdge(PrevEdge);
          CurNode->eraseCalleeEdge(PrevEdge);
        }
      }
      PrevNode = CurNode;
    }
  }
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::updateStackNodes() {
  // Map of stack id to all calls with that as the last (outermost caller)
  // callsite id that has a context node (some might not due to pruning
  // performed during matching of the allocation profile contexts).
  // The CallContextInfo contains the Call and a list of its stack ids with
  // ContextNodes, the function containing Call, and the set of context ids
  // the analysis will eventually identify for use in any new node created
  // for that callsite.
  DenseMap<uint64_t, std::vector<CallContextInfo>> StackIdToMatchingCalls;
  for (auto &[Func, CallsWithMetadata] : FuncToCallsWithMetadata) {
    for (auto &Call : CallsWithMetadata) {
      // Ignore allocations, already handled.
      if (AllocationCallToContextNodeMap.count(Call))
        continue;
      auto StackIdsWithContextNodes =
          getStackIdsWithContextNodesForCall(Call.call());
      // If there were no nodes created for MIBs on allocs (maybe this was in
      // the unambiguous part of the MIB stack that was pruned), ignore.
      if (StackIdsWithContextNodes.empty())
        continue;
      // Otherwise, record this Call along with the list of ids for the last
      // (outermost caller) stack id with a node.
      StackIdToMatchingCalls[StackIdsWithContextNodes.back()].push_back(
          {Call.call(), StackIdsWithContextNodes, Func, {}});
    }
  }

  // First make a pass through all stack ids that correspond to a call,
  // as identified in the above loop. Compute the context ids corresponding to
  // each of these calls when they correspond to multiple stack ids due to
  // due to inlining. Perform any duplication of context ids required when
  // there is more than one call with the same stack ids. Their (possibly newly
  // duplicated) context ids are saved in the StackIdToMatchingCalls map.
  DenseMap<uint32_t, DenseSet<uint32_t>> OldToNewContextIds;
  for (auto &It : StackIdToMatchingCalls) {
    auto &Calls = It.getSecond();
    // Skip single calls with a single stack id. These don't need a new node.
    if (Calls.size() == 1) {
      auto &Ids = std::get<1>(Calls[0]);
      if (Ids.size() == 1)
        continue;
    }
    // In order to do the best and maximal matching of inlined calls to context
    // node sequences we will sort the vectors of stack ids in descending order
    // of length, and within each length, lexicographically by stack id. The
    // latter is so that we can specially handle calls that have identical stack
    // id sequences (either due to cloning or artificially because of the MIB
    // context pruning).
    std::stable_sort(Calls.begin(), Calls.end(),
                     [](const CallContextInfo &A, const CallContextInfo &B) {
                       auto &IdsA = std::get<1>(A);
                       auto &IdsB = std::get<1>(B);
                       return IdsA.size() > IdsB.size() ||
                              (IdsA.size() == IdsB.size() && IdsA < IdsB);
                     });

    // Find the node for the last stack id, which should be the same
    // across all calls recorded for this id, and is the id for this
    // entry in the StackIdToMatchingCalls map.
    uint64_t LastId = It.getFirst();
    ContextNode *LastNode = getNodeForStackId(LastId);
    // We should only have kept stack ids that had nodes.
    assert(LastNode);

    if (LastNode->Recursive)
      continue;

    // Initialize the context ids with the last node's. We will subsequently
    // refine the context ids by computing the intersection along all edges.
    DenseSet<uint32_t> LastNodeContextIds = LastNode->ContextIds;
    assert(!LastNodeContextIds.empty());

    for (unsigned I = 0; I < Calls.size(); I++) {
      auto &[Call, Ids, Func, SavedContextIds] = Calls[I];
      assert(SavedContextIds.empty());
      assert(LastId == Ids.back());

      // First compute the context ids for this stack id sequence (the
      // intersection of the context ids of the corresponding nodes).
      // Start with the remaining saved ids for the last node.
      assert(!LastNodeContextIds.empty());
      DenseSet<uint32_t> StackSequenceContextIds = LastNodeContextIds;

      ContextNode *PrevNode = LastNode;
      ContextNode *CurNode = LastNode;
      bool Skip = false;

      // Iterate backwards through the stack Ids, starting after the last Id
      // in the list, which was handled once outside for all Calls.
      for (auto IdIter = Ids.rbegin() + 1; IdIter != Ids.rend(); IdIter++) {
        auto Id = *IdIter;
        CurNode = getNodeForStackId(Id);
        // We should only have kept stack ids that had nodes.
        assert(CurNode);

        if (CurNode->Recursive) {
          Skip = true;
          break;
        }

        auto *Edge = CurNode->findEdgeFromCaller(PrevNode);
        // If there is no edge then the nodes belong to different MIB contexts,
        // and we should skip this inlined context sequence. For example, this
        // particular inlined context may include stack ids A->B, and we may
        // indeed have nodes for both A and B, but it is possible that they were
        // never profiled in sequence in a single MIB for any allocation (i.e.
        // we might have profiled an allocation that involves the callsite A,
        // but through a different one of its callee callsites, and we might
        // have profiled an allocation that involves callsite B, but reached
        // from a different caller callsite).
        if (!Edge) {
          Skip = true;
          break;
        }
        PrevNode = CurNode;

        // Update the context ids, which is the intersection of the ids along
        // all edges in the sequence.
        set_intersect(StackSequenceContextIds, Edge->getContextIds());

        // If we now have no context ids for clone, skip this call.
        if (StackSequenceContextIds.empty()) {
          Skip = true;
          break;
        }
      }
      if (Skip)
        continue;

      // If some of this call's stack ids did not have corresponding nodes (due
      // to pruning), don't include any context ids for contexts that extend
      // beyond these nodes. Otherwise we would be matching part of unrelated /
      // not fully matching stack contexts. To do this, subtract any context ids
      // found in caller nodes of the last node found above.
      if (Ids.back() != getLastStackId(Call)) {
        for (const auto &PE : CurNode->CallerEdges) {
          set_subtract(StackSequenceContextIds, PE->getContextIds());
          if (StackSequenceContextIds.empty())
            break;
        }
        // If we now have no context ids for clone, skip this call.
        if (StackSequenceContextIds.empty())
          continue;
      }

      // Check if the next set of stack ids is the same (since the Calls vector
      // of tuples is sorted by the stack ids we can just look at the next one).
      bool DuplicateContextIds = false;
      if (I + 1 < Calls.size()) {
        auto NextIds = std::get<1>(Calls[I + 1]);
        DuplicateContextIds = Ids == NextIds;
      }

      // If we don't have duplicate context ids, then we can assign all the
      // context ids computed for the original node sequence to this call.
      // If there are duplicate calls with the same stack ids then we synthesize
      // new context ids that are duplicates of the originals. These are
      // assigned to SavedContextIds, which is a reference into the map entry
      // for this call, allowing us to access these ids later on.
      OldToNewContextIds.reserve(OldToNewContextIds.size() +
                                 StackSequenceContextIds.size());
      SavedContextIds =
          DuplicateContextIds
              ? duplicateContextIds(StackSequenceContextIds, OldToNewContextIds)
              : StackSequenceContextIds;
      assert(!SavedContextIds.empty());

      if (!DuplicateContextIds) {
        // Update saved last node's context ids to remove those that are
        // assigned to other calls, so that it is ready for the next call at
        // this stack id.
        set_subtract(LastNodeContextIds, StackSequenceContextIds);
        if (LastNodeContextIds.empty())
          break;
      }
    }
  }

  // Propagate the duplicate context ids over the graph.
  propagateDuplicateContextIds(OldToNewContextIds);

  if (VerifyCCG)
    check();

  // Now perform a post-order traversal over the graph, starting with the
  // allocation nodes, essentially processing nodes from callers to callees.
  // For any that contains an id in the map, update the graph to contain new
  // nodes representing any inlining at interior callsites. Note we move the
  // associated context ids over to the new nodes.
  DenseSet<const ContextNode *> Visited;
  for (auto &Entry : AllocationCallToContextNodeMap)
    assignStackNodesPostOrder(Entry.second, Visited, StackIdToMatchingCalls);
}

uint64_t ModuleCallsiteContextGraph::getLastStackId(Instruction *Call) {
  CallStack<MDNode, MDNode::op_iterator> CallsiteContext(
      Call->getMetadata(LLVMContext::MD_callsite));
  return CallsiteContext.back();
}

uint64_t IndexCallsiteContextGraph::getLastStackId(IndexCall &Call) {
  assert(isa<CallsiteInfo *>(Call.getBase()));
  CallStack<CallsiteInfo, SmallVector<unsigned>::const_iterator>
      CallsiteContext(dyn_cast_if_present<CallsiteInfo *>(Call.getBase()));
  // Need to convert index into stack id.
  return Index.getStackIdAtIndex(CallsiteContext.back());
}

static const std::string MemProfCloneSuffix = ".memprof.";

static std::string getMemProfFuncName(Twine Base, unsigned CloneNo) {
  // We use CloneNo == 0 to refer to the original version, which doesn't get
  // renamed with a suffix.
  if (!CloneNo)
    return Base.str();
  return (Base + MemProfCloneSuffix + Twine(CloneNo)).str();
}

std::string ModuleCallsiteContextGraph::getLabel(const Function *Func,
                                                 const Instruction *Call,
                                                 unsigned CloneNo) const {
  return (Twine(Call->getFunction()->getName()) + " -> " +
          cast<CallBase>(Call)->getCalledFunction()->getName())
      .str();
}

std::string IndexCallsiteContextGraph::getLabel(const FunctionSummary *Func,
                                                const IndexCall &Call,
                                                unsigned CloneNo) const {
  auto VI = FSToVIMap.find(Func);
  assert(VI != FSToVIMap.end());
  if (isa<AllocInfo *>(Call.getBase()))
    return (VI->second.name() + " -> alloc").str();
  else {
    auto *Callsite = dyn_cast_if_present<CallsiteInfo *>(Call.getBase());
    return (VI->second.name() + " -> " +
            getMemProfFuncName(Callsite->Callee.name(),
                               Callsite->Clones[CloneNo]))
        .str();
  }
}

std::vector<uint64_t>
ModuleCallsiteContextGraph::getStackIdsWithContextNodesForCall(
    Instruction *Call) {
  CallStack<MDNode, MDNode::op_iterator> CallsiteContext(
      Call->getMetadata(LLVMContext::MD_callsite));
  return getStackIdsWithContextNodes<MDNode, MDNode::op_iterator>(
      CallsiteContext);
}

std::vector<uint64_t>
IndexCallsiteContextGraph::getStackIdsWithContextNodesForCall(IndexCall &Call) {
  assert(isa<CallsiteInfo *>(Call.getBase()));
  CallStack<CallsiteInfo, SmallVector<unsigned>::const_iterator>
      CallsiteContext(dyn_cast_if_present<CallsiteInfo *>(Call.getBase()));
  return getStackIdsWithContextNodes<CallsiteInfo,
                                     SmallVector<unsigned>::const_iterator>(
      CallsiteContext);
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
template <class NodeT, class IteratorT>
std::vector<uint64_t>
CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::getStackIdsWithContextNodes(
    CallStack<NodeT, IteratorT> &CallsiteContext) {
  std::vector<uint64_t> StackIds;
  for (auto IdOrIndex : CallsiteContext) {
    auto StackId = getStackId(IdOrIndex);
    ContextNode *Node = getNodeForStackId(StackId);
    if (!Node)
      break;
    StackIds.push_back(StackId);
  }
  return StackIds;
}

ModuleCallsiteContextGraph::ModuleCallsiteContextGraph(
    Module &M, function_ref<OptimizationRemarkEmitter &(Function *)> OREGetter)
    : Mod(M), OREGetter(OREGetter) {
  for (auto &F : M) {
    std::vector<CallInfo> CallsWithMetadata;
    for (auto &BB : F) {
      for (auto &I : BB) {
        if (!isa<CallBase>(I))
          continue;
        if (auto *MemProfMD = I.getMetadata(LLVMContext::MD_memprof)) {
          CallsWithMetadata.push_back(&I);
          auto *AllocNode = addAllocNode(&I, &F);
          auto *CallsiteMD = I.getMetadata(LLVMContext::MD_callsite);
          assert(CallsiteMD);
          CallStack<MDNode, MDNode::op_iterator> CallsiteContext(CallsiteMD);
          // Add all of the MIBs and their stack nodes.
          for (auto &MDOp : MemProfMD->operands()) {
            auto *MIBMD = cast<const MDNode>(MDOp);
            MDNode *StackNode = getMIBStackNode(MIBMD);
            assert(StackNode);
            CallStack<MDNode, MDNode::op_iterator> StackContext(StackNode);
            addStackNodesForMIB<MDNode, MDNode::op_iterator>(
                AllocNode, StackContext, CallsiteContext,
                getMIBAllocType(MIBMD));
          }
          assert(AllocNode->AllocTypes != (uint8_t)AllocationType::None);
          // Memprof and callsite metadata on memory allocations no longer
          // needed.
          I.setMetadata(LLVMContext::MD_memprof, nullptr);
          I.setMetadata(LLVMContext::MD_callsite, nullptr);
        }
        // For callsite metadata, add to list for this function for later use.
        else if (I.getMetadata(LLVMContext::MD_callsite))
          CallsWithMetadata.push_back(&I);
      }
    }
    if (!CallsWithMetadata.empty())
      FuncToCallsWithMetadata.push_back({&F, CallsWithMetadata});
  }

  if (DumpCCG) {
    dbgs() << "CCG before updating call stack chains:\n";
    dbgs() << *this;
  }

  if (ExportToDot)
    exportToDot("prestackupdate");

  updateStackNodes();

  handleCallsitesWithMultipleTargets();

  // Strip off remaining callsite metadata, no longer needed.
  for (auto &FuncEntry : FuncToCallsWithMetadata)
    for (auto &Call : FuncEntry.second)
      Call.call()->setMetadata(LLVMContext::MD_callsite, nullptr);
}

IndexCallsiteContextGraph::IndexCallsiteContextGraph(
    ModuleSummaryIndex &Index,
    function_ref<bool(GlobalValue::GUID, const GlobalValueSummary *)>
        isPrevailing)
    : Index(Index) {
  for (auto &I : Index) {
    auto VI = Index.getValueInfo(I);
    for (auto &S : VI.getSummaryList()) {
      // We should only add the prevailing nodes. Otherwise we may try to clone
      // in a weak copy that won't be linked (and may be different than the
      // prevailing version).
      // We only keep the memprof summary on the prevailing copy now when
      // building the combined index, as a space optimization, however don't
      // rely on this optimization. The linker doesn't resolve local linkage
      // values so don't check whether those are prevailing.
      if (!GlobalValue::isLocalLinkage(S->linkage()) &&
          !isPrevailing(VI.getGUID(), S.get()))
        continue;
      auto *FS = dyn_cast<FunctionSummary>(S.get());
      if (!FS)
        continue;
      std::vector<CallInfo> CallsWithMetadata;
      if (!FS->allocs().empty()) {
        for (auto &AN : FS->mutableAllocs()) {
          // This can happen because of recursion elimination handling that
          // currently exists in ModuleSummaryAnalysis. Skip these for now.
          // We still added them to the summary because we need to be able to
          // correlate properly in applyImport in the backends.
          if (AN.MIBs.empty())
            continue;
          CallsWithMetadata.push_back({&AN});
          auto *AllocNode = addAllocNode({&AN}, FS);
          // Pass an empty CallStack to the CallsiteContext (second)
          // parameter, since for ThinLTO we already collapsed out the inlined
          // stack ids on the allocation call during ModuleSummaryAnalysis.
          CallStack<MIBInfo, SmallVector<unsigned>::const_iterator>
              EmptyContext;
          // Now add all of the MIBs and their stack nodes.
          for (auto &MIB : AN.MIBs) {
            CallStack<MIBInfo, SmallVector<unsigned>::const_iterator>
                StackContext(&MIB);
            addStackNodesForMIB<MIBInfo, SmallVector<unsigned>::const_iterator>(
                AllocNode, StackContext, EmptyContext, MIB.AllocType);
          }
          assert(AllocNode->AllocTypes != (uint8_t)AllocationType::None);
          // Initialize version 0 on the summary alloc node to the current alloc
          // type, unless it has both types in which case make it default, so
          // that in the case where we aren't able to clone the original version
          // always ends up with the default allocation behavior.
          AN.Versions[0] = (uint8_t)allocTypeToUse(AllocNode->AllocTypes);
        }
      }
      // For callsite metadata, add to list for this function for later use.
      if (!FS->callsites().empty())
        for (auto &SN : FS->mutableCallsites())
          CallsWithMetadata.push_back({&SN});

      if (!CallsWithMetadata.empty())
        FuncToCallsWithMetadata.push_back({FS, CallsWithMetadata});

      if (!FS->allocs().empty() || !FS->callsites().empty())
        FSToVIMap[FS] = VI;
    }
  }

  if (DumpCCG) {
    dbgs() << "CCG before updating call stack chains:\n";
    dbgs() << *this;
  }

  if (ExportToDot)
    exportToDot("prestackupdate");

  updateStackNodes();

  handleCallsitesWithMultipleTargets();
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy,
                          CallTy>::handleCallsitesWithMultipleTargets() {
  // Look for and workaround callsites that call multiple functions.
  // This can happen for indirect calls, which needs better handling, and in
  // more rare cases (e.g. macro expansion).
  // TODO: To fix this for indirect calls we will want to perform speculative
  // devirtualization using either the normal PGO info with ICP, or using the
  // information in the profiled MemProf contexts. We can do this prior to
  // this transformation for regular LTO, and for ThinLTO we can simulate that
  // effect in the summary and perform the actual speculative devirtualization
  // while cloning in the ThinLTO backend.
  for (auto Entry = NonAllocationCallToContextNodeMap.begin();
       Entry != NonAllocationCallToContextNodeMap.end();) {
    auto *Node = Entry->second;
    assert(Node->Clones.empty());
    // Check all node callees and see if in the same function.
    bool Removed = false;
    auto Call = Node->Call.call();
    for (auto &Edge : Node->CalleeEdges) {
      if (!Edge->Callee->hasCall())
        continue;
      assert(NodeToCallingFunc.count(Edge->Callee));
      // Check if the called function matches that of the callee node.
      if (calleeMatchesFunc(Call, NodeToCallingFunc[Edge->Callee]))
        continue;
      // Work around by setting Node to have a null call, so it gets
      // skipped during cloning. Otherwise assignFunctions will assert
      // because its data structures are not designed to handle this case.
      Entry = NonAllocationCallToContextNodeMap.erase(Entry);
      Node->setCall(CallInfo());
      Removed = true;
      break;
    }
    if (!Removed)
      Entry++;
  }
}

uint64_t ModuleCallsiteContextGraph::getStackId(uint64_t IdOrIndex) const {
  // In the Module (IR) case this is already the Id.
  return IdOrIndex;
}

uint64_t IndexCallsiteContextGraph::getStackId(uint64_t IdOrIndex) const {
  // In the Index case this is an index into the stack id list in the summary
  // index, convert it to an Id.
  return Index.getStackIdAtIndex(IdOrIndex);
}

bool ModuleCallsiteContextGraph::calleeMatchesFunc(Instruction *Call,
                                                   const Function *Func) {
  auto *CB = dyn_cast<CallBase>(Call);
  if (!CB->getCalledOperand())
    return false;
  auto *CalleeVal = CB->getCalledOperand()->stripPointerCasts();
  auto *CalleeFunc = dyn_cast<Function>(CalleeVal);
  if (CalleeFunc == Func)
    return true;
  auto *Alias = dyn_cast<GlobalAlias>(CalleeVal);
  return Alias && Alias->getAliasee() == Func;
}

bool IndexCallsiteContextGraph::calleeMatchesFunc(IndexCall &Call,
                                                  const FunctionSummary *Func) {
  ValueInfo Callee =
      dyn_cast_if_present<CallsiteInfo *>(Call.getBase())->Callee;
  // If there is no summary list then this is a call to an externally defined
  // symbol.
  AliasSummary *Alias =
      Callee.getSummaryList().empty()
          ? nullptr
          : dyn_cast<AliasSummary>(Callee.getSummaryList()[0].get());
  assert(FSToVIMap.count(Func));
  return Callee == FSToVIMap[Func] ||
         // If callee is an alias, check the aliasee, since only function
         // summary base objects will contain the stack node summaries and thus
         // get a context node.
         (Alias && Alias->getAliaseeVI() == FSToVIMap[Func]);
}

static std::string getAllocTypeString(uint8_t AllocTypes) {
  if (!AllocTypes)
    return "None";
  std::string Str;
  if (AllocTypes & (uint8_t)AllocationType::NotCold)
    Str += "NotCold";
  if (AllocTypes & (uint8_t)AllocationType::Cold)
    Str += "Cold";
  return Str;
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::ContextNode::dump()
    const {
  print(dbgs());
  dbgs() << "\n";
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::ContextNode::print(
    raw_ostream &OS) const {
  OS << "Node " << this << "\n";
  OS << "\t";
  printCall(OS);
  if (Recursive)
    OS << " (recursive)";
  OS << "\n";
  OS << "\tAllocTypes: " << getAllocTypeString(AllocTypes) << "\n";
  OS << "\tContextIds:";
  std::vector<uint32_t> SortedIds(ContextIds.begin(), ContextIds.end());
  std::sort(SortedIds.begin(), SortedIds.end());
  for (auto Id : SortedIds)
    OS << " " << Id;
  OS << "\n";
  OS << "\tCalleeEdges:\n";
  for (auto &Edge : CalleeEdges)
    OS << "\t\t" << *Edge << "\n";
  OS << "\tCallerEdges:\n";
  for (auto &Edge : CallerEdges)
    OS << "\t\t" << *Edge << "\n";
  if (!Clones.empty()) {
    OS << "\tClones: ";
    FieldSeparator FS;
    for (auto *Clone : Clones)
      OS << FS << Clone;
    OS << "\n";
  } else if (CloneOf) {
    OS << "\tClone of " << CloneOf << "\n";
  }
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::ContextEdge::dump()
    const {
  print(dbgs());
  dbgs() << "\n";
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::ContextEdge::print(
    raw_ostream &OS) const {
  OS << "Edge from Callee " << Callee << " to Caller: " << Caller
     << " AllocTypes: " << getAllocTypeString(AllocTypes);
  OS << " ContextIds:";
  std::vector<uint32_t> SortedIds(ContextIds.begin(), ContextIds.end());
  std::sort(SortedIds.begin(), SortedIds.end());
  for (auto Id : SortedIds)
    OS << " " << Id;
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::dump() const {
  print(dbgs());
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::print(
    raw_ostream &OS) const {
  OS << "Callsite Context Graph:\n";
  using GraphType = const CallsiteContextGraph<DerivedCCG, FuncTy, CallTy> *;
  for (const auto Node : nodes<GraphType>(this)) {
    if (Node->isRemoved())
      continue;
    Node->print(OS);
    OS << "\n";
  }
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
static void checkEdge(
    const std::shared_ptr<ContextEdge<DerivedCCG, FuncTy, CallTy>> &Edge) {
  // Confirm that alloc type is not None and that we have at least one context
  // id.
  assert(Edge->AllocTypes != (uint8_t)AllocationType::None);
  assert(!Edge->ContextIds.empty());
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
static void checkNode(const ContextNode<DerivedCCG, FuncTy, CallTy> *Node,
                      bool CheckEdges = true) {
  if (Node->isRemoved())
    return;
  // Node's context ids should be the union of both its callee and caller edge
  // context ids.
  if (Node->CallerEdges.size()) {
    auto EI = Node->CallerEdges.begin();
    auto &FirstEdge = *EI;
    EI++;
    DenseSet<uint32_t> CallerEdgeContextIds(FirstEdge->ContextIds);
    for (; EI != Node->CallerEdges.end(); EI++) {
      const auto &Edge = *EI;
      if (CheckEdges)
        checkEdge<DerivedCCG, FuncTy, CallTy>(Edge);
      set_union(CallerEdgeContextIds, Edge->ContextIds);
    }
    // Node can have more context ids than callers if some contexts terminate at
    // node and some are longer.
    assert(Node->ContextIds == CallerEdgeContextIds ||
           set_is_subset(CallerEdgeContextIds, Node->ContextIds));
  }
  if (Node->CalleeEdges.size()) {
    auto EI = Node->CalleeEdges.begin();
    auto &FirstEdge = *EI;
    EI++;
    DenseSet<uint32_t> CalleeEdgeContextIds(FirstEdge->ContextIds);
    for (; EI != Node->CalleeEdges.end(); EI++) {
      const auto &Edge = *EI;
      if (CheckEdges)
        checkEdge<DerivedCCG, FuncTy, CallTy>(Edge);
      set_union(CalleeEdgeContextIds, Edge->ContextIds);
    }
    assert(Node->ContextIds == CalleeEdgeContextIds);
  }
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::check() const {
  using GraphType = const CallsiteContextGraph<DerivedCCG, FuncTy, CallTy> *;
  for (const auto Node : nodes<GraphType>(this)) {
    checkNode<DerivedCCG, FuncTy, CallTy>(Node, /*CheckEdges=*/false);
    for (auto &Edge : Node->CallerEdges)
      checkEdge<DerivedCCG, FuncTy, CallTy>(Edge);
  }
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
struct GraphTraits<const CallsiteContextGraph<DerivedCCG, FuncTy, CallTy> *> {
  using GraphType = const CallsiteContextGraph<DerivedCCG, FuncTy, CallTy> *;
  using NodeRef = const ContextNode<DerivedCCG, FuncTy, CallTy> *;

  using NodePtrTy = std::unique_ptr<ContextNode<DerivedCCG, FuncTy, CallTy>>;
  static NodeRef getNode(const NodePtrTy &P) { return P.get(); }

  using nodes_iterator =
      mapped_iterator<typename std::vector<NodePtrTy>::const_iterator,
                      decltype(&getNode)>;

  static nodes_iterator nodes_begin(GraphType G) {
    return nodes_iterator(G->NodeOwner.begin(), &getNode);
  }

  static nodes_iterator nodes_end(GraphType G) {
    return nodes_iterator(G->NodeOwner.end(), &getNode);
  }

  static NodeRef getEntryNode(GraphType G) {
    return G->NodeOwner.begin()->get();
  }

  using EdgePtrTy = std::shared_ptr<ContextEdge<DerivedCCG, FuncTy, CallTy>>;
  static const ContextNode<DerivedCCG, FuncTy, CallTy> *
  GetCallee(const EdgePtrTy &P) {
    return P->Callee;
  }

  using ChildIteratorType =
      mapped_iterator<typename std::vector<std::shared_ptr<ContextEdge<
                          DerivedCCG, FuncTy, CallTy>>>::const_iterator,
                      decltype(&GetCallee)>;

  static ChildIteratorType child_begin(NodeRef N) {
    return ChildIteratorType(N->CalleeEdges.begin(), &GetCallee);
  }

  static ChildIteratorType child_end(NodeRef N) {
    return ChildIteratorType(N->CalleeEdges.end(), &GetCallee);
  }
};

template <typename DerivedCCG, typename FuncTy, typename CallTy>
struct DOTGraphTraits<const CallsiteContextGraph<DerivedCCG, FuncTy, CallTy> *>
    : public DefaultDOTGraphTraits {
  DOTGraphTraits(bool IsSimple = false) : DefaultDOTGraphTraits(IsSimple) {}

  using GraphType = const CallsiteContextGraph<DerivedCCG, FuncTy, CallTy> *;
  using GTraits = GraphTraits<GraphType>;
  using NodeRef = typename GTraits::NodeRef;
  using ChildIteratorType = typename GTraits::ChildIteratorType;

  static std::string getNodeLabel(NodeRef Node, GraphType G) {
    std::string LabelString =
        (Twine("OrigId: ") + (Node->IsAllocation ? "Alloc" : "") +
         Twine(Node->OrigStackOrAllocId))
            .str();
    LabelString += "\n";
    if (Node->hasCall()) {
      auto Func = G->NodeToCallingFunc.find(Node);
      assert(Func != G->NodeToCallingFunc.end());
      LabelString +=
          G->getLabel(Func->second, Node->Call.call(), Node->Call.cloneNo());
    } else {
      LabelString += "null call";
      if (Node->Recursive)
        LabelString += " (recursive)";
      else
        LabelString += " (external)";
    }
    return LabelString;
  }

  static std::string getNodeAttributes(NodeRef Node, GraphType) {
    std::string AttributeString = (Twine("tooltip=\"") + getNodeId(Node) + " " +
                                   getContextIds(Node->ContextIds) + "\"")
                                      .str();
    AttributeString +=
        (Twine(",fillcolor=\"") + getColor(Node->AllocTypes) + "\"").str();
    AttributeString += ",style=\"filled\"";
    if (Node->CloneOf) {
      AttributeString += ",color=\"blue\"";
      AttributeString += ",style=\"filled,bold,dashed\"";
    } else
      AttributeString += ",style=\"filled\"";
    return AttributeString;
  }

  static std::string getEdgeAttributes(NodeRef, ChildIteratorType ChildIter,
                                       GraphType) {
    auto &Edge = *(ChildIter.getCurrent());
    return (Twine("tooltip=\"") + getContextIds(Edge->ContextIds) + "\"" +
            Twine(",fillcolor=\"") + getColor(Edge->AllocTypes) + "\"")
        .str();
  }

  // Since the NodeOwners list includes nodes that are no longer connected to
  // the graph, skip them here.
  static bool isNodeHidden(NodeRef Node, GraphType) {
    return Node->isRemoved();
  }

private:
  static std::string getContextIds(const DenseSet<uint32_t> &ContextIds) {
    std::string IdString = "ContextIds:";
    if (ContextIds.size() < 100) {
      std::vector<uint32_t> SortedIds(ContextIds.begin(), ContextIds.end());
      std::sort(SortedIds.begin(), SortedIds.end());
      for (auto Id : SortedIds)
        IdString += (" " + Twine(Id)).str();
    } else {
      IdString += (" (" + Twine(ContextIds.size()) + " ids)").str();
    }
    return IdString;
  }

  static std::string getColor(uint8_t AllocTypes) {
    if (AllocTypes == (uint8_t)AllocationType::NotCold)
      // Color "brown1" actually looks like a lighter red.
      return "brown1";
    if (AllocTypes == (uint8_t)AllocationType::Cold)
      return "cyan";
    if (AllocTypes ==
        ((uint8_t)AllocationType::NotCold | (uint8_t)AllocationType::Cold))
      // Lighter purple.
      return "mediumorchid1";
    return "gray";
  }

  static std::string getNodeId(NodeRef Node) {
    std::stringstream SStream;
    SStream << std::hex << "N0x" << (unsigned long long)Node;
    std::string Result = SStream.str();
    return Result;
  }
};

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::exportToDot(
    std::string Label) const {
  WriteGraph(this, "", false, Label,
             DotFilePathPrefix + "ccg." + Label + ".dot");
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
typename CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::ContextNode *
CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::moveEdgeToNewCalleeClone(
    const std::shared_ptr<ContextEdge> &Edge, EdgeIter *CallerEdgeI) {
  ContextNode *Node = Edge->Callee;
  NodeOwner.push_back(
      std::make_unique<ContextNode>(Node->IsAllocation, Node->Call));
  ContextNode *Clone = NodeOwner.back().get();
  Node->addClone(Clone);
  assert(NodeToCallingFunc.count(Node));
  NodeToCallingFunc[Clone] = NodeToCallingFunc[Node];
  moveEdgeToExistingCalleeClone(Edge, Clone, CallerEdgeI, /*NewClone=*/true);
  return Clone;
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::
    moveEdgeToExistingCalleeClone(const std::shared_ptr<ContextEdge> &Edge,
                                  ContextNode *NewCallee, EdgeIter *CallerEdgeI,
                                  bool NewClone) {
  // NewCallee and Edge's current callee must be clones of the same original
  // node (Edge's current callee may be the original node too).
  assert(NewCallee->getOrigNode() == Edge->Callee->getOrigNode());
  auto &EdgeContextIds = Edge->getContextIds();
  ContextNode *OldCallee = Edge->Callee;
  if (CallerEdgeI)
    *CallerEdgeI = OldCallee->CallerEdges.erase(*CallerEdgeI);
  else
    OldCallee->eraseCallerEdge(Edge.get());
  Edge->Callee = NewCallee;
  NewCallee->CallerEdges.push_back(Edge);
  // Don't need to update Edge's context ids since we are simply reconnecting
  // it.
  set_subtract(OldCallee->ContextIds, EdgeContextIds);
  NewCallee->ContextIds.insert(EdgeContextIds.begin(), EdgeContextIds.end());
  NewCallee->AllocTypes |= Edge->AllocTypes;
  OldCallee->AllocTypes = computeAllocType(OldCallee->ContextIds);
  // OldCallee alloc type should be None iff its context id set is now empty.
  assert((OldCallee->AllocTypes == (uint8_t)AllocationType::None) ==
         OldCallee->ContextIds.empty());
  // Now walk the old callee node's callee edges and move Edge's context ids
  // over to the corresponding edge into the clone (which is created here if
  // this is a newly created clone).
  for (auto &OldCalleeEdge : OldCallee->CalleeEdges) {
    // The context ids moving to the new callee are the subset of this edge's
    // context ids and the context ids on the caller edge being moved.
    DenseSet<uint32_t> EdgeContextIdsToMove =
        set_intersection(OldCalleeEdge->getContextIds(), EdgeContextIds);
    set_subtract(OldCalleeEdge->getContextIds(), EdgeContextIdsToMove);
    OldCalleeEdge->AllocTypes =
        computeAllocType(OldCalleeEdge->getContextIds());
    if (!NewClone) {
      // Update context ids / alloc type on corresponding edge to NewCallee.
      // There is a chance this may not exist if we are reusing an existing
      // clone, specifically during function assignment, where we would have
      // removed none type edges after creating the clone. If we can't find
      // a corresponding edge there, fall through to the cloning below.
      if (auto *NewCalleeEdge =
              NewCallee->findEdgeFromCallee(OldCalleeEdge->Callee)) {
        NewCalleeEdge->getContextIds().insert(EdgeContextIdsToMove.begin(),
                                              EdgeContextIdsToMove.end());
        NewCalleeEdge->AllocTypes |= computeAllocType(EdgeContextIdsToMove);
        continue;
      }
    }
    auto NewEdge = std::make_shared<ContextEdge>(
        OldCalleeEdge->Callee, NewCallee,
        computeAllocType(EdgeContextIdsToMove), EdgeContextIdsToMove);
    NewCallee->CalleeEdges.push_back(NewEdge);
    NewEdge->Callee->CallerEdges.push_back(NewEdge);
  }
  if (VerifyCCG) {
    checkNode<DerivedCCG, FuncTy, CallTy>(OldCallee, /*CheckEdges=*/false);
    checkNode<DerivedCCG, FuncTy, CallTy>(NewCallee, /*CheckEdges=*/false);
    for (const auto &OldCalleeEdge : OldCallee->CalleeEdges)
      checkNode<DerivedCCG, FuncTy, CallTy>(OldCalleeEdge->Callee,
                                            /*CheckEdges=*/false);
    for (const auto &NewCalleeEdge : NewCallee->CalleeEdges)
      checkNode<DerivedCCG, FuncTy, CallTy>(NewCalleeEdge->Callee,
                                            /*CheckEdges=*/false);
  }
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::identifyClones() {
  DenseSet<const ContextNode *> Visited;
  for (auto &Entry : AllocationCallToContextNodeMap)
    identifyClones(Entry.second, Visited);
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
void CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::identifyClones(
    ContextNode *Node, DenseSet<const ContextNode *> &Visited) {
  if (VerifyNodes)
    checkNode<DerivedCCG, FuncTy, CallTy>(Node);
  assert(!Node->CloneOf);

  // If Node as a null call, then either it wasn't found in the module (regular
  // LTO) or summary index (ThinLTO), or there were other conditions blocking
  // cloning (e.g. recursion, calls multiple targets, etc).
  // Do this here so that we don't try to recursively clone callers below, which
  // isn't useful at least for this node.
  if (!Node->hasCall())
    return;

#ifndef NDEBUG
  auto Insert =
#endif
      Visited.insert(Node);
  // We should not have visited this node yet.
  assert(Insert.second);
  // The recursive call to identifyClones may delete the current edge from the
  // CallerEdges vector. Make a copy and iterate on that, simpler than passing
  // in an iterator and having recursive call erase from it. Other edges may
  // also get removed during the recursion, which will have null Callee and
  // Caller pointers (and are deleted later), so we skip those below.
  {
    auto CallerEdges = Node->CallerEdges;
    for (auto &Edge : CallerEdges) {
      // Skip any that have been removed by an earlier recursive call.
      if (Edge->Callee == nullptr && Edge->Caller == nullptr) {
        assert(!std::count(Node->CallerEdges.begin(), Node->CallerEdges.end(),
                           Edge));
        continue;
      }
      // Ignore any caller we previously visited via another edge.
      if (!Visited.count(Edge->Caller) && !Edge->Caller->CloneOf) {
        identifyClones(Edge->Caller, Visited);
      }
    }
  }

  // Check if we reached an unambiguous call or have have only a single caller.
  if (hasSingleAllocType(Node->AllocTypes) || Node->CallerEdges.size() <= 1)
    return;

  // We need to clone.

  // Try to keep the original version as alloc type NotCold. This will make
  // cases with indirect calls or any other situation with an unknown call to
  // the original function get the default behavior. We do this by sorting the
  // CallerEdges of the Node we will clone by alloc type.
  //
  // Give NotCold edge the lowest sort priority so those edges are at the end of
  // the caller edges vector, and stay on the original version (since the below
  // code clones greedily until it finds all remaining edges have the same type
  // and leaves the remaining ones on the original Node).
  //
  // We shouldn't actually have any None type edges, so the sorting priority for
  // that is arbitrary, and we assert in that case below.
  const unsigned AllocTypeCloningPriority[] = {/*None*/ 3, /*NotCold*/ 4,
                                               /*Cold*/ 1,
                                               /*NotColdCold*/ 2};
  assert(std::size(AllocTypeCloningPriority) ==
         (std::size_t)AllocationType::All + 1);
  std::stable_sort(Node->CallerEdges.begin(), Node->CallerEdges.end(),
                   [&](const std::shared_ptr<ContextEdge> &A,
                       const std::shared_ptr<ContextEdge> &B) {
                     assert(A->AllocTypes != (uint8_t)AllocationType::None &&
                            B->AllocTypes != (uint8_t)AllocationType::None);
                     if (A->AllocTypes == B->AllocTypes)
                       // Use the first context id for each edge as a
                       // tie-breaker.
                       return *A->ContextIds.begin() < *B->ContextIds.begin();
                     return AllocTypeCloningPriority[A->AllocTypes] <
                            AllocTypeCloningPriority[B->AllocTypes];
                   });

  assert(Node->AllocTypes != (uint8_t)AllocationType::None);

  // Iterate until we find no more opportunities for disambiguating the alloc
  // types via cloning. In most cases this loop will terminate once the Node
  // has a single allocation type, in which case no more cloning is needed.
  // We need to be able to remove Edge from CallerEdges, so need to adjust
  // iterator inside the loop.
  for (auto EI = Node->CallerEdges.begin(); EI != Node->CallerEdges.end();) {
    auto CallerEdge = *EI;

    // See if cloning the prior caller edge left this node with a single alloc
    // type or a single caller. In that case no more cloning of Node is needed.
    if (hasSingleAllocType(Node->AllocTypes) || Node->CallerEdges.size() <= 1)
      break;

    // Compute the node callee edge alloc types corresponding to the context ids
    // for this caller edge.
    std::vector<uint8_t> CalleeEdgeAllocTypesForCallerEdge;
    CalleeEdgeAllocTypesForCallerEdge.reserve(Node->CalleeEdges.size());
    for (auto &CalleeEdge : Node->CalleeEdges)
      CalleeEdgeAllocTypesForCallerEdge.push_back(intersectAllocTypes(
          CalleeEdge->getContextIds(), CallerEdge->getContextIds()));

    // Don't clone if doing so will not disambiguate any alloc types amongst
    // caller edges (including the callee edges that would be cloned).
    // Otherwise we will simply move all edges to the clone.
    //
    // First check if by cloning we will disambiguate the caller allocation
    // type from node's allocation type. Query allocTypeToUse so that we don't
    // bother cloning to distinguish NotCold+Cold from NotCold. Note that
    // neither of these should be None type.
    //
    // Then check if by cloning node at least one of the callee edges will be
    // disambiguated by splitting out different context ids.
    assert(CallerEdge->AllocTypes != (uint8_t)AllocationType::None);
    assert(Node->AllocTypes != (uint8_t)AllocationType::None);
    if (allocTypeToUse(CallerEdge->AllocTypes) ==
            allocTypeToUse(Node->AllocTypes) &&
        allocTypesMatch<DerivedCCG, FuncTy, CallTy>(
            CalleeEdgeAllocTypesForCallerEdge, Node->CalleeEdges)) {
      ++EI;
      continue;
    }

    // First see if we can use an existing clone. Check each clone and its
    // callee edges for matching alloc types.
    ContextNode *Clone = nullptr;
    for (auto *CurClone : Node->Clones) {
      if (allocTypeToUse(CurClone->AllocTypes) !=
          allocTypeToUse(CallerEdge->AllocTypes))
        continue;

      if (!allocTypesMatch<DerivedCCG, FuncTy, CallTy>(
              CalleeEdgeAllocTypesForCallerEdge, CurClone->CalleeEdges))
        continue;
      Clone = CurClone;
      break;
    }

    // The edge iterator is adjusted when we move the CallerEdge to the clone.
    if (Clone)
      moveEdgeToExistingCalleeClone(CallerEdge, Clone, &EI);
    else
      Clone = moveEdgeToNewCalleeClone(CallerEdge, &EI);

    assert(EI == Node->CallerEdges.end() ||
           Node->AllocTypes != (uint8_t)AllocationType::None);
    // Sanity check that no alloc types on clone or its edges are None.
    assert(Clone->AllocTypes != (uint8_t)AllocationType::None);
    assert(llvm::none_of(
        Clone->CallerEdges, [&](const std::shared_ptr<ContextEdge> &E) {
          return E->AllocTypes == (uint8_t)AllocationType::None;
        }));
  }

  // Cloning may have resulted in some cloned callee edges with type None,
  // because they aren't carrying any contexts. Remove those edges.
  for (auto *Clone : Node->Clones) {
    removeNoneTypeCalleeEdges(Clone);
    if (VerifyNodes)
      checkNode<DerivedCCG, FuncTy, CallTy>(Clone);
  }
  // We should still have some context ids on the original Node.
  assert(!Node->ContextIds.empty());

  // Remove any callee edges that ended up with alloc type None after creating
  // clones and updating callee edges.
  removeNoneTypeCalleeEdges(Node);

  // Sanity check that no alloc types on node or edges are None.
  assert(Node->AllocTypes != (uint8_t)AllocationType::None);
  assert(llvm::none_of(Node->CalleeEdges,
                       [&](const std::shared_ptr<ContextEdge> &E) {
                         return E->AllocTypes == (uint8_t)AllocationType::None;
                       }));
  assert(llvm::none_of(Node->CallerEdges,
                       [&](const std::shared_ptr<ContextEdge> &E) {
                         return E->AllocTypes == (uint8_t)AllocationType::None;
                       }));

  if (VerifyNodes)
    checkNode<DerivedCCG, FuncTy, CallTy>(Node);
}

void ModuleCallsiteContextGraph::updateAllocationCall(
    CallInfo &Call, AllocationType AllocType) {
  std::string AllocTypeString = getAllocTypeAttributeString(AllocType);
  auto A = llvm::Attribute::get(Call.call()->getFunction()->getContext(),
                                "memprof", AllocTypeString);
  cast<CallBase>(Call.call())->addFnAttr(A);
  OREGetter(Call.call()->getFunction())
      .emit(OptimizationRemark(DEBUG_TYPE, "MemprofAttribute", Call.call())
            << ore::NV("AllocationCall", Call.call()) << " in clone "
            << ore::NV("Caller", Call.call()->getFunction())
            << " marked with memprof allocation attribute "
            << ore::NV("Attribute", AllocTypeString));
}

void IndexCallsiteContextGraph::updateAllocationCall(CallInfo &Call,
                                                     AllocationType AllocType) {
  auto *AI = Call.call().dyn_cast<AllocInfo *>();
  assert(AI);
  assert(AI->Versions.size() > Call.cloneNo());
  AI->Versions[Call.cloneNo()] = (uint8_t)AllocType;
}

void ModuleCallsiteContextGraph::updateCall(CallInfo &CallerCall,
                                            FuncInfo CalleeFunc) {
  if (CalleeFunc.cloneNo() > 0)
    cast<CallBase>(CallerCall.call())->setCalledFunction(CalleeFunc.func());
  OREGetter(CallerCall.call()->getFunction())
      .emit(OptimizationRemark(DEBUG_TYPE, "MemprofCall", CallerCall.call())
            << ore::NV("Call", CallerCall.call()) << " in clone "
            << ore::NV("Caller", CallerCall.call()->getFunction())
            << " assigned to call function clone "
            << ore::NV("Callee", CalleeFunc.func()));
}

void IndexCallsiteContextGraph::updateCall(CallInfo &CallerCall,
                                           FuncInfo CalleeFunc) {
  auto *CI = CallerCall.call().dyn_cast<CallsiteInfo *>();
  assert(CI &&
         "Caller cannot be an allocation which should not have profiled calls");
  assert(CI->Clones.size() > CallerCall.cloneNo());
  CI->Clones[CallerCall.cloneNo()] = CalleeFunc.cloneNo();
}

CallsiteContextGraph<ModuleCallsiteContextGraph, Function,
                     Instruction *>::FuncInfo
ModuleCallsiteContextGraph::cloneFunctionForCallsite(
    FuncInfo &Func, CallInfo &Call, std::map<CallInfo, CallInfo> &CallMap,
    std::vector<CallInfo> &CallsWithMetadataInFunc, unsigned CloneNo) {
  // Use existing LLVM facilities for cloning and obtaining Call in clone
  ValueToValueMapTy VMap;
  auto *NewFunc = CloneFunction(Func.func(), VMap);
  std::string Name = getMemProfFuncName(Func.func()->getName(), CloneNo);
  assert(!Func.func()->getParent()->getFunction(Name));
  NewFunc->setName(Name);
  for (auto &Inst : CallsWithMetadataInFunc) {
    // This map always has the initial version in it.
    assert(Inst.cloneNo() == 0);
    CallMap[Inst] = {cast<Instruction>(VMap[Inst.call()]), CloneNo};
  }
  OREGetter(Func.func())
      .emit(OptimizationRemark(DEBUG_TYPE, "MemprofClone", Func.func())
            << "created clone " << ore::NV("NewFunction", NewFunc));
  return {NewFunc, CloneNo};
}

CallsiteContextGraph<IndexCallsiteContextGraph, FunctionSummary,
                     IndexCall>::FuncInfo
IndexCallsiteContextGraph::cloneFunctionForCallsite(
    FuncInfo &Func, CallInfo &Call, std::map<CallInfo, CallInfo> &CallMap,
    std::vector<CallInfo> &CallsWithMetadataInFunc, unsigned CloneNo) {
  // Check how many clones we have of Call (and therefore function).
  // The next clone number is the current size of versions array.
  // Confirm this matches the CloneNo provided by the caller, which is based on
  // the number of function clones we have.
  assert(CloneNo ==
         (Call.call().is<AllocInfo *>()
              ? Call.call().dyn_cast<AllocInfo *>()->Versions.size()
              : Call.call().dyn_cast<CallsiteInfo *>()->Clones.size()));
  // Walk all the instructions in this function. Create a new version for
  // each (by adding an entry to the Versions/Clones summary array), and copy
  // over the version being called for the function clone being cloned here.
  // Additionally, add an entry to the CallMap for the new function clone,
  // mapping the original call (clone 0, what is in CallsWithMetadataInFunc)
  // to the new call clone.
  for (auto &Inst : CallsWithMetadataInFunc) {
    // This map always has the initial version in it.
    assert(Inst.cloneNo() == 0);
    if (auto *AI = Inst.call().dyn_cast<AllocInfo *>()) {
      assert(AI->Versions.size() == CloneNo);
      // We assign the allocation type later (in updateAllocationCall), just add
      // an entry for it here.
      AI->Versions.push_back(0);
    } else {
      auto *CI = Inst.call().dyn_cast<CallsiteInfo *>();
      assert(CI && CI->Clones.size() == CloneNo);
      // We assign the clone number later (in updateCall), just add an entry for
      // it here.
      CI->Clones.push_back(0);
    }
    CallMap[Inst] = {Inst.call(), CloneNo};
  }
  return {Func.func(), CloneNo};
}

// This method assigns cloned callsites to functions, cloning the functions as
// needed. The assignment is greedy and proceeds roughly as follows:
//
// For each function Func:
//   For each call with graph Node having clones:
//     Initialize ClonesWorklist to Node and its clones
//     Initialize NodeCloneCount to 0
//     While ClonesWorklist is not empty:
//        Clone = pop front ClonesWorklist
//        NodeCloneCount++
//        If Func has been cloned less than NodeCloneCount times:
//           If NodeCloneCount is 1:
//             Assign Clone to original Func
//             Continue
//           Create a new function clone
//           If other callers not assigned to call a function clone yet:
//              Assign them to call new function clone
//              Continue
//           Assign any other caller calling the cloned version to new clone
//
//        For each caller of Clone:
//           If caller is assigned to call a specific function clone:
//             If we cannot assign Clone to that function clone:
//               Create new callsite Clone NewClone
//               Add NewClone to ClonesWorklist
//               Continue
//             Assign Clone to existing caller's called function clone
//           Else:
//             If Clone not already assigned to a function clone:
//                Assign to first function clone without assignment
//             Assign caller to selected function clone
template <typename DerivedCCG, typename FuncTy, typename CallTy>
bool CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::assignFunctions() {
  bool Changed = false;

  // Keep track of the assignment of nodes (callsites) to function clones they
  // call.
  DenseMap<ContextNode *, FuncInfo> CallsiteToCalleeFuncCloneMap;

  // Update caller node to call function version CalleeFunc, by recording the
  // assignment in CallsiteToCalleeFuncCloneMap.
  auto RecordCalleeFuncOfCallsite = [&](ContextNode *Caller,
                                        const FuncInfo &CalleeFunc) {
    assert(Caller->hasCall());
    CallsiteToCalleeFuncCloneMap[Caller] = CalleeFunc;
  };

  // Walk all functions for which we saw calls with memprof metadata, and handle
  // cloning for each of its calls.
  for (auto &[Func, CallsWithMetadata] : FuncToCallsWithMetadata) {
    FuncInfo OrigFunc(Func);
    // Map from each clone of OrigFunc to a map of remappings of each call of
    // interest (from original uncloned call to the corresponding cloned call in
    // that function clone).
    std::map<FuncInfo, std::map<CallInfo, CallInfo>> FuncClonesToCallMap;
    for (auto &Call : CallsWithMetadata) {
      ContextNode *Node = getNodeForInst(Call);
      // Skip call if we do not have a node for it (all uses of its stack ids
      // were either on inlined chains or pruned from the MIBs), or if we did
      // not create any clones for it.
      if (!Node || Node->Clones.empty())
        continue;
      assert(Node->hasCall() &&
             "Not having a call should have prevented cloning");

      // Track the assignment of function clones to clones of the current
      // callsite Node being handled.
      std::map<FuncInfo, ContextNode *> FuncCloneToCurNodeCloneMap;

      // Assign callsite version CallsiteClone to function version FuncClone,
      // and also assign (possibly cloned) Call to CallsiteClone.
      auto AssignCallsiteCloneToFuncClone = [&](const FuncInfo &FuncClone,
                                                CallInfo &Call,
                                                ContextNode *CallsiteClone,
                                                bool IsAlloc) {
        // Record the clone of callsite node assigned to this function clone.
        FuncCloneToCurNodeCloneMap[FuncClone] = CallsiteClone;

        assert(FuncClonesToCallMap.count(FuncClone));
        std::map<CallInfo, CallInfo> &CallMap = FuncClonesToCallMap[FuncClone];
        CallInfo CallClone(Call);
        if (CallMap.count(Call))
          CallClone = CallMap[Call];
        CallsiteClone->setCall(CallClone);
      };

      // Keep track of the clones of callsite Node that need to be assigned to
      // function clones. This list may be expanded in the loop body below if we
      // find additional cloning is required.
      std::deque<ContextNode *> ClonesWorklist;
      // Ignore original Node if we moved all of its contexts to clones.
      if (!Node->ContextIds.empty())
        ClonesWorklist.push_back(Node);
      ClonesWorklist.insert(ClonesWorklist.end(), Node->Clones.begin(),
                            Node->Clones.end());

      // Now walk through all of the clones of this callsite Node that we need,
      // and determine the assignment to a corresponding clone of the current
      // function (creating new function clones as needed).
      unsigned NodeCloneCount = 0;
      while (!ClonesWorklist.empty()) {
        ContextNode *Clone = ClonesWorklist.front();
        ClonesWorklist.pop_front();
        NodeCloneCount++;
        if (VerifyNodes)
          checkNode<DerivedCCG, FuncTy, CallTy>(Clone);

        // Need to create a new function clone if we have more callsite clones
        // than existing function clones, which would have been assigned to an
        // earlier clone in the list (we assign callsite clones to function
        // clones greedily).
        if (FuncClonesToCallMap.size() < NodeCloneCount) {
          // If this is the first callsite copy, assign to original function.
          if (NodeCloneCount == 1) {
            // Since FuncClonesToCallMap is empty in this case, no clones have
            // been created for this function yet, and no callers should have
            // been assigned a function clone for this callee node yet.
            assert(llvm::none_of(
                Clone->CallerEdges, [&](const std::shared_ptr<ContextEdge> &E) {
                  return CallsiteToCalleeFuncCloneMap.count(E->Caller);
                }));
            // Initialize with empty call map, assign Clone to original function
            // and its callers, and skip to the next clone.
            FuncClonesToCallMap[OrigFunc] = {};
            AssignCallsiteCloneToFuncClone(
                OrigFunc, Call, Clone,
                AllocationCallToContextNodeMap.count(Call));
            for (auto &CE : Clone->CallerEdges) {
              // Ignore any caller that does not have a recorded callsite Call.
              if (!CE->Caller->hasCall())
                continue;
              RecordCalleeFuncOfCallsite(CE->Caller, OrigFunc);
            }
            continue;
          }

          // First locate which copy of OrigFunc to clone again. If a caller
          // of this callsite clone was already assigned to call a particular
          // function clone, we need to redirect all of those callers to the
          // new function clone, and update their other callees within this
          // function.
          FuncInfo PreviousAssignedFuncClone;
          auto EI = llvm::find_if(
              Clone->CallerEdges, [&](const std::shared_ptr<ContextEdge> &E) {
                return CallsiteToCalleeFuncCloneMap.count(E->Caller);
              });
          bool CallerAssignedToCloneOfFunc = false;
          if (EI != Clone->CallerEdges.end()) {
            const std::shared_ptr<ContextEdge> &Edge = *EI;
            PreviousAssignedFuncClone =
                CallsiteToCalleeFuncCloneMap[Edge->Caller];
            CallerAssignedToCloneOfFunc = true;
          }

          // Clone function and save it along with the CallInfo map created
          // during cloning in the FuncClonesToCallMap.
          std::map<CallInfo, CallInfo> NewCallMap;
          unsigned CloneNo = FuncClonesToCallMap.size();
          assert(CloneNo > 0 && "Clone 0 is the original function, which "
                                "should already exist in the map");
          FuncInfo NewFuncClone = cloneFunctionForCallsite(
              OrigFunc, Call, NewCallMap, CallsWithMetadata, CloneNo);
          FuncClonesToCallMap.emplace(NewFuncClone, std::move(NewCallMap));
          FunctionClonesAnalysis++;
          Changed = true;

          // If no caller callsites were already assigned to a clone of this
          // function, we can simply assign this clone to the new func clone
          // and update all callers to it, then skip to the next clone.
          if (!CallerAssignedToCloneOfFunc) {
            AssignCallsiteCloneToFuncClone(
                NewFuncClone, Call, Clone,
                AllocationCallToContextNodeMap.count(Call));
            for (auto &CE : Clone->CallerEdges) {
              // Ignore any caller that does not have a recorded callsite Call.
              if (!CE->Caller->hasCall())
                continue;
              RecordCalleeFuncOfCallsite(CE->Caller, NewFuncClone);
            }
            continue;
          }

          // We may need to do additional node cloning in this case.
          // Reset the CallsiteToCalleeFuncCloneMap entry for any callers
          // that were previously assigned to call PreviousAssignedFuncClone,
          // to record that they now call NewFuncClone.
          for (auto CE : Clone->CallerEdges) {
            // Ignore any caller that does not have a recorded callsite Call.
            if (!CE->Caller->hasCall())
              continue;

            if (!CallsiteToCalleeFuncCloneMap.count(CE->Caller) ||
                // We subsequently fall through to later handling that
                // will perform any additional cloning required for
                // callers that were calling other function clones.
                CallsiteToCalleeFuncCloneMap[CE->Caller] !=
                    PreviousAssignedFuncClone)
              continue;

            RecordCalleeFuncOfCallsite(CE->Caller, NewFuncClone);

            // If we are cloning a function that was already assigned to some
            // callers, then essentially we are creating new callsite clones
            // of the other callsites in that function that are reached by those
            // callers. Clone the other callees of the current callsite's caller
            // that were already assigned to PreviousAssignedFuncClone
            // accordingly. This is important since we subsequently update the
            // calls from the nodes in the graph and their assignments to callee
            // functions recorded in CallsiteToCalleeFuncCloneMap.
            for (auto CalleeEdge : CE->Caller->CalleeEdges) {
              // Skip any that have been removed on an earlier iteration when
              // cleaning up newly None type callee edges.
              if (!CalleeEdge)
                continue;
              ContextNode *Callee = CalleeEdge->Callee;
              // Skip the current callsite, we are looking for other
              // callsites Caller calls, as well as any that does not have a
              // recorded callsite Call.
              if (Callee == Clone || !Callee->hasCall())
                continue;
              ContextNode *NewClone = moveEdgeToNewCalleeClone(CalleeEdge);
              removeNoneTypeCalleeEdges(NewClone);
              // Moving the edge may have resulted in some none type
              // callee edges on the original Callee.
              removeNoneTypeCalleeEdges(Callee);
              assert(NewClone->AllocTypes != (uint8_t)AllocationType::None);
              // If the Callee node was already assigned to call a specific
              // function version, make sure its new clone is assigned to call
              // that same function clone.
              if (CallsiteToCalleeFuncCloneMap.count(Callee))
                RecordCalleeFuncOfCallsite(
                    NewClone, CallsiteToCalleeFuncCloneMap[Callee]);
              // Update NewClone with the new Call clone of this callsite's Call
              // created for the new function clone created earlier.
              // Recall that we have already ensured when building the graph
              // that each caller can only call callsites within the same
              // function, so we are guaranteed that Callee Call is in the
              // current OrigFunc.
              // CallMap is set up as indexed by original Call at clone 0.
              CallInfo OrigCall(Callee->getOrigNode()->Call);
              OrigCall.setCloneNo(0);
              std::map<CallInfo, CallInfo> &CallMap =
                  FuncClonesToCallMap[NewFuncClone];
              assert(CallMap.count(OrigCall));
              CallInfo NewCall(CallMap[OrigCall]);
              assert(NewCall);
              NewClone->setCall(NewCall);
            }
          }
          // Fall through to handling below to perform the recording of the
          // function for this callsite clone. This enables handling of cases
          // where the callers were assigned to different clones of a function.
        }

        // See if we can use existing function clone. Walk through
        // all caller edges to see if any have already been assigned to
        // a clone of this callsite's function. If we can use it, do so. If not,
        // because that function clone is already assigned to a different clone
        // of this callsite, then we need to clone again.
        // Basically, this checking is needed to handle the case where different
        // caller functions/callsites may need versions of this function
        // containing different mixes of callsite clones across the different
        // callsites within the function. If that happens, we need to create
        // additional function clones to handle the various combinations.
        //
        // Keep track of any new clones of this callsite created by the
        // following loop, as well as any existing clone that we decided to
        // assign this clone to.
        std::map<FuncInfo, ContextNode *> FuncCloneToNewCallsiteCloneMap;
        FuncInfo FuncCloneAssignedToCurCallsiteClone;
        // We need to be able to remove Edge from CallerEdges, so need to adjust
        // iterator in the loop.
        for (auto EI = Clone->CallerEdges.begin();
             EI != Clone->CallerEdges.end();) {
          auto Edge = *EI;
          // Ignore any caller that does not have a recorded callsite Call.
          if (!Edge->Caller->hasCall()) {
            EI++;
            continue;
          }
          // If this caller already assigned to call a version of OrigFunc, need
          // to ensure we can assign this callsite clone to that function clone.
          if (CallsiteToCalleeFuncCloneMap.count(Edge->Caller)) {
            FuncInfo FuncCloneCalledByCaller =
                CallsiteToCalleeFuncCloneMap[Edge->Caller];
            // First we need to confirm that this function clone is available
            // for use by this callsite node clone.
            //
            // While FuncCloneToCurNodeCloneMap is built only for this Node and
            // its callsite clones, one of those callsite clones X could have
            // been assigned to the same function clone called by Edge's caller
            // - if Edge's caller calls another callsite within Node's original
            // function, and that callsite has another caller reaching clone X.
            // We need to clone Node again in this case.
            if ((FuncCloneToCurNodeCloneMap.count(FuncCloneCalledByCaller) &&
                 FuncCloneToCurNodeCloneMap[FuncCloneCalledByCaller] !=
                     Clone) ||
                // Detect when we have multiple callers of this callsite that
                // have already been assigned to specific, and different, clones
                // of OrigFunc (due to other unrelated callsites in Func they
                // reach via call contexts). Is this Clone of callsite Node
                // assigned to a different clone of OrigFunc? If so, clone Node
                // again.
                (FuncCloneAssignedToCurCallsiteClone &&
                 FuncCloneAssignedToCurCallsiteClone !=
                     FuncCloneCalledByCaller)) {
              // We need to use a different newly created callsite clone, in
              // order to assign it to another new function clone on a
              // subsequent iteration over the Clones array (adjusted below).
              // Note we specifically do not reset the
              // CallsiteToCalleeFuncCloneMap entry for this caller, so that
              // when this new clone is processed later we know which version of
              // the function to copy (so that other callsite clones we have
              // assigned to that function clone are properly cloned over). See
              // comments in the function cloning handling earlier.

              // Check if we already have cloned this callsite again while
              // walking through caller edges, for a caller calling the same
              // function clone. If so, we can move this edge to that new clone
              // rather than creating yet another new clone.
              if (FuncCloneToNewCallsiteCloneMap.count(
                      FuncCloneCalledByCaller)) {
                ContextNode *NewClone =
                    FuncCloneToNewCallsiteCloneMap[FuncCloneCalledByCaller];
                moveEdgeToExistingCalleeClone(Edge, NewClone, &EI);
                // Cleanup any none type edges cloned over.
                removeNoneTypeCalleeEdges(NewClone);
              } else {
                // Create a new callsite clone.
                ContextNode *NewClone = moveEdgeToNewCalleeClone(Edge, &EI);
                removeNoneTypeCalleeEdges(NewClone);
                FuncCloneToNewCallsiteCloneMap[FuncCloneCalledByCaller] =
                    NewClone;
                // Add to list of clones and process later.
                ClonesWorklist.push_back(NewClone);
                assert(EI == Clone->CallerEdges.end() ||
                       Clone->AllocTypes != (uint8_t)AllocationType::None);
                assert(NewClone->AllocTypes != (uint8_t)AllocationType::None);
              }
              // Moving the caller edge may have resulted in some none type
              // callee edges.
              removeNoneTypeCalleeEdges(Clone);
              // We will handle the newly created callsite clone in a subsequent
              // iteration over this Node's Clones. Continue here since we
              // already adjusted iterator EI while moving the edge.
              continue;
            }

            // Otherwise, we can use the function clone already assigned to this
            // caller.
            if (!FuncCloneAssignedToCurCallsiteClone) {
              FuncCloneAssignedToCurCallsiteClone = FuncCloneCalledByCaller;
              // Assign Clone to FuncCloneCalledByCaller
              AssignCallsiteCloneToFuncClone(
                  FuncCloneCalledByCaller, Call, Clone,
                  AllocationCallToContextNodeMap.count(Call));
            } else
              // Don't need to do anything - callsite is already calling this
              // function clone.
              assert(FuncCloneAssignedToCurCallsiteClone ==
                     FuncCloneCalledByCaller);

          } else {
            // We have not already assigned this caller to a version of
            // OrigFunc. Do the assignment now.

            // First check if we have already assigned this callsite clone to a
            // clone of OrigFunc for another caller during this iteration over
            // its caller edges.
            if (!FuncCloneAssignedToCurCallsiteClone) {
              // Find first function in FuncClonesToCallMap without an assigned
              // clone of this callsite Node. We should always have one
              // available at this point due to the earlier cloning when the
              // FuncClonesToCallMap size was smaller than the clone number.
              for (auto &CF : FuncClonesToCallMap) {
                if (!FuncCloneToCurNodeCloneMap.count(CF.first)) {
                  FuncCloneAssignedToCurCallsiteClone = CF.first;
                  break;
                }
              }
              assert(FuncCloneAssignedToCurCallsiteClone);
              // Assign Clone to FuncCloneAssignedToCurCallsiteClone
              AssignCallsiteCloneToFuncClone(
                  FuncCloneAssignedToCurCallsiteClone, Call, Clone,
                  AllocationCallToContextNodeMap.count(Call));
            } else
              assert(FuncCloneToCurNodeCloneMap
                         [FuncCloneAssignedToCurCallsiteClone] == Clone);
            // Update callers to record function version called.
            RecordCalleeFuncOfCallsite(Edge->Caller,
                                       FuncCloneAssignedToCurCallsiteClone);
          }

          EI++;
        }
      }
      if (VerifyCCG) {
        checkNode<DerivedCCG, FuncTy, CallTy>(Node);
        for (const auto &PE : Node->CalleeEdges)
          checkNode<DerivedCCG, FuncTy, CallTy>(PE->Callee);
        for (const auto &CE : Node->CallerEdges)
          checkNode<DerivedCCG, FuncTy, CallTy>(CE->Caller);
        for (auto *Clone : Node->Clones) {
          checkNode<DerivedCCG, FuncTy, CallTy>(Clone);
          for (const auto &PE : Clone->CalleeEdges)
            checkNode<DerivedCCG, FuncTy, CallTy>(PE->Callee);
          for (const auto &CE : Clone->CallerEdges)
            checkNode<DerivedCCG, FuncTy, CallTy>(CE->Caller);
        }
      }
    }
  }

  auto UpdateCalls = [&](ContextNode *Node,
                         DenseSet<const ContextNode *> &Visited,
                         auto &&UpdateCalls) {
    auto Inserted = Visited.insert(Node);
    if (!Inserted.second)
      return;

    for (auto *Clone : Node->Clones)
      UpdateCalls(Clone, Visited, UpdateCalls);

    for (auto &Edge : Node->CallerEdges)
      UpdateCalls(Edge->Caller, Visited, UpdateCalls);

    // Skip if either no call to update, or if we ended up with no context ids
    // (we moved all edges onto other clones).
    if (!Node->hasCall() || Node->ContextIds.empty())
      return;

    if (Node->IsAllocation) {
      updateAllocationCall(Node->Call, allocTypeToUse(Node->AllocTypes));
      return;
    }

    if (!CallsiteToCalleeFuncCloneMap.count(Node))
      return;

    auto CalleeFunc = CallsiteToCalleeFuncCloneMap[Node];
    updateCall(Node->Call, CalleeFunc);
  };

  // Performs DFS traversal starting from allocation nodes to update calls to
  // reflect cloning decisions recorded earlier. For regular LTO this will
  // update the actual calls in the IR to call the appropriate function clone
  // (and add attributes to allocation calls), whereas for ThinLTO the decisions
  // are recorded in the summary entries.
  DenseSet<const ContextNode *> Visited;
  for (auto &Entry : AllocationCallToContextNodeMap)
    UpdateCalls(Entry.second, Visited, UpdateCalls);

  return Changed;
}

static SmallVector<std::unique_ptr<ValueToValueMapTy>, 4> createFunctionClones(
    Function &F, unsigned NumClones, Module &M, OptimizationRemarkEmitter &ORE,
    std::map<const Function *, SmallPtrSet<const GlobalAlias *, 1>>
        &FuncToAliasMap) {
  // The first "clone" is the original copy, we should only call this if we
  // needed to create new clones.
  assert(NumClones > 1);
  SmallVector<std::unique_ptr<ValueToValueMapTy>, 4> VMaps;
  VMaps.reserve(NumClones - 1);
  FunctionsClonedThinBackend++;
  for (unsigned I = 1; I < NumClones; I++) {
    VMaps.emplace_back(std::make_unique<ValueToValueMapTy>());
    auto *NewF = CloneFunction(&F, *VMaps.back());
    FunctionClonesThinBackend++;
    // Strip memprof and callsite metadata from clone as they are no longer
    // needed.
    for (auto &BB : *NewF) {
      for (auto &Inst : BB) {
        Inst.setMetadata(LLVMContext::MD_memprof, nullptr);
        Inst.setMetadata(LLVMContext::MD_callsite, nullptr);
      }
    }
    std::string Name = getMemProfFuncName(F.getName(), I);
    auto *PrevF = M.getFunction(Name);
    if (PrevF) {
      // We might have created this when adjusting callsite in another
      // function. It should be a declaration.
      assert(PrevF->isDeclaration());
      NewF->takeName(PrevF);
      PrevF->replaceAllUsesWith(NewF);
      PrevF->eraseFromParent();
    } else
      NewF->setName(Name);
    ORE.emit(OptimizationRemark(DEBUG_TYPE, "MemprofClone", &F)
             << "created clone " << ore::NV("NewFunction", NewF));

    // Now handle aliases to this function, and clone those as well.
    if (!FuncToAliasMap.count(&F))
      continue;
    for (auto *A : FuncToAliasMap[&F]) {
      std::string Name = getMemProfFuncName(A->getName(), I);
      auto *PrevA = M.getNamedAlias(Name);
      auto *NewA = GlobalAlias::create(A->getValueType(),
                                       A->getType()->getPointerAddressSpace(),
                                       A->getLinkage(), Name, NewF);
      NewA->copyAttributesFrom(A);
      if (PrevA) {
        // We might have created this when adjusting callsite in another
        // function. It should be a declaration.
        assert(PrevA->isDeclaration());
        NewA->takeName(PrevA);
        PrevA->replaceAllUsesWith(NewA);
        PrevA->eraseFromParent();
      }
    }
  }
  return VMaps;
}

// Locate the summary for F. This is complicated by the fact that it might
// have been internalized or promoted.
static ValueInfo findValueInfoForFunc(const Function &F, const Module &M,
                                      const ModuleSummaryIndex *ImportSummary) {
  // FIXME: Ideally we would retain the original GUID in some fashion on the
  // function (e.g. as metadata), but for now do our best to locate the
  // summary without that information.
  ValueInfo TheFnVI = ImportSummary->getValueInfo(F.getGUID());
  if (!TheFnVI)
    // See if theFn was internalized, by checking index directly with
    // original name (this avoids the name adjustment done by getGUID() for
    // internal symbols).
    TheFnVI = ImportSummary->getValueInfo(GlobalValue::getGUID(F.getName()));
  if (TheFnVI)
    return TheFnVI;
  // Now query with the original name before any promotion was performed.
  StringRef OrigName =
      ModuleSummaryIndex::getOriginalNameBeforePromote(F.getName());
  std::string OrigId = GlobalValue::getGlobalIdentifier(
      OrigName, GlobalValue::InternalLinkage, M.getSourceFileName());
  TheFnVI = ImportSummary->getValueInfo(GlobalValue::getGUID(OrigId));
  if (TheFnVI)
    return TheFnVI;
  // Could be a promoted local imported from another module. We need to pass
  // down more info here to find the original module id. For now, try with
  // the OrigName which might have been stored in the OidGuidMap in the
  // index. This would not work if there were same-named locals in multiple
  // modules, however.
  auto OrigGUID =
      ImportSummary->getGUIDFromOriginalID(GlobalValue::getGUID(OrigName));
  if (OrigGUID)
    TheFnVI = ImportSummary->getValueInfo(OrigGUID);
  return TheFnVI;
}

bool MemProfContextDisambiguation::applyImport(Module &M) {
  assert(ImportSummary);
  bool Changed = false;

  auto IsMemProfClone = [](const Function &F) {
    return F.getName().contains(MemProfCloneSuffix);
  };

  // We also need to clone any aliases that reference cloned functions, because
  // the modified callsites may invoke via the alias. Keep track of the aliases
  // for each function.
  std::map<const Function *, SmallPtrSet<const GlobalAlias *, 1>>
      FuncToAliasMap;
  for (auto &A : M.aliases()) {
    auto *Aliasee = A.getAliaseeObject();
    if (auto *F = dyn_cast<Function>(Aliasee))
      FuncToAliasMap[F].insert(&A);
  }

  for (auto &F : M) {
    if (F.isDeclaration() || IsMemProfClone(F))
      continue;

    OptimizationRemarkEmitter ORE(&F);

    SmallVector<std::unique_ptr<ValueToValueMapTy>, 4> VMaps;
    bool ClonesCreated = false;
    unsigned NumClonesCreated = 0;
    auto CloneFuncIfNeeded = [&](unsigned NumClones) {
      // We should at least have version 0 which is the original copy.
      assert(NumClones > 0);
      // If only one copy needed use original.
      if (NumClones == 1)
        return;
      // If we already performed cloning of this function, confirm that the
      // requested number of clones matches (the thin link should ensure the
      // number of clones for each constituent callsite is consistent within
      // each function), before returning.
      if (ClonesCreated) {
        assert(NumClonesCreated == NumClones);
        return;
      }
      VMaps = createFunctionClones(F, NumClones, M, ORE, FuncToAliasMap);
      // The first "clone" is the original copy, which doesn't have a VMap.
      assert(VMaps.size() == NumClones - 1);
      Changed = true;
      ClonesCreated = true;
      NumClonesCreated = NumClones;
    };

    // Locate the summary for F.
    ValueInfo TheFnVI = findValueInfoForFunc(F, M, ImportSummary);
    // If not found, this could be an imported local (see comment in
    // findValueInfoForFunc). Skip for now as it will be cloned in its original
    // module (where it would have been promoted to global scope so should
    // satisfy any reference in this module).
    if (!TheFnVI)
      continue;

    auto *GVSummary =
        ImportSummary->findSummaryInModule(TheFnVI, M.getModuleIdentifier());
    if (!GVSummary)
      // Must have been imported, use the first summary (might be multiple if
      // this was a linkonce_odr).
      GVSummary = TheFnVI.getSummaryList().front().get();

    // If this was an imported alias skip it as we won't have the function
    // summary, and it should be cloned in the original module.
    if (isa<AliasSummary>(GVSummary))
      continue;

    auto *FS = cast<FunctionSummary>(GVSummary->getBaseObject());

    if (FS->allocs().empty() && FS->callsites().empty())
      continue;

    auto SI = FS->callsites().begin();
    auto AI = FS->allocs().begin();

    // Assume for now that the instructions are in the exact same order
    // as when the summary was created, but confirm this is correct by
    // matching the stack ids.
    for (auto &BB : F) {
      for (auto &I : BB) {
        auto *CB = dyn_cast<CallBase>(&I);
        // Same handling as when creating module summary.
        if (!mayHaveMemprofSummary(CB))
          continue;

        CallStack<MDNode, MDNode::op_iterator> CallsiteContext(
            I.getMetadata(LLVMContext::MD_callsite));
        auto *MemProfMD = I.getMetadata(LLVMContext::MD_memprof);

        // Include allocs that were already assigned a memprof function
        // attribute in the statistics.
        if (CB->getAttributes().hasFnAttr("memprof")) {
          assert(!MemProfMD);
          CB->getAttributes().getFnAttr("memprof").getValueAsString() == "cold"
              ? AllocTypeColdThinBackend++
              : AllocTypeNotColdThinBackend++;
          OrigAllocsThinBackend++;
          AllocVersionsThinBackend++;
          if (!MaxAllocVersionsThinBackend)
            MaxAllocVersionsThinBackend = 1;
          // Remove any remaining callsite metadata and we can skip the rest of
          // the handling for this instruction, since no cloning needed.
          I.setMetadata(LLVMContext::MD_callsite, nullptr);
          continue;
        }

        if (MemProfMD) {
          // Consult the next alloc node.
          assert(AI != FS->allocs().end());
          auto &AllocNode = *(AI++);

          // Sanity check that the MIB stack ids match between the summary and
          // instruction metadata.
          auto MIBIter = AllocNode.MIBs.begin();
          for (auto &MDOp : MemProfMD->operands()) {
            assert(MIBIter != AllocNode.MIBs.end());
            LLVM_ATTRIBUTE_UNUSED auto StackIdIndexIter =
                MIBIter->StackIdIndices.begin();
            auto *MIBMD = cast<const MDNode>(MDOp);
            MDNode *StackMDNode = getMIBStackNode(MIBMD);
            assert(StackMDNode);
            SmallVector<unsigned> StackIdsFromMetadata;
            CallStack<MDNode, MDNode::op_iterator> StackContext(StackMDNode);
            for (auto ContextIter =
                     StackContext.beginAfterSharedPrefix(CallsiteContext);
                 ContextIter != StackContext.end(); ++ContextIter) {
              // If this is a direct recursion, simply skip the duplicate
              // entries, to be consistent with how the summary ids were
              // generated during ModuleSummaryAnalysis.
              if (!StackIdsFromMetadata.empty() &&
                  StackIdsFromMetadata.back() == *ContextIter)
                continue;
              assert(StackIdIndexIter != MIBIter->StackIdIndices.end());
              assert(ImportSummary->getStackIdAtIndex(*StackIdIndexIter) ==
                     *ContextIter);
              StackIdIndexIter++;
            }
            MIBIter++;
          }

          // Perform cloning if not yet done.
          CloneFuncIfNeeded(/*NumClones=*/AllocNode.Versions.size());

          OrigAllocsThinBackend++;
          AllocVersionsThinBackend += AllocNode.Versions.size();
          if (MaxAllocVersionsThinBackend < AllocNode.Versions.size())
            MaxAllocVersionsThinBackend = AllocNode.Versions.size();

          // If there is only one version that means we didn't end up
          // considering this function for cloning, and in that case the alloc
          // will still be none type or should have gotten the default NotCold.
          // Skip that after calling clone helper since that does some sanity
          // checks that confirm we haven't decided yet that we need cloning.
          if (AllocNode.Versions.size() == 1) {
            assert((AllocationType)AllocNode.Versions[0] ==
                       AllocationType::NotCold ||
                   (AllocationType)AllocNode.Versions[0] ==
                       AllocationType::None);
            UnclonableAllocsThinBackend++;
            continue;
          }

          // All versions should have a singular allocation type.
          assert(llvm::none_of(AllocNode.Versions, [](uint8_t Type) {
            return Type == ((uint8_t)AllocationType::NotCold |
                            (uint8_t)AllocationType::Cold);
          }));

          // Update the allocation types per the summary info.
          for (unsigned J = 0; J < AllocNode.Versions.size(); J++) {
            // Ignore any that didn't get an assigned allocation type.
            if (AllocNode.Versions[J] == (uint8_t)AllocationType::None)
              continue;
            AllocationType AllocTy = (AllocationType)AllocNode.Versions[J];
            AllocTy == AllocationType::Cold ? AllocTypeColdThinBackend++
                                            : AllocTypeNotColdThinBackend++;
            std::string AllocTypeString = getAllocTypeAttributeString(AllocTy);
            auto A = llvm::Attribute::get(F.getContext(), "memprof",
                                          AllocTypeString);
            CallBase *CBClone;
            // Copy 0 is the original function.
            if (!J)
              CBClone = CB;
            else
              // Since VMaps are only created for new clones, we index with
              // clone J-1 (J==0 is the original clone and does not have a VMaps
              // entry).
              CBClone = cast<CallBase>((*VMaps[J - 1])[CB]);
            CBClone->addFnAttr(A);
            ORE.emit(OptimizationRemark(DEBUG_TYPE, "MemprofAttribute", CBClone)
                     << ore::NV("AllocationCall", CBClone) << " in clone "
                     << ore::NV("Caller", CBClone->getFunction())
                     << " marked with memprof allocation attribute "
                     << ore::NV("Attribute", AllocTypeString));
          }
        } else if (!CallsiteContext.empty()) {
          // Consult the next callsite node.
          assert(SI != FS->callsites().end());
          auto &StackNode = *(SI++);

#ifndef NDEBUG
          // Sanity check that the stack ids match between the summary and
          // instruction metadata.
          auto StackIdIndexIter = StackNode.StackIdIndices.begin();
          for (auto StackId : CallsiteContext) {
            assert(StackIdIndexIter != StackNode.StackIdIndices.end());
            assert(ImportSummary->getStackIdAtIndex(*StackIdIndexIter) ==
                   StackId);
            StackIdIndexIter++;
          }
#endif

          // Perform cloning if not yet done.
          CloneFuncIfNeeded(/*NumClones=*/StackNode.Clones.size());

          // Should have skipped indirect calls via mayHaveMemprofSummary.
          assert(CB->getCalledFunction());
          assert(!IsMemProfClone(*CB->getCalledFunction()));

          // Update the calls per the summary info.
          // Save orig name since it gets updated in the first iteration
          // below.
          auto CalleeOrigName = CB->getCalledFunction()->getName();
          for (unsigned J = 0; J < StackNode.Clones.size(); J++) {
            // Do nothing if this version calls the original version of its
            // callee.
            if (!StackNode.Clones[J])
              continue;
            auto NewF = M.getOrInsertFunction(
                getMemProfFuncName(CalleeOrigName, StackNode.Clones[J]),
                CB->getCalledFunction()->getFunctionType());
            CallBase *CBClone;
            // Copy 0 is the original function.
            if (!J)
              CBClone = CB;
            else
              CBClone = cast<CallBase>((*VMaps[J - 1])[CB]);
            CBClone->setCalledFunction(NewF);
            ORE.emit(OptimizationRemark(DEBUG_TYPE, "MemprofCall", CBClone)
                     << ore::NV("Call", CBClone) << " in clone "
                     << ore::NV("Caller", CBClone->getFunction())
                     << " assigned to call function clone "
                     << ore::NV("Callee", NewF.getCallee()));
          }
        }
        // Memprof and callsite metadata on memory allocations no longer needed.
        I.setMetadata(LLVMContext::MD_memprof, nullptr);
        I.setMetadata(LLVMContext::MD_callsite, nullptr);
      }
    }
  }

  return Changed;
}

template <typename DerivedCCG, typename FuncTy, typename CallTy>
bool CallsiteContextGraph<DerivedCCG, FuncTy, CallTy>::process() {
  if (DumpCCG) {
    dbgs() << "CCG before cloning:\n";
    dbgs() << *this;
  }
  if (ExportToDot)
    exportToDot("postbuild");

  if (VerifyCCG) {
    check();
  }

  identifyClones();

  if (VerifyCCG) {
    check();
  }

  if (DumpCCG) {
    dbgs() << "CCG after cloning:\n";
    dbgs() << *this;
  }
  if (ExportToDot)
    exportToDot("cloned");

  bool Changed = assignFunctions();

  if (DumpCCG) {
    dbgs() << "CCG after assigning function clones:\n";
    dbgs() << *this;
  }
  if (ExportToDot)
    exportToDot("clonefuncassign");

  return Changed;
}

bool MemProfContextDisambiguation::processModule(
    Module &M,
    function_ref<OptimizationRemarkEmitter &(Function *)> OREGetter) {

  // If we have an import summary, then the cloning decisions were made during
  // the thin link on the index. Apply them and return.
  if (ImportSummary)
    return applyImport(M);

  ModuleCallsiteContextGraph CCG(M, OREGetter);
  return CCG.process();
}

MemProfContextDisambiguation::MemProfContextDisambiguation(
    const ModuleSummaryIndex *Summary)
    : ImportSummary(Summary) {
  if (ImportSummary) {
    // The MemProfImportSummary should only be used for testing ThinLTO
    // distributed backend handling via opt, in which case we don't have a
    // summary from the pass pipeline.
    assert(MemProfImportSummary.empty());
    return;
  }
  if (MemProfImportSummary.empty())
    return;

  auto ReadSummaryFile =
      errorOrToExpected(MemoryBuffer::getFile(MemProfImportSummary));
  if (!ReadSummaryFile) {
    logAllUnhandledErrors(ReadSummaryFile.takeError(), errs(),
                          "Error loading file '" + MemProfImportSummary +
                              "': ");
    return;
  }
  auto ImportSummaryForTestingOrErr = getModuleSummaryIndex(**ReadSummaryFile);
  if (!ImportSummaryForTestingOrErr) {
    logAllUnhandledErrors(ImportSummaryForTestingOrErr.takeError(), errs(),
                          "Error parsing file '" + MemProfImportSummary +
                              "': ");
    return;
  }
  ImportSummaryForTesting = std::move(*ImportSummaryForTestingOrErr);
  ImportSummary = ImportSummaryForTesting.get();
}

PreservedAnalyses MemProfContextDisambiguation::run(Module &M,
                                                    ModuleAnalysisManager &AM) {
  auto &FAM = AM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
  auto OREGetter = [&](Function *F) -> OptimizationRemarkEmitter & {
    return FAM.getResult<OptimizationRemarkEmitterAnalysis>(*F);
  };
  if (!processModule(M, OREGetter))
    return PreservedAnalyses::all();
  return PreservedAnalyses::none();
}

void MemProfContextDisambiguation::run(
    ModuleSummaryIndex &Index,
    function_ref<bool(GlobalValue::GUID, const GlobalValueSummary *)>
        isPrevailing) {
  IndexCallsiteContextGraph CCG(Index, isPrevailing);
  CCG.process();
}
