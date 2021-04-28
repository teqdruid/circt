//===- RTLCleanup.cpp - RTL Cleanup Pass ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This transformation pass performs various cleanups and canonicalization
// transformations for rtl.module bodies.
//
//===----------------------------------------------------------------------===//

#include "SVPassDetail.h"
#include "circt/Dialect/SV/SVPasses.h"
#include "circt/Support/ImplicitLocOpBuilder.h"

#include "llvm/ADT/TypeSwitch.h"

using namespace circt;

//===----------------------------------------------------------------------===//
// Helper utilities
//===----------------------------------------------------------------------===//

namespace {

/// Check the equivalence of operations by doing a deep comparison of operands
/// and attributes, but does not compare the content of any regions attached to
/// each op.
struct SimpleOperationInfo : public llvm::DenseMapInfo<Operation *> {
  static unsigned getHashValue(const Operation *opC) {
    return mlir::OperationEquivalence::computeHash(
        const_cast<Operation *>(opC));
  }
  static bool isEqual(const Operation *lhsC, const Operation *rhsC) {
    auto *lhs = const_cast<Operation *>(lhsC);
    auto *rhs = const_cast<Operation *>(rhsC);
    if (lhs == rhs)
      return true;
    if (lhs == getTombstoneKey() || lhs == getEmptyKey() ||
        rhs == getTombstoneKey() || rhs == getEmptyKey())
      return false;
    return mlir::OperationEquivalence::isEquivalentTo(lhs, rhs);
  }
};

} // end anonymous namespace

// Merge two regions together. These regions must only have a one block.
static void mergeRegions(Region *region1, Region *region2) {
  assert(region1->getBlocks().size() <= 1 && region2->getBlocks().size() <= 1 &&
         "Can only merge regions with a single block");
  if (region1->empty()) {
    // If both regions are empty, move on to the next pair of regions
    if (region2->empty())
      return;
    // If the first region has no block, move the second region's block over.
    region1->getBlocks().splice(region1->end(), region2->getBlocks());
    return;
  }

  // If the second region is not empty, splice its block into the start of the
  // first region.
  if (!region2->empty()) {
    auto &block1 = region1->front();
    auto &block2 = region2->front();
    block1.getOperations().splice(block1.begin(), block2.getOperations());
  }
}

//===----------------------------------------------------------------------===//
// AlwaysFusionPass
//===----------------------------------------------------------------------===//

namespace {
struct RTLCleanupPass : public sv::RTLCleanupBase<RTLCleanupPass> {
  void runOnOperation() override;

  void runOnRegionsInOp(Operation &op);
  void runOnGraphRegion(Region &region, bool shallow);
  void runOnProceduralRegion(Region &region, bool shallow);
  void runOnEveryOp(Operation &op);

private:
  /// Inline all regions from the second operation into the first and delete the
  /// second operation.
  void mergeOperationsIntoFrom(Operation *op1, Operation *op2,
                               DenseSet<Operation *> &opsToRevisitRegionsIn) {
    assert(op1 != op2 && "Cannot merge an op into itself");
    for (size_t i = 0, e = op1->getNumRegions(); i != e; ++i)
      mergeRegions(&op1->getRegion(i), &op2->getRegion(i));

    // Remember that we need to revisit op1 because it changed.
    opsToRevisitRegionsIn.erase(op2);
    opsToRevisitRegionsIn.insert(op1);
    op2->erase();
    anythingChanged = true;
  }

  bool anythingChanged;
};
} // end anonymous namespace

void RTLCleanupPass::runOnOperation() {
  // Keeps track if anything changed during this pass, used to determine if
  // the analyses were preserved.
  anythingChanged = false;
  runOnGraphRegion(getOperation().getBody(), /*shallow=*/false);

  // If we did not change anything in the graph mark all analysis as
  // preserved.
  if (!anythingChanged)
    markAllAnalysesPreserved();
}

/// Recursively process all of the regions in the specified op, dispatching to
/// graph or procedural processing as appropriate.
void RTLCleanupPass::runOnRegionsInOp(Operation &op) {
  if (op.hasTrait<sv::ProceduralRegion>()) {
    for (auto &region : op.getRegions())
      runOnProceduralRegion(region, /*shallow=*/false);
  } else {
    for (auto &region : op.getRegions())
      runOnGraphRegion(region, /*shallow=*/false);
  }
}

/// Run simplifications on the specified graph region.  If shallow is true, then
/// we only look at the specified region, we don't recurse into subregions.
void RTLCleanupPass::runOnGraphRegion(Region &region, bool shallow) {
  if (region.getBlocks().size() != 1)
    return;
  Block &body = region.front();

  // A set of operations in the current block which are mergable. Any
  // operation in this set is a candidate for another similar operation to
  // merge in to.
  DenseSet<Operation *, SimpleOperationInfo> alwaysFFOpsSeen;
  llvm::SmallDenseMap<Attribute, Operation *, 4> ifdefOps;
  sv::InitialOp initialOpSeen;
  sv::AlwaysCombOp alwaysCombOpSeen;

  // As we merge operations with regions, we need to revisit the regions within
  // them to see if merging the outer level allows simplifications in the inner
  // level.  We do that after our pass so we only revisit each subregion once.
  DenseSet<Operation *> opsToRevisitRegionsIn;

  for (Operation &op : llvm::make_early_inc_range(body)) {
    // Recursively process any regions in the op before we visit it.
    if (!shallow && op.getNumRegions() != 0)
      runOnRegionsInOp(op);
    // Merge alwaysff and always operations by hashing them to check to see if
    // we've already encountered one.  If so, merge them and reprocess the body.
    if (isa<sv::AlwaysOp, sv::AlwaysFFOp>(op)) {
      // Merge identical alwaysff's together and delete the old operation.
      auto itAndInserted = alwaysFFOpsSeen.insert(&op);
      if (itAndInserted.second)
        continue;
      auto *existingAlways = *itAndInserted.first;
      mergeOperationsIntoFrom(&op, existingAlways, opsToRevisitRegionsIn);

      *itAndInserted.first = &op;
      continue;
    }

    // Merge graph ifdefs anywhere in the module.
    if (auto ifdefOp = dyn_cast<sv::IfDefOp>(op)) {
      auto *&entry = ifdefOps[ifdefOp.condAttr()];
      if (entry)
        mergeOperationsIntoFrom(ifdefOp, entry, opsToRevisitRegionsIn);

      entry = ifdefOp;
      continue;
    }

    // Merge initial ops anywhere in the module.
    if (auto initialOp = dyn_cast<sv::InitialOp>(op)) {
      if (initialOpSeen)
        mergeOperationsIntoFrom(initialOp, initialOpSeen,
                                opsToRevisitRegionsIn);
      initialOpSeen = initialOp;
      continue;
    }

    // Merge always_comb ops anywhere in the module.
    if (auto alwaysComb = dyn_cast<sv::AlwaysCombOp>(op)) {
      if (alwaysCombOpSeen)
        mergeOperationsIntoFrom(alwaysComb, alwaysCombOpSeen,
                                opsToRevisitRegionsIn);
      alwaysCombOpSeen = alwaysComb;
      continue;
    }

    // Run common clean ups.
    runOnEveryOp(op);
  }

  // Reprocess the merged body because this may have uncovered other
  // simplifications.  Note that iterating over a set is generally not a stable
  // thing to do, but this is a parallel operation whose order of visitation
  // doesn't matter.
  // TODO: This could be a parallel for-each loop.
  for (auto *op : opsToRevisitRegionsIn) {
    for (auto &reg : op->getRegions())
      runOnGraphRegion(reg, /*shallow=*/true);
  }
}

/// Run simplifications on the specified procedural region.  If shallow is true,
/// then we only look at the specified region, we don't recurse into subregions.
void RTLCleanupPass::runOnProceduralRegion(Region &region, bool shallow) {
  if (region.getBlocks().size() != 1)
    return;
  Block &body = region.front();

  // As we merge operations with regions, we need to revisit the regions within
  // them to see if merging the outer level allows simplifications in the inner
  // level.  We do that after our pass so we only revisit each subregion once.
  DenseSet<Operation *> opsToRevisitRegionsIn;

  Operation *lastSideEffectingOp = nullptr;
  for (Operation &op : llvm::make_early_inc_range(body)) {
    // Recursively process any regions in the op before we visit it.
    if (!shallow && op.getNumRegions() != 0)
      runOnRegionsInOp(op);

    // Merge procedural ifdefs with neighbors in the procedural region.
    if (auto ifdef = dyn_cast<sv::IfDefProceduralOp>(op)) {
      if (auto prevIfDef =
              dyn_cast_or_null<sv::IfDefProceduralOp>(lastSideEffectingOp)) {
        if (ifdef.cond() == prevIfDef.cond()) {
          // We know that there are no side effective operations between the
          // two, so merge the first one into this one.
          mergeOperationsIntoFrom(ifdef, prevIfDef, opsToRevisitRegionsIn);
        }
      }
    }

    // Merge 'if' operations with the same condition.
    if (auto ifop = dyn_cast<sv::IfOp>(op)) {
      if (auto prevIf = dyn_cast_or_null<sv::IfOp>(lastSideEffectingOp)) {
        if (ifop.cond() == prevIf.cond()) {
          // We know that there are no side effective operations between the
          // two, so merge the first one into this one.
          mergeOperationsIntoFrom(ifop, prevIf, opsToRevisitRegionsIn);
        }
      }
    }

    // Keep track of the last side effecting operation we've seen.
    if (!mlir::MemoryEffectOpInterface::hasNoEffect(&op))
      lastSideEffectingOp = &op;

    // Run common clean ups.
    runOnEveryOp(op);
  }

  // Reprocess the merged body because this may have uncovered other
  // simplifications.  Note that iterating over a set is generally not a stable
  // thing to do, but this is a parallel operation whose order of visitation
  // doesn't matter.
  // TODO: This could be a parallel for-each loop.
  for (auto *op : opsToRevisitRegionsIn) {
    for (auto &region : op->getRegions())
      runOnProceduralRegion(region, /*shallow=*/true);
  }
}

static mlir::Type padType(mlir::Type orig) {
  return TypeSwitch<Type, Type>(orig);
  int64_t uSize = rtl::getBitWidth(orig);

  ArrayRef<rtl::UnionType::FieldInfo> fields = orig.getElements();
  SmallVector<rtl::UnionType::FieldInfo, 8> paddedFields;

  for (rtl::UnionType::FieldInfo field : fields) {
    // If it's
    if (auto uField = field.type.dyn_cast<rtl::UnionType>())
      field.type = padType(uField);
    int64_t
  }
}

/// This method gets run on every operation regardless of whether it's in a
/// procedural region or not.
void RTLCleanupPass::runOnEveryOp(Operation &op) {
  if (auto uCreateOp = dyn_cast<rtl::UnionCreateOp>(op)) {
    rtl::UnionType ut = uCreateOp.getType();
    ImplicitLocOpBuilder build(&op);
  }
}

std::unique_ptr<Pass> circt::sv::createRTLCleanupPass() {
  return std::make_unique<RTLCleanupPass>();
}
