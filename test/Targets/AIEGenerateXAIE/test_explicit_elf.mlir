//===- test_explicit_elf.mlir ----------------------------------*- MLIR -*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2023 Advanced Micro Devices, Inc.
//
//===----------------------------------------------------------------------===//

// RUN: aie-translate --aie-generate-xaie %s | FileCheck %s

// CHECK: mlir_aie_configure_cores
// CHECK: __mlir_aie_try(XAie_CoreReset(&(ctx->DevInst), XAie_TileLoc(3,3)));
// CHECK: __mlir_aie_try(XAie_CoreDisable(&(ctx->DevInst), XAie_TileLoc(3,3)));
// CHECK: XAie_LoadElf(&(ctx->DevInst), XAie_TileLoc(3,3), (const char*)"test.elf",0);
// CHECK: mlir_aie_start_cores
// CHECK: __mlir_aie_try(XAie_CoreUnreset(&(ctx->DevInst), XAie_TileLoc(3,3)));
// CHECK: __mlir_aie_try(XAie_CoreEnable(&(ctx->DevInst), XAie_TileLoc(3,3)));

module @test_xaie0 {
 AIE.device(xcvc1902) {
  %t33 = AIE.tile(3, 3)
  AIE.core(%t33) {
    AIE.end
  } { elf_file = "test.elf" }
 }
}
