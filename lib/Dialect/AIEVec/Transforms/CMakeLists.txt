#
# This file is licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#
# (c) Copyright 2022 Xilinx Inc.

add_mlir_dialect_library(MLIRAIEVecTransforms
  IntervalReuse.cpp
  AIEVectorize.cpp
  ConvertVectorToAIEVec.cpp
  VectorToVectorConversions.cpp
  VectorToAIEVecConversions.cpp
  AIEVecOptimizations.cpp
  FoldMulAddChainToConvOp.cpp

  ADDITIONAL_HEADER_DIRS
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../include/aie/Dialect/AIEVec/Transforms

  DEPENDS
  MLIRAIEVecPassIncGen

  LINK_LIBS PUBLIC
  MLIRIR
  MLIRPass
  )
