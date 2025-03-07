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
// RUN: aiecc.py %VitisSysrootFlag% --host-target=%aieHostTargetTriplet% %s -I%aie_runtime_lib%/test_lib/include %extraAieCcFlags% -L%aie_runtime_lib%/test_lib/lib -ltest_lib %S/test.cpp -o tutorial-7.exe
// RUN: %run_on_board ./tutorial-7.exe


// Declare this MLIR module. A wrapper that can contain all 
// AIE tiles, buffers, and data movement
module @tutorial_7 {

    // 2 tiles in row 4 (col 1 and col 3) and 1 in row 5 (col 3)
    // even rows have local memory to its left
    // odd rows have local memory to its right
    %tile14 = AIE.tile(1, 4) 
    %tile34 = AIE.tile(3, 4)
    %tile35 = AIE.tile(3, 5)

    // Declare local memory of tile(1,4), tile(3,4) and tile(3,5) which are not shared
    %buf14 = AIE.buffer(%tile14) { sym_name = "a14" } : memref<256xi32>
    %buf34 = AIE.buffer(%tile34) { sym_name = "a34" } : memref<256xi32>
    %buf35 = AIE.buffer(%tile35) { sym_name = "a35" } : memref<256xi32>

    // Declare local locks for tile(1,4), tile(3,4) and tile(3,5) giving new
    // unique lock ID values 6 and 7
    %lock14_6 = AIE.lock(%tile14, 6) { sym_name = "lock_a14_6" }
    %lock34_7 = AIE.lock(%tile34, 7) { sym_name = "lock_a34_7" }
    %lock35_7 = AIE.lock(%tile35, 7) { sym_name = "lock_a35_7" }

    // These locks will be used to gate when our end cores are done
    %lock34_8 = AIE.lock(%tile34, 8) { sym_name = "lock_a34_8" }
    %lock35_8 = AIE.lock(%tile35, 8) { sym_name = "lock_a35_8" }

    // Broadcast DMA channel 0 on tile(1,4) to both DMA channel 1 in 
    // tile(3,4) and tile(3,5) with automatic shortest distance routing for
    // packets (ID=0xD). Additional routes can be defined for each 
    // unique AIE.bp_id ID value by sepcifying their definitions in a new
    // AIE.bp_id(newID) { AIE.bp_dest routes ... } within the 
    // AIE.broadcast_packet block.
    // NOTE: By default, packet header are dropped at destination
    AIEX.broadcast_packet(%tile14, DMA: 0) {
      AIEX.bp_id(0xD) {
        AIEX.bp_dest<%tile34, DMA: 1>
        AIEX.bp_dest<%tile35, DMA: 1>
      }
    }

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
            // Insert header for packet routing
            // 0x4 - packet type, arbitary value
            // 0xD - packet ID, arbitary value but used for routing
            AIE.dmaBdPacket(0x4, 0xD) 
            AIE.dmaBd(<%buf14 : memref<256xi32>, 0, 256>, 0)
            AIE.useLock(%lock14_6, Release, 0)
            AIE.nextBd ^end
        ^end:
            AIE.end
    }    

 
    // Define core algorithm for tile(3,4) which reads value set by tile(1,4)
    // buf[5] = buf[3] + 100
    %core34 = AIE.core(%tile34) {
        // This acquire succeeds when the core is enabled
        AIE.useLock(%lock34_8, "Acquire", 0)
        // This acquire will stall since locks are initialized to Release, 0
        AIE.useLock(%lock34_7, "Acquire", 1)

        %idx1 = arith.constant 3 : index
        %d1   = memref.load %buf34[%idx1] : memref<256xi32>
        %c1   = arith.constant 100 : i32 
        %d2   = arith.addi %d1, %c1 : i32
		%idx2 = arith.constant 5 : index
		memref.store %d2, %buf34[%idx2] : memref<256xi32> 

        AIE.useLock(%lock34_7, "Release", 0)
        // This release means our 2nd core is done
        AIE.useLock(%lock34_8, "Release", 1)
        AIE.end
    }

    // Define local tile memory behavior (i.e. tileDMA)
    %mem34 = AIE.mem(%tile34) {
        AIE.dmaStart("S2MM", 1, ^bd0, ^end) 
        ^bd0:
            AIE.useLock(%lock34_7, Acquire, 0)
            // Packets headers are dropped so no need to define packet behavior here
            AIE.dmaBd(<%buf34 : memref<256xi32>, 0, 256>, 0)
            AIE.useLock(%lock34_7, Release, 1)
            AIE.nextBd ^end
        ^end:
            AIE.end
    }    


    // Define core algorithm for tile(3,5) which reads value set by tile(1,4)
    // buf[5] = buf[3] + 100
    %core35 = AIE.core(%tile35) {
        // This acquire succeeds when the core is enabled
        AIE.useLock(%lock35_8, "Acquire", 0)
        // This acquire will stall since locks are initialized to Release, 0
        AIE.useLock(%lock35_7, "Acquire", 1)

        %idx1 = arith.constant 3 : index
        %d1   = memref.load %buf35[%idx1] : memref<256xi32>
        %c1   = arith.constant 100 : i32 
        %d2   = arith.addi %d1, %c1 : i32
		%idx2 = arith.constant 5 : index
		memref.store %d2, %buf35[%idx2] : memref<256xi32> 

        AIE.useLock(%lock35_7, "Release", 0)
        // This release means our 2nd core is done
        AIE.useLock(%lock35_8, "Release", 1)
        AIE.end
    }

    // Define local tile memory behavior (i.e. tileDMA)
    %mem35 = AIE.mem(%tile35) {
        AIE.dmaStart("S2MM", 1, ^bd0, ^end) 
        ^bd0:
            AIE.useLock(%lock35_7, Acquire, 0)
            // Packets headers are dropped so no need to define packet behavior here
            AIE.dmaBd(<%buf35 : memref<256xi32>, 0, 256>, 0)
            AIE.useLock(%lock35_7, Release, 1)
            AIE.nextBd ^end
        ^end:
            AIE.end
    }    

}
