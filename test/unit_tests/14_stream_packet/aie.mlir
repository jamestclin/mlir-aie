// RUN: aiecc.py --sysroot=${VITIS_SYSROOT} %s -I%S/../../../runtime_lib %S/../../../runtime_lib/test_library.cpp %S/test.cpp -o test.elf

module @test09_simple_shim_dma {
  %t73 = AIE.tile(7, 3)
  %t72 = AIE.tile(7, 2)
  %t62 = AIE.tile(6, 2)
  %t71 = AIE.tile(7, 1)
  
  %sw73 = AIE.switchbox(%t73) {
    AIE.connect<"DMA" : 0, "South" : 3>
  }
  %sw71 = AIE.switchbox(%t71) {
    AIE.connect<"DMA" : 0, "North" : 1>
  }  
  //%sw72 = AIE.switchbox(%t72) {
  //  AIE.connect<"North" : 3, "West" : 3>
  //  AIE.connect<"South" : 1, "West" : 1>
  //}
  %sw72 = AIE.switchbox(%t72) {
    %tmsel = AIE.amsel<1> (0) // <arbiter> (mask). mask is msel_enable
    %tmaster = AIE.masterset(West : 3, %tmsel)
    AIE.packetrules(North : 3)  {
      AIE.rule(0x1f, 0xd, %tmsel) // (mask, id)
    }
    AIE.packetrules(South : 1)  {
      AIE.rule(0x1f, 0xc, %tmsel)
    }
  }
  %sw62 = AIE.switchbox(%t62) {
    AIE.connect<"East" : 3, "DMA" : 0>
    //AIE.connect<"East" : 1, "DMA" : 1>
  }

  %buf73 = AIE.buffer(%t73) {sym_name = "buf73" } : memref<256xi32>
  %buf71 = AIE.buffer(%t71) {sym_name = "buf71" } : memref<256xi32>

  %l73 = AIE.lock(%t73, 0)
  %l71 = AIE.lock(%t71, 0)

  %m73 = AIE.mem(%t73) {
      %srcDma = AIE.dmaStart("MM2S0", ^bd0, ^end)
    ^bd0:
      AIE.useLock(%l73, "Acquire", 0, 0)
      AIE.dmaBdPacket(0x5, 0xD)
      AIE.dmaBd(<%buf73 : memref<256xi32>, 0, 256>, 0)
      AIE.useLock(%l73, "Release", 1, 0)
      br ^end
    ^end:
      AIE.end
  }

  %m71 = AIE.mem(%t71) {
      %srcDma = AIE.dmaStart("MM2S0", ^bd0, ^end)
    ^bd0:
      AIE.useLock(%l71, "Acquire", 0, 0)
      AIE.dmaBdPacket(0x4, 0xC)
      AIE.dmaBd(<%buf71 : memref<256xi32>, 0, 256>, 0)
      AIE.useLock(%l71, "Release", 1, 0)
      br ^end
    ^end:
      AIE.end
  }

  //%buf62_0 = AIE.buffer(%t62) {sym_name = "buf62_0" } : memref<256xi32>
  //%buf62_1 = AIE.buffer(%t62) {sym_name = "buf62_1" } : memref<256xi32>
  //%l62_0 = AIE.lock(%t62, 0)
  //%l62_1 = AIE.lock(%t62, 1)
  %buf62 = AIE.buffer(%t62) {sym_name = "buf62" } : memref<512xi32>
  %l62 = AIE.lock(%t62, 0)

  %m62 = AIE.mem(%t62) {
      %srcDma0 = AIE.dmaStart("S2MM0", ^bd0, ^end)
    //^dma:
    //  %srcDma1 = AIE.dmaStart("S2MM1", ^bd1, ^end)
    ^bd0:
      AIE.useLock(%l62, "Acquire", 0, 0)
      AIE.dmaBd(<%buf62 : memref<512xi32>, 0, 512>, 0)
      AIE.useLock(%l62, "Release", 1, 0)
      br ^end
    //^bd1:
    //  AIE.useLock(%l62_1, "Acquire", 0, 0)
    //  AIE.dmaBd(<%buf62_1 : memref<256xi32>, 0, 256>, 0)
    //  AIE.useLock(%l62_1, "Release", 1, 0)
    //  br ^bd0
    ^end:
      AIE.end
  }  

}
