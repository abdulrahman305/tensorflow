/* Copyright 2024 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "mlir/Dialect/Arith/IR/Arith.h"  // from @llvm-project
#include "mlir/Dialect/Func/IR/FuncOps.h"  // from @llvm-project
#include "mlir/Dialect/GPU/IR/GPUDialect.h"  // from @llvm-project
#include "mlir/Dialect/Utils/StaticValueUtils.h"  // from @llvm-project
#include "mlir/IR/PatternMatch.h"  // from @llvm-project
#include "mlir/IR/Value.h"  // from @llvm-project
#include "mlir/Pass/Pass.h"  // from @llvm-project
#include "mlir/Support/LogicalResult.h"  // from @llvm-project
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"  // from @llvm-project
#include "xla/service/gpu/fusions/mlir/ir/xla_gpu_ops.h"
#include "xla/service/gpu/fusions/mlir/passes.h"
#include "xla/service/gpu/model/indexing_map.h"

namespace xla {
namespace gpu {

#define GEN_PASS_DEF_SIMPLIFYARITHPASS
#include "xla/service/gpu/fusions/mlir/passes.h.inc"

namespace {

Interval::ComparisonResult EvaluateCmpI(mlir::arith::CmpIPredicate pred,
                                        Interval lhs, Interval rhs) {
  switch (pred) {
    case mlir::arith::CmpIPredicate::eq:
      return lhs.Eq(rhs);
    case mlir::arith::CmpIPredicate::ne:
      return lhs.Ne(rhs);
    case mlir::arith::CmpIPredicate::slt:
    case mlir::arith::CmpIPredicate::ult:
      return lhs.Lt(rhs);
    case mlir::arith::CmpIPredicate::sle:
    case mlir::arith::CmpIPredicate::ule:
      return lhs.Le(rhs);
    case mlir::arith::CmpIPredicate::sgt:
    case mlir::arith::CmpIPredicate::ugt:
      return lhs.Gt(rhs);
    case mlir::arith::CmpIPredicate::sge:
    case mlir::arith::CmpIPredicate::uge:
      return lhs.Ge(rhs);
  }
}

struct RewriteCmpI : mlir::OpRewritePattern<mlir::arith::CmpIOp> {
  using OpRewritePattern::OpRewritePattern;

  mlir::LogicalResult matchAndRewrite(
      mlir::arith::CmpIOp op, mlir::PatternRewriter& rewriter) const override {
    auto rhs = GetRange(op.getRhs());
    auto lhs = GetRange(op.getLhs());
    if (!lhs || !rhs) {
      return rewriter.notifyMatchFailure(op, "failed to deduce input ranges");
    }
    Interval::ComparisonResult result =
        EvaluateCmpI(op.getPredicate(), *lhs, *rhs);
    if (result != std::nullopt) {
      rewriter.replaceOpWithNewOp<mlir::arith::ConstantIntOp>(
          op, *result, rewriter.getI1Type());
      return mlir::success();
    }
    return rewriter.notifyMatchFailure(op, "not a constant result");
  }
};

struct RewriteMaxSi : mlir::OpRewritePattern<mlir::arith::MaxSIOp> {
  using OpRewritePattern::OpRewritePattern;

  mlir::LogicalResult matchAndRewrite(
      mlir::arith::MaxSIOp op, mlir::PatternRewriter& rewriter) const override {
    auto lhs = GetRange(op.getLhs());
    auto rhs = GetRange(op.getRhs());
    if (!lhs || !rhs) {
      return rewriter.notifyMatchFailure(op, "failed to deduce input ranges");
    }
    if (auto lhs_ge_rhs = lhs->Ge(*rhs); lhs_ge_rhs == true) {
      rewriter.replaceOp(op, op.getLhs());
    } else if (auto rhs_ge_lhs = rhs->Ge(*lhs); rhs_ge_lhs == true) {
      rewriter.replaceOp(op, op.getRhs());
    } else {
      return rewriter.notifyMatchFailure(op, "not equal to lhs or rhs");
    }
    return mlir::success();
  }
};

struct RewriteMinSi : mlir::OpRewritePattern<mlir::arith::MinSIOp> {
  using OpRewritePattern::OpRewritePattern;

  mlir::LogicalResult matchAndRewrite(
      mlir::arith::MinSIOp op, mlir::PatternRewriter& rewriter) const override {
    auto lhs = GetRange(op.getLhs());
    auto rhs = GetRange(op.getRhs());
    if (!lhs || !rhs) {
      return rewriter.notifyMatchFailure(op, "failed to deduce input ranges");
    }
    if (auto lhs_le_rhs = lhs->Le(*rhs); lhs_le_rhs == true) {
      rewriter.replaceOp(op, op.getLhs());
    } else if (auto rhs_le_lhs = rhs->Le(*lhs); rhs_le_lhs == true) {
      rewriter.replaceOp(op, op.getRhs());
    } else {
      return rewriter.notifyMatchFailure(op, "not equal to lhs or rhs");
    }
    return mlir::success();
  }
};

// Finds the narrowest value in a use-def chain of truncis/extuis.
mlir::Value FindNarrowestValueInChain(mlir::Value value) {
  if (auto ext = value.getDefiningOp<mlir::arith::ExtUIOp>()) {
    return FindNarrowestValueInChain(ext.getOperand());
  }
  auto defining_op = value.getDefiningOp<mlir::arith::TruncIOp>();
  if (defining_op) {
    auto first_trunc = FindNarrowestValueInChain(defining_op.getOperand());
    if (first_trunc && first_trunc.getType().getIntOrFloatBitWidth() <=
                           defining_op.getType().getIntOrFloatBitWidth()) {
      return first_trunc;
    }
    return defining_op;
  }
  return value;
}

// Rewrites trunc-bitwise to bitwise-trunc.
//
// For pred reductions, we generate code like this:
//
//   %1 = arith.trunci %0 : i32 to i1
//   %2 = arith.ori %1, %x
//   %3 = arith.extui %2 : i1 to i32
//   %4 = gpu.shuffle %3
//
// By swapping the trunc with the or, we get a trunc-ext-shuffle sequence, which
// can be rewritten to shuffle-trunc-ext. If there is another copy of the
// pattern afterwards, we can push the truncs/exts further down.
template <typename Op>
struct RewriteTruncBitExt : mlir::OpRewritePattern<Op> {
  using mlir::OpRewritePattern<Op>::OpRewritePattern;

  mlir::LogicalResult matchAndRewrite(
      Op op, mlir::PatternRewriter& rewriter) const override {
    mlir::Value lhs = FindNarrowestValueInChain(op.getLhs());
    mlir::Value rhs = FindNarrowestValueInChain(op.getRhs());

    if (lhs.getType() != rhs.getType()) {
      return rewriter.notifyMatchFailure(op, "mismatched narrowest types");
    }

    auto trunci_lhs = lhs.getDefiningOp<mlir::arith::TruncIOp>();
    auto trunci_rhs = rhs.getDefiningOp<mlir::arith::TruncIOp>();
    if (!trunci_lhs && !trunci_rhs) {
      return rewriter.notifyMatchFailure(
          op, "neither narrowest value is the result of a truncation");
    }

    auto wide_type =
        (trunci_lhs ? trunci_lhs : trunci_rhs).getOperand().getType();
    if (trunci_rhs && trunci_rhs.getOperand().getType() != wide_type) {
      return rewriter.notifyMatchFailure(op, "mismatched truncation types");
    }

    mlir::Value new_lhs = trunci_lhs ? trunci_lhs.getOperand()
                                     : rewriter.create<mlir::arith::ExtUIOp>(
                                           op.getLoc(), wide_type, lhs);
    mlir::Value new_rhs = trunci_rhs ? trunci_rhs.getOperand()
                                     : rewriter.create<mlir::arith::ExtUIOp>(
                                           op.getLoc(), wide_type, rhs);
    mlir::Value new_op = rewriter.create<Op>(op.getLoc(), new_lhs, new_rhs);
    rewriter.replaceOpWithNewOp<mlir::arith::TruncIOp>(op, op.getType(),
                                                       new_op);

    return mlir::success();
  }
};

// Rewrites trunc-ext-shuffle to shuffle-trunc-ext. This pattern is designed to
// work together with RewriteTruncBitExt to optimize pred reductions.
struct RewriteTruncExtShuffle
    : public mlir::OpRewritePattern<mlir::gpu::ShuffleOp> {
  using OpRewritePattern::OpRewritePattern;

  mlir::LogicalResult matchAndRewrite(
      mlir::gpu::ShuffleOp op, mlir::PatternRewriter& rewriter) const override {
    auto ext = op.getOperand(0).getDefiningOp<mlir::arith::ExtUIOp>();
    if (!ext) {
      return rewriter.notifyMatchFailure(op, "no ext");
    }
    auto trunc = ext.getOperand().getDefiningOp<mlir::arith::TruncIOp>();
    if (!trunc || trunc.getOperand().getType() != ext.getType()) {
      return rewriter.notifyMatchFailure(op, "no trunc or type mismatch");
    }
    rewriter.setInsertionPointAfter(op);
    auto new_trunc = rewriter.create<mlir::arith::TruncIOp>(
        op.getLoc(), trunc.getType(), op.getResult(0));
    auto new_ext = rewriter.create<mlir::arith::ExtUIOp>(
        op.getLoc(), ext.getType(), new_trunc.getResult());
    rewriter.modifyOpInPlace(op,
                             [&]() { op->setOperand(0, trunc.getOperand()); });
    rewriter.replaceAllUsesExcept(op.getResult(0), new_ext, new_trunc);
    return mlir::success();
  }
};

void AnnotateRanges(mlir::func::FuncOp func) {
  func->walk([](mlir::Operation* op) {
    if (op->getNumResults() != 1) {
      return;
    }

    auto result = op->getResult(0);
    if (GetRange(result).has_value()) {
      return;
    }

    auto get_range = [](mlir::Value value) -> Interval {
      auto range = GetRange(value);
      if (range) {
        return *range;
      }
      return {std::numeric_limits<int64_t>::min(),
              std::numeric_limits<int64_t>::max()};
    };

    std::optional<Interval> out_range = std::nullopt;
    if (mlir::isa<mlir::arith::MaxSIOp, mlir::arith::MinSIOp,
                  mlir::arith::AddIOp, mlir::arith::MulIOp>(op)) {
      auto lhs_range = get_range(op->getOperand(0));
      auto rhs_range = get_range(op->getOperand(1));
      if (mlir::isa<mlir::arith::MaxSIOp>(op)) {
        out_range = lhs_range.max(rhs_range);
      } else if (mlir::isa<mlir::arith::MinSIOp>(op)) {
        out_range = lhs_range.min(rhs_range);
      } else if (mlir::isa<mlir::arith::AddIOp>(op)) {
        out_range = lhs_range + rhs_range;
      } else {
        out_range = lhs_range * rhs_range;
      }
    }

    if (out_range) {
      mlir::OpBuilder b(op);
      op->setAttr("xla.range",
                  b.getIndexArrayAttr({out_range->lower, out_range->upper}));
    }
  });
}

class SimplifyArithPass
    : public impl::SimplifyArithPassBase<SimplifyArithPass> {
 public:
  void runOnOperation() override {
    mlir::RewritePatternSet patterns(&getContext());
    AnnotateRanges(getOperation());
    patterns.add<RewriteCmpI, RewriteMaxSi, RewriteMinSi>(&getContext());
    patterns
        .add<RewriteTruncBitExt<mlir::arith::OrIOp>,
             RewriteTruncBitExt<mlir::arith::AndIOp>, RewriteTruncExtShuffle>(
            &getContext());
    if (mlir::failed(mlir::applyPatternsAndFoldGreedily(getOperation(),
                                                        std::move(patterns)))) {
      signalPassFailure();
    }
  }
};

}  // namespace

std::unique_ptr<mlir::Pass> CreateSimplifyArithPass() {
  return std::make_unique<SimplifyArithPass>();
}

}  // namespace gpu
}  // namespace xla
