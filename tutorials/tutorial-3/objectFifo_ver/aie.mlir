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
// RUN: aiecc.py %VitisSysrootFlag% --host-target=%aieHostTargetTriplet% %s -I%aie_runtime_lib%/test_lib/include %extraAieCcFlags% -L%aie_runtime_lib%/test_lib/lib -ltest_lib %S/test.cpp -o tutorial-3.exe
// RUN: %run_on_board ./tutorial-3.exe


// Declare this MLIR module. A wrapper that can contain all 
// AIE tiles, buffers, and data movement
module @tutorial_3 {

    // 2 tiles in row 4 (col 1 and col 2)
    // even rows have local memory to its left
    %tile14 = AIE.tile(1, 4) 
    %tile24 = AIE.tile(2, 4)

    // Declare an object FIFO between the producer tile (1,4) and consumer tile (2,4).
    // The size of the object FIFO, i.e. its number of elements, is 1.
    // Objects, i.e. allocated memory elements, have type memref<256xi32>
    // These tiles share memory between them.
    %objFifo = AIE.objectFifo.createObjectFifo(%tile14, {%tile24}, 1 : i32) {sym_name = "of"} : !AIE.objectFifo<memref<256xi32>>

    // This lock will be used to gate when our 2nd core is done
    %lock24_2 = AIE.lock(%tile24, 2) { sym_name = "lock_a24_2" }

    // Define core algorithm for tile(1,4)
    // buf[3] = 14
    %core14 = AIE.core(%tile14) {
        // Acquire a subview with one object from the object FIFO.
        // This is equivalent to acquiring an AIE lock before accessing an AIE buffer.
        // This core acquires objects as a Producer: this impacts the acquire value of the lock 
        // that is generated through the object FIFO lowering.
        %inputSubview = AIE.objectFifo.acquire<Produce>(%objFifo : !AIE.objectFifo<memref<256xi32>>, 1) : !AIE.objectFifoSubview<memref<256xi32>>
        
        // Access the first, and only, element of the subview.
        %input = AIE.objectFifo.subview.access %inputSubview[0] : !AIE.objectFifoSubview<memref<256xi32>> -> memref<256xi32>

        %val = arith.constant 14 : i32 
        %idx = arith.constant 3 : index 
        memref.store %val, %input[%idx] : memref<256xi32>
        
        // Release the previously acquired object.
        // This is equivalent to releasing an AIE lock after accessing an AIE buffer.
        // This core releases objects as a Producer: this impacts the release value of the lock 
        // that is generated through the object FIFO lowering.
        AIE.objectFifo.release<Produce>(%objFifo : !AIE.objectFifo<memref<256xi32>>, 1)
        AIE.end
    }

    // Define core algorithm for tile(2,4) which reads value set by tile(1,4)
    // buf[5] = buf[3] + 100
    %core24 = AIE.core(%tile24) {
        // This acquire succeeds when the core is enabled
        AIE.useLock(%lock24_2, "Acquire", 0)

        %inputSubview = AIE.objectFifo.acquire<Consume>(%objFifo : !AIE.objectFifo<memref<256xi32>>, 1) : !AIE.objectFifoSubview<memref<256xi32>>
        %input = AIE.objectFifo.subview.access %inputSubview[0] : !AIE.objectFifoSubview<memref<256xi32>> -> memref<256xi32>

        %idx1 = arith.constant 3 : index
        %d1   = memref.load %input[%idx1] : memref<256xi32>
        %c1   = arith.constant 100 : i32 
        %d2   = arith.addi %d1, %c1 : i32
        %idx2 = arith.constant 5 : index
        memref.store %d2, %input[%idx2] : memref<256xi32> 

        AIE.objectFifo.release<Consume>(%objFifo : !AIE.objectFifo<memref<256xi32>>, 1)

        // This release means our 2nd core is done
        AIE.useLock(%lock24_2, "Release", 1)
        AIE.end
    }
}
