//===- aie.mlir ------------------------------------------------*- MLIR -*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Copyright (C) 2023, Advanced Micro Devices, Inc.
//
//===----------------------------------------------------------------------===//

// RUN: aiecc.py -j4 %VitisSysrootFlag% --host-target=%aieHostTargetTriplet% %s -I%aie_runtime_lib%/test_lib/include %extraAieCcFlags% -L%aie_runtime_lib%/test_lib/lib -ltest_lib %S/test.cpp -o test.elf
// RUN: %run_on_board ./test.elf

module @multi_depth {
    AIE.device(xcvc1902) {
        %tile20 = AIE.tile(2, 0)
        %tile23 = AIE.tile(2, 3)
        %tile25 = AIE.tile(2, 5)

        %lock_pc = AIE.lock(%tile25, 0) {sym_name = "lock_pc"}

        %lock_out = AIE.lock(%tile25, 1) {sym_name = "lock_out"}
        %buff_out = AIE.buffer(%tile25) {sym_name = "buff_out"} : memref<4x32xi32>

        %of_in = AIE.objectFifo.createObjectFifo(%tile20, {%tile23, %tile25}, [2, 2, 3]) {sym_name = "of_in"} : !AIE.objectFifo<memref<32xi32>>
        %of_inter = AIE.objectFifo.createObjectFifo(%tile23, {%tile25}, 2 : i32) {sym_name = "of_inter"} : !AIE.objectFifo<memref<32xi32>>

        %ext_buffer_in_0  = AIE.external_buffer {sym_name = "ext_buffer_in_0"} : memref<32xi32>
        %ext_buffer_in_1  = AIE.external_buffer {sym_name = "ext_buffer_in_1"} : memref<32xi32>
        AIE.objectFifo.registerExternalBuffers(%tile20, %of_in : !AIE.objectFifo<memref<32xi32>>, {%ext_buffer_in_0, %ext_buffer_in_1}) : (memref<32xi32>, memref<32xi32>)

        func.func @add_one(%elemIn : memref<32xi32>, %elemOut : memref<32xi32>) -> () {
            %c0 = arith.constant 0 : index
            %c1 = arith.constant 1 : index
            %v1 = arith.constant 1 : i32
            %height = arith.constant 32 : index
            
            scf.for %indexInHeight = %c0 to %height step %c1 {
                %d1 = memref.load %elemIn[%indexInHeight] : memref<32xi32>
                %val = arith.addi %d1, %v1 : i32
                memref.store %val, %elemOut[%indexInHeight] : memref<32xi32> 
            }
            return
        }

        func.func @add_store(%elemIn0 : memref<32xi32>, %elemIn1 : memref<32xi32>, %elemOut : memref<4x32xi32>, %index : index) -> () {
            %c0 = arith.constant 0 : index
            %c1 = arith.constant 1 : index
            %height = arith.constant 32 : index
            
            scf.for %indexInHeight = %c0 to %height step %c1 {
                %d1 = memref.load %elemIn0[%indexInHeight] : memref<32xi32>
                %d2 = memref.load %elemIn1[%indexInHeight] : memref<32xi32>
                %val = arith.addi %d1, %d2 : i32
                memref.store %val, %elemOut[%index, %indexInHeight] : memref<4x32xi32> 
            }
            return
        }

        %core23 = AIE.core(%tile23) {
            %c0 = arith.constant 0 : index
            %c1 = arith.constant 1 : index
            %iter_max = arith.constant 4 : index

            scf.for %iter = %c0 to %iter_max step %c1 {
                %subviewIn = AIE.objectFifo.acquire<Consume>(%of_in : !AIE.objectFifo<memref<32xi32>>, 1) : !AIE.objectFifoSubview<memref<32xi32>>
                %elemIn = AIE.objectFifo.subview.access %subviewIn[0] : !AIE.objectFifoSubview<memref<32xi32>> -> memref<32xi32>

                %subviewOut = AIE.objectFifo.acquire<Produce>(%of_inter : !AIE.objectFifo<memref<32xi32>>, 1) : !AIE.objectFifoSubview<memref<32xi32>>
                %elemOut = AIE.objectFifo.subview.access %subviewOut[0] : !AIE.objectFifoSubview<memref<32xi32>> -> memref<32xi32>

                func.call @add_one(%elemIn, %elemOut) : (memref<32xi32>, memref<32xi32>) -> ()
                
                AIE.objectFifo.release<Consume>(%of_in : !AIE.objectFifo<memref<32xi32>>, 1)
                AIE.objectFifo.release<Produce>(%of_inter : !AIE.objectFifo<memref<32xi32>>, 1)
            }
                
            AIE.end
        } 

        %core25 = AIE.core(%tile25) {
            %c0 = arith.constant 0 : index
            %c1 = arith.constant 1 : index
            %height = arith.constant 32 : index
            %iter_max = arith.constant 4 : index

            AIE.useLock(%lock_pc, Acquire, 0)

            AIE.useLock(%lock_out, Acquire, 0)

            scf.for %iter = %c0 to %iter_max step %c1 {
                %subviewIn_21 = AIE.objectFifo.acquire<Consume>(%of_in : !AIE.objectFifo<memref<32xi32>>, 1) : !AIE.objectFifoSubview<memref<32xi32>>
                %elemIn_21 = AIE.objectFifo.subview.access %subviewIn_21[0] : !AIE.objectFifoSubview<memref<32xi32>> -> memref<32xi32>

                %subviewIn_22 = AIE.objectFifo.acquire<Consume>(%of_inter : !AIE.objectFifo<memref<32xi32>>, 1) : !AIE.objectFifoSubview<memref<32xi32>>
                %elemIn_22 = AIE.objectFifo.subview.access %subviewIn_22[0] : !AIE.objectFifoSubview<memref<32xi32>> -> memref<32xi32>

                func.call @add_store(%elemIn_21, %elemIn_22, %buff_out, %iter) : (memref<32xi32>, memref<32xi32>, memref<4x32xi32>, index) -> ()
                
                AIE.objectFifo.release<Consume>(%of_in : !AIE.objectFifo<memref<32xi32>>, 1)
                AIE.objectFifo.release<Consume>(%of_inter : !AIE.objectFifo<memref<32xi32>>, 1)
            }

            AIE.useLock(%lock_out, Release, 1)

            AIE.useLock(%lock_pc, Release, 1)
                
            AIE.end
        } 
    }
}
