//===- TranslateAIEVecToCpp.cpp - AIE vector dialect to C++ -----*- C++ -*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2022 Xilinx Inc.
//
//===----------------------------------------------------------------------===//
// This file defines helpers to emit C++ code for AIE vector dialect.
//===----------------------------------------------------------------------===//

#include "TranslateAIEVecToCpp.h"
#include "aie/Dialect/AIEVec/AIEVecUtils.h"
#include "aie/Dialect/AIEVec/IR/AIEVecOps.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/EmitC/IR/EmitC.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/Vector/IR/VectorOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/Operation.h"
#include "mlir/Support/IndentedOstream.h"
#include "mlir/Support/MathExtras.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FormatVariadic.h"
#include <limits>

#define DEBUG_TYPE "aievec-to-cpp"

static llvm::cl::opt<bool> AIEML("aieml", llvm::cl::desc("AI Engine-ML"),
                                 llvm::cl::init(false));

using namespace mlir;
using namespace xilinx;
using namespace xilinx::aievec;
using llvm::formatv;

/// Convenience functions to produce interleaved output with functions returning
/// a LogicalResult. This is different than those in STLExtras as functions used
/// on each element doesn't return a string.
template <typename ForwardIterator, typename UnaryFunctor,
          typename NullaryFunctor>
inline LogicalResult
interleaveWithError(ForwardIterator begin, ForwardIterator end,
                    UnaryFunctor eachFn, NullaryFunctor betweenFn) {
  if (begin == end)
    return success();
  if (failed(eachFn(*begin)))
    return failure();
  ++begin;
  for (; begin != end; ++begin) {
    betweenFn();
    if (failed(eachFn(*begin)))
      return failure();
  }
  return success();
}

template <typename Container, typename UnaryFunctor, typename NullaryFunctor>
inline LogicalResult interleaveWithError(const Container &c,
                                         UnaryFunctor eachFn,
                                         NullaryFunctor betweenFn) {
  return interleaveWithError(c.begin(), c.end(), eachFn, betweenFn);
}

template <typename Container, typename UnaryFunctor>
inline LogicalResult interleaveCommaWithError(const Container &c,
                                              raw_ostream &os,
                                              UnaryFunctor eachFn) {
  return interleaveWithError(c.begin(), c.end(), eachFn, [&]() { os << ", "; });
}

namespace {
/// Emitter that uses dialect specific emitters to emit C++ code.
struct CppEmitter {
  explicit CppEmitter(raw_ostream &os, bool declareVariablesAtTop);

  /// Emits attribute or returns failure.
  LogicalResult emitAttribute(Location loc, Attribute attr);

  /// Emits operation 'op' with/without training semicolon or returns failure.
  LogicalResult emitOperation(Operation &op, bool trailingSemicolon);

  /// Emits type 'type' or returns failure. stdintType is true when the
  // type is from stdint.h
  LogicalResult emitType(Location loc, Type type, bool stdintType = true,
                         bool isAcc = false);

  /// Emits array of types as a std::tuple of the emitted types.
  /// - emits void for an empty array;
  /// - emits the type of the only element for arrays of size one;
  /// - emits a std::tuple otherwise;
  LogicalResult emitTypes(Location loc, ArrayRef<Type> types);

  /// Emits array of types as a std::tuple of the emitted types independently of
  /// the array size.
  LogicalResult emitTupleType(Location loc, ArrayRef<Type> types);

  /// Emits an assignment for a variable which has been declared previously.
  LogicalResult emitVariableAssignment(OpResult result);

  /// Emits a variable declaration for a result of an operation.
  LogicalResult emitVariableDeclaration(OpResult result, bool trailingSemicolon,
                                        bool isAcc = false);

  /// Emits the variable declaration and assignment prefix for 'op'.
  /// - emits separate variable followed by std::tie for multi-valued operation;
  /// - emits single type followed by variable for single result;
  /// - emits nothing if no value produced by op;
  /// Emits final '=' operator where a type is produced. Returns failure if
  /// any result type could not be converted.
  LogicalResult emitAssignPrefix(Operation &op, bool isAcc = false);

  /// Emits a label for the block.
  LogicalResult emitLabel(Block &block);

  /// Emits the operands and atttributes of the operation. All operands are
  /// emitted first and then all attributes in alphabetical order.
  LogicalResult emitOperandsAndAttributes(Operation &op,
                                          ArrayRef<StringRef> exclude = {});

  /// Emits the operands of the operation. All operands are emitted in order.
  LogicalResult emitOperands(Operation &op);

  /// Return the existing or a new name for a Value.
  StringRef getOrCreateName(Value val, std::string prefix = "v");

  /// Set the name of the value to an existing name
  void setName(Value val, StringRef name);

  /// Return a new name that is not associated with any value
  std::string getNewName(std::string prefix = "v");

  // Set the dim size at position index of the memref to the parameter
  void setMemRefDimParam(Value memref, unsigned index, std::string parameter);

  // For the dynamic shaped memref, return the parametric size at index
  StringRef getMemRefDimParam(Value memref, unsigned index);

  // Return true if the specified dim of memref is parametric
  bool isMemRefDimParam(Value memref, unsigned index);

  /// Return the existing or a new label of a Block.
  StringRef getOrCreateName(Block &block, std::string prefix = "label");

  /// Whether to map an mlir integer to a unsigned integer in C++.
  bool shouldMapToUnsigned(IntegerType::SignednessSemantics val);

  /// RAII helper function to manage entering/exiting C++ scopes.
  struct Scope {
    Scope(CppEmitter &emitter)
        : valueMapperScope(emitter.valueMapper),
          blockMapperScope(emitter.blockMapper), emitter(emitter) {
      emitter.valueInScopeCount.push(emitter.valueInScopeCount.top());
      emitter.labelInScopeCount.push(emitter.labelInScopeCount.top());
    }
    ~Scope() {
      emitter.valueInScopeCount.pop();
      emitter.labelInScopeCount.pop();
    }

  private:
    llvm::ScopedHashTableScope<Value, std::string> valueMapperScope;
    llvm::ScopedHashTableScope<Block *, std::string> blockMapperScope;
    CppEmitter &emitter;
  };

  /// Returns wether the Value is assigned to a C++ variable in the scope.
  bool hasValueInScope(Value val);

  // Returns whether a label is assigned to the block.
  bool hasBlockLabel(Block &block);

  /// Returns the output stream.
  raw_indented_ostream &ostream() { return os; };

  /// Returns if all variables for op results and basic block arguments need to
  /// be declared at the beginning of a function.
  bool shouldDeclareVariablesAtTop() { return declareVariablesAtTop; };

private:
  using ValueMapper = llvm::ScopedHashTable<Value, std::string>;
  using BlockMapper = llvm::ScopedHashTable<Block *, std::string>;

  /// Output stream to emit to.
  raw_indented_ostream os;

  /// Boolean to enforce that all variables for op results and block
  /// arguments are declared at the beginning of the function. This also
  /// includes results from ops located in nested regions.
  bool declareVariablesAtTop;

  /// Map from value to name of C++ variable that contain the name.
  ValueMapper valueMapper;

  /// Map from block to name of C++ label.
  BlockMapper blockMapper;

  /// Map from a dynamic memref index to the parameter
  DenseMap<std::pair<Value, unsigned>, std::string> paramIndexMapper;

  /// The number of values in the current scope. This is used to declare the
  /// names of values in a scope.
  std::stack<int64_t> valueInScopeCount;
  std::stack<int64_t> labelInScopeCount;
};
} // namespace

//===----------------------------------------------------------------------===//
// Helper Routines
//===----------------------------------------------------------------------===//

// Return true if this op should be skipped in codegen. Ops like memref::DimOp,
// aievec::srs and aievec::ups for fp operands fall in this category.
// Certain ops should only be emitted if they are used in the computation of an
// op that is not skipped. An example of such an op is the index defining op for
// memref::DimOp. Since DimOp is skipped, we don't need to generate the index
// defining op. If checkStrongLiveness is true, then also skip such ops.
static bool skippedOp(Operation *op, CppEmitter &emitter,
                      bool checkStrongLiveness = true) {
  // Ops that must be skipped:
  // skip op 1 : all dimOp
  bool skip = isa<memref::DimOp>(op);
  // skip op 2 : some aievec::srs for float types
  if (auto srsOp = dyn_cast<aievec::SRSOp>(op)) {
    // Get the datatype of the source accumulator and result vector
    VectorType accType = srsOp.getSource().getType().cast<VectorType>();
    Type eltType = accType.getElementType();
    Value source = srsOp.getSource();
    // If the underlying element types are float, then we do not really need an
    // srs op if source of srsOp has only one use.
    if (!AIEML && eltType.isa<FloatType>() &&
        source.getDefiningOp()->hasOneUse()) {
      StringRef srcName = emitter.getOrCreateName(source);
      emitter.setName(srsOp->getResult(0), srcName);
      skip = true;
    }
  }
  // skip op 3 : some aievec::ups for float ops
  else if (auto upsOp = dyn_cast<aievec::UPSOp>(op)) {
    // Get the datatype of the source vector and result accumulator
    VectorType accType = upsOp.getResult().getType().cast<VectorType>();
    Type eltType = accType.getElementType();
    Value source = upsOp.getSource();
    // If the underlying element types are float, then we do not really need a
    // ups op if the source accumulator has only one use.
    if (!AIEML && eltType.isa<FloatType>() &&
        source.getDefiningOp()->hasOneUse()) {
      StringRef srcName = emitter.getOrCreateName(source);
      emitter.setName(upsOp->getResult(0), srcName);
      skip = true;
    }
  }

  // Ops whose strong liveness must be determined
  checkStrongLiveness &= isa<arith::ConstantOp>(op);

  // If we already know that this op must be skipped, or that don't need to
  // check strong liveness of the op, we are done
  if (skip || !checkStrongLiveness)
    return skip;

  // We need to check if this op is strongly live. i.e., its result is used in
  // an op that is not skipped. We iterate over all its immediate users, and
  // return false if any of them is not skipped in codegen.
  for (auto user : op->getUsers()) {
    if (!skippedOp(user, emitter, false))
      return false;
  }
  return true;
}

// Print the memref dims, if the memref has dynamic shape
static LogicalResult parseMemRefDynamicDims(CppEmitter &emitter,
                                            func::FuncOp func) {
  // Step1: Walk over all the operations that are memref dimOp
  func.walk([&](mlir::Operation *Op) {
    if (auto op = dyn_cast<memref::DimOp>(Op)) {
      // Extract the source memref, result, and index
      Value source = op.getSource();
      Value result = op.getResult();
      auto indexOp = dyn_cast<arith::ConstantOp>(op.getIndex().getDefiningOp());
      assert(indexOp && "Failed to get the index value of dimOp");
      // Get the constant index value
      llvm::APInt idxVal = indexOp.getValue().cast<IntegerAttr>().getValue();
      unsigned index = idxVal.getZExtValue();
      // Assign a printable name to the result
      StringRef name = emitter.getOrCreateName(result, "m");
      emitter.setMemRefDimParam(source, index, name.str());
    }
  });

  // Step2: Iterate over all the block arguments, and make sure that the memref
  // args have a parameter associated with the dynamic sized dimension
  for (BlockArgument arg : func.getArguments()) {
    MemRefType argType = arg.getType().dyn_cast<MemRefType>();
    if (!argType)
      continue;
    for (unsigned dim = 0; dim < argType.getRank(); ++dim) {
      if (argType.isDynamicDim(dim)) {
        // If the dynamic dim size is not already parametrized, assign it one
        if (!emitter.isMemRefDimParam(arg, dim)) {
          std::string name = emitter.getNewName("m");
          emitter.setMemRefDimParam(arg, dim, name);
        }
      }
    }
  }
  return success();
}

// Print the memref dims, if the memref has dynamic shape
static LogicalResult printMemRefDims(CppEmitter &emitter, BlockArgument arg) {
  raw_indented_ostream &os = emitter.ostream();
  MemRefType argType = arg.getType().dyn_cast<MemRefType>();
  if (argType) {
    for (unsigned dim = 0; dim < argType.getRank(); ++dim) {
      if (argType.isDynamicDim(dim)) {
        StringRef param = emitter.getMemRefDimParam(arg, dim);
        os << ", size_t " << param;
      }
    }
  }
  return success();
}

// Get the linearized access for the source memref
static LogicalResult createLinearizedAccess(CppEmitter &emitter, Value source,
                                            SmallVector<Value, 4> indices,
                                            std::string &access) {
  MemRefType memRefType = source.getType().dyn_cast<MemRefType>();
  assert(memRefType &&
         "cannot creating linearized expression for non-memref type");
  ArrayRef<int64_t> stride = memRefType.getShape();

  // The stride and indices size must match
  if (stride.size() != indices.size() ||
      (int)stride.size() != memRefType.getRank())
    return failure();

  // A stride contains two parts:
  int64_t numPart = 1;   // for static shaped dims
  std::string paramPart; // for dynamic shaped dims

  SmallVector<std::string, 4> accessVec;
  for (int dim = memRefType.getRank() - 1; dim >= 0; --dim) {
    // All the indices in the access expression must already be emitted
    if (!emitter.hasValueInScope(indices[dim]))
      return failure();

    // Form the access string for this dimension
    std::string cur;
    if (!paramPart.empty())
      cur = paramPart + "*";
    if (numPart > 1)
      cur += std::to_string(numPart) + "*";
    cur += emitter.getOrCreateName(indices[dim]);
    accessVec.push_back(cur);

    // Now update the numPart and paramPart to form the stride for the next
    // dimension
    if (memRefType.isDynamicDim(dim)) {
      StringRef param = emitter.getMemRefDimParam(source, dim);
      paramPart = param.str() + (paramPart.empty() ? "" : "*" + paramPart);
    } else
      numPart *= stride[dim];
  }
  // All the strides are in accessVec. Compose them
  while (!accessVec.empty()) {
    access += (access.empty() ? "" : "+") + accessVec.back();
    accessVec.pop_back();
  }
  // If the access is empty, make '0' as default access
  if (access.empty())
    access = "0";

  return success();
}

// Return true if the array accessed by this value is readonly
static bool isReadOnly(Value read) {
  for (auto *user : read.getUsers()) {
    if (isa<vector::TransferWriteOp>(user))
      return false;
  }
  return true;
}

//===----------------------------------------------------------------------===//
// Print non-AIE dialect ops
//===----------------------------------------------------------------------===//

// Get the loop trip count of the for operator
static std::pair<bool, int64_t> getTripCount(scf::ForOp forOp) {
  // If the upper and lower bounds are constant values, return the difference.
  auto lb = forOp.getLowerBound().getDefiningOp<arith::ConstantOp>();
  auto ub = forOp.getUpperBound().getDefiningOp<arith::ConstantOp>();
  if (lb && ub) {
    llvm::APInt ubValue = ub.getValue().cast<IntegerAttr>().getValue();
    llvm::APInt lbValue = lb.getValue().cast<IntegerAttr>().getValue();
    return std::make_pair(true,
                          ubValue.getSExtValue() - lbValue.getSExtValue());
  }
  return std::make_pair(false, 0);
}

// Get the loop step size of the for operator
static std::pair<bool, int64_t> getStep(scf::ForOp forOp) {
  if (auto step = forOp.getStep().getDefiningOp<arith::ConstantOp>()) {
    llvm::APInt stepValue = step.getValue().cast<IntegerAttr>().getValue();
    return std::make_pair(true, stepValue.getSExtValue());
  }
  return std::make_pair(false, 0);
}

// Return the operator string of the SCF dialect binary operator
template <typename T> static StringRef getOperator(T binOp) {
  if (isa<arith::AddIOp, arith::AddFOp>(binOp))
    return " + ";
  if (isa<arith::MulIOp, arith::MulFOp>(binOp))
    return " * ";
  if (isa<arith::SubIOp, arith::SubFOp>(binOp))
    return " - ";
  if (isa<arith::DivFOp, arith::DivUIOp, arith::DivSIOp>(binOp))
    return " / ";
  llvm_unreachable("Cannot print the operation of binary operator");
}

// Print the SCF dialect binary operation
template <typename T>
static LogicalResult printOperation(CppEmitter &emitter, T binOp) {
  if (failed(emitter.emitAssignPrefix(*binOp)))
    return failure();
  raw_indented_ostream &os = emitter.ostream();
  auto lhs = binOp.getLhs();
  if (!emitter.hasValueInScope(lhs))
    return failure();
  os << emitter.getOrCreateName(lhs);
  os << getOperator(binOp);
  auto rhs = binOp.getRhs();
  if (!emitter.hasValueInScope(rhs))
    return failure();
  os << emitter.getOrCreateName(rhs);

  return success();
}

//===----------------------------------------------------------------------===//
// Print AIE dialect ops
//===----------------------------------------------------------------------===//

// Print the AIE dialect UPD op
static LogicalResult printOperation(CppEmitter &emitter, aievec::UPDOp updOp) {
  Value source = updOp.getSource();
  // If the source is not already emitted, error out
  if (!emitter.hasValueInScope(source))
    return failure();

  // Construct the access expression using memref shape and indices
  auto indices = updOp.getIndices();
  std::string access;
  if (failed(createLinearizedAccess(emitter, source, indices, access)))
    return failure();

  raw_indented_ostream &os = emitter.ostream();
  Value result = updOp.getResult();
  VectorType resultType = result.getType().cast<VectorType>();
  int32_t vecSizeInBits = getVectorSizeInBits(resultType);
  int32_t elementSizeInBits = getElementSizeInBits(resultType);

  // If the UPD op had an offset, add it to the access expr
  if (updOp.getOffset() != 0) {
    if (std::abs(updOp.getOffset()) % elementSizeInBits)
      return failure();
    int32_t updOffset = updOp.getOffset() / elementSizeInBits;
    access += updOffset > 0 ? " + " : " - ";
    access += std::to_string(std::abs(updOffset));
  }

  // If the vector size to be loaded is less than or equal to 256, we
  // can just do a direct memory copy. If the translation is for AIEML,
  // this number should be doubled
  if (vecSizeInBits <= (AIEML ? 1024 : 256)) {
    // Print the lhs
    if (failed(emitter.emitAssignPrefix(*updOp)))
      return failure();
    os << "*(";
    if (failed(emitter.emitType(updOp->getLoc(), resultType)))
      return failure();
    os << " *)";
    os << "(";
    os << emitter.getOrCreateName(source);
    if (!access.empty())
      os << " + " << access;
    os << ")";
  } else {
    Value vector = updOp.getVector();
    // If this is the first upd op (between idx=0 and idx=1), then generate
    // declaration
    if (!vector) {
      if (!emitter.shouldDeclareVariablesAtTop()) {
        if (failed(emitter.emitVariableDeclaration(updOp->getResult(0), true)))
          return failure();
      }
    } else {
      if (!emitter.hasValueInScope(vector))
        return failure();
      emitter.setName(updOp->getResult(0), emitter.getOrCreateName(vector));
    }

    // The granularity of upd is 128/256/512 for 256/512/1024 bit values
    int32_t granularity = vecSizeInBits == 256   ? 128
                          : vecSizeInBits == 512 ? 256
                                                 : 512;
    // Create a vector type with number of lanes halved of the result
    unsigned lanes = getVectorLaneSize(resultType);
    assert(lanes % 2 == 0 &&
           "The number of vector lanes of UPD result is not even");
    SmallVector<int64_t, 4> updShape = {lanes / 2};
    VectorType updType = VectorType::get(updShape, resultType.getElementType());

    if (!emitter.hasValueInScope(result))
      return failure();
    // If the source array of upd is read-only, load from restrict pointer
    bool readOnly = isReadOnly(source);
    std::string restrictPrefix =
        readOnly ? ("r_" + emitter.getOrCreateName(result).str() + "_") : "";
    // Create a restrict pointer
    if (readOnly && !vector) {
      if (failed(emitter.emitType(updOp->getLoc(), source.getType())))
        return failure();
      os << " " << restrictPrefix << emitter.getOrCreateName(source);
      os << " = ";
      os << emitter.getOrCreateName(source);
      os << ";\n";
    }
    os << emitter.getOrCreateName(result);
    os << " = ";
    os << (granularity == 128   ? "upd_v"
           : granularity == 256 ? "upd_w"
                                : "upd_x");
    os << "(";
    os << emitter.getOrCreateName(result);
    os << ", ";
    os << std::to_string(updOp.getIndex());
    os << ", ";
    os << "*(";
    if (failed(emitter.emitType(updOp->getLoc(), updType)))
      return failure();
    os << " *)";
    os << "(";
    os << restrictPrefix << emitter.getOrCreateName(source);
    if (!access.empty())
      os << " + " << access;
    os << ")";
    os << ")";
  }

  return success();
}

// Print the UPS intrinsic
static LogicalResult printOperation(CppEmitter &emitter, aievec::UPSOp upsOp) {
  Value source = upsOp.getSource();
  int32_t shift = upsOp.getShift();

  raw_indented_ostream &os = emitter.ostream();

  // Generate the initialization for the accumulator
  if (failed(emitter.emitAssignPrefix(*upsOp, /*isAcc=*/true)))
    return failure();

  // The source vector should have already been emitted
  if (!emitter.hasValueInScope(source))
    return failure();

  VectorType accType = upsOp.getResult().getType().cast<VectorType>();
  unsigned lanes = getVectorLaneSize(accType);
  Type eltType = accType.getElementType();

  // If the underlying element types are float, then we do not really need a
  // ups op. We can simply generate an assignment
  if (!AIEML && eltType.isa<FloatType>()) {
    os << emitter.getOrCreateName(source);
    return success();
  }

  // Determine if it is lups or ups based on accumulator type
  auto iType = eltType.dyn_cast<IntegerType>();
  auto fType = eltType.dyn_cast<FloatType>();
  if (iType) {
    if (iType.getWidth() == 80)
      os << "l";
  }

  if (iType && AIEML) {
    os << "ups_to_v" << lanes << "acc" << iType.getWidth();
  } else if (fType && AIEML) {
    os << "ups_to_v16accfloat";
  } else {
    os << "ups";
  }

  os << "(";
  os << emitter.getOrCreateName(source);
  if (!(fType && AIEML)) {
    os << ", ";
    os << std::to_string(shift);
  }
  os << ")";

  return success();
}

// Generate the cast intrinsic for AIE-ML
static LogicalResult printOperation(CppEmitter &emitter,
                                    aievec::CastOp castOp) {
  if (!AIEML) {
    return failure();
  }

  // The source should have already been emitted
  Value source = castOp.getSource();
  if (!emitter.hasValueInScope(source))
    return failure();

  bool isResAcc = castOp.getIsResAcc();

  // Generate the initialization for the vector
  if (failed(emitter.emitAssignPrefix(*castOp, /*isAcc=*/isResAcc)))
    return failure();

  // Get the datatype of the source and result vector
  VectorType resType = castOp->getResult(0).getType().cast<VectorType>();
  Type eltType = resType.getElementType();
  unsigned lanes = getVectorLaneSize(resType);

  raw_indented_ostream &os = emitter.ostream();

  unsigned width = 0;
  if (isResAcc) {
    if (eltType.isa<FloatType>()) {
      os << "v" << lanes << "accfloat";
    } else {
      width = getElementSizeInBits(resType);
      os << "v" << lanes << "acc" << width;
    }
  } else {
    if (eltType.isa<FloatType>()) {
      width = eltType.cast<FloatType>().getWidth();
      os << "v" << lanes;
      if (width == 16) {
        os << "bfloat16";
      } else {
        os << "float";
      }
    } else {
      width = getElementSizeInBits(resType);
      os << "v" << lanes << "int" << width;
    }
  }
  os << "(";
  os << emitter.getOrCreateName(source);
  os << ")";
  return success();
}

// Generate the srs intrinsic
static LogicalResult printOperation(CppEmitter &emitter, aievec::SRSOp srsOp) {
  Value source = srsOp.getSource();
  int32_t shift = srsOp.getShift();

  // Get the datatype of the source accumulator and result vector
  VectorType accType = srsOp.getSource().getType().cast<VectorType>();
  VectorType resType = srsOp->getResult(0).getType().cast<VectorType>();
  Type eltType = accType.getElementType();
  unsigned lanes = getVectorLaneSize(resType);

  raw_indented_ostream &os = emitter.ostream();

  // Generate the initialization for the vector
  if (failed(emitter.emitAssignPrefix(*srsOp)))
    return failure();

  // The source accumulator should have already been emitted
  if (!emitter.hasValueInScope(source))
    return failure();

  // If the underlying element types are float, then we do not really need an
  // srs op. We can simply generate an assignment
  if (eltType.isa<FloatType>()) {
    if (AIEML) {
      unsigned width = getElementSizeInBits(resType);
      if (width == 32) {
        os << "srs";
      } else if (width == 16) {
        os << "to_v16bfloat16";
      }
      os << "(";
      os << emitter.getOrCreateName(source);
      os << ")";
    } else {
      os << emitter.getOrCreateName(source);
    }
    return success();
  }

  // Otheriwse, get the datatype width of the source accumulator and result
  // vector
  unsigned resultWidth = getElementSizeInBits(accType);
  unsigned resWidth = getElementSizeInBits(resType);
  unsigned srcWidth = 0;
  if (auto iType = eltType.dyn_cast<IntegerType>())
    srcWidth = iType.getWidth();

  // Based on the datatypes, generate srs version
  if ((srcWidth == 80 && resultWidth == 64) ||
      (srcWidth == 48 && resultWidth == 32))
    os << "l";
  else if (srcWidth == 48 && resultWidth == 8)
    os << "b";

  if (AIEML) {
    os << "srs_to_v" << std::to_string(lanes) << "int"
       << std::to_string(resWidth);
  } else {
    os << "srs";
  }

  os << "(";
  os << emitter.getOrCreateName(source);
  os << ", ";
  os << std::to_string(shift);
  os << ")";
  return success();
}

// Generate the broadcast intrinsic
static LogicalResult printOperation(CppEmitter &emitter,
                                    aievec::BroadcastOp broadcastOp) {
  Value source = broadcastOp.getSource();
  int8_t idx = broadcastOp.getIdx();

  raw_indented_ostream &os = emitter.ostream();

  // Generate the initialization for the vector
  if (failed(emitter.emitAssignPrefix(*broadcastOp)))
    return failure();

  // The source vector should have already been emitted
  if (!emitter.hasValueInScope(source))
    return failure();

  os << "broadcast_elem";
  os << "(";
  os << emitter.getOrCreateName(source);
  os << ", ";
  os << std::to_string(idx);
  os << ")";
  return success();
}

// Generate the broadcast_scalar intrinsic
static LogicalResult
printOperation(CppEmitter &emitter,
               aievec::BroadcastScalarOp broadcastScalarOp) {
  auto source = broadcastScalarOp.getSource();
  VectorType resType =
      broadcastScalarOp.getResult().getType().cast<VectorType>();
  unsigned width = getElementSizeInBits(resType);
  unsigned lanes = getVectorLaneSize(resType);
  raw_indented_ostream &os = emitter.ostream();

  // Generate the initialization for the vector
  if (failed(emitter.emitAssignPrefix(*broadcastScalarOp)))
    return failure();

  Type eltType = resType.getElementType();
  os << "broadcast_to_v";
  if (eltType.isa<IntegerType>()) {
    os << lanes << "int";
    os << width;
  } else if (width == 16) {
    os << lanes << "bfloat16";
  } else {
    os << lanes << "float";
  }
  os << "(" << emitter.getOrCreateName(source) << ")";
  return success();
}

// Generate the ext intrinsic
static LogicalResult printOperation(CppEmitter &emitter, aievec::ExtOp extOp) {
  Value source = extOp.getSource();
  int8_t index = extOp.getIndex();

  raw_indented_ostream &os = emitter.ostream();

  // Generate the initialization for the result
  if (failed(emitter.emitAssignPrefix(*extOp)))
    return failure();

  if (!emitter.hasValueInScope(source))
    return failure();

  VectorType resType = extOp.getResult().getType().cast<VectorType>();
  Type eltType = resType.getElementType();
  unsigned lanes = getVectorLaneSize(resType);
  unsigned resWidth = getElementSizeInBits(resType);

  // Print the version of ext for aie-ml
  if (AIEML) {
    os << "extract_v" << std::to_string(lanes);
    if (eltType.isa<IntegerType>()) {
      os << "int" << std::to_string(resWidth);
    } else if (resWidth == 16) {
      os << "bfloat16";
    } else {
      os << "float";
    }
  } else {
    // Print the version of ext for aie1
    int32_t vecSizeInBits = getVectorSizeInBits(resType);
    assert(vecSizeInBits == 128 || vecSizeInBits == 256 ||
           vecSizeInBits == 512);
    os << (vecSizeInBits == 128   ? "ext_v"
           : vecSizeInBits == 256 ? "ext_w"
                                  : "ext_x");
  }
  os << "(";
  // The source accumulator should have already been emitted
  os << emitter.getOrCreateName(source);
  os << ", ";
  os << std::to_string(index);
  os << ")";

  return success();
}

// Generate the concat intrinsic
static LogicalResult printOperation(CppEmitter &emitter,
                                    aievec::ConcatOp concatOp) {
  SmallVector<Value> sources = concatOp.getSources();

  raw_indented_ostream &os = emitter.ostream();

  // Generate the initialization for the result
  if (failed(emitter.emitAssignPrefix(*concatOp)))
    return failure();

  os << "concat";
  os << "(";
  // Print the sources sources
  bool first = true;
  for (auto source : sources) {
    // source should have already been emitted
    if (!emitter.hasValueInScope(source))
      return failure();
    if (!first)
      os << ", ";
    os << emitter.getOrCreateName(source);
    first = false;
  }
  os << ")";
  return success();
}

// Generate the shift intrinsic
static LogicalResult printOperation(CppEmitter &emitter,
                                    aievec::ShiftOp shiftOp) {
  Value lhs = shiftOp.getLhs();
  Value rhs = shiftOp.getRhs();
  Value shift = shiftOp.getShift();
  bool isAcc = shiftOp.getIsAcc();

  raw_indented_ostream &os = emitter.ostream();

  // Generate the initialization for the result
  if (failed(emitter.emitAssignPrefix(*shiftOp, isAcc)))
    return failure();

  os << "shift_bytes";
  os << "(";
  // Print the lhs, rhs and shift
  if (!emitter.hasValueInScope(lhs) || !emitter.hasValueInScope(rhs))
    return failure();
  os << emitter.getOrCreateName(lhs);
  os << ", ";
  os << emitter.getOrCreateName(rhs);
  os << ", ";

  if (!emitter.hasValueInScope(shift))
    return failure();
  os << emitter.getOrCreateName(shift);
  os << ")";
  return success();
}

// Generate the shuffle intrinsic
static LogicalResult printOperation(CppEmitter &emitter,
                                    aievec::ShuffleOp shuffleOp) {
  Value source = shuffleOp.getSource();
  unsigned mode = shuffleOp.getMode();

  raw_indented_ostream &os = emitter.ostream();

  // Generate the initialization for the result
  if (failed(emitter.emitAssignPrefix(*shuffleOp)))
    return failure();

  os << "shuffle";
  os << "(";
  // Print the source and mode
  // source should have already been emitted
  if (!emitter.hasValueInScope(source))
    return failure();
  os << emitter.getOrCreateName(source);
  os << ", ";
  os << std::to_string(mode);
  os << ")";
  return success();
}

// Generate the select intrinsic
static LogicalResult printOperation(CppEmitter &emitter,
                                    aievec::SelectOp selectOp) {
  Value xbuff = selectOp.getXbuff();
  assert(xbuff && "xbuff empty in select op");

  raw_indented_ostream &os = emitter.ostream();

  // Generate the initialization for the result
  if (failed(emitter.emitAssignPrefix(*selectOp)))
    return failure();

  // Determine if we want to geneate select32, or select16, or select8
  VectorType xbuffType = selectOp.getXbuff().getType().cast<VectorType>();
  int32_t elementSizeInBits = getElementSizeInBits(xbuffType);
  assert(elementSizeInBits == 16 || elementSizeInBits == 32 ||
         elementSizeInBits == 64);
  // Print name
  os << (elementSizeInBits == 16   ? "select32"
         : elementSizeInBits == 32 ? "select16"
                                   : "select8");
  os << "(";
  // Print select bits
  assert(!selectOp.getSelect().empty());
  os << selectOp.getSelect();
  // xbuff should have already been emitted
  if (!emitter.hasValueInScope(xbuff))
    return failure();
  // Print xbuff
  os << ", ";
  os << emitter.getOrCreateName(xbuff);
  // Print attributes related to lower lane selection
  if (!selectOp.getXstart().empty())
    os << ", " << selectOp.getXstart();
  if (!selectOp.getXoffsets().empty())
    os << ", " << selectOp.getXoffsets();
  if (!selectOp.getXoffsetsHi().empty())
    os << ", " << selectOp.getXoffsetsHi();
  if (!selectOp.getXsquare().empty())
    os << ", " << selectOp.getXsquare();
  // If ybuff is not null, print it
  if (selectOp.getYbuff()) {
    Value ybuff = selectOp.getYbuff();
    // ybuff should have already been emitted
    if (!emitter.hasValueInScope(ybuff))
      return failure();
    // Print ybuff
    os << ", ";
    os << emitter.getOrCreateName(ybuff);
  }
  // Print attributes related to higher lane selection
  if (!selectOp.getYstart().empty())
    os << ", " << selectOp.getYstart();
  if (!selectOp.getYoffsets().empty())
    os << ", " << selectOp.getYoffsets();
  if (!selectOp.getYoffsetsHi().empty())
    os << ", " << selectOp.getYoffsetsHi();
  if (!selectOp.getYsquare().empty())
    os << ", " << selectOp.getYsquare();

  os << ")";
  return success();
}

// Generate the pack intrinsic
static LogicalResult printOperation(CppEmitter &emitter,
                                    aievec::PackOp packOp) {
  Value source = packOp.getSource();

  raw_indented_ostream &os = emitter.ostream();

  // Generate the initialization for the result
  if (failed(emitter.emitAssignPrefix(*packOp)))
    return failure();

  // Determine the flavor of result
  VectorType sourceType = packOp.getSource().getType().cast<VectorType>();
  Type scalarType = sourceType.getElementType();
  os << (scalarType.isUnsignedInteger() ? "upack" : "pack");
  os << "(";
  // source should have already been emitted
  if (!emitter.hasValueInScope(source))
    return failure();
  os << emitter.getOrCreateName(source);
  os << ")";
  return success();
}

// Print lhs or rhs operand of add/sub intrinsic
template <typename T>
static LogicalResult printAddOrSubOperand(CppEmitter &emitter, T op,
                                          unsigned opNum) {
  // We currently only support printing operands 0 and 1
  if (opNum > 1)
    return failure();

  // The operand should have already been emitted
  Value operand = opNum == 0 ? op.getLhs() : op.getRhs();
  if (!emitter.hasValueInScope(operand))
    return failure();

  raw_indented_ostream &os = emitter.ostream();

  StringRef start = op.getStart(opNum);
  StringRef offset = op.getOffset(opNum);
  StringRef offsetHi = op.getOffsetHi(opNum);
  StringRef square = op.getSquare(opNum);

  os << emitter.getOrCreateName(operand);
  if (!start.empty())
    os << ", " << start;
  if (!offset.empty())
    os << ", " << offset;
  if (!offsetHi.empty())
    os << ", " << offsetHi;
  if (!square.empty())
    os << ", " << square;

  return success();
}

// Print lhs or rhs operand of min/max intrinsic
template <typename T>
static LogicalResult printMinMaxOperand(CppEmitter &emitter, T op,
                                        unsigned opNum) {
  // We currently only support printing operands 0 and 1
  if (opNum > 1)
    return failure();

  // The operand should have already been emitted
  Value operand = opNum == 0 ? op.getLhs() : op.getRhs();
  if (!emitter.hasValueInScope(operand))
    return failure();

  raw_indented_ostream &os = emitter.ostream();

  os << emitter.getOrCreateName(operand);

  return success();
}

// Print lhs or rhs operand of add_elem/sub_elem intrinsic
template <typename T>
static LogicalResult printAddElemOrSubElemOperand(CppEmitter &emitter, T op,
                                                  unsigned opNum) {
  // We currently only support printing operands 0 and 1
  if (opNum > 1)
    return failure();

  // The operand should have already been emitted
  Value operand = opNum == 0 ? op.getLhs() : op.getRhs();
  if (!emitter.hasValueInScope(operand))
    return failure();

  raw_indented_ostream &os = emitter.ostream();

  os << emitter.getOrCreateName(operand);

  return success();
}

// Print lhs or rhs operand of mul/mac intrinsic
template <typename T>
static LogicalResult printFMAOrMulOperand(CppEmitter &emitter, T op,
                                          unsigned opNum) {
  // We currently only support printing operands 0 and 1
  if (opNum > 1)
    return failure();

  // The operand should have already been emitted
  Value operand = opNum == 0 ? op.getLhs() : op.getRhs();
  if (!emitter.hasValueInScope(operand))
    return failure();

  raw_indented_ostream &os = emitter.ostream();

  StringRef start = op.getStart(opNum);
  StringRef offset = op.getOffset(opNum);
  StringRef offsetHi = op.getOffsetHi(opNum);
  StringRef step = op.getStep(opNum);
  StringRef square = op.getSquare(opNum);

  os << emitter.getOrCreateName(operand);
  if (!start.empty())
    os << ", " << start;
  if (!offset.empty())
    os << ", " << offset;
  if (!offsetHi.empty())
    os << ", " << offsetHi;
  if (!step.empty())
    os << ", " << step;
  if (!square.empty())
    os << ", " << square;

  return success();
}

// Print lhs or rhs operand of mul_elem/mac_elem intrinsic
template <typename T>
static LogicalResult printFMAOrMulElemOperand(CppEmitter &emitter, T op,
                                              Type iType, int32_t size,
                                              unsigned opNum) {
  // We currently only support printing operands 0 and 1
  if (opNum > 1)
    return failure();

  // The operand should have already been emitted
  Value operand = opNum == 0 ? op.getLhs() : op.getRhs();
  if (!emitter.hasValueInScope(operand))
    return failure();

  raw_indented_ostream &os = emitter.ostream();

  os << emitter.getOrCreateName(operand);

  if (size == 32 && iType) {
    StringRef str = opNum == 0 ? "undef_v16int32()" : "broadcast_zero_s32()";
    os << ", " << str;
  }

  return success();
}

// Print lhs or rhs operand of mul_conv/mac_conv intrinsic
template <typename T>
static LogicalResult printFMAOrMulConvOperand(CppEmitter &emitter, T op,
                                              int32_t M, int32_t N,
                                              unsigned opNum) {
  // We currently only support printing operands 0 and 1
  if (opNum > 1)
    return failure();

  // The operand should have already been emitted
  Value operand = opNum == 0 ? op.getLhs() : op.getRhs();
  if (!emitter.hasValueInScope(operand))
    return failure();

  raw_indented_ostream &os = emitter.ostream();

  os << emitter.getOrCreateName(operand);

  return success();
}

// Generate the Mul op
static LogicalResult printOperation(CppEmitter &emitter, aievec::MulOp mulOp) {
  auto lhs = mulOp.getLhs();
  auto rhs = mulOp.getRhs();

  // The sources should have already been emitted
  if (!emitter.hasValueInScope(lhs) || !emitter.hasValueInScope(rhs))
    return failure();

  // Determine if the mul scheme is simple or complex
  bool simpleScheme = mulOp.getStart(0).empty();

  std::string opname;
  // Create opname based on the result type
  VectorType resType = mulOp.getResult().getType().cast<VectorType>();
  Type eltType = resType.getElementType();
  if (!simpleScheme) {
    if (auto iType = eltType.dyn_cast<IntegerType>()) {
      if (iType.getWidth() == 80)
        opname = "l";
    } else if (eltType.isa<FloatType>())
      opname = "fp";
  }
  opname += "mul";
  if (!simpleScheme && !eltType.isa<FloatType>())
    opname += std::to_string(getVectorLaneSize(resType));

  raw_indented_ostream &os = emitter.ostream();

  // Generate the initialization for the accumulator
  if (failed(emitter.emitAssignPrefix(*mulOp)))
    return failure();

  os << opname;
  os << "(";
  if (failed(printFMAOrMulOperand<aievec::MulOp>(emitter, mulOp, 0)))
    return failure();
  os << ", ";
  if (failed(printFMAOrMulOperand<aievec::MulOp>(emitter, mulOp, 1)))
    return failure();
  os << ")";

  return success();
}

// Generate the MulElem op
static LogicalResult printOperation(CppEmitter &emitter,
                                    aievec::MulElemOp mul_elemOp) {
  auto lhs = mul_elemOp.getLhs();
  auto rhs = mul_elemOp.getRhs();

  // The sources should have already been emitted
  if (!emitter.hasValueInScope(lhs) || !emitter.hasValueInScope(rhs))
    return failure();

  std::string opname;
  opname = "mul_elem";

  // Create opname based on the source type
  VectorType lhsType = mul_elemOp.getLhs().getType().cast<VectorType>();
  Type eltType = lhsType.getElementType();
  int32_t lsize = getElementSizeInBits(lhsType);
  auto iType = eltType.dyn_cast<IntegerType>();

  if (iType) {
    if (lsize == 32) {
      opname += "_16_2";
    } else if (lsize == 16) {
      opname += "_32";
    } else if (lsize == 8) {
      opname += "_32_2";
    }
  } else if (eltType.isa<FloatType>()) {
    if (lsize == 32) {
      opname += "_16";
    } else if (lsize == 16) {
      opname += "_16_2";
    }
  }

  raw_indented_ostream &os = emitter.ostream();

  // Generate the initialization for the accumulator
  if (failed(emitter.emitAssignPrefix(*mul_elemOp, true /*isAcc*/)))
    return failure();

  os << opname;
  os << "(";
  if (failed(printFMAOrMulElemOperand<aievec::MulElemOp>(emitter, mul_elemOp,
                                                         iType, lsize, 1)))
    return failure();
  os << ", ";
  if (failed(printFMAOrMulElemOperand<aievec::MulElemOp>(emitter, mul_elemOp,
                                                         iType, lsize, 0)))
    return failure();
  os << ")";

  return success();
}

// Generate the MulConv op
static LogicalResult printOperation(CppEmitter &emitter,
                                    aievec::MulConvOp mul_convOp) {
  auto lhs = mul_convOp.getLhs();
  auto rhs = mul_convOp.getRhs();

  // The sources should have already been emitted
  if (!emitter.hasValueInScope(lhs) || !emitter.hasValueInScope(rhs))
    return failure();

  std::string opname;
  opname = "mul_conv";

  // Create opname based on the source type
  VectorType lhsType = mul_convOp.getLhs().getType().cast<VectorType>();
  Type eltType = lhsType.getElementType();
  int32_t lsize = getElementSizeInBits(lhsType);
  auto iType = eltType.dyn_cast<IntegerType>();

  // Only support int16 and int8 cases
  if (!iType || !(lsize == 16 || lsize == 8)) {
    return failure();
  }

  int32_t M = mul_convOp.getM();
  int32_t N = mul_convOp.getN();
  opname += ("_" + std::to_string(M) + "x" + std::to_string(N));

  raw_indented_ostream &os = emitter.ostream();

  // Generate the initialization for the accumulator
  if (failed(emitter.emitAssignPrefix(*mul_convOp, true /*isAcc*/)))
    return failure();

  os << opname;
  os << "(";

  if (failed(printFMAOrMulConvOperand<aievec::MulConvOp>(emitter, mul_convOp, M,
                                                         N, 0)))
    return failure();
  os << ", ";
  if (failed(printFMAOrMulConvOperand<aievec::MulConvOp>(emitter, mul_convOp, M,
                                                         N, 1)))
    return failure();
  os << ")";

  return success();
}

// Generate the Add op
static LogicalResult printOperation(CppEmitter &emitter, aievec::AddOp addOp) {
  auto lhs = addOp.getLhs();
  auto rhs = addOp.getRhs();

  // The sources should have already been emitted
  if (!emitter.hasValueInScope(lhs) || !emitter.hasValueInScope(rhs))
    return failure();

  raw_indented_ostream &os = emitter.ostream();

  // Generate the initialization for the result
  if (failed(emitter.emitAssignPrefix(*addOp)))
    return failure();

  // Get the scalar type of result vector
  VectorType resultType = addOp.getResult().getType().cast<VectorType>();
  unsigned lanes = getVectorLaneSize(resultType);
  Type elementType = resultType.getElementType();
  bool floatType = elementType.isa<FloatType>();

  // Detemine if the add scheme is simple or complex
  bool simpleScheme = addOp.getStart(0).empty();

  if (simpleScheme) {
    // Handle float type operation
    if (floatType) {
      os << "fpadd";
      os << "(";
      os << emitter.getOrCreateName(lhs);
      os << ", ";
      os << emitter.getOrCreateName(rhs);
      os << ")";
    }
    // Otherwise we can simply print this as overloaded +
    else {
      os << emitter.getOrCreateName(lhs);
      os << " + ";
      os << emitter.getOrCreateName(rhs);
    }
    return success();
  }
  // Otherwise this is complex scheme
  os << (floatType ? "fpadd" : "add" + std::to_string(lanes));
  os << "(";
  if (failed(printAddOrSubOperand<aievec::AddOp>(emitter, addOp, 0)))
    return failure();
  os << ", ";
  if (failed(printAddOrSubOperand<aievec::AddOp>(emitter, addOp, 1)))
    return failure();
  os << ")";
  return success();
}

// Generate the Sub op
static LogicalResult printOperation(CppEmitter &emitter, aievec::SubOp subOp) {
  auto lhs = subOp.getLhs();
  auto rhs = subOp.getRhs();

  // The sources should have already been emitted
  if (!emitter.hasValueInScope(lhs) || !emitter.hasValueInScope(rhs))
    return failure();

  raw_indented_ostream &os = emitter.ostream();

  // Generate the initialization for the result
  if (failed(emitter.emitAssignPrefix(*subOp)))
    return failure();

  // Get the scalar type of result vector
  VectorType resultType = subOp.getResult().getType().cast<VectorType>();
  unsigned lanes = getVectorLaneSize(resultType);
  Type elementType = resultType.getElementType();
  bool floatType = elementType.isa<FloatType>();

  // Detemine if the sub scheme is simple or complex
  bool simpleScheme = subOp.getStart(0).empty();

  if (simpleScheme) {
    // Handle float type operation
    if (floatType) {
      os << "fpsub";
      os << "(";
      os << emitter.getOrCreateName(lhs);
      os << ", ";
      os << emitter.getOrCreateName(rhs);
      os << ")";
    }
    // Otherwise we can simply print this as overloaded -
    else {
      os << emitter.getOrCreateName(lhs);
      os << " - ";
      os << emitter.getOrCreateName(rhs);
    }
    return success();
  }
  // Otherwise this is complex scheme
  os << (floatType ? "fpsub" : "sub" + std::to_string(lanes));
  os << "(";
  if (failed(printAddOrSubOperand<aievec::SubOp>(emitter, subOp, 0)))
    return failure();
  os << ", ";
  if (failed(printAddOrSubOperand<aievec::SubOp>(emitter, subOp, 1)))
    return failure();
  os << ")";
  return success();
}

// Generate the Min op
static LogicalResult printOperation(CppEmitter &emitter, aievec::MinOp minOp) {
  auto lhs = minOp.getLhs();
  auto rhs = minOp.getRhs();

  // The sources should have already been emitted
  if (!emitter.hasValueInScope(lhs) || !emitter.hasValueInScope(rhs))
    return failure();

  raw_indented_ostream &os = emitter.ostream();

  // Generate the initialization for the result
  if (failed(emitter.emitAssignPrefix(*minOp)))
    return failure();

  os << "min(";
  if (failed(printMinMaxOperand<aievec::MinOp>(emitter, minOp, 0)))
    return failure();
  os << ", ";
  if (failed(printMinMaxOperand<aievec::MinOp>(emitter, minOp, 1)))
    return failure();
  os << ")";
  return success();
}

// Generate the Max op
static LogicalResult printOperation(CppEmitter &emitter, aievec::MaxOp maxOp) {
  auto lhs = maxOp.getLhs();
  auto rhs = maxOp.getRhs();

  // The sources should have already been emitted
  if (!emitter.hasValueInScope(lhs) || !emitter.hasValueInScope(rhs))
    return failure();

  raw_indented_ostream &os = emitter.ostream();

  // Generate the initialization for the result
  if (failed(emitter.emitAssignPrefix(*maxOp)))
    return failure();

  os << "max(";
  if (failed(printMinMaxOperand<aievec::MaxOp>(emitter, maxOp, 0)))
    return failure();
  os << ", ";
  if (failed(printMinMaxOperand<aievec::MaxOp>(emitter, maxOp, 1)))
    return failure();
  os << ")";
  return success();
}

// Generate the AddElem op
static LogicalResult printOperation(CppEmitter &emitter,
                                    aievec::AddElemOp add_elemOp) {
  auto lhs = add_elemOp.getLhs();
  auto rhs = add_elemOp.getRhs();

  // The sources should have already been emitted
  if (!emitter.hasValueInScope(lhs) || !emitter.hasValueInScope(rhs))
    return failure();

  raw_indented_ostream &os = emitter.ostream();

  // Generate the initialization for the result
  if (failed(emitter.emitAssignPrefix(*add_elemOp, true)))
    return failure();

  os << "add(";
  if (failed(printAddElemOrSubElemOperand<aievec::AddElemOp>(emitter,
                                                             add_elemOp, 0)))
    return failure();
  os << ", ";
  if (failed(printAddElemOrSubElemOperand<aievec::AddElemOp>(emitter,
                                                             add_elemOp, 1)))
    return failure();
  os << ")";
  return success();
}

// Generate the SubElem op
static LogicalResult printOperation(CppEmitter &emitter,
                                    aievec::SubElemOp sub_elemOp) {
  auto lhs = sub_elemOp.getLhs();
  auto rhs = sub_elemOp.getRhs();

  // The sources should have already been emitted
  if (!emitter.hasValueInScope(lhs) || !emitter.hasValueInScope(rhs))
    return failure();

  raw_indented_ostream &os = emitter.ostream();

  // Generate the initialization for the result
  if (failed(emitter.emitAssignPrefix(*sub_elemOp, true)))
    return failure();

  os << "sub(";
  if (failed(printAddElemOrSubElemOperand<aievec::SubElemOp>(emitter,
                                                             sub_elemOp, 0)))
    return failure();
  os << ", ";
  if (failed(printAddElemOrSubElemOperand<aievec::SubElemOp>(emitter,
                                                             sub_elemOp, 1)))
    return failure();
  os << ")";
  return success();
}

// Generate the FMA op
static LogicalResult printOperation(CppEmitter &emitter, aievec::FMAOp fmaOp) {
  auto acc = fmaOp.getAcc();
  auto lhs = fmaOp.getLhs();
  auto rhs = fmaOp.getRhs();

  // The sources should have already been emitted
  if (!emitter.hasValueInScope(acc) || !emitter.hasValueInScope(lhs) ||
      !emitter.hasValueInScope(rhs))
    return failure();

  // Detemine if the mul scheme is simple or complex
  bool simpleScheme = fmaOp.getStart(0).empty();

  std::string opname;
  // Create opname based on the result type
  VectorType resType = fmaOp.getResult().getType().cast<VectorType>();
  Type eltType = resType.getElementType();
  if (!simpleScheme) {
    if (auto iType = eltType.dyn_cast<IntegerType>()) {
      if (iType.getWidth() == 80)
        opname = "l";
    } else if (eltType.isa<FloatType>())
      opname = "fp";
  }
  opname += fmaOp.getFmsub() ? "msc" : "mac";
  if (!simpleScheme && !eltType.isa<FloatType>())
    opname += std::to_string(getVectorLaneSize(resType));

  raw_indented_ostream &os = emitter.ostream();

  StringRef accName = emitter.getOrCreateName(acc);
  os << accName;
  os << " = ";
  os << opname;
  os << "(";
  os << accName;
  os << ", ";
  if (failed(printFMAOrMulOperand<aievec::FMAOp>(emitter, fmaOp, 0)))
    return failure();
  os << ", ";
  if (failed(printFMAOrMulOperand<aievec::FMAOp>(emitter, fmaOp, 1)))
    return failure();
  os << ")";

  // Finally, set the name of the result to the accumulator's name
  emitter.setName(fmaOp->getResult(0), accName);

  return success();
}

// Generate the FMAElem op
static LogicalResult printOperation(CppEmitter &emitter,
                                    aievec::FMAElemOp fma_elemOp) {
  auto acc = fma_elemOp.getAcc();
  auto lhs = fma_elemOp.getLhs();
  auto rhs = fma_elemOp.getRhs();

  // The sources should have already been emitted
  if (!emitter.hasValueInScope(acc) || !emitter.hasValueInScope(lhs) ||
      !emitter.hasValueInScope(rhs))
    return failure();

  std::string opname;
  opname = fma_elemOp.getFmsub() ? "msc_elem" : "mac_elem";
  // Create opname based on the lhs and rhs type
  VectorType lhsType = fma_elemOp.getLhs().getType().cast<VectorType>();
  Type eltType = lhsType.getElementType();
  int32_t lsize = getElementSizeInBits(lhsType);
  auto iType = eltType.dyn_cast<IntegerType>();

  if (iType) {
    if (lsize == 32) {
      opname += "_16_2";
    } else if (lsize == 16) {
      opname += "_32";
    } else if (lsize == 8) {
      opname += "_32_2";
    }
  } else if (eltType.isa<FloatType>()) {
    if (lsize == 32) {
      opname += "_16";
    } else if (lsize == 16) {
      opname += "_16_2";
    }
  }

  raw_indented_ostream &os = emitter.ostream();

  StringRef accName = emitter.getOrCreateName(acc);
  os << accName;
  os << " = ";
  os << opname;
  os << "(";
  if (failed(printFMAOrMulElemOperand<aievec::FMAElemOp>(emitter, fma_elemOp,
                                                         iType, lsize, 1)))
    return failure();
  os << ", ";
  if (failed(printFMAOrMulElemOperand<aievec::FMAElemOp>(emitter, fma_elemOp,
                                                         iType, lsize, 0)))
    return failure();
  os << ", ";
  os << accName;
  os << ")";

  // Finally, set the name of the result to the accumulator's name
  emitter.setName(fma_elemOp->getResult(0), accName);

  return success();
}

// Generate the FMAConv op
static LogicalResult printOperation(CppEmitter &emitter,
                                    aievec::FMAConvOp fma_convOp) {
  auto acc = fma_convOp.getAcc();
  auto lhs = fma_convOp.getLhs();
  auto rhs = fma_convOp.getRhs();

  // The sources should have already been emitted
  if (!emitter.hasValueInScope(acc) || !emitter.hasValueInScope(lhs) ||
      !emitter.hasValueInScope(rhs))
    return failure();

  std::string opname;
  opname = fma_convOp.getFmsub() ? "msc_conv" : "mac_conv";
  // Create opname based on the lhs and rhs type
  VectorType lhsType = fma_convOp.getLhs().getType().cast<VectorType>();
  Type eltType = lhsType.getElementType();
  int32_t lsize = getElementSizeInBits(lhsType);
  auto iType = eltType.dyn_cast<IntegerType>();

  // Only support int16 and int8 cases
  if (!iType || !(lsize == 16 || lsize == 8)) {
    return failure();
  }

  int32_t M = fma_convOp.getM();
  int32_t N = fma_convOp.getN();
  opname += ("_" + std::to_string(M) + "x" + std::to_string(N));

  raw_indented_ostream &os = emitter.ostream();

  StringRef accName = emitter.getOrCreateName(acc);
  os << accName;
  os << " = ";
  os << opname;
  os << "(";
  if (failed(printFMAOrMulConvOperand<aievec::FMAConvOp>(emitter, fma_convOp, M,
                                                         N, 0)))
    return failure();
  os << ", ";
  if (failed(printFMAOrMulConvOperand<aievec::FMAConvOp>(emitter, fma_convOp, M,
                                                         N, 1)))
    return failure();
  os << ", ";
  os << accName;
  os << ")";

  // Finally, set the name of the result to the accumulator's name
  emitter.setName(fma_convOp->getResult(0), accName);

  return success();
}

// Generate the comparison intrinsics(eq, ne, lt, le, gt, ge) for AIE-ML
static LogicalResult printOperation(CppEmitter &emitter, aievec::CmpOp cmpOp) {
  if (!AIEML) {
    return failure();
  }

  // The lhs and rhs should have already been emitted
  Value lhs = cmpOp.getLhs();
  Value rhs = cmpOp.getRhs();

  if (!emitter.hasValueInScope(lhs) || !emitter.hasValueInScope(rhs))
    return failure();

  // Generate the initialization for the vector
  if (failed(emitter.emitAssignPrefix(*cmpOp)))
    return failure();

  raw_indented_ostream &os = emitter.ostream();

  StringRef pred = cmpOp.getPred();
  if (pred == "eq") {
    os << "eq";
  } else if (pred == "ne") {
    os << "ne";
  } else if (pred == "slt" || pred == "ult") {
    os << "lt";
  } else if (pred == "sle" || pred == "ule") {
    os << "le";
  } else if (pred == "sgt" || pred == "ugt") {
    os << "gt";
  } else if (pred == "sge" || pred == "uge") {
    os << "ge";
  } else {
    return failure();
  }

  os << "(";
  VectorType vType = lhs.getType().cast<VectorType>();
  Type eltType = vType.getElementType();

  if (eltType.isa<IntegerType>() &&
      (pred == "ult" || pred == "ule" || pred == "ugt" || pred == "uge")) {
    unsigned lanes = getVectorLaneSize(vType);
    unsigned width = getElementSizeInBits(vType);
    os << "v" << std::to_string(lanes) << "uint" << std::to_string(width);
    os << "(";
    os << emitter.getOrCreateName(lhs);
    os << "), ";
    os << "v" << std::to_string(lanes) << "uint" << std::to_string(width);
    os << "(";
    os << emitter.getOrCreateName(rhs);
    os << ")";
  } else {
    os << emitter.getOrCreateName(lhs);
    os << ", ";
    os << emitter.getOrCreateName(rhs);
  }
  os << ")";
  return success();
}

// Generate the sel intrinsic for AIE-ML
static LogicalResult printOperation(CppEmitter &emitter, aievec::SelOp selOp) {
  if (!AIEML) {
    return failure();
  }

  // The lhs, rhs and sel should have already been emitted
  Value lhs = selOp.getLhs();
  Value rhs = selOp.getRhs();
  Value sel = selOp.getSel();

  if (!emitter.hasValueInScope(lhs) || !emitter.hasValueInScope(rhs) ||
      !emitter.hasValueInScope(sel))
    return failure();

  // Generate the initialization for the vector
  if (failed(emitter.emitAssignPrefix(*selOp)))
    return failure();

  raw_indented_ostream &os = emitter.ostream();

  os << "sel(";
  os << emitter.getOrCreateName(rhs);
  os << ", ";
  os << emitter.getOrCreateName(lhs);
  os << ", ";
  os << emitter.getOrCreateName(sel);
  os << ")";
  return success();
}

// Generate the extract elem intrinsic
static LogicalResult printOperation(CppEmitter &emitter,
                                    aievec::ExtElemOp extElemOp) {
  Value source = extElemOp.getSource();
  Value index = extElemOp.getIndex();

  raw_indented_ostream &os = emitter.ostream();

  // Generate the initialization for the result
  if (failed(emitter.emitAssignPrefix(*extElemOp)))
    return failure();

  // source should have already been emitted
  if (!emitter.hasValueInScope(source))
    return failure();

  os << "extract_elem";
  os << "(";
  // Print the source and index
  os << emitter.getOrCreateName(source);
  os << ", ";
  os << emitter.getOrCreateName(index);
  os << ")";
  return success();
}

// Generate the transfer write op
static LogicalResult printOperation(CppEmitter &emitter,
                                    vector::TransferWriteOp writeOp) {
  Value source = writeOp.getSource();
  Value vector = writeOp.getVector();

  // If the aray, or the vector being outputted is not already emitted,
  // error out
  if (!emitter.hasValueInScope(source) || !emitter.hasValueInScope(vector))
    return failure();

  // Construct the access expression using memref shape and indices
  std::string access;
  auto indices = writeOp.getIndices();
  if (failed(createLinearizedAccess(emitter, source, indices, access)))
    return failure();

  raw_indented_ostream &os = emitter.ostream();

  os << "*(";
  if (failed(emitter.emitType(writeOp->getLoc(), vector.getType())))
    return failure();
  os << " *)";
  os << "(";
  os << emitter.getOrCreateName(source);
  if (!access.empty())
    os << " + " << access;
  os << ")";
  os << " = ";
  os << emitter.getOrCreateName(vector);
  return success();
}

// Generate the memref store op
static LogicalResult printOperation(CppEmitter &emitter,
                                    memref::StoreOp storeOp) {
  Value value = storeOp.getValue();
  Value memref = storeOp.getMemref();

  // If the value, or the memref being outputted is not already emitted,
  // error out
  if (!emitter.hasValueInScope(value) || !emitter.hasValueInScope(memref))
    return failure();

  raw_indented_ostream &os = emitter.ostream();

  os << "*(";
  if (failed(emitter.emitType(
          storeOp->getLoc(),
          cast<MemRefType>(memref.getType()).getElementType())))
    return failure();
  os << " *)";
  os << emitter.getOrCreateName(memref);
  os << " = ";
  os << emitter.getOrCreateName(value);
  return success();
}

static LogicalResult printConstantOp(CppEmitter &emitter, Operation *operation,
                                     Attribute value) {
  OpResult result = operation->getResult(0);

  // Only emit an assignment as the variable was already declared when printing
  // the FuncOp.
  if (emitter.shouldDeclareVariablesAtTop()) {
    // Skip the assignment if the emitc.constant has no value.
    if (auto oAttr = value.dyn_cast<emitc::OpaqueAttr>()) {
      if (oAttr.getValue().empty())
        return success();
    }

    if (failed(emitter.emitVariableAssignment(result)))
      return failure();
    return emitter.emitAttribute(operation->getLoc(), value);
  }

  // Emit a variable declaration for an emitc.constant op without value.
  if (auto oAttr = value.dyn_cast<emitc::OpaqueAttr>()) {
    if (oAttr.getValue().empty())
      // The semicolon gets printed by the emitOperation function.
      return emitter.emitVariableDeclaration(result,
                                             /*trailingSemicolon=*/false);
  }

  // Emit a variable declaration.
  if (failed(emitter.emitAssignPrefix(*operation)))
    return failure();
  return emitter.emitAttribute(operation->getLoc(), value);
}

static LogicalResult printOperation(CppEmitter &emitter,
                                    emitc::ConstantOp constantOp) {
  Operation *operation = constantOp.getOperation();
  Attribute value = constantOp.getValue();

  return printConstantOp(emitter, operation, value);
}

static LogicalResult printOperation(CppEmitter &emitter,
                                    arith::ConstantOp constantOp) {
  Operation *operation = constantOp.getOperation();
  Attribute value = constantOp.getValue();

  return printConstantOp(emitter, operation, value);
}

static LogicalResult printOperation(CppEmitter &emitter,
                                    cf::BranchOp branchOp) {
  raw_ostream &os = emitter.ostream();
  Block &successor = *branchOp.getSuccessor();

  for (auto pair :
       llvm::zip(branchOp.getOperands(), successor.getArguments())) {
    Value &operand = std::get<0>(pair);
    BlockArgument &argument = std::get<1>(pair);
    os << emitter.getOrCreateName(argument) << " = "
       << emitter.getOrCreateName(operand) << ";\n";
  }

  os << "goto ";
  if (!(emitter.hasBlockLabel(successor)))
    return branchOp.emitOpError("unable to find label for successor block");
  os << emitter.getOrCreateName(successor);
  return success();
}

static LogicalResult printOperation(CppEmitter &emitter,
                                    cf::CondBranchOp condBranchOp) {
  raw_indented_ostream &os = emitter.ostream();
  Block &trueSuccessor = *condBranchOp.getTrueDest();
  Block &falseSuccessor = *condBranchOp.getFalseDest();

  os << "if (" << emitter.getOrCreateName(condBranchOp.getCondition())
     << ") {\n";

  os.indent();

  // If condition is true.
  for (auto pair : llvm::zip(condBranchOp.getTrueOperands(),
                             trueSuccessor.getArguments())) {
    Value &operand = std::get<0>(pair);
    BlockArgument &argument = std::get<1>(pair);
    os << emitter.getOrCreateName(argument) << " = "
       << emitter.getOrCreateName(operand) << ";\n";
  }

  os << "goto ";
  if (!(emitter.hasBlockLabel(trueSuccessor))) {
    return condBranchOp.emitOpError("unable to find label for successor block");
  }
  os << emitter.getOrCreateName(trueSuccessor) << ";\n";
  os.unindent() << "} else {\n";
  os.indent();
  // If condition is false.
  for (auto pair : llvm::zip(condBranchOp.getFalseOperands(),
                             falseSuccessor.getArguments())) {
    Value &operand = std::get<0>(pair);
    BlockArgument &argument = std::get<1>(pair);
    os << emitter.getOrCreateName(argument) << " = "
       << emitter.getOrCreateName(operand) << ";\n";
  }

  os << "goto ";
  if (!(emitter.hasBlockLabel(falseSuccessor))) {
    return condBranchOp.emitOpError()
           << "unable to find label for successor block";
  }
  os << emitter.getOrCreateName(falseSuccessor) << ";\n";
  os.unindent() << "}";
  return success();
}

static LogicalResult printOperation(CppEmitter &emitter, func::CallOp callOp) {
  if (failed(emitter.emitAssignPrefix(*callOp.getOperation())))
    return failure();

  raw_ostream &os = emitter.ostream();
  os << callOp.getCallee() << "(";
  if (failed(emitter.emitOperands(*callOp.getOperation())))
    return failure();
  os << ")";
  return success();
}

static LogicalResult printOperation(CppEmitter &emitter, emitc::CallOp callOp) {
  raw_ostream &os = emitter.ostream();
  Operation &op = *callOp.getOperation();

  if (failed(emitter.emitAssignPrefix(op)))
    return failure();
  os << callOp.getCallee();

  auto emitArgs = [&](Attribute attr) -> LogicalResult {
    if (auto t = attr.dyn_cast<IntegerAttr>()) {
      // Index attributes are treated specially as operand index.
      if (t.getType().isIndex()) {
        int64_t idx = t.getInt();
        if ((idx < 0) || (idx >= op.getNumOperands()))
          return op.emitOpError("invalid operand index");
        if (!emitter.hasValueInScope(op.getOperand(idx)))
          return op.emitOpError("operand ")
                 << idx << "'s value not defined in scope";
        os << emitter.getOrCreateName(op.getOperand(idx));
        return success();
      }
    }
    if (failed(emitter.emitAttribute(op.getLoc(), attr)))
      return failure();

    return success();
  };

  if (callOp.getTemplateArgs()) {
    os << "<";
    if (failed(
            interleaveCommaWithError(*callOp.getTemplateArgs(), os, emitArgs)))
      return failure();
    os << ">";
  }

  os << "(";

  LogicalResult emittedArgs =
      callOp.getArgs()
          ? interleaveCommaWithError(*callOp.getArgs(), os, emitArgs)
          : emitter.emitOperands(op);
  if (failed(emittedArgs))
    return failure();
  os << ")";
  return success();
}

static LogicalResult printOperation(CppEmitter &emitter,
                                    emitc::ApplyOp applyOp) {
  raw_ostream &os = emitter.ostream();
  Operation &op = *applyOp.getOperation();

  if (failed(emitter.emitAssignPrefix(op)))
    return failure();
  os << applyOp.getApplicableOperator();
  os << emitter.getOrCreateName(applyOp.getOperand());

  return success();
}

static LogicalResult printOperation(CppEmitter &emitter,
                                    emitc::IncludeOp includeOp) {
  raw_ostream &os = emitter.ostream();

  os << "#include ";
  if (includeOp.getIsStandardIncludeAttrName())
    os << "<" << includeOp.getIncludeAttrName() << ">";
  else
    os << "\"" << includeOp.getIncludeAttrName() << "\"";

  return success();
}

static LogicalResult printOperation(CppEmitter &emitter, scf::ForOp forOp) {

  raw_indented_ostream &os = emitter.ostream();

  OperandRange operands = forOp.getIterOperands();
  Block::BlockArgListType iterArgs = forOp.getRegionIterArgs();
  Operation::result_range results = forOp.getResults();

  if (!emitter.shouldDeclareVariablesAtTop()) {
    for (OpResult result : results) {
      if (failed(emitter.emitVariableDeclaration(result,
                                                 /*trailingSemicolon=*/true)))
        return failure();
    }
  }

  for (auto pair : llvm::zip(iterArgs, operands)) {
    if (failed(emitter.emitType(forOp.getLoc(), std::get<0>(pair).getType())))
      return failure();
    os << " " << emitter.getOrCreateName(std::get<0>(pair)) << " = ";
    os << emitter.getOrCreateName(std::get<1>(pair)) << ";";
    os << "\n";
  }

  os << "for (";
  if (failed(
          emitter.emitType(forOp.getLoc(), forOp.getInductionVar().getType())))
    return failure();
  os << " ";
  os << emitter.getOrCreateName(forOp.getInductionVar());
  os << " = ";
  os << emitter.getOrCreateName(forOp.getLowerBound());
  os << "; ";
  os << emitter.getOrCreateName(forOp.getInductionVar());
  os << " < ";
  os << emitter.getOrCreateName(forOp.getUpperBound());
  os << "; ";
  os << emitter.getOrCreateName(forOp.getInductionVar());
  os << " += ";
  os << emitter.getOrCreateName(forOp.getStep());
  os << ")\n";
  os << "chess_prepare_for_pipelining\n";
  // Try to find the upper bound and step of the for operator.
  // If the bounds are found, print them
  auto tc = getTripCount(forOp);
  if (tc.first) {
    auto step = getStep(forOp);
    int64_t lb =
        step.first && step.second > 0 ? floorDiv(tc.second, step.second) : 1;
    int64_t ub =
        step.first && step.second > 0 ? ceilDiv(tc.second, step.second) : 0;
    os << "chess_loop_range(";
    os << std::to_string(lb);
    os << ", ";
    if (step.first && step.second > 0)
      os << std::to_string(ub);
    os << ")\n";
  }
  os << "{\n";
  os.indent();

  Region &forRegion = forOp.getRegion();
  auto regionOps = forRegion.getOps();

  // We skip the trailing yield op because this updates the result variables
  // of the for op in the generated code. Instead we update the iterArgs at
  // the end of a loop iteration and set the result variables after the for
  // loop.
  for (auto it = regionOps.begin(); std::next(it) != regionOps.end(); ++it) {
    bool trailingSemicolon = !isa<scf::IfOp, scf::ForOp, cf::CondBranchOp>(*it);
    if (failed(emitter.emitOperation(*it, trailingSemicolon)))
      return failure();
  }

  Operation *yieldOp = forRegion.getBlocks().front().getTerminator();
  // Copy yield operands into iterArgs at the end of a loop iteration.
  for (auto pair : llvm::zip(iterArgs, yieldOp->getOperands())) {
    BlockArgument iterArg = std::get<0>(pair);
    Value operand = std::get<1>(pair);
    os << emitter.getOrCreateName(iterArg) << " = "
       << emitter.getOrCreateName(operand) << ";\n";
  }

  os.unindent() << "}";

  // Copy iterArgs into results after the for loop.
  for (auto pair : llvm::zip(results, iterArgs)) {
    OpResult result = std::get<0>(pair);
    BlockArgument iterArg = std::get<1>(pair);
    os << "\n"
       << emitter.getOrCreateName(result) << " = "
       << emitter.getOrCreateName(iterArg) << ";";
  }

  return success();
}

static LogicalResult printOperation(CppEmitter &emitter, scf::IfOp ifOp) {
  raw_indented_ostream &os = emitter.ostream();

  if (!emitter.shouldDeclareVariablesAtTop()) {
    for (OpResult result : ifOp.getResults()) {
      if (failed(emitter.emitVariableDeclaration(result,
                                                 /*trailingSemicolon=*/true)))
        return failure();
    }
  }

  os << "if (";
  if (failed(emitter.emitOperands(*ifOp.getOperation())))
    return failure();
  os << ") {\n";
  os.indent();

  Region &thenRegion = ifOp.getThenRegion();
  for (Operation &op : thenRegion.getOps()) {
    // Note: This prints a superfluous semicolon if the terminating yield op has
    // zero results.
    if (failed(emitter.emitOperation(op, /*trailingSemicolon=*/true)))
      return failure();
  }

  os.unindent() << "}";

  Region &elseRegion = ifOp.getElseRegion();
  if (!elseRegion.empty()) {
    os << " else {\n";
    os.indent();

    for (Operation &op : elseRegion.getOps()) {
      // Note: This prints a superfluous semicolon if the terminating yield op
      // has zero results.
      if (failed(emitter.emitOperation(op, /*trailingSemicolon=*/true)))
        return failure();
    }

    os.unindent() << "}";
  }

  return success();
}

static LogicalResult printOperation(CppEmitter &emitter, scf::YieldOp yieldOp) {
  raw_ostream &os = emitter.ostream();
  Operation &parentOp = *yieldOp.getOperation()->getParentOp();

  if (yieldOp.getNumOperands() != parentOp.getNumResults()) {
    return yieldOp.emitError("number of operands does not to match the number "
                             "of the parent op's results");
  }

  if (failed(interleaveWithError(
          llvm::zip(parentOp.getResults(), yieldOp.getOperands()),
          [&](auto pair) -> LogicalResult {
            auto result = std::get<0>(pair);
            auto operand = std::get<1>(pair);
            os << emitter.getOrCreateName(result) << " = ";

            if (!emitter.hasValueInScope(operand))
              return yieldOp.emitError("operand value not in scope");
            os << emitter.getOrCreateName(operand);
            return success();
          },
          [&]() { os << ";\n"; })))
    return failure();

  return success();
}

static LogicalResult printOperation(CppEmitter &emitter,
                                    func::ReturnOp returnOp) {
  raw_ostream &os = emitter.ostream();
  os << "return";
  switch (returnOp.getNumOperands()) {
  case 0:
    return success();
  case 1:
    os << " " << emitter.getOrCreateName(returnOp.getOperand(0));
    return success(emitter.hasValueInScope(returnOp.getOperand(0)));
  default:
    os << " std::make_tuple(";
    if (failed(emitter.emitOperandsAndAttributes(*returnOp.getOperation())))
      return failure();
    os << ")";
    return success();
  }
}

static LogicalResult printOperation(CppEmitter &emitter, ModuleOp moduleOp) {
  CppEmitter::Scope scope(emitter);

  for (Operation &op : moduleOp) {
    if (failed(emitter.emitOperation(op, /*trailingSemicolon=*/false)))
      return failure();
  }
  return success();
}

static LogicalResult printOperation(CppEmitter &emitter,
                                    func::FuncOp functionOp) {
  // We need to declare variables at top if the function has multiple blocks.
  if (!emitter.shouldDeclareVariablesAtTop() &&
      functionOp.getBlocks().size() > 1) {
    return functionOp.emitOpError(
        "with multiple blocks needs variables declared at top");
  }
  CppEmitter::Scope scope(emitter);

  // Find any memref dim op in the function, and parse the dimension of each
  // dynamic shaped memref
  if (failed(parseMemRefDynamicDims(emitter, functionOp)))
    return failure();

  raw_indented_ostream &os = emitter.ostream();
  if (failed(emitter.emitTypes(functionOp.getLoc(),
                               functionOp.getFunctionType().getResults())))
    return failure();
  os << " " << functionOp.getName();

  os << "(";
  if (functionOp.isDeclaration()) {
    if (failed(interleaveCommaWithError(
            functionOp.getArgumentTypes(), os, [&](Type type) -> LogicalResult {
              if (failed(emitter.emitType(functionOp.getLoc(), type)))
                return failure();
              // If it is a memref argument, we need to check if it has dynamic
              // shape. If so, the dimensions have to be printed out
              MemRefType argType = dyn_cast<MemRefType>(type);
              if (argType)
                for (unsigned dim = 0; dim < argType.getRank(); ++dim)
                  if (argType.isDynamicDim(dim))
                    os << ", size_t";
              return success();
            })))
      return failure();
    os << ");\n";
    return success();
  }

  if (failed(interleaveCommaWithError(
          functionOp.getArguments(), os,
          [&](BlockArgument arg) -> LogicalResult {
            if (failed(emitter.emitType(functionOp.getLoc(), arg.getType())))
              return failure();
            os << " " << emitter.getOrCreateName(arg);
            // If it is a memref argument, we need to check if it has dynamic
            // shape. If so, the dimensions have to be printed out
            if (failed(printMemRefDims(emitter, arg)))
              return failure();
            return success();
          })))
    return failure();

  os << ") {\n";
  os.indent();
  if (emitter.shouldDeclareVariablesAtTop()) {
    // Declare all variables that hold op results including those from nested
    // regions.
    WalkResult result =
        functionOp.walk<WalkOrder::PreOrder>([&](Operation *op) -> WalkResult {
          for (OpResult result : op->getResults()) {
            if (failed(emitter.emitVariableDeclaration(
                    result, /*trailingSemicolon=*/true))) {
              return WalkResult(
                  op->emitError("unable to declare result variable for op"));
            }
          }
          return WalkResult::advance();
        });
    if (result.wasInterrupted())
      return failure();
  }

  Region::BlockListType &blocks = functionOp.getBlocks();
  // Create label names for basic blocks.
  for (Block &block : blocks) {
    emitter.getOrCreateName(block);
  }

  // Declare variables for basic block arguments.
  for (auto it = std::next(blocks.begin()); it != blocks.end(); ++it) {
    Block &block = *it;
    for (BlockArgument &arg : block.getArguments()) {
      if (emitter.hasValueInScope(arg))
        return functionOp.emitOpError(" block argument #")
               << arg.getArgNumber() << " is out of scope";
      if (failed(
              emitter.emitType(block.getParentOp()->getLoc(), arg.getType()))) {
        return failure();
      }
      os << " " << emitter.getOrCreateName(arg) << ";\n";
    }
  }

  for (Block &block : blocks) {
    // Only print a label if there is more than one block.
    if (blocks.size() > 1) {
      if (failed(emitter.emitLabel(block)))
        return failure();
    }
    for (Operation &op : block.getOperations()) {
      // When generating code for an scf.if or std.cond_br op no semicolon needs
      // to be printed after the closing brace.
      // When generating code for an scf.for op, printing a trailing semicolon
      // is handled within the printOperation function.
      bool trailingSemicolon =
          !isa<scf::IfOp, scf::ForOp, cf::CondBranchOp>(op);

      if (failed(emitter.emitOperation(
              op, /*trailingSemicolon=*/trailingSemicolon)))
        return failure();
    }
  }
  os.unindent() << "}\n";
  return success();
}

CppEmitter::CppEmitter(raw_ostream &os, bool declareVariablesAtTop)
    : os(os), declareVariablesAtTop(declareVariablesAtTop) {
  valueInScopeCount.push(0);
  labelInScopeCount.push(0);
}

/// Return the existing or a new name for a Value.
StringRef CppEmitter::getOrCreateName(Value val, std::string prefix) {
  if (!valueMapper.count(val))
    valueMapper.insert(val,
                       formatv("{0}{1}", prefix, ++valueInScopeCount.top()));
  return *valueMapper.begin(val);
}

/// Set the name of a value to an existing name
void CppEmitter::setName(Value val, StringRef name) {
  valueMapper.insert(val, name.str());
}

/// Get a new name that is not associated with any value
std::string CppEmitter::getNewName(std::string prefix) {
  std::string ret = formatv("{0}{1}", prefix, ++valueInScopeCount.top());
  return ret;
}

/// Given a dynamic shaped memref, set its size at position 'index' to
// parameter 'result'
void CppEmitter::setMemRefDimParam(Value memref, unsigned index,
                                   std::string parameter) {
  auto p = std::make_pair(memref, index);
  assert(!paramIndexMapper.count(p) && "memref dimension already set");
  paramIndexMapper[p] = parameter;
}

/// Return the memref parameteric dimension size at given index
StringRef CppEmitter::getMemRefDimParam(Value memref, unsigned index) {
  auto p = std::make_pair(memref, index);
  assert(paramIndexMapper.count(p) && "memref dimension not found");
  return paramIndexMapper[p];
}

/// Return true if the specified dim of memref has a parameter
/// associated with it
bool CppEmitter::isMemRefDimParam(Value memref, unsigned index) {
  assert([&] {
    MemRefType type = memref.getType().dyn_cast<MemRefType>();
    if (!(type && type.isDynamicDim(index))) {
      printf("the dimension size at index is not dynamic\n");
      return false;
    }
    return true;
  }());

  auto p = std::make_pair(memref, index);
  return paramIndexMapper.count(p);
}

/// Return the existing or a new label for a Block.
StringRef CppEmitter::getOrCreateName(Block &block, std::string prefix) {
  if (!blockMapper.count(&block))
    blockMapper.insert(&block,
                       formatv("{0}{1}", prefix, ++labelInScopeCount.top()));
  return *blockMapper.begin(&block);
}

bool CppEmitter::shouldMapToUnsigned(IntegerType::SignednessSemantics val) {
  switch (val) {
  case IntegerType::Signless:
    return false;
  case IntegerType::Signed:
    return false;
  case IntegerType::Unsigned:
    return true;
  }
  llvm_unreachable("Unexpected IntegerType::SignednessSemantics");
}

bool CppEmitter::hasValueInScope(Value val) { return valueMapper.count(val); }

bool CppEmitter::hasBlockLabel(Block &block) {
  return blockMapper.count(&block);
}

// Check whether the int type dense value has a splat value and get the int
// value as a string.
template <typename ElTy>
static std::string getSplatValueOfIntDense(DenseIntElementsAttr dense) {
  ElTy splatVal = dense.getSplatValue<ElTy>();
  return std::to_string(splatVal);
}

// Get the first float value of a dense type value as a string.
static std::string getSplatValueOfFloatDense(DenseFPElementsAttr dense,
                                             bool isBFloat = false) {
  APFloat apFloat = dense.getSplatValue<APFloat>();
  float splatVal = apFloat.convertToFloat();
  std::string firstValue = std::to_string(splatVal);

  if (apFloat.isPosInfinity()) {
    if (isBFloat) {
      // TODO: Clean this up; emitting largest finite value in lieu of infinity;
      // system headers do not provide a simple way to initialize a bfloat16 to
      // infinity.
      firstValue = std::to_string(0x1.FEp+127f);
    } else {
      firstValue = std::to_string(std::numeric_limits<float>::max());
    }
  } else if (apFloat.isNegInfinity()) {
    if (isBFloat) {
      firstValue = std::to_string(-0x1.FEp+127f);
    } else {
      firstValue = std::to_string(std::numeric_limits<float>::lowest());
    }
  } else if (!apFloat.isNonZero()) {
    firstValue = "0";
  }

  return firstValue;
}

LogicalResult CppEmitter::emitAttribute(Location loc, Attribute attr) {
  auto printInt = [&](const APInt &val, bool isUnsigned) {
    if (val.getBitWidth() == 1) {
      if (val.getBoolValue())
        os << "true";
      else
        os << "false";
    } else {
      SmallString<128> strValue;
      val.toString(strValue, 10, !isUnsigned, false);
      os << strValue;
    }
  };

  auto printFloat = [&](const APFloat &val) {
    if (val.isFinite()) {
      SmallString<128> strValue;
      // Use default values of toString except don't truncate zeros.
      val.toString(strValue, 0, 0, false);
      switch (llvm::APFloatBase::SemanticsToEnum(val.getSemantics())) {
      case llvm::APFloatBase::S_IEEEsingle:
        os << "(float)";
        break;
      case llvm::APFloatBase::S_IEEEdouble:
        os << "(double)";
        break;
      default:
        break;
      };
      os << strValue;
    } else if (val.isNaN()) {
      os << "NAN";
    } else if (val.isInfinity()) {
      if (val.isNegative())
        os << "-";
      os << "INFINITY";
    }
  };

  // Print floating point attributes.
  if (auto fAttr = attr.dyn_cast<FloatAttr>()) {
    printFloat(fAttr.getValue());
    return success();
  }
  if (auto dense = attr.dyn_cast<DenseFPElementsAttr>()) {
    if (AIEML && dense.isSplat()) {
      if (auto vType = dense.getType().dyn_cast<VectorType>()) {
        if (auto fType = vType.getElementType().dyn_cast<FloatType>()) {
          unsigned width = fType.getWidth();
          std::string splatValue = "";
          if (width == 32) {
            splatValue = getSplatValueOfFloatDense(dense);
          } else if (width == 16) {
            splatValue = getSplatValueOfFloatDense(dense, /*isBFloat*/ true);
          }
          if (width == 32 || (width == 16 && getVectorLaneSize(vType) == 32)) {
            if (splatValue == "0") {
              os << "broadcast_zero_";
              if (failed(emitType(loc, fType)))
                return failure();
              os << "()";
            } else {
              os << "broadcast_to_";
              if (failed(emitType(loc, vType)))
                return failure();
              os << "((";
              if (failed(emitType(loc, fType)))
                return failure();
              os << ")";
              os << splatValue;
              os << ")";
            }
          } else if (width == 16 && getVectorLaneSize(vType) == 16) {
            os << "extract_v16bfloat16(";
            if (splatValue == "0") {
              os << "broadcast_zero_bfloat16()";
            } else {
              os << "broadcast_to_v32bfloat16";
              os << "((";
              if (failed(emitType(loc, fType)))
                return failure();
              os << ")";
              os << splatValue;
              os << ")";
            }
            os << ", 0)";
          }
        }
      }
      // TODO: Deal with multiple dense value case for AIEML.
    } else {
      os << '{';
      interleaveComma(dense, os, [&](const APFloat &val) { printFloat(val); });
      os << '}';
    }
    return success();
  }

  // Print integer attributes.
  if (auto iAttr = attr.dyn_cast<IntegerAttr>()) {
    if (auto iType = iAttr.getType().dyn_cast<IntegerType>()) {
      printInt(iAttr.getValue(), shouldMapToUnsigned(iType.getSignedness()));
      return success();
    }
    if (auto iType = iAttr.getType().dyn_cast<IndexType>()) {
      printInt(iAttr.getValue(), false);
      return success();
    }
  }
  if (auto dense = attr.dyn_cast<DenseIntElementsAttr>()) {
    if (auto tType = dense.getType().dyn_cast<TensorType>()) {
      if (auto iType = tType.getElementType().dyn_cast<IntegerType>()) {
        os << '{';
        interleaveComma(dense, os, [&](const APInt &val) {
          printInt(val, shouldMapToUnsigned(iType.getSignedness()));
        });
        os << '}';
        return success();
      }
      if (auto iType = tType.getElementType().dyn_cast<IndexType>()) {
        os << '{';
        interleaveComma(dense, os,
                        [&](const APInt &val) { printInt(val, false); });
        os << '}';
        return success();
      }
    }
    if (auto vType = dense.getType().dyn_cast<VectorType>()) {
      if (auto iType = vType.getElementType().dyn_cast<IntegerType>()) {
        unsigned width = iType.getWidth();
        if (llvm::all_of(dense, [](const APInt &val) { return val == 0; })) {
          if (AIEML) {
            if (width * getVectorLaneSize(vType) == 1024) {
              os << "concat(broadcast_zero_s" << width << "(), broadcast_zero_s"
                 << width << "())";
              return success();
            }
            os << "broadcast_zero_s";
            os << width;
          } else {
            os << "null_";
            if (failed(emitType(loc, vType)))
              return failure();
          }
          os << "()";
          return success();
        }

        if (AIEML && dense.isSplat()) {
          std::string splatValue = "";
          if (width == 32) {
            splatValue = getSplatValueOfIntDense<int32_t>(dense);
          } else if (width == 16) {
            splatValue = getSplatValueOfIntDense<int16_t>(dense);
          } else if (width == 8) {
            splatValue = getSplatValueOfIntDense<int8_t>(dense);
          }
          os << "broadcast_to_";
          if (failed(emitType(loc, vType)))
            return failure();
          os << "((";
          if (failed(emitType(loc, iType)))
            return failure();
          os << ")";
          os << splatValue;
          os << ")";
          // TODO: Handle multiple dense value case in AIEML.
        } else {
          os << '{';
          interleaveComma(dense, os, [&](const APInt &val) {
            printInt(val, shouldMapToUnsigned(iType.getSignedness()));
          });
          os << '}';
        }
        return success();
      }
      if (auto iType = vType.getElementType().dyn_cast<IndexType>()) {
        os << '{';
        interleaveComma(dense, os,
                        [&](const APInt &val) { printInt(val, false); });
        os << '}';
        return success();
      }
    }
  }

  // Print opaque attributes.
  if (auto oAttr = attr.dyn_cast<emitc::OpaqueAttr>()) {
    os << oAttr.getValue();
    return success();
  }

  // Print symbolic reference attributes.
  if (auto sAttr = attr.dyn_cast<SymbolRefAttr>()) {
    if (sAttr.getNestedReferences().size() > 1)
      return emitError(loc, "attribute has more than 1 nested reference");
    os << sAttr.getRootReference().getValue();
    return success();
  }

  // Print type attributes.
  if (auto type = attr.dyn_cast<TypeAttr>())
    return emitType(loc, type.getValue());

  return emitError(loc, "cannot emit attribute of type ") << attr;
}

LogicalResult CppEmitter::emitOperands(Operation &op) {
  auto emitOperandName = [&](Value result) -> LogicalResult {
    if (!hasValueInScope(result))
      return op.emitOpError() << "operand value not in scope";
    os << getOrCreateName(result);
    return success();
  };
  return interleaveCommaWithError(op.getOperands(), os, emitOperandName);
}

LogicalResult
CppEmitter::emitOperandsAndAttributes(Operation &op,
                                      ArrayRef<StringRef> exclude) {
  if (failed(emitOperands(op)))
    return failure();
  // Insert comma in between operands and non-filtered attributes if needed.
  if (op.getNumOperands() > 0) {
    for (NamedAttribute attr : op.getAttrs()) {
      if (!llvm::is_contained(exclude, attr.getName().strref())) {
        os << ", ";
        break;
      }
    }
  }
  // Emit attributes.
  auto emitNamedAttribute = [&](NamedAttribute attr) -> LogicalResult {
    if (llvm::is_contained(exclude, attr.getName().strref()))
      return success();
    os << "/* " << attr.getName().getValue() << " */";
    if (failed(emitAttribute(op.getLoc(), attr.getValue())))
      return failure();
    return success();
  };
  return interleaveCommaWithError(op.getAttrs(), os, emitNamedAttribute);
}

LogicalResult CppEmitter::emitVariableAssignment(OpResult result) {
  if (!hasValueInScope(result)) {
    return result.getDefiningOp()->emitOpError(
        "result variable for the operation has not been declared");
  }
  os << getOrCreateName(result) << " = ";
  return success();
}

LogicalResult CppEmitter::emitVariableDeclaration(OpResult result,
                                                  bool trailingSemicolon,
                                                  bool isAcc) {
  if (hasValueInScope(result)) {
    return result.getDefiningOp()->emitError(
        "result variable for the operation already declared");
  }
  if (failed(
          emitType(result.getOwner()->getLoc(), result.getType(), true, isAcc)))
    return failure();
  os << " " << getOrCreateName(result);
  if (trailingSemicolon)
    os << ";\n";
  return success();
}

LogicalResult CppEmitter::emitAssignPrefix(Operation &op, bool isAcc) {
  switch (op.getNumResults()) {
  case 0:
    break;
  case 1: {
    OpResult result = op.getResult(0);
    if (shouldDeclareVariablesAtTop()) {
      if (failed(emitVariableAssignment(result)))
        return failure();
    } else {
      if (failed(emitVariableDeclaration(result, /*trailingSemicolon=*/false,
                                         isAcc)))
        return failure();
      os << " = ";
    }
    break;
  }
  default:
    if (!shouldDeclareVariablesAtTop()) {
      for (OpResult result : op.getResults()) {
        if (failed(emitVariableDeclaration(result, /*trailingSemicolon=*/true)))
          return failure();
      }
    }
    os << "std::tie(";
    interleaveComma(op.getResults(), os,
                    [&](Value result) { os << getOrCreateName(result); });
    os << ") = ";
  }
  return success();
}

LogicalResult CppEmitter::emitLabel(Block &block) {
  if (!hasBlockLabel(block))
    return block.getParentOp()->emitError("label for block not found");
  // FIXME: Add feature in `raw_indented_ostream` to ignore indent for block
  // label instead of using `getOStream`.
  os.getOStream() << getOrCreateName(block) << ":\n";
  return success();
}

LogicalResult CppEmitter::emitOperation(Operation &op, bool trailingSemicolon) {
  // Some operations in AIE become nops. Check if this operation must be skipped
  // from codegen
  if (skippedOp(&op, *this))
    return success();

  LogicalResult status =
      llvm::TypeSwitch<Operation *, LogicalResult>(&op)
          // EmitC ops.
          .Case<emitc::ApplyOp, emitc::CallOp, emitc::ConstantOp,
                emitc::IncludeOp>(
              [&](auto op) { return printOperation(*this, op); })
          // SCF ops.
          .Case<scf::ForOp, scf::IfOp, scf::YieldOp>(
              [&](auto op) { return printOperation(*this, op); })
          // Standard ops.
          .Case<cf::BranchOp, func::CallOp, cf::CondBranchOp, func::FuncOp,
                ModuleOp, func::ReturnOp>(
              [&](auto op) { return printOperation(*this, op); })
          // Arith ops.
          .Case<arith::ConstantOp>(
              [&](auto op) { return printOperation(*this, op); })
          // Extra ops added for AIE
          //  Arith ops.
          .Case<arith::AddIOp>(
              [&](auto op) { return printOperation<arith::AddIOp>(*this, op); })
          // Vector ops.
          .Case<vector::TransferWriteOp>(
              [&](auto op) { return printOperation(*this, op); })
          // Memref ops.
          .Case<memref::StoreOp>(
              [&](auto op) { return printOperation(*this, op); })
          .Case<aievec::AddOp, aievec::AddElemOp, aievec::ConcatOp,
                aievec::ExtOp, aievec::FMAOp, aievec::MulOp, aievec::PackOp,
                aievec::SelectOp, aievec::SRSOp, aievec::SubOp,
                aievec::SubElemOp, aievec::UPDOp, aievec::UPSOp,
                aievec::FMAElemOp, aievec::MulElemOp, aievec::BroadcastOp,
                aievec::BroadcastScalarOp, aievec::MulConvOp, aievec::FMAConvOp,
                aievec::ShiftOp, aievec::ShuffleOp, aievec::CastOp,
                aievec::MinOp, aievec::MaxOp, aievec::CmpOp, aievec::SelOp,
                aievec::ExtElemOp>(
              [&](auto op) { return printOperation(*this, op); })
          .Default([&](Operation *) {
            return op.emitOpError("unable to find printer for op");
          });

  if (failed(status))
    return failure();
  os << (trailingSemicolon ? ";\n" : "\n");
  return success();
}

LogicalResult CppEmitter::emitType(Location loc, Type type, bool stdintType,
                                   bool isAcc) {
  if (auto iType = type.dyn_cast<IntegerType>()) {
    switch (iType.getWidth()) {
    case 1:
      return (os << "bool"), success();
    case 8:
    case 16:
    case 32:
    case 64:
      if (shouldMapToUnsigned(iType.getSignedness()))
        return (os << "uint" << iType.getWidth() << (stdintType ? "_t" : "")),
               success();
      else
        return (os << "int" << iType.getWidth() << (stdintType ? "_t" : "")),
               success();
    case 48:
    case 80:
      return (os << "acc" << iType.getWidth()), success();
    default:
      return emitError(loc, "cannot emit integer type ") << type;
    }
  }
  if (auto fType = type.dyn_cast<FloatType>()) {
    switch (fType.getWidth()) {
    case 16:
      return (os << "bfloat16"), success();
    case 32:
      return (os << "float"), success();
    case 64:
      return (os << "double"), success();
    default:
      return emitError(loc, "cannot emit float type ") << type;
    }
  }
  if (auto iType = type.dyn_cast<IndexType>())
    return (os << "size_t"), success();

  if (auto tType = type.dyn_cast<TensorType>()) {
    if (!tType.hasRank())
      return emitError(loc, "cannot emit unranked tensor type");
    if (!tType.hasStaticShape())
      return emitError(loc, "cannot emit tensor type with non static shape");
    os << "Tensor<";
    if (failed(emitType(loc, tType.getElementType())))
      return failure();
    auto shape = tType.getShape();
    for (auto dimSize : shape) {
      os << ", ";
      os << dimSize;
    }
    os << ">";
    return success();
  }
  if (auto tType = type.dyn_cast<TupleType>())
    return emitTupleType(loc, tType.getTypes());
  if (auto oType = type.dyn_cast<emitc::OpaqueType>()) {
    os << oType.getValue();
    return success();
  }
  // Types added for AIE
  // MemRefType: printed as 'eltType'*
  if (auto tType = type.dyn_cast<MemRefType>()) {
    if (failed(emitType(loc, tType.getElementType())))
      return failure();
    os << " * restrict";
    return success();
  }
  // VectorType: printed as v'lane''eltType'
  if (auto tType = type.dyn_cast<VectorType>()) {
    Type eltType = tType.getElementType();
    if (tType.getRank() != 1)
      return failure();

    unsigned dimSize = tType.getDimSize(tType.getRank() - 1);

    if (eltType.isa<IntegerType>()) {
      os << "v" << std::to_string(dimSize);
      auto iType = eltType.cast<IntegerType>();
      unsigned width = iType.getWidth();
      if ((dimSize == 16 && width == 64) || (dimSize == 32 && width == 32)) {
        if (isAcc) {
          return (os << "acc" << width), success();
        } else {
          return (os << "int" << width), success();
        }
      }
    } else if (eltType.isa<FloatType>()) {
      if (AIEML) {
        if (isAcc) {
          return (os << "v16accfloat"), success();
        } else {
          auto fType = eltType.cast<FloatType>();
          unsigned width = fType.getWidth();
          if (width == 16) {
            return (os << "v" << std::to_string(dimSize) << "bfloat16"),
                   success();
          } else {
            return (os << "v" << std::to_string(dimSize) << "float"), success();
          }
        }
      } else {
        os << "v" << std::to_string(dimSize);
      }
    }

    if (failed(emitType(loc, eltType, false)))
      return failure();
    return success();
  }
  return emitError(loc, "cannot emit type ") << type;
}

LogicalResult CppEmitter::emitTypes(Location loc, ArrayRef<Type> types) {
  switch (types.size()) {
  case 0:
    os << "void";
    return success();
  case 1:
    return emitType(loc, types.front());
  default:
    return emitTupleType(loc, types);
  }
}

LogicalResult CppEmitter::emitTupleType(Location loc, ArrayRef<Type> types) {
  os << "std::tuple<";
  if (failed(interleaveCommaWithError(
          types, os, [&](Type type) { return emitType(loc, type); })))
    return failure();
  os << ">";
  return success();
}

LogicalResult aievec::translateAIEVecToCpp(Operation *op, raw_ostream &os) {
  CppEmitter emitter(os, false);
  return emitter.emitOperation(*op, /*trailingSemicolon=*/false);
}
