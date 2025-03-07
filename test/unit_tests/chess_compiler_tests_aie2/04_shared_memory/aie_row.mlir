//===- aie_row.mlir --------------------------------------------*- MLIR -*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2021 Xilinx Inc.
// (c) Copyright 2023 Advanced Micro Devices, Inc.
//
//===----------------------------------------------------------------------===//

// RUN: aiecc.py --aiesim --xchesscc --xbridge %VitisSysrootFlag% --host-target=%aieHostTargetTriplet% %s -I%aie_runtime_lib%/test_lib/include %extraAieCcFlags% %S/test.cpp -o test.elf -L%aie_runtime_lib%/test_lib/lib -ltest_lib
// RUN: %run_on_board ./test.elf
// RUN: aie_row.mlir.prj/aiesim.sh | FileCheck %s

// CHECK: AIE2 ISS
// CHECK: test start.
// CHECK: PASS!

// XFAIL: *

module @test04_shared_memory {
  AIE.device(xcve2802) {
    %tile13 = AIE.tile(1, 3)
    %tile23 = AIE.tile(2, 3)

    %buf13_0 = AIE.buffer(%tile13) { sym_name = "a" } : memref<256xi32>
    %buf13_1 = AIE.buffer(%tile13) { sym_name = "b" } : memref<256xi32>
    %buf23_0 = AIE.buffer(%tile23) { sym_name = "c" } : memref<256xi32>

    %lock13_2 = AIE.lock(%tile13, 2) { sym_name = "test_lock" } // test lock

    %lock13_3 = AIE.lock(%tile13, 3) { sym_name = "input_write_lock", init = 1 : i32 } // input buffer lock
    %lock13_4 = AIE.lock(%tile13, 4) { sym_name = "input_read_lock" } // input buffer lock
    %lock13_5 = AIE.lock(%tile13, 5) { sym_name = "hidden_write_lock", init = 1 : i32 } // interbuffer lock
    %lock13_6 = AIE.lock(%tile13, 6) { sym_name = "hidden_read_lock" } // interbuffer lock
    %lock23_7 = AIE.lock(%tile23, 7) { sym_name = "output_write_lock", init = 1 : i32 } // output buffer lock
    %lock23_8 = AIE.lock(%tile23, 8) { sym_name = "output_read_lock" } // output buffer lock

    %core13 = AIE.core(%tile13) {
      AIE.useLock(%lock13_4, AcquireGreaterEqual, 1) // acquire input for read(e.g. input ping)
      AIE.useLock(%lock13_5, AcquireGreaterEqual, 1) // acquire input for write
      %idx1 = arith.constant 3 : index
      %val1 = memref.load %buf13_0[%idx1] : memref<256xi32>
      %2    = arith.addi %val1, %val1 : i32
      %3 = arith.addi %2, %val1 : i32
      %4 = arith.addi %3, %val1 : i32
      %5 = arith.addi %4, %val1 : i32
      %idx2 = arith.constant 5 : index
      memref.store %5, %buf13_1[%idx2] : memref<256xi32>
      AIE.useLock(%lock13_3, Release, 1) // release input for write
      AIE.useLock(%lock13_6, Release, 1) // release output for read
      AIE.end
    }

    %core23 = AIE.core(%tile23) {
      AIE.useLock(%lock13_6, AcquireGreaterEqual, 1) // acquire input for read(e.g. input ping)
      AIE.useLock(%lock23_7, AcquireGreaterEqual, 1) // acquire output for write
      %idx1 = arith.constant 5 : index
      %val1 = memref.load %buf13_1[%idx1] : memref<256xi32>
      %2    = arith.addi %val1, %val1 : i32
      %3 = arith.addi %2, %val1 : i32
      %4 = arith.addi %3, %val1 : i32
      %5 = arith.addi %4, %val1 : i32
      %idx2 = arith.constant 5 : index
      memref.store %5, %buf23_0[%idx2] : memref<256xi32>
      AIE.useLock(%lock13_5, Release, 1) // release input for write
      AIE.useLock(%lock23_8, Release, 1) // release output for read
      AIE.end
    }
  }
}
