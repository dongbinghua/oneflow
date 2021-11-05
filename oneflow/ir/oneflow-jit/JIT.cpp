/*
Copyright 2020 The OneFlow Authors. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/
#include "OneFlow/JIT.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "OneFlow/OneFlowDialect.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Transforms/Passes.h"
#include "oneflow/core/framework/op_interpreter/jit_op_interpreter.h"
#include "oneflow/core/framework/op_interpreter/op_interpreter_util.h"
#include "oneflow/core/operator/operator.h"
#include "oneflow/core/framework/user_op_registry_manager.h"
#include "oneflow/core/framework/user_op_def.h"
#include "mlir/Dialect/LLVMIR/LLVMTypes.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "oneflow/core/rpc/include/global_process_ctx.h"
#include "oneflow/core/device/device_context_adapter.h"

namespace {

using namespace mlir;
using namespace ::oneflow::user_op;
using namespace ::oneflow;

std::unique_ptr<BlobDesc> GetBlobDescFromMlirTensorType(TensorType tensor_type) {
  auto dtype = DataType::kInvalidDataType;
  if (tensor_type.getElementType().isF32()) {
    dtype = DataType::kFloat;
  } else {
    tensor_type.dump();
    LOG(FATAL) << "fail to get BlobDesc from TensorType";
  }
  auto shape_from_mlir = new Shape({tensor_type.getShape().begin(), tensor_type.getShape().end()});
  return std::make_unique<BlobDesc>(*shape_from_mlir, dtype);
}

ParallelContext GetSingleDeviceParallelContext() {
  ParallelContext parallel_ctx;
  parallel_ctx.set_parallel_id(0);
  parallel_ctx.set_parallel_num(1);
  return parallel_ctx;
}

void InsertLbnSegmentIntoMapping(const ::mlir::ArrayAttr& lbn_segment_keys,
                                 const ::mlir::ArrayAttr& lbn_segment_sizes, ValueRange values,
                                 std::unordered_map<std::string, mlir::Value>& value_mapping_) {
  auto operand_it = values.begin();
  for (const auto& bn_size_pair : llvm::zip(lbn_segment_keys, lbn_segment_sizes)) {
    const auto& bn = std::get<0>(bn_size_pair).dyn_cast<StringAttr>().getValue().str();
    const auto& length = std::get<1>(bn_size_pair).dyn_cast<IntegerAttr>().getInt();
    for (size_t i = 0; i < length; i++) {
      const auto indexed_bn = bn + "_" + std::to_string(i);
      CHECK(value_mapping_.emplace(indexed_bn, *operand_it).second) << "indexed_bn: " << indexed_bn;
      operand_it += 1;
    }
  }
}

class ReturnAllLeaveResultPass : public ReturnAllLeaveResultPassBase<ReturnAllLeaveResultPass> {
  void runOnFunction() override {
    auto CollectNotUsedResults = [&](Operation* op) {
      for (auto result : op->getOpResults()) {
        if (result.use_empty()) {
          llvm::errs() << "use_empty: ";
          result.dump();
        }
      }
      return WalkResult::advance();
    };
    getFunction()->walk(CollectNotUsedResults);
  }
};

class CreateComputeCtxPass : public CreateComputeCtxPassBase<CreateComputeCtxPass> {
  void runOnFunction() override {
    ModuleOp top_module = getFunction()->getParentOfType<ModuleOp>();
    mlir::MLIRContext& context = getContext();
    auto jit_interpreter =
        dynamic_cast<::oneflow::one::JitInterpreter*>(::oneflow::one::GetJitInterpreter().get());
    auto importer = jit_interpreter->GetImporter();
    Builder builder(&context);
    // external func to launch kernel
    auto func_type = builder.getFunctionType(
        LLVM::LLVMPointerType::get(IntegerType::get(&context, 8)), llvm::None);
    auto function = mlir::FuncOp::create(getFunction()->getLoc(), "LaunchOneFlowKernel", func_type);
    top_module.push_back(function);
    auto CollectLowering = [&](Operation* op) {
      if (llvm::dyn_cast<mlir::oneflow::UserOp>(op) || op->hasAttr("op_type_name")) {
        mlir::oneflow::UserOpAdaptor user_op_adaptor(op->getOperands(), op->getAttrDictionary());
        llvm::errs() << "lowering op to launch kernel: ";
        user_op_adaptor.op_name().dump();
        ::oneflow::OperatorConf op_conf;
        const std::string op_name = user_op_adaptor.op_name().getValue().str();
        auto user_conf = op_conf.mutable_user_conf();
        if (succeeded(ConvertUserOpInputs(op, user_op_adaptor, user_conf))
            && succeeded(ConvertUserOpOutputs(op, user_op_adaptor, user_conf))
            && succeeded(importer.ConvertUserOpAttributes(op, user_op_adaptor, op_conf))
            && succeeded(ConvertCtrlInputs(op, op_conf))) {
          // pass
        } else {
          return WalkResult::interrupt();
        }
        auto oneflow_op = CHECK_JUST(ConstructOp(op_conf));
        std::unordered_map<std::string, mlir::Value> value_mapping_;  // "a0" => %result
        InsertLbnSegmentIntoMapping(user_op_adaptor.input_lbn_segment_keys(),
                                    user_op_adaptor.input_lbn_segment_sizes(), op->getOperands(),
                                    value_mapping_);
        InsertLbnSegmentIntoMapping(user_op_adaptor.output_lbn_segment_keys(),
                                    user_op_adaptor.output_lbn_segment_sizes(), op->getResults(),
                                    value_mapping_);
        HashMap<std::string, std::unique_ptr<BlobDesc>> lbi2logical_blob_desc_;
        static ParallelContext parallel_ctx = GetSingleDeviceParallelContext();
        auto GetBlobDesc4BnInOp = [&](const std::string& bn) -> BlobDesc* {
          if (lbi2logical_blob_desc_.find(bn) == lbi2logical_blob_desc_.end()) {
            auto value_it = value_mapping_.find(bn);
            if (value_it == value_mapping_.end()) {
              auto blob_desc = std::make_unique<BlobDesc>(DataType::kInvalidDataType);
              CHECK(lbi2logical_blob_desc_.emplace(bn, std::move(blob_desc)).second);
              if (bn != "tmp_buffer_0") {
                op->dump();
                LOG(FATAL) << "value not found in MLIR op for indexed bn: " << bn;
              }
            } else {
              auto found =
                  GetBlobDescFromMlirTensorType(value_it->second.getType().cast<TensorType>());
              CHECK(lbi2logical_blob_desc_.emplace(bn, std::move(found)).second);
            }
          }
          return lbi2logical_blob_desc_.at(bn).get();
        };
        KernelConf kernel_conf;
        oneflow_op->GenKernelConf(GetBlobDesc4BnInOp, &parallel_ctx, &kernel_conf);
      }
      return WalkResult::advance();
    };
    getFunction()->walk(CollectLowering);
  }
};

}  // namespace

namespace mlir {
namespace oneflow {

std::unique_ptr<Pass> createReturnAllLeaveResultPass() {
  return std::make_unique<ReturnAllLeaveResultPass>();
}

std::unique_ptr<Pass> createCreateComputeCtxPass() {
  return std::make_unique<CreateComputeCtxPass>();
}

}  // namespace oneflow
}  // namespace mlir

namespace oneflow {

namespace one {

namespace ir {

using namespace mlir;

OwningOpRef<ModuleOp> CreateJitModule(MLIRContext* context) {
  context->loadDialect<mlir::oneflow::OneFlowDialect>();
  context->loadDialect<StandardOpsDialect>();
  context->loadDialect<LLVM::LLVMDialect>();
  OwningOpRef<ModuleOp> module(
      ModuleOp::create(FileLineColLoc::get(context, "", /*line=*/0, /*column=*/0)));
  return module;
}

LogicalResult JitImporter::AppendDataInOperand(const std::string& key, const int32_t index,
                                               const std::string& lbn,
                                               std::vector<::mlir::Value>& operand_vec) {
  operand_vec.push_back(GetResultByBnAndIndex(key, index).getValue());
  return success();
}
LogicalResult JitImporter::AddDeviceName(const ::oneflow::OperatorConf& op,
                                         std::vector<NamedAttribute>& attr_vec) {
  const ::oneflow::ParallelConf& pc = parallel_desc_->parallel_conf();
  std::vector<llvm::StringRef> device_vec = {pc.device_name().begin(), pc.device_name().end()};
  attr_vec.push_back(
      GetBuilder().getNamedAttr("device_name", GetBuilder().getStrArrayAttr(device_vec)));
  if (pc.has_hierarchy()) {
    attr_vec.push_back(GetBuilder().getNamedAttr(
        "hierarchy",
        GetBuilder().getI64ArrayAttr({pc.hierarchy().dim().begin(), pc.hierarchy().dim().end()})));
  }
  return success();
}
Type JitImporter::GetTensorTypeOfLbn(const std::string& lbn) {
  LogicalBlobId lbi = GenLogicalBlobId(lbn);
  return result_type_mapping_.at(lbi.blob_name());
}
std::shared_ptr<Tensor> JitImporter::MakeIntermediateTensor(
    const std::string& lbn, Value result,
    const std::shared_ptr<const ParallelDesc>& parallel_desc) {
  auto tensor_type = result.getType().cast<TensorType>();
  auto dtype = DataType::kInvalidDataType;
  if (tensor_type.getElementType().isF32()) {
    dtype = DataType::kFloat;
  } else {
    GetModule().dump();
    result.dump();
    LOG(FATAL) << "fail to creat tensor";
  }
  const auto& device = CHECK_JUST(Device::MakeDeviceByParallelDesc(*parallel_desc));
  auto shape_from_mlir = new Shape({tensor_type.getShape().begin(), tensor_type.getShape().end()});
  auto shape = std::make_shared<Shape>();
  shape.reset(shape_from_mlir);
  auto tensor =
      CHECK_JUST(MirroredTensor::MakeTensor(shape, dtype, device, /* is_lazy */ true,
                                            /* requires_grad= */ false, /* is_leaf= */ true));
  auto tensor_ref = std::make_shared<TensorRef>(tensor);
  CHECK(intermediate_tensors_.insert({result, tensor_ref}).second)
      << "Intermediate tensor already created, lbn: " << lbn;
  CHECK(result_mapping_.emplace(tensor_ref.get(), result).second)
      << "Intermediate tensor already mapped to mlir value, lbn: " << lbn;
  return tensor_ref;
}
LogicalResult JitImporter::InsertOpResults(const ::oneflow::OperatorConf& op_conf,
                                           Operation* created_op) {
  auto output_lbns = created_op->getAttrOfType<ArrayAttr>("output_lbns");
  CHECK_EQ(output_lbns.size(), outputs_->size());
  for (auto data_out : llvm::enumerate(GetDataOutputResults(created_op))) {
    auto lbn = output_lbns[data_out.index()].dyn_cast<StringAttr>().getValue().str();
    auto tensor = MakeIntermediateTensor(lbn, data_out.value(), parallel_desc_);
    (*outputs_)[data_out.index()] = tensor;
  }
  return success();
}
::oneflow::AttrType JitImporter::QueryAttrType(const std::string& op_type_name,
                                               const std::string& attr_name) {
  const user_op::OpRegistryResult* val =
      user_op::UserOpRegistryMgr::Get().GetOpRegistryResult(op_type_name);
  CHECK(val) << " Cannot find op_type_name: " << op_type_name;
  user_op::UserOpDefWrapper op_def(val->op_def);
  CHECK(op_def.IsAttrName(attr_name)) << attr_name << " not a attr name for op: " << op_type_name;
  return op_def.GetAttrType(attr_name);
}

mlir::FuncOp JitImporter::GetOrInsertFunc(const std::string& func_name, const TensorTuple& inputs,
                                          TensorTuple* outputs) {
  // convert data types from oneflow
  outputs_ = outputs;
  auto result_types = llvm::SmallVector<Type, 8>();
  SymbolTable symbol_table(GetModule());
  FuncOp found_func = symbol_table.lookup<FuncOp>(func_name);
  if (found_func) {
    return found_func;
  } else {
    auto arg_tensors = GetJitForwardArgs();
    auto arg_types = llvm::SmallVector<Type, 8>();
    for (const auto& arg_tensor : arg_tensors) {
      auto mlir_dtype = GetTypeFromOneFlowDataType(arg_tensor->dtype()->data_type());
      auto mlir_tensor_type =
          RankedTensorType::get(ArrayRef<int64_t>(arg_tensor->shape()->dim_vec().begin(),
                                                  arg_tensor->shape()->dim_vec().end()),
                                mlir_dtype.getValue());
      arg_types.push_back(mlir_tensor_type);
    }
    auto func_type = GetBuilder().getFunctionType(arg_types, llvm::NoneType());
    FuncOp function = mlir::FuncOp::create(GetRootLocation(), func_name, func_type);
    auto entryBlock = function.addEntryBlock();
    CHECK_EQ(arg_tensors.size(), function.body().getArguments().size());
    for (auto argument_pair : llvm::zip(arg_tensors, function.body().getArguments())) {
      CHECK(result_mapping_.emplace(std::get<0>(argument_pair).get(), std::get<1>(argument_pair))
                .second);
    }
    GetBuilder().setInsertionPointToStart(entryBlock);
    GetModule().push_back(function);
    return function;
  }
}

llvm::Optional<TensorType> JitImporter::GetMlirTensorTypeFromBlobDesc(const BlobDesc& blob_desc) {
  if (auto t = GetTypeFromOneFlowDataType(blob_desc.data_type())) {
    return RankedTensorType::get(
        ArrayRef<int64_t>(blob_desc.shape().dim_vec().begin(), blob_desc.shape().dim_vec().end()),
        t.getValue());
  } else {
    return llvm::None;
  }
}

void JitImporter::CreateOperandMapping(const ::oneflow::OperatorConf& op_conf,
                                       const std::shared_ptr<const ParallelDesc> parallel_desc,
                                       const std::shared_ptr<const ArgTuple>& input_arg_tuple,
                                       const TensorTuple& inputs) {
  operand_mapping_.clear();
  input_arg_tuple_ = input_arg_tuple;
  inputs_ = inputs;
  result_type_mapping_.clear();
  HashMap<std::string, std::unique_ptr<BlobDesc>> lbi2logical_blob_desc_;
  auto op = CHECK_JUST(ConstructOp(op_conf));
  for (auto pair : llvm::zip(input_arg_tuple->indexed_bns(),
                             input_arg_tuple->indexed_arg_name_and_index(), inputs)) {
    const auto& indexed_bn = std::get<0>(pair);
    const auto& indexed_arg_name_and_index = std::get<1>(pair);
    const auto& tensor = std::get<2>(pair);
    if (auto result = GetResultByBnAndIndex(indexed_arg_name_and_index.first,
                                            indexed_arg_name_and_index.second)) {
      CHECK(operand_mapping_.emplace(indexed_bn, result.getValue()).second);
    } else {
      LOG(FATAL) << "result not found, indexed_bn: " << indexed_bn << ", tensor: " << tensor.get()
                 << ", shape: " << tensor->shape()->DebugStr()
                 << ", dtype: " << tensor->dtype()->name()
                 << ", op conf: " << op_conf.DebugString();
    }
  }
  // TODO: refine here
  auto GetLogicalBlobDesc4BnInOp = [&](const std::string& bn) -> BlobDesc* {
    if (lbi2logical_blob_desc_.find(bn) == lbi2logical_blob_desc_.end()) {
      auto operand_it = operand_mapping_.find(bn);
      if (operand_it == operand_mapping_.end()) {
        auto blob_desc = std::make_unique<BlobDesc>(DataType::kInvalidDataType);
        CHECK(lbi2logical_blob_desc_.emplace(bn, std::move(blob_desc)).second);
      } else {
        auto found = GetBlobDescFromMlirTensorType(operand_it->second.getType().cast<TensorType>());
        CHECK(lbi2logical_blob_desc_.emplace(bn, std::move(found)).second);
      }
    }
    return lbi2logical_blob_desc_.at(bn).get();
  };
  CHECK_JUST(op->InferLogicalOutBlobDescs(GetLogicalBlobDesc4BnInOp, *parallel_desc));
  static ParallelContext parallel_ctx = GetSingleDeviceParallelContext();
  for (auto& kv : lbi2logical_blob_desc_) {
    CHECK(
        result_type_mapping_.emplace(kv.first, GetMlirTensorTypeFromBlobDesc(*kv.second).getValue())
            .second);
  }
}

llvm::Optional<mlir::Value> JitImporter::GetResultByBnAndIndex(const std::string& bn,
                                                               const int32_t index) {
  auto idx = input_arg_tuple_->TensorTupleIndex4ArgNameAndIndex(bn, index);
  auto tensor = inputs_[idx];
  auto result_it = result_mapping_.find(tensor.get());
  if (result_it == result_mapping_.end()) {
    return llvm::None;
  } else {
    return result_it->second;
  }
}

LogicalResult JitImporter::LowerToOneFlowKernel() {
  llvm::SmallVector<Value, 8> return_values;
  llvm::SmallVector<Type, 8> out_types{};
  GetModule()
      ->walk([&](mlir::Operation* op) {
        if (llvm::dyn_cast<mlir::oneflow::UserOp>(op) || op->hasAttr("op_type_name")) {
          for (auto result : op->getOpResults()) {
            // TODO: define a control edge type
            const bool is_ctrl_edge =
                result.getType().isa<mlir::RankedTensorType>()
                && result.getType().cast<mlir::RankedTensorType>().getElementType().isInteger(1);
            if (result.use_empty() && !is_ctrl_edge) {
              auto found = intermediate_tensors_.find(result);
              if (found == intermediate_tensors_.end()) {
                result.dump();
                result.getType().dump();
                llvm::errs() << "\n";
                LOG(FATAL) << "tensor not found for result #" << result.getResultNumber();
              }
              return_tensors_.push_back(found->second);
              return_values.push_back(result);
              out_types.push_back(result.getType());
            }
          }
          return WalkResult::advance();
        } else {
          return WalkResult::advance();
        }
      })
      .wasInterrupted();
  auto return_op = GetBuilder().create<ReturnOp>(GetModule()->getLoc(), return_values);
  auto func_op = llvm::dyn_cast<mlir::FuncOp>(return_op->getParentOp());
  auto new_func_type = GetBuilder().getFunctionType(func_op.getType().getInputs(), out_types);
  func_op.setType(new_func_type);
  mlir::PassManager pm(GetModule()->getContext());
  pm.addNestedPass<mlir::FuncOp>(::mlir::createCanonicalizerPass());
  // pm.addNestedPass<mlir::FuncOp>(::mlir::oneflow::createReturnAllLeaveResultPass());
  // pm.addNestedPass<mlir::FuncOp>(::mlir::oneflow::createCreateComputeCtxPass());
  return pm.run(GetModule());
}

}  // namespace ir

}  // namespace one

}  // namespace oneflow
