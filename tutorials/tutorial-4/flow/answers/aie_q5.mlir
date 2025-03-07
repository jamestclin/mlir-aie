//===- aie.mlir ------------------------------------------------*- MLIR -*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Copyright (C) 2022, Advanced Micro Devices, Inc.
//
//===----------------------------------------------------------------------===//

// REQUIRES: valid_xchess_license
// XFAIL: *
// RUN: aiecc.py -j4 --sysroot=%VITIS_SYSROOT% --host-target=aarch64-linux-gnu %s -I%aie_runtime_lib%/ %extraAieCcFlags% %aie_runtime_lib%/test_library.cpp %S/../test.cpp -o tutorial-4.exe
// RUN: %run_on_board ./tutorial-4.exe


// Declare this MLIR module. A wrapper that can contain all 
// AIE tiles, buffers, and data movement
module @tutorial_4 {

    // 2 tiles in row 4 (col 1 and col 3)
    // even rows have local memory to its left
    %tile14 = AIE.tile(1, 4) 
    %tile24 = AIE.tile(2, 4) // TODO Declare dummy tile for manual routing
    %tile34 = AIE.tile(3, 4)

    // Declare local memory of tile(1,4) and tile (3,4) which are not shared
    %buf14 = AIE.buffer(%tile14) { sym_name = "a14" } : memref<256xi32>
    %buf34 = AIE.buffer(%tile34) { sym_name = "a34" } : memref<256xi32>

    // Declare local locks for tile(1,4) and tile(3,4) giving new
    // unique lock ID values 6 and 7
    %lock14_6 = AIE.lock(%tile14, 6) { sym_name = "lock_a14_6" }
    %lock34_7 = AIE.lock(%tile34, 7) { sym_name = "lock_a34_7" }

    // Connect DMA channel 0 on tile(1,4) to DMA channel 1 in tile(3,4)
    // with automatic shortest distance routing
    AIE.flow(%tile14, DMA: 0, %tile34, DMA:1)

    // Define core algorithm for tile(1,4)
    // buf[3] = 14
    %core14 = AIE.core(%tile14) {
        // Locks init value is Release 0, so this will always succeed first
        AIE.useLock(%lock14_6, "Acquire", 0)

		%val = arith.constant 14 : i32 
		%idx = arith.constant 3 : index 
		memref.store %val, %buf14[%idx] : memref<256xi32> 

        // Release lock to 1 so tile(2,4) can acquire and begin processing
        AIE.useLock(%lock14_6, "Release", 1)
        AIE.end
    }

    %mem14 = AIE.mem(%tile14) {
        AIE.dmaStart("MM2S", 0, ^bd0, ^end)
        ^bd0:
            AIE.useLock(%lock14_6, Acquire, 1)
            AIE.dmaBd(<%buf14 : memref<256xi32>, 0, 256>, 0)
            AIE.useLock(%lock14_6, Release, 0)
            AIE.nextBd ^bd0
        ^end:
            AIE.end
    }    

 
    // Define core algorithm for tile(3,4) which reads value set by tile(1,4)
    // buf[5] = buf[3] + 100
    %core34 = AIE.core(%tile34) {
        // This acquire will stall since locks are initialized to Release, 0
        AIE.useLock(%lock34_7, "Acquire", 1)

        %idx1 = arith.constant 3 : index
        %d1   = memref.load %buf34[%idx1] : memref<256xi32>
        %c1   = arith.constant 100 : i32 
        %d2   = arith.addi %d1, %c1 : i32
		%idx2 = arith.constant 5 : index
		memref.store %d2, %buf34[%idx2] : memref<256xi32> 

        // This release doesn't do much in our example but mimics ping-pong
        AIE.useLock(%lock34_7, "Release", 0)
        AIE.end
    }

    // Define local tile memory behavior (i.e. tileDMA)
    %mem34 = AIE.mem(%tile34) {
        // sequence of DMAs declaration and buffer descriptors (bd)
        // ^bd0 - first label/ bd definition to set
        // ^end - next label/ bd definition to set 
        // (here, that is AIE.end to indicate no more)
        AIE.dmaStart("S2MM", 1, ^bd0, ^end) 
        ^bd0:
            // Add locks behvaior around bd definition
            AIE.useLock(%lock34_7, Acquire, 0)
            // bd definition
            // %buf34 - local buffer
            // 0   - offset of transfer
            // 256 - length of transfer
            // 0   - A/B mode enable (default is disabled)
            AIE.dmaBd(<%buf34 : memref<256xi32>, 0, 256>, 0)
            AIE.useLock(%lock34_7, Release, 1)
            AIE.nextBd ^bd0
        ^end:
            AIE.end
    }    

}
