// RUN: aie-opt %s -canonicalize-vector-for-aievec -canonicalize -split-input-file | FileCheck %s

// CHECK-LABEL: func.func @splat(
// CHECK-SAME: %[[MEM:.*]]: memref<?xi32>,
// CHECK-SAME: %[[POS:.*]]: index
func.func @splat(%m : memref<?xi32>, %pos : index) -> vector<8xi32> {
    // CHECK: %[[C0:.*]] = arith.constant 0 : i32
    %c0_i32 = arith.constant 0 : i32
    %i = affine.apply affine_map<(d0) -> (d0 + 5)>(%pos)
    // CHECK: %[[V:.*]] = vector.transfer_read %[[MEM]][%[[POS]]], %[[C0]] : memref<?xi32>, vector<8xi32>
    // CHECK: %[[E:.*]] = vector.extract %[[V]][5] : vector<8xi32>
    // CHECK: %[[S:.*]] = vector.broadcast %[[E]] : i32 to vector<8xi32>
    %v = vector.transfer_read %m[%i], %c0_i32 {permutation_map = affine_map<(d0) -> (0)>} : memref<?xi32>, vector<8xi32>
    // CHECK: return %[[S]] : vector<8xi32>
    return %v : vector<8xi32>
}

// -----

// CHECK: #[[IDXMAP:.*]] = affine_map<()[s0] -> (s0 + 24)>
// CHECK-LABEL: func.func @far_splat(
// CHECK-SAME: %[[MEM:.*]]: memref<?xi32>,
// CHECK-SAME: %[[POS:.*]]: index
func.func @far_splat(%m : memref<?xi32>, %pos : index) -> vector<8xi32> {
    // CHECK: %[[C0:.*]] = arith.constant 0 : i32
    %c0_i32 = arith.constant 0 : i32
    // CHECK: %[[IDX:.*]] = affine.apply #[[IDXMAP]]()[%[[POS]]]
    %i = affine.apply affine_map<(d0) -> (d0 + 29)>(%pos)
    // CHECK: %[[V:.*]] = vector.transfer_read %[[MEM]][%[[IDX]]], %[[C0]] : memref<?xi32>, vector<8xi32>
    // CHECK: %[[E:.*]] = vector.extract %[[V]][5] : vector<8xi32>
    // CHECK: %[[S:.*]] = vector.broadcast %[[E]] : i32 to vector<8xi32>
    %v = vector.transfer_read %m[%i], %c0_i32 {permutation_map = affine_map<(d0) -> (0)>} : memref<?xi32>, vector<8xi32>
    // CHECK: return %[[S]] : vector<8xi32>
    return %v : vector<8xi32>
}

// -----

// CHECK-LABEL: func.func @unaligned_transfer_read(
// CHECK-SAME: %[[MEM:.*]]: memref<1024xi32>,
// CHECK-SAME: %[[POS:.*]]: index
func.func @unaligned_transfer_read(%m : memref<1024xi32>, %pos : index) -> vector<8xi32> {
    // CHECK-DAG: %[[C0:.*]] = arith.constant 0 : i32
    %c0_i32 = arith.constant 0 : i32
    %i = affine.apply affine_map<(d0) -> (d0 + 5)>(%pos)
    // CHECK-DAG: %[[LV:.*]] = vector.transfer_read %[[MEM]][%[[POS]]], %[[C0]] : memref<1024xi32>, vector<16xi32>
    // CHECK-DAG: %[[AV:.*]] = vector.extract_strided_slice %[[LV]] {offsets = [5], sizes = [8], strides = [1]} : vector<16xi32> to vector<8xi32>
    %v = vector.transfer_read %m[%i], %c0_i32 : memref<1024xi32>, vector<8xi32>
    // CHECK: return %[[AV]] : vector<8xi32>
    return %v : vector<8xi32>
}
