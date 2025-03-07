// RUN: aie-opt %s --convert-vector-to-aievec="aie-target=aieml" -lower-affine | aie-translate -aieml=true --aievec-to-cpp -o dut.cc
// RUN: xchesscc_wrapper aie2 -f -g +s +w work +o work -I%S -I. %S/testbench.cc dut.cc
// RUN: mkdir -p data
// RUN: xca_udm_dbg --aiearch aie-ml -qf -T -P %aietools/data/aie_ml/lib/ -t "%S/../profiling.tcl ./work/a.out" >& xca_udm_dbg.stdout
// RUN: FileCheck --input-file=./xca_udm_dbg.stdout %s
// CHECK: TEST PASSED

module {
  func.func @dut(%arg0: memref<1024xi16>, %arg1: memref<i16>) {
    %c0_i16 = arith.constant 0 : i16
    %cst = arith.constant dense<32767> : vector<32xi16>
    %0 = affine.for %arg2 = 0 to 1024 step 32 iter_args(%arg3 = %cst) -> (vector<32xi16>) {
      %2 = vector.transfer_read %arg0[%arg2], %c0_i16 : memref<1024xi16>, vector<32xi16>
      %3 = arith.minsi %arg3, %2 : vector<32xi16>
      affine.yield %3 : vector<32xi16>
    }
    %1 = vector.reduction <minsi>, %0 : vector<32xi16> into i16
    affine.store %1, %arg1[] : memref<i16>
    return
  }
}
