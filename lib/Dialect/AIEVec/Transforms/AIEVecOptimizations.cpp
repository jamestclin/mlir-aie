#include "aie/Dialect/AIEVec/AIEVecUtils.h"
#include "aie/Dialect/AIEVec/IR/AIEVecOps.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/TypeUtilities.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "mlir/Transforms/Passes.h"

#include "AIEVecOptimizations.h"
#include "FoldMulAddChainToConvOp.h"

#define DEBUG_TYPE "aievec-optimize"

using namespace mlir;
using namespace arith;
using namespace vector;
using namespace xilinx;
using namespace xilinx::aievec;

//===----------------------------------------------------------------------===//
// Utility functions
//===----------------------------------------------------------------------===//
namespace xilinx {
namespace aievec {

SmallVector<NamedAttribute> buildFMAOpSplatAttrForElemTy(aievec::FMAOp fmaOp,
                                                         int64_t bcastPos,
                                                         int64_t step = 1);

} // namespace aievec
} // namespace xilinx

static bool canFoldAIEShiftAndBroadcast(aievec::BroadcastOp op,
                                        aievec::ShiftOp &shiftOp,
                                        int32_t &idx) {
  if (!op.getSource().getDefiningOp())
    return false;

  shiftOp = dyn_cast<aievec::ShiftOp>(op.getSource().getDefiningOp());

  if (!shiftOp)
    return false;

  VectorType vType = shiftOp->getResult(0).getType().cast<VectorType>();
  int32_t elemSize = getElementSizeInBits(vType);
  auto constOp = cast<arith::ConstantOp>(shiftOp.getShift().getDefiningOp());
  int32_t shiftBytes = cast<IntegerAttr>(constOp.getValue()).getInt();
  idx = shiftBytes * 8 / elemSize + op.getIdx();

  if (idx <= 0 || idx >= (int32_t)getVectorLaneSize(vType)) {
    return false;
  }

  return true;
}

template <typename AIEv1MACLikeOp,
          typename = std::enable_if_t<
              std::is_same_v<AIEv1MACLikeOp, aievec::FMAOp> ||
              std::is_same_v<AIEv1MACLikeOp, aievec::FMAOp::Adaptor>>>
static bool isSingleColumnInt16VectorTimesScalarMac(AIEv1MACLikeOp fmaOp) {
  // lhs is a 32xi16 vector
  VectorType lhsVTy = cast<VectorType>(fmaOp.getLhs().getType());
  auto intTy = dyn_cast<IntegerType>(lhsVTy.getElementType());
  if (!intTy || intTy.getWidth() != 16)
    return false;
  if (lhsVTy.getShape()[0] != 32)
    return false;
  // Attributes match a Vector x Scalar mac
  if (fmaOp.getXoffsets() != "0x73727170" ||
      fmaOp.getXoffsetsHi() != "0x77767574" || fmaOp.getXstart() != "0" ||
      fmaOp.getXsquare() != "0x3120" || fmaOp.getZoffsets() != "0" ||
      fmaOp.getZoffsetsHi() != "0" || fmaOp.getZstep() != "1")
    return false;
  // lhs op is a concat of a vector and a dense<0> constant vector
  if (!fmaOp.getLhs().getDefiningOp())
    return false;
  aievec::ConcatOp concatOp =
      dyn_cast<aievec::ConcatOp>(fmaOp.getLhs().getDefiningOp());
  if (!concatOp)
    return false;
  auto tailVec = concatOp.getSources()[1];
  if (!tailVec.getDefiningOp())
    return false;
  auto constOp = dyn_cast<arith::ConstantOp>(tailVec.getDefiningOp());
  if (!constOp)
    return false;
  auto cstDense = dyn_cast<DenseIntElementsAttr>(constOp.getValue());
  if (!cstDense)
    return false;
  return llvm::all_of(cstDense, [](const APInt &val) { return val == 0; });
}

static bool singleColumnFMAOpCanFold(aievec::FMAOp fmaOp) {
  auto accProdOp = fmaOp.getAcc().getDefiningOp();
  if (!accProdOp)
    return false;
  auto accFmaOp = dyn_cast<aievec::FMAOp>(accProdOp);
  if (!accFmaOp)
    return false;
  if (!isSingleColumnInt16VectorTimesScalarMac(accFmaOp))
    return false;
  return fmaOp.getRhs() == accFmaOp.getRhs() &&
         !singleColumnFMAOpCanFold(accFmaOp);
}

//===----------------------------------------------------------------------===//
// Lowering patterns
//===----------------------------------------------------------------------===//
struct MergeSingleColumnI16FMAOpPattern
    : public OpConversionPattern<aievec::FMAOp> {
  using OpConversionPattern<aievec::FMAOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(aievec::FMAOp fmaOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    if (!isSingleColumnInt16VectorTimesScalarMac(adaptor))
      return failure();
    auto accProdOp = adaptor.getAcc().getDefiningOp();
    if (!accProdOp)
      return failure();
    auto accFmaOp = dyn_cast<aievec::FMAOp>(accProdOp);
    if (!accFmaOp)
      return failure();
    if (!isSingleColumnInt16VectorTimesScalarMac(accFmaOp))
      return failure();
    if (adaptor.getRhs() != accFmaOp.getRhs())
      return failure();
    auto accConcatOp =
        cast<aievec::ConcatOp>(accFmaOp.getLhs().getDefiningOp());
    auto fmaConcatOp = cast<aievec::ConcatOp>(adaptor.getLhs().getDefiningOp());
    unsigned fmaZstart, accFmaZstart;
    if (adaptor.getZstart().getAsInteger(10, fmaZstart) ||
        accFmaOp.getZstart().getAsInteger(10, accFmaZstart))
      return failure();
    auto start = std::min(fmaZstart, accFmaZstart);
    auto step = std::max(fmaZstart, accFmaZstart) - start;
    auto lowV = accConcatOp.getSources()[0];
    auto hiV = fmaConcatOp.getSources()[0];
    if (accFmaZstart > fmaZstart)
      std::swap(lowV, hiV);
    auto newConcatOp = rewriter.create<aievec::ConcatOp>(
        fmaOp.getLoc(), adaptor.getLhs().getType(),
        SmallVector<Value, 2>({lowV, hiV}));
    auto newFmaOpAttr = buildFMAOpSplatAttrForElemTy(fmaOp, start, step);
    rewriter.replaceOpWithNewOp<aievec::FMAOp>(
        fmaOp, TypeRange({fmaOp.getResult().getType()}),
        ValueRange({newConcatOp, adaptor.getRhs(), accFmaOp.getAcc()}),
        newFmaOpAttr);
    return success();
  }
};

struct FoldAIEShiftAndBroadcast
    : public OpConversionPattern<aievec::BroadcastOp> {
  using OpConversionPattern<aievec::BroadcastOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(aievec::BroadcastOp bcastOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    aievec::ShiftOp shiftOp = nullptr;
    int32_t idx = 0;

    if (!canFoldAIEShiftAndBroadcast(bcastOp, shiftOp, idx)) {
      return failure();
    }

    VectorType resultType = bcastOp.getResult().getType().cast<VectorType>();

    rewriter.replaceOpWithNewOp<aievec::BroadcastOp>(bcastOp, resultType,
                                                     shiftOp.getLhs(), idx);

    return success();
  }
};

//===----------------------------------------------------------------------===//
// Pattern collection
//===----------------------------------------------------------------------===//
static void
populateAIEVecV1TransformationPatterns(RewritePatternSet &patterns) {
  patterns.add<MergeSingleColumnI16FMAOpPattern>(patterns.getContext());
}

static void
populateAIEVecV2TransformationPatterns(RewritePatternSet &patterns) {
  patterns.add<FoldAIEShiftAndBroadcast>(patterns.getContext());
}

static void
configureAIEVecV1TransformationLegalizations(ConversionTarget &target) {
  target.addLegalDialect<aievec::AIEVecDialect>();
  target.addDynamicallyLegalOp<aievec::FMAOp>([](aievec::FMAOp fmaOp) {
    if (isSingleColumnInt16VectorTimesScalarMac(fmaOp))
      return !singleColumnFMAOpCanFold(fmaOp);
    return true;
  });
}

//===----------------------------------------------------------------------===//
// Legalizations
//===----------------------------------------------------------------------===//
static void
configureAIEVecV2TransformationLegalizations(ConversionTarget &target) {
  target.addDynamicallyLegalOp<xilinx::aievec::BroadcastOp>(
      [](xilinx::aievec::BroadcastOp op) {
        aievec::ShiftOp shiftOp = nullptr;
        int32_t idx = 0;
        return !canFoldAIEShiftAndBroadcast(op, shiftOp, idx);
      });
}

//===----------------------------------------------------------------------===//
// Lowering passes
//===----------------------------------------------------------------------===//
struct AIEVecTransformationPass
    : public PassWrapper<AIEVecTransformationPass,
                         OperationPass<func::FuncOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(AIEVecTransformationPass)

  AIEVecTransformationPass() = default;
  AIEVecTransformationPass(const AIEVecTransformationPass &pass)
      : PassWrapper(pass) {}

  AIEVecTransformationPass(const OptimizeAIEVecOptions &options)
      : AIEVecTransformationPass() {
    aieTarget = options.aieTarget;
  }

  // In case we want to register this pass as a standalone pass for test
  // purposes.
  StringRef getArgument() const final { return "test-aievec-optimize"; }
  StringRef getDescription() const final {
    return "Optimize groups of simple aievec ops into complex aievec ops.";
  }
  void getDependentDialects(DialectRegistry &registry) const override {
    // TODO: Review list of dependent dialects.
    registry.insert<AffineDialect, xilinx::aievec::AIEVecDialect,
                    arith::ArithDialect, memref::MemRefDialect, scf::SCFDialect,
                    vector::VectorDialect>();
  }

  Option<std::string> aieTarget{
      *this, "aie-target",
      llvm::cl::desc("Select AIE version: \"aie\" or \"aieml\". This will "
                     "determine the vector size and available operations."),
      llvm::cl::init("aie")};

  void runOnOperation() override {
    auto func = getOperation();
    MLIRContext *context = &getContext();
    RewritePatternSet patterns(context);
    ConversionTarget target(*context);
    AIEArch aieVersion = AIEArch::AIE;
    if (!aieTarget.empty()) {
      std::string target = aieTarget;
      if (target == "aieml") {
        aieVersion = AIEArch::AIE_ML;
      } else if (target != "aie") {
        func.emitError() << "unknown AIE target '" << aieTarget << "'";
        signalPassFailure();
        return;
      }
    }
    if (aieVersion == AIEArch::AIE) {
      populateAIEVecV1TransformationPatterns(patterns);
      configureAIEVecV1TransformationLegalizations(target);
    } else {
      populateAIEVecV2TransformationPatterns(patterns);
      configureAIEVecV2TransformationLegalizations(target);
    }

    if (failed(applyPartialConversion(func, target, std::move(patterns)))) {
      signalPassFailure();
    }
  }
};

static std::unique_ptr<::mlir::Pass>
createAIEVecTransformationPass(const OptimizeAIEVecOptions &options) {
  return std::make_unique<AIEVecTransformationPass>(options);
}

struct AIEVecConvOpTransformationPass
    : public PassWrapper<AIEVecConvOpTransformationPass,
                         OperationPass<func::FuncOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(AIEVecConvOpTransformationPass)

  AIEVecConvOpTransformationPass() = default;
  AIEVecConvOpTransformationPass(const AIEVecConvOpTransformationPass &pass)
      : PassWrapper(pass) {}

  AIEVecConvOpTransformationPass(const OptimizeAIEVecOptions &options)
      : AIEVecConvOpTransformationPass() {
    aieTarget = options.aieTarget;
    shiftParam = options.shiftParam;
  }

  // In case we want to register this pass as a standalone pass for test
  // purposes.
  StringRef getArgument() const final {
    return "test-aievec-convolution-optimize";
  }
  StringRef getDescription() const final {
    return "Optimize chains of macs into AIEML conv ops.";
  }
  void getDependentDialects(DialectRegistry &registry) const override {
    // TODO: Review list of dependent dialects.
    registry.insert<AffineDialect, xilinx::aievec::AIEVecDialect,
                    arith::ArithDialect, memref::MemRefDialect, scf::SCFDialect,
                    vector::VectorDialect>();
  }

  Option<std::string> aieTarget{
      *this, "aie-target",
      llvm::cl::desc("Select AIE version: \"aie\" or \"aieml\". This will "
                     "determine the vector size and available operations."),
      llvm::cl::init("aie")};

  Option<unsigned> shiftParam{
      *this, "shift",
      llvm::cl::desc("Shift parameter for rounding and saturation."),
      llvm::cl::init(0)};

  void runOnOperation() override {
    auto func = getOperation();
    MLIRContext *context = &getContext();
    RewritePatternSet patterns(context);
    ConversionTarget target(*context);
    AIEArch aieVersion = AIEArch::AIE;
    if (!aieTarget.empty()) {
      std::string target = aieTarget;
      if (target == "aieml") {
        aieVersion = AIEArch::AIE_ML;
      } else if (target != "aie") {
        func.emitError() << "unknown AIE target '" << aieTarget << "'";
        signalPassFailure();
        return;
      }
    }

    AnalysisManager am = getAnalysisManager();
    if (aieVersion == AIEArch::AIE_ML) {
      populateAIEVecConvOpTransformationPatterns(patterns, am, shiftParam);
      configureAIEVecConvOpTransformationLegalizations(target, am);
    }

    if (failed(applyPartialConversion(func, target, std::move(patterns)))) {
      signalPassFailure();
    }
  }
};

static std::unique_ptr<::mlir::Pass>
createAIEVecConvOpTransformationPass(const OptimizeAIEVecOptions &options) {
  return std::make_unique<AIEVecConvOpTransformationPass>(options);
}

//============================================================================//
//=============== Main AIEVec2AIEVec Pipeline Configuration ==================//
//============================================================================//

void xilinx::aievec::buildOptimizeAIEVec(OpPassManager &pm,
                                         const OptimizeAIEVecOptions &options) {
  // Add AIEVec transformation pass.
  pm.addPass(createAIEVecTransformationPass(options));

  pm.addPass(createCSEPass());
  pm.addPass(createCanonicalizerPass());

  // TODO: This pass should only be included if the target is AIEML.
  // Add generating aievec convolution ops pass
  pm.addPass(createAIEVecConvOpTransformationPass(options));

  // Add post-lowering canonicalization passes.
  pm.addPass(createCSEPass());
  pm.addPass(createCanonicalizerPass());
}
