// RUN: aie-opt %s -affine-super-vectorize="virtual-vector-size=16 test-fastest-varying=0 vectorize-reductions=true" --convert-vector-to-aievec="aie-target=aieml" -lower-affine | aie-translate -aieml=true --aievec-to-cpp -o dut.cc
// RUN: xchesscc_wrapper aie2 -f -g +s +w work +o work -I%S -I. %S/testbench.cc dut.cc
// RUN: mkdir -p data
// RUN: xca_udm_dbg --aiearch aie-ml -qf -T -P %aietools/data/aie_ml/lib/ -t "%S/../profiling.tcl ./work/a.out" >& xca_udm_dbg.stdout
// RUN: FileCheck --input-file=./xca_udm_dbg.stdout %s
// CHECK: TEST PASSED

module {
func.func @dut(%arg0: memref<1024xi32>, %arg1: memref<i32>) {
    %c0_i32 = arith.constant 0 : i32
    %0 = affine.for %arg2 = 0 to 1024 iter_args(%arg3 = %c0_i32) -> (i32) {
      %1 = affine.load %arg0[%arg2] : memref<1024xi32>
      %2 = arith.addi %arg3, %1 : i32
      affine.yield %2 : i32
    }
    affine.store %0, %arg1[] : memref<i32>
    return
  }
}
