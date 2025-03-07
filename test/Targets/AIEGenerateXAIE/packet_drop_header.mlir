//===- packet_drop_header.mlir ---------------------------------*- MLIR -*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2023 Advanced Micro Devices, Inc.
//
//===----------------------------------------------------------------------===//

// RUN: aie-translate --aie-generate-xaie %s | FileCheck %s

// CHECK: mlir_aie_configure_switchboxes
// CHECK: x = 7;
// CHECK: y = 0;
// CHECK: __mlir_aie_try(XAie_StrmPktSwMstrPortEnable(&(ctx->DevInst), XAie_TileLoc(x,y), SOUTH, 0, {{.*}} XAIE_SS_PKT_DONOT_DROP_HEADER, {{.*}} 0, {{.*}} 0x2));
// CHECK: __mlir_aie_try(XAie_StrmPktSwMstrPortEnable(&(ctx->DevInst), XAie_TileLoc(x,y), NORTH, 0, {{.*}} XAIE_SS_PKT_DONOT_DROP_HEADER, {{.*}} 0, {{.*}} 0x1));
// CHECK: __mlir_aie_try(XAie_StrmPktSwSlavePortEnable(&(ctx->DevInst), XAie_TileLoc(x,y), NORTH, 0));
// CHECK: __mlir_aie_try(XAie_StrmPktSwSlaveSlotEnable(&(ctx->DevInst), XAie_TileLoc(x,y), NORTH, 0, {{.*}} 0, {{.*}} XAie_PacketInit(10,0), {{.*}} 0x1F, {{.*}} 1, {{.*}} 0));
// CHECK: __mlir_aie_try(XAie_StrmPktSwSlavePortEnable(&(ctx->DevInst), XAie_TileLoc(x,y), SOUTH, 4));
// CHECK: __mlir_aie_try(XAie_StrmPktSwSlaveSlotEnable(&(ctx->DevInst), XAie_TileLoc(x,y), SOUTH, 4, {{.*}} 0, {{.*}} XAie_PacketInit(3,0), {{.*}} 0x1F, {{.*}} 0, {{.*}} 0));
// CHECK: x = 7;
// CHECK: y = 1;
// CHECK: __mlir_aie_try(XAie_StrmPktSwMstrPortEnable(&(ctx->DevInst), XAie_TileLoc(x,y), DMA, 0, {{.*}} XAIE_SS_PKT_DROP_HEADER, {{.*}} 0, {{.*}} 0x1));
// CHECK: __mlir_aie_try(XAie_StrmPktSwMstrPortEnable(&(ctx->DevInst), XAie_TileLoc(x,y), SOUTH, 0, {{.*}} XAIE_SS_PKT_DONOT_DROP_HEADER, {{.*}} 0, {{.*}} 0x2));
// CHECK: __mlir_aie_try(XAie_StrmPktSwSlavePortEnable(&(ctx->DevInst), XAie_TileLoc(x,y), DMA, 0));
// CHECK: __mlir_aie_try(XAie_StrmPktSwSlaveSlotEnable(&(ctx->DevInst), XAie_TileLoc(x,y), DMA, 0, {{.*}} 0, {{.*}} XAie_PacketInit(10,0), {{.*}} 0x1F, {{.*}} 1, {{.*}} 0));
// CHECK: __mlir_aie_try(XAie_StrmPktSwSlavePortEnable(&(ctx->DevInst), XAie_TileLoc(x,y), SOUTH, 0));
// CHECK: __mlir_aie_try(XAie_StrmPktSwSlaveSlotEnable(&(ctx->DevInst), XAie_TileLoc(x,y), SOUTH, 0, {{.*}} 0, {{.*}} XAie_PacketInit(3,0), {{.*}} 0x1F, {{.*}} 0, {{.*}} 0));

//
// This tests the switchbox configuration lowering for packet switched routing
// to drop headers when the packet's destination is a DMA.
//
module @aie_module {
  AIE.device(xcvc1902) {
    %0 = AIE.tile(7, 0)
    %1 = AIE.switchbox(%0) {
      %7 = AIE.amsel<0> (0)
      %8 = AIE.amsel<0> (1)
      %9 = AIE.masterset(South : 0, %8)
      %10 = AIE.masterset(North : 0, %7)
      AIE.packetrules(North : 0) {
        AIE.rule(31, 10, %8)
      }
      AIE.packetrules(South : 4) {
        AIE.rule(31, 3, %7)
      }
    }
    %2 = AIE.tile(7, 1)
    %3 = AIE.switchbox(%2) {
      %7 = AIE.amsel<0> (0)
      %8 = AIE.amsel<0> (1)
      %9 = AIE.masterset(DMA : 0, %7)
      %10 = AIE.masterset(South : 0, %8)
      AIE.packetrules(DMA : 0) {
        AIE.rule(31, 10, %8)
      }
      AIE.packetrules(South : 0) {
        AIE.rule(31, 3, %7)
      }
    }
    %4 = AIE.lock(%2, 1)
    %5 = AIE.buffer(%2) {address = 3072 : i32, sym_name = "buf1"} : memref<16xi32, 2>
    %6 = AIE.mem(%2) {
      %7 = AIE.dmaStart(S2MM, 0, ^bb2, ^bb1)
    ^bb1:  // pred: ^bb0
      %8 = AIE.dmaStart(MM2S, 0, ^bb3, ^bb4)
    ^bb2:  // 2 preds: ^bb0, ^bb2
      AIE.useLock(%4, Acquire, 0)
      AIE.dmaBdPacket(2, 3)
      AIE.dmaBd(<%5 : memref<16xi32, 2>, 0, 16>, 0)
      AIE.useLock(%4, Release, 1)
      AIE.nextBd ^bb2
    ^bb3:  // 2 preds: ^bb1, ^bb3
      AIE.useLock(%4, Acquire, 1)
      AIE.dmaBdPacket(6, 10)
      AIE.dmaBd(<%5 : memref<16xi32, 2>, 0, 16>, 0)
      AIE.useLock(%4, Release, 0)
      AIE.nextBd ^bb3
    ^bb4:  // pred: ^bb1
      AIE.end
    }
  }
}
