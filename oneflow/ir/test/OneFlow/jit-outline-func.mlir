// RUN: oneflow-opt -outline-jit-function %s | FileCheck %s
builtin.module  {
  builtin.func @FuseCastScaleJob() {
    %data_output = "oneflow_foundation.system"() {device_name = ["@0:0"], device_tag = "gpu", hierarchy = [1], input_bns = [], op_name = "Input_0", op_type_case = 137 : i32, operand_segment_sizes = dense<0> : vector<2xi32>, output_lbns = ["Input_0/out"], result_segment_sizes = dense<[1, 0]> : vector<2xi32>, scope_symbol_id = 4611686018427432958 : i64} : () -> tensor<96x96xi64>
    %data_output_0 = "oneflow_foundation.system"() {device_name = ["@0:0"], device_tag = "cpu", hierarchy = [1], input_bns = [], op_name = "scale", op_type_case = 122 : i32, operand_segment_sizes = dense<0> : vector<2xi32>, output_lbns = ["scale/out"], result_segment_sizes = dense<[1, 0]> : vector<2xi32>, scope_symbol_id = 4611686018427437054 : i64} : () -> tensor<1xf32>
    %0 = "oneflow_foundation.cast"(%data_output) {device_name = ["@0:0"], device_tag = "cpu", dtype = "DT_Float", hierarchy = [1], op_name = "Cast_1", scope_symbol_id = 4611686018427437054 : i64} : (tensor<96x96xi64>) -> tensor<96x96xf32>
    "oneflow_foundation.system"(%data_output_0) {device_name = ["@0:0"], device_tag = "cpu", hierarchy = [1], input_bns = ["in"], op_name = "Return_4", op_type_case = 146 : i32, operand_segment_sizes = dense<[1, 0]> : vector<2xi32>, output_lbns = [], result_segment_sizes = dense<0> : vector<2xi32>, scope_symbol_id = 4611686018427445246 : i64} : (tensor<1xf32>) -> ()
    %1 = "oneflow_foundation.scalar_mul_by_tensor"(%0, %data_output_0) {device_name = ["@0:0"], device_tag = "cpu", hierarchy = [1], op_name = "ScalarMulByTensor_2", scope_symbol_id = 4611686018427437054 : i64} : (tensor<96x96xf32>, tensor<1xf32>) -> tensor<96x96xf32>
    "oneflow_foundation.system"(%1) {device_name = ["@0:0"], device_tag = "cpu", hierarchy = [1], input_bns = ["in"], op_name = "Return_3", op_type_case = 146 : i32, operand_segment_sizes = dense<[1, 0]> : vector<2xi32>, output_lbns = [], result_segment_sizes = dense<0> : vector<2xi32>, scope_symbol_id = 4611686018427445246 : i64} : (tensor<96x96xf32>) -> ()
    return
  }
}
// CHECK: %0 = oneflow_foundation.mlir_jit
