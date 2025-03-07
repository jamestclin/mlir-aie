//===- tileDMA_channels.aie.mlir --------------------------*- MLIR -*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Copyright (C) 2022, Xilinx Inc.
// Copyright (C) 2022, Advanced Micro Devices, Inc.
//
// Date: September 15th 2022
// 
//===----------------------------------------------------------------------===//

// RUN: aiecc.py %VitisSysrootFlag% --host-target=%aieHostTargetTriplet% %s -I%aie_runtime_lib%/test_lib/include %extraAieCcFlags% -L%aie_runtime_lib%/test_lib/lib -ltest_lib %S/test.cpp -o test.elf
// RUN: %run_on_board ./test.elf

// This test uses all four channels of the tileDMAs of tiles (1, 2) and (3, 3) as four object FIFOs.
// Tile (3, 3) produces the input of two object FIFOs while tile (1, 2) copies these objects into
// two other object FIFOs whose objects are read by tile (1, 3) and added together to produce the
// final output. 

module @dmaChannels {
    %tile12 = AIE.tile(1, 2)
    %tile33 = AIE.tile(3, 3)

    %buff_out = AIE.buffer(%tile33) { sym_name = "out" } :  memref<10x16xi32>
    %lock_out = AIE.lock(%tile33, 0) { sym_name = "lock_out" }

    %objFifoIn0 = AIE.objectFifo.createObjectFifo(%tile33, {%tile12}, 2 : i32) {sym_name = "of_in0"} : !AIE.objectFifo<memref<16xi32>>
    %objFifoIn1 = AIE.objectFifo.createObjectFifo(%tile33, {%tile12}, 2 : i32) {sym_name = "of_in1"} : !AIE.objectFifo<memref<16xi32>>

    %objFifoOut0 = AIE.objectFifo.createObjectFifo(%tile12, {%tile33}, 2 : i32) {sym_name = "of_out0"} : !AIE.objectFifo<memref<16xi32>>
    %objFifoOut1 = AIE.objectFifo.createObjectFifo(%tile12, {%tile33}, 2 : i32) {sym_name = "of_out1"} : !AIE.objectFifo<memref<16xi32>>

    func.func @copy(%lineIn : memref<16xi32>, %lineOut : memref<16xi32>) -> () {
        %c0 = arith.constant 0 : index
        %c1 = arith.constant 1 : index
        %lineWidth = arith.constant 16 : index
        
        scf.for %indexInLine = %c0 to %lineWidth step %c1 {
            %value = memref.load %lineIn[%indexInLine] : memref<16xi32>
            memref.store %value, %lineOut[%indexInLine] : memref<16xi32>
        }
        return
    }

    %core12 = AIE.core(%tile12) {
        %c0 = arith.constant 0 : index
        %c1 = arith.constant 1 : index
        %height = arith.constant 10 : index

        scf.for %indexInHeight = %c0 to %height step %c1 { 
            %subviewIn0 = AIE.objectFifo.acquire<Consume>(%objFifoIn0 : !AIE.objectFifo<memref<16xi32>>, 1) : !AIE.objectFifoSubview<memref<16xi32>>
            %elemIn0 = AIE.objectFifo.subview.access %subviewIn0[0] : !AIE.objectFifoSubview<memref<16xi32>> -> memref<16xi32>

            %subviewIn1 = AIE.objectFifo.acquire<Consume>(%objFifoIn1 : !AIE.objectFifo<memref<16xi32>>, 1) : !AIE.objectFifoSubview<memref<16xi32>>
            %elemIn1 = AIE.objectFifo.subview.access %subviewIn1[0] : !AIE.objectFifoSubview<memref<16xi32>> -> memref<16xi32>

            %subviewOut0 = AIE.objectFifo.acquire<Produce>(%objFifoOut0 : !AIE.objectFifo<memref<16xi32>>, 1) : !AIE.objectFifoSubview<memref<16xi32>>
            %elemOut0 = AIE.objectFifo.subview.access %subviewOut0[0] : !AIE.objectFifoSubview<memref<16xi32>> -> memref<16xi32>

            %subviewOut1 = AIE.objectFifo.acquire<Produce>(%objFifoOut1 : !AIE.objectFifo<memref<16xi32>>, 1) : !AIE.objectFifoSubview<memref<16xi32>>
            %elemOut1 = AIE.objectFifo.subview.access %subviewOut1[0] : !AIE.objectFifoSubview<memref<16xi32>> -> memref<16xi32>

            func.call @copy(%elemIn0, %elemOut0) : (memref<16xi32>, memref<16xi32>) -> ()
            func.call @copy(%elemIn1, %elemOut1) : (memref<16xi32>, memref<16xi32>) -> ()

            AIE.objectFifo.release<Consume>(%objFifoIn0 : !AIE.objectFifo<memref<16xi32>>, 1)
            AIE.objectFifo.release<Consume>(%objFifoIn1 : !AIE.objectFifo<memref<16xi32>>, 1)
            AIE.objectFifo.release<Produce>(%objFifoOut0 : !AIE.objectFifo<memref<16xi32>>, 1)
            AIE.objectFifo.release<Produce>(%objFifoOut1 : !AIE.objectFifo<memref<16xi32>>, 1)
        }
        
        AIE.end
    }

    // Fills the given memref.
    func.func @generateLineScalar(%lineOut : memref<16xi32>) -> () {
        %c0 = arith.constant 0 : index
        %c1 = arith.constant 1 : index
        %lineWidth = arith.constant 16 : index
        
        scf.for %indexInLine = %c0 to %lineWidth step %c1 {
            %i = arith.index_cast %indexInLine : index to i32
            memref.store %i, %lineOut[%indexInLine] : memref<16xi32>
        }
        return
    }

    // Stores sum of two input memrefs in the bufferOut at the given row index.
    func.func @addAndStore(%lineIn0 : memref<16xi32>, %lineIn1 : memref<16xi32>, %row : index, %bufferOut : memref<10x16xi32>) -> () {
        %c0 = arith.constant 0 : index
        %c1 = arith.constant 1 : index
        %lineWidth = arith.constant 16 : index

        scf.for %indexInLine = %c0 to %lineWidth step %c1 {
            %value0 = memref.load %lineIn0[%indexInLine] : memref<16xi32>
            %value1 = memref.load %lineIn1[%indexInLine] : memref<16xi32>
            %sum = arith.addi %value0, %value1 : i32
            memref.store %sum, %bufferOut[%row,%indexInLine] : memref<10x16xi32>
        }
        return
    }

    %core33 = AIE.core(%tile33) {
        %c0 = arith.constant 0 : index
        %c1 = arith.constant 1 : index
        %height = arith.constant 10 : index

        // acquire output buffer
        AIE.useLock(%lock_out, "Acquire", 0) // acquire for produce

        scf.for %indexInHeight = %c0 to %height step %c1 { 
            %subviewOut0 = AIE.objectFifo.acquire<Produce>(%objFifoIn0 : !AIE.objectFifo<memref<16xi32>>, 1) : !AIE.objectFifoSubview<memref<16xi32>>
            %elemOut0 = AIE.objectFifo.subview.access %subviewOut0[0] : !AIE.objectFifoSubview<memref<16xi32>> -> memref<16xi32>

            %subviewOut1 = AIE.objectFifo.acquire<Produce>(%objFifoIn1 : !AIE.objectFifo<memref<16xi32>>, 1) : !AIE.objectFifoSubview<memref<16xi32>>
            %elemOut1 = AIE.objectFifo.subview.access %subviewOut1[0] : !AIE.objectFifoSubview<memref<16xi32>> -> memref<16xi32>

            func.call @generateLineScalar(%elemOut0) : (memref<16xi32>) -> ()
            func.call @generateLineScalar(%elemOut1) : (memref<16xi32>) -> ()

            AIE.objectFifo.release<Produce>(%objFifoIn0 : !AIE.objectFifo<memref<16xi32>>, 1)
            AIE.objectFifo.release<Produce>(%objFifoIn1 : !AIE.objectFifo<memref<16xi32>>, 1)


            %subviewIn0 = AIE.objectFifo.acquire<Consume>(%objFifoOut0 : !AIE.objectFifo<memref<16xi32>>, 1) : !AIE.objectFifoSubview<memref<16xi32>>
            %elemIn0 = AIE.objectFifo.subview.access %subviewIn0[0] : !AIE.objectFifoSubview<memref<16xi32>> -> memref<16xi32>

            %subviewIn1 = AIE.objectFifo.acquire<Consume>(%objFifoOut1 : !AIE.objectFifo<memref<16xi32>>, 1) : !AIE.objectFifoSubview<memref<16xi32>>
            %elemIn1 = AIE.objectFifo.subview.access %subviewIn1[0] : !AIE.objectFifoSubview<memref<16xi32>> -> memref<16xi32>

            func.call @addAndStore(%elemIn0, %elemIn1, %indexInHeight, %buff_out) : (memref<16xi32>, memref<16xi32>, index, memref<10x16xi32>) -> ()

            AIE.objectFifo.release<Consume>(%objFifoOut0 : !AIE.objectFifo<memref<16xi32>>, 1)
            AIE.objectFifo.release<Consume>(%objFifoOut1 : !AIE.objectFifo<memref<16xi32>>, 1)
        }

        // release output buffer
        AIE.useLock(%lock_out, "Release", 1) // release for consume
        
        AIE.end
    }
}
