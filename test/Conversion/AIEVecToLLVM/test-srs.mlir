// RUN: aie-opt %s --convert-aievec-to-llvm | FileCheck %s
module {
  func.func @test() {
    %v8i48 = llvm.mlir.undef : vector<8xi48>
    %v16i48 = llvm.mlir.undef : vector<16xi48>
    %v4i80 = llvm.mlir.undef : vector<4xi80>
    %v8i80 = llvm.mlir.undef : vector<8xi80>
    // sweep the variants of SRS and values of shift
    %0 = aievec.srs %v16i48 {shift = 0 : i8} : vector<16xi48>, vector<16xi8>
    %1 = aievec.srs %v16i48 {shift = 1 : i8} : vector<16xi48>, vector<16xui8>
    %2 = aievec.srs %v16i48 {shift = 2 : i8} : vector<16xi48>, vector<16xi16>
    %3 = aievec.srs %v8i48 {shift = 3 : i8} : vector<8xi48>, vector<8xi32>
    %4 = aievec.srs %v4i80 {shift = 4 : i8} : vector<4xi80>, vector<4xi32>
    %5 = aievec.srs %v8i80 {shift = 5 : i8} : vector<8xi80>, vector<8xi64>
    return
  }
}

// The function declarations are in reverse order of their declarations
// CHECK: llvm.func @llvm.aie.lsrs.v8i64.v8acc80(vector<8xi80>, i32) -> vector<8xi64>
// CHECK: llvm.func @llvm.aie.srs.v4i32.v4acc80(vector<4xi80>, i32) -> vector<4xi32>
// CHECK: llvm.func @llvm.aie.lsrs.v8i32.v8acc48(vector<8xi48>, i32) -> vector<8xi32>
// CHECK: llvm.func @llvm.aie.srs.v16i16.v16acc48(vector<16xi48>, i32) -> vector<16xi16>
// CHECK: llvm.func @llvm.aie.usrs.v16i8.v16acc48(vector<16xi48>, i32) -> vector<16xui8>
// CHECK: llvm.func @llvm.aie.bsrs.v16i8.v16acc48(vector<16xi48>, i32) -> vector<16xi8>
// CHECK: [[UNDEF_V8I48:%.+]] = llvm.mlir.undef : vector<8xi48>
// CHECK: [[UNDEF_V16I48:%.+]] = llvm.mlir.undef : vector<16xi48>
// CHECK: [[UNDEF_V4I80:%.+]] = llvm.mlir.undef : vector<4xi80>
// CHECK: [[UNDEF_V8I80:%.+]] = llvm.mlir.undef : vector<8xi80>
// CHECK: [[SHIFT:%.+]] = llvm.mlir.constant(0 : i32) : i32
// CHECK: {{.*}} = llvm.call @llvm.aie.bsrs.v16i8.v16acc48([[UNDEF_V16I48]], [[SHIFT]]) : (vector<16xi48>, i32) -> vector<16xi8>
// CHECK: [[SHIFT:%.+]] = llvm.mlir.constant(1 : i32) : i32
// CHECK: {{.*}} = llvm.call @llvm.aie.usrs.v16i8.v16acc48([[UNDEF_V16I48]], [[SHIFT]]) : (vector<16xi48>, i32) -> vector<16xui8>
// CHECK: [[SHIFT:%.+]] = llvm.mlir.constant(2 : i32) : i32
// CHECK: {{.*}} = llvm.call @llvm.aie.srs.v16i16.v16acc48([[UNDEF_V16I48]], [[SHIFT]]) : (vector<16xi48>, i32) -> vector<16xi16>
// CHECK: [[SHIFT:%.+]] = llvm.mlir.constant(3 : i32) : i32
// CHECK: {{.*}} = llvm.call @llvm.aie.lsrs.v8i32.v8acc48([[UNDEF_V8I48]], [[SHIFT]]) : (vector<8xi48>, i32) -> vector<8xi32>
// CHECK: [[SHIFT:%.+]] = llvm.mlir.constant(4 : i32) : i32
// CHECK: {{.*}} = llvm.call @llvm.aie.srs.v4i32.v4acc80([[UNDEF_V4I80]], [[SHIFT]]) : (vector<4xi80>, i32) -> vector<4xi32>
// CHECK: [[SHIFT:%.+]] = llvm.mlir.constant(5 : i32) : i32
// CHECK: {{.*}} = llvm.call @llvm.aie.lsrs.v8i64.v8acc80([[UNDEF_V8I80]], [[SHIFT]]) : (vector<8xi80>, i32) -> vector<8xi64>
