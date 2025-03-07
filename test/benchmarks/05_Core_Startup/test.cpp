//===- test.cpp -------------------------------------------------*- C++ -*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2021 Xilinx Inc.
//
//===----------------------------------------------------------------------===//

#include "test_library.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <xaiengine.h>

#include "aie_inc.cpp"

int main(int argc, char *argv[]) {

  int n = 1;
  u32 pc0_times[n];
  u32 pc1_times[n];

  printf("05_Core_Startup test start.\n");
  printf("Running %d times ...\n", n);

  for (int iters = 0; iters < n; iters++) {

    aie_libxaie_ctx_t *_xaie = mlir_aie_init_libxaie();
    mlir_aie_init_device(_xaie);
    mlir_aie_configure_cores(_xaie);
    mlir_aie_configure_switchboxes(_xaie);
    mlir_aie_initialize_locks(_xaie);

    //    mlir_aie_clear_tile_memory(_xaie, 1, 3);

    mlir_aie_configure_dmas(_xaie);

    XAie_EventPCEnable(&(_xaie->DevInst), XAie_TileLoc(1, 3), 0, 0);
    XAie_EventPCEnable(&(_xaie->DevInst), XAie_TileLoc(1, 3), 1, 240);

    EventMonitor pc0(_xaie, 1, 3, 0, XAIE_EVENT_ACTIVE_CORE,
                 XAIE_EVENT_DISABLED_CORE, XAIE_EVENT_NONE_CORE,
                 XAIE_CORE_MOD);

    pc0.set();

    EventMonitor pc1(_xaie, 1, 3, 1, XAIE_EVENT_PC_0_CORE, XAIE_EVENT_PC_1_CORE,
                     XAIE_EVENT_NONE_CORE, XAIE_CORE_MOD);
    pc1.set();

    mlir_aie_start_cores(_xaie);
    usleep(1000);

    pc0_times[iters] = pc0.diff();
    pc1_times[iters] = pc1.diff();

    mlir_aie_deinit_libxaie(_xaie);
  }

  computeStats(pc0_times, n);
  computeStats(pc1_times, n);
}
