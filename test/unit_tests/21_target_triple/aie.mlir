//===- aie.mlir ------------------------------------------------*- MLIR -*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2021 Xilinx Inc.
//
//===----------------------------------------------------------------------===//

// RUN: aiecc.py %VitisSysrootFlag% --host-target=%aieHostTargetTriplet% %s -I%aie_runtime_lib%/test_lib/include %extraAieCcFlags% -L%aie_runtime_lib%/test_lib/lib -ltest_lib %S/test.cpp -o test.elf
// RUN: %run_on_board ./test.elf

// When this IR is lowered to LLVM, the affine.load operation resulting "%val0"
// is converted to the following LLVM IR due to the "x86_64-unknown-linux-gnu"
// target triple:
//    %1 = load i32, i32* @b, align 4
//    %3 = insertelement <4 x i32> poison, i32 %1, i32 0
//    %4 = shufflevector <4 x i32> %3, <4 x i32> poison, <4 x i32> zeroinitializer
//
// However, the "poison" value cannot be properly handled by xchesscc. This test
// is intended to test whether the poison->undef replacement in aiecc works
// correctly.

module attributes {llvm.target_triple = "x86_64-unknown-linux-gnu"}  {
  %tile32 = AIE.tile(1, 3)

  %buf_a = AIE.buffer(%tile32) {sym_name = "a"} : memref<16xi32>
  %buf_b = AIE.buffer(%tile32) {sym_name = "b"} : memref<i32>

  %core32 = AIE.core(%tile32)  {
    %val0 = affine.load %buf_b[] : memref<i32>
    affine.for %arg0 = 0 to 16 {
      %val1 = affine.load %buf_a[%arg0] : memref<16xi32>
      %val2 = arith.addi %val0, %val1 : i32
      affine.store %val2, %buf_a[%arg0] : memref<16xi32>
    }
    AIE.end
  }
}
