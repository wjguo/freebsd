//===- PhiValues.cpp - Phi Value Analysis ---------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/PhiValues.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Instructions.h"

using namespace llvm;

bool PhiValues::invalidate(Function &, const PreservedAnalyses &PA,
                           FunctionAnalysisManager::Invalidator &) {
  // PhiValues is invalidated if it isn't preserved.
  auto PAC = PA.getChecker<PhiValuesAnalysis>();
  return !(PAC.preserved() || PAC.preservedSet<AllAnalysesOn<Function>>());
}

// The goal here is to find all of the non-phi values reachable from this phi,
// and to do the same for all of the phis reachable from this phi, as doing so
// is necessary anyway in order to get the values for this phi. We do this using
// Tarjan's algorithm with Nuutila's improvements to find the strongly connected
// components of the phi graph rooted in this phi:
//  * All phis in a strongly connected component will have the same reachable
//    non-phi values. The SCC may not be the maximal subgraph for that set of
//    reachable values, but finding out that isn't really necessary (it would
//    only reduce the amount of memory needed to store the values).
//  * Tarjan's algorithm completes components in a bottom-up manner, i.e. it
//    never completes a component before the components reachable from it have
//    been completed. This means that when we complete a component we have
//    everything we need to collect the values reachable from that component.
//  * We collect both the non-phi values reachable from each SCC, as that's what
//    we're ultimately interested in, and all of the reachable values, i.e.
//    including phis, as that makes invalidateValue easier.
void PhiValues::processPhi(const PHINode *Phi,
                           SmallVector<const PHINode *, 8> &Stack) {
  // Initialize the phi with the next depth number.
  assert(DepthMap.lookup(Phi) == 0);
  assert(NextDepthNumber != UINT_MAX);
  unsigned int DepthNumber = ++NextDepthNumber;
  DepthMap[Phi] = DepthNumber;

  // Recursively process the incoming phis of this phi.
  for (Value *PhiOp : Phi->incoming_values()) {
    if (PHINode *PhiPhiOp = dyn_cast<PHINode>(PhiOp)) {
      // Recurse if the phi has not yet been visited.
      if (DepthMap.lookup(PhiPhiOp) == 0)
        processPhi(PhiPhiOp, Stack);
      assert(DepthMap.lookup(PhiPhiOp) != 0);
      // If the phi did not become part of a component then this phi and that
      // phi are part of the same component, so adjust the depth number.
      if (!ReachableMap.count(DepthMap[PhiPhiOp]))
        DepthMap[Phi] = std::min(DepthMap[Phi], DepthMap[PhiPhiOp]);
    }
  }

  // Now that incoming phis have been handled, push this phi to the stack.
  Stack.push_back(Phi);

  // If the depth number has not changed then we've finished collecting the phis
  // of a strongly connected component.
  if (DepthMap[Phi] == DepthNumber) {
    // Collect the reachable values for this component. The phis of this
    // component will be those on top of the depth stach with the same or
    // greater depth number.
    ConstValueSet Reachable;
    while (!Stack.empty() && DepthMap[Stack.back()] >= DepthNumber) {
      const PHINode *ComponentPhi = Stack.pop_back_val();
      Reachable.insert(ComponentPhi);
      DepthMap[ComponentPhi] = DepthNumber;
      for (Value *Op : ComponentPhi->incoming_values()) {
        if (PHINode *PhiOp = dyn_cast<PHINode>(Op)) {
          // If this phi is not part of the same component then that component
          // is guaranteed to have been completed before this one. Therefore we
          // can just add its reachable values to the reachable values of this
          // component.
          auto It = ReachableMap.find(DepthMap[PhiOp]);
          if (It != ReachableMap.end())
            Reachable.insert(It->second.begin(), It->second.end());
        } else {
          Reachable.insert(Op);
        }
      }
    }
    ReachableMap.insert({DepthNumber,Reachable});

    // Filter out phis to get the non-phi reachable values.
    ValueSet NonPhi;
    for (const Value *V : Reachable)
      if (!isa<PHINode>(V))
        NonPhi.insert(const_cast<Value*>(V));
    NonPhiReachableMap.insert({DepthNumber,NonPhi});
  }
}

const PhiValues::ValueSet &PhiValues::getValuesForPhi(const PHINode *PN) {
  if (DepthMap.count(PN) == 0) {
    SmallVector<const PHINode *, 8> Stack;
    processPhi(PN, Stack);
    assert(Stack.empty());
  }
  assert(DepthMap.lookup(PN) != 0);
  return NonPhiReachableMap[DepthMap[PN]];
}

void PhiValues::invalidateValue(const Value *V) {
  // Components that can reach V are invalid.
  SmallVector<unsigned int, 8> InvalidComponents;
  for (auto &Pair : ReachableMap)
    if (Pair.second.count(V))
      InvalidComponents.push_back(Pair.first);

  for (unsigned int N : InvalidComponents) {
    for (const Value *V : ReachableMap[N])
      if (const PHINode *PN = dyn_cast<PHINode>(V))
        DepthMap.erase(PN);
    NonPhiReachableMap.erase(N);
    ReachableMap.erase(N);
  }
}

void PhiValues::releaseMemory() {
  DepthMap.clear();
  NonPhiReachableMap.clear();
  ReachableMap.clear();
}

void PhiValues::print(raw_ostream &OS) const {
  // Iterate through the phi nodes of the function rather than iterating through
  // DepthMap in order to get predictable ordering.
  for (const BasicBlock &BB : F) {
    for (const PHINode &PN : BB.phis()) {
      OS << "PHI ";
      PN.printAsOperand(OS, false);
      OS << " has values:\n";
      unsigned int N = DepthMap.lookup(&PN);
      auto It = NonPhiReachableMap.find(N);
      if (It == NonPhiReachableMap.end())
        OS << "  UNKNOWN\n";
      else if (It->second.empty())
        OS << "  NONE\n";
      else
        for (Value *V : It->second)
          // Printing of an instruction prints two spaces at the start, so
          // handle instructions and everything else slightly differently in
          // order to get consistent indenting.
          if (Instruction *I = dyn_cast<Instruction>(V))
            OS << *I << "\n";
          else
            OS << "  " << *V << "\n";
    }
  }
}

AnalysisKey PhiValuesAnalysis::Key;
PhiValues PhiValuesAnalysis::run(Function &F, FunctionAnalysisManager &) {
  return PhiValues(F);
}

PreservedAnalyses PhiValuesPrinterPass::run(Function &F,
                                            FunctionAnalysisManager &AM) {
  OS << "PHI Values for function: " << F.getName() << "\n";
  PhiValues &PI = AM.getResult<PhiValuesAnalysis>(F);
  for (const BasicBlock &BB : F)
    for (const PHINode &PN : BB.phis())
      PI.getValuesForPhi(&PN);
  PI.print(OS);
  return PreservedAnalyses::all();
}

PhiValuesWrapperPass::PhiValuesWrapperPass() : FunctionPass(ID) {
  initializePhiValuesWrapperPassPass(*PassRegistry::getPassRegistry());
}

bool PhiValuesWrapperPass::runOnFunction(Function &F) {
  Result.reset(new PhiValues(F));
  return false;
}

void PhiValuesWrapperPass::releaseMemory() {
  Result->releaseMemory();
}

void PhiValuesWrapperPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
}

char PhiValuesWrapperPass::ID = 0;

INITIALIZE_PASS(PhiValuesWrapperPass, "phi-values", "Phi Values Analysis", false,
                true)
