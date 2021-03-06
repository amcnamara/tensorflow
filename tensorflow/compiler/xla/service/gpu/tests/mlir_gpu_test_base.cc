/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/xla/service/gpu/tests/mlir_gpu_test_base.h"

#include "llvm/IR/LLVMContext.h"
#include "mlir/InitAllDialects.h"  // from @llvm-project
#include "mlir/Parser.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/hlo/include/mlir-hlo/Dialect/mhlo/IR/register.h"
#include "tensorflow/compiler/mlir/xla/type_to_shape.h"
#include "tensorflow/compiler/xla/debug_options_flags.h"
#include "tensorflow/compiler/xla/service/gpu/gpu_compiler.h"

namespace xla {
namespace gpu {

MlirGpuTestBase::MlirGpuTestBase() {
  se::Platform* platform =
      se::MultiPlatformManager::PlatformWithName("cuda").ConsumeValueOrDie();
  BackendOptions options;
  options.set_platform(platform);
  backend_ = xla::Backend::CreateBackend(options).ConsumeValueOrDie();
}

StatusOr<ExecutionOutput> MlirGpuTestBase::RunMlirModule(
    mlir::ModuleOp module, se::Stream* stream,
    absl::Span<const se::DeviceMemoryBase> arguments) {
  llvm::LLVMContext llvm_context;
  auto llvm_module = absl::make_unique<llvm::Module>("", llvm_context);
  llvm_module->setTargetTriple("nvptx");

  se::StreamExecutor* stream_exec = stream->parent();
  GpuDeviceInfo gpu_device_info = GetGpuDeviceInfo(stream_exec);

  absl::optional<CudaComputeCapability> cuda_compute_capability =
      [&]() -> absl::optional<CudaComputeCapability> {
    CudaComputeCapability cuda_compute_capability;
    stream_exec->GetDeviceDescription().cuda_compute_capability(
        &cuda_compute_capability.cc_major, &cuda_compute_capability.cc_minor);
    if (cuda_compute_capability.cc_major == -1) {
      return absl::nullopt;
    }
    return cuda_compute_capability;
  }();

  IrEmitterContext ir_emitter_context(
      /*hlo_module=*/nullptr, /*buffer_assignment=*/nullptr,
      backend_->platform()->Name(), gpu_device_info, cuda_compute_capability,
      /*profile_index_map=*/nullptr, /*mlir_context=*/nullptr,
      llvm_module.get());

  HloModuleConfig module_config;
  module_config.set_debug_options(DefaultDebugOptionsIgnoringFlags());
  TF_ASSIGN_OR_RETURN(
      auto executable,
      CompileLmhloToExecutable(static_cast<GpuCompiler*>(backend_->compiler()),
                               module, "TestModule", module_config,
                               Compiler::CompileOptions(), "main", stream_exec,
                               std::move(llvm_module), &ir_emitter_context));

  ExecutableRunOptions executable_run_options;
  executable_run_options.set_stream(stream);
  executable_run_options.set_allocator(backend_->memory_allocator());
  ServiceExecutableRunOptions run_options(executable_run_options);
  std::vector<ExecutionInput> execution_inputs;

  for (auto arg : arguments) {
    Shape shape =
        ShapeUtil::MakeShape(xla::U8, {static_cast<int64>(arg.size())});
    execution_inputs.emplace_back(shape);
    execution_inputs.back().SetBuffer({}, MaybeOwningDeviceMemory(arg));
  }

  TF_ASSIGN_OR_RETURN(auto output,
                      executable->ExecuteAsyncOnStream(
                          &run_options, std::move(execution_inputs),
                          /*hlo_execution_profile=*/nullptr));

  TF_CHECK_OK(stream->BlockHostUntilDone());

  return std::move(output);
}

StatusOr<std::vector<std::vector<uint8>>>
MlirGpuTestBase::RunMlirModuleWithHostBuffers(
    mlir::ModuleOp module, std::vector<absl::Span<uint8>> arguments) {
  auto* allocator = backend_->memory_allocator();
  std::vector<se::OwningDeviceMemory> owning_memory;
  owning_memory.reserve(arguments.size());
  for (auto host_buffer : arguments) {
    owning_memory.push_back(
        allocator
            ->Allocate(backend_->default_device_ordinal(), host_buffer.size())
            .ConsumeValueOrDie());
  }
  auto stream = backend_->BorrowStream(backend_->default_device_ordinal())
                    .ConsumeValueOrDie();
  std::vector<se::DeviceMemoryBase> args;
  for (int i = 0; i < owning_memory.size(); i++) {
    se::DeviceMemoryBase memory(*owning_memory[i]);
    stream->ThenMemcpy(&memory, static_cast<void*>(arguments[i].data()),
                       memory.size());
    args.push_back(memory);
  }
  TF_ASSIGN_OR_RETURN(ExecutionOutput output,
                      RunMlirModule(module, stream.get(), args));

  std::vector<std::vector<uint8>> host_outputs;
  for (const auto& result : output.Result().buffers().leaves()) {
    host_outputs.emplace_back();
    host_outputs.back().resize(result.second.size());
    stream->ThenMemcpy(static_cast<void*>(host_outputs.back().data()),
                       result.second, result.second.size());
  }
  TF_CHECK_OK(stream->BlockHostUntilDone());
  return host_outputs;
}

StatusOr<std::vector<std::vector<uint8>>>
MlirGpuTestBase::RunMlirTextWithHostBuffers(
    absl::string_view module_text, std::vector<absl::Span<uint8>> arguments) {
  mlir::MLIRContext context;
  context.loadDialect<mlir::lmhlo::LmhloDialect, mlir::mhlo::MhloDialect,
                      mlir::StandardOpsDialect,
                      mlir::lmhlo_gpu::LmhloGpuDialect>();

  mlir::OwningModuleRef module = parseSourceString(
      llvm::StringRef(module_text.data(), module_text.size()), &context);
  CHECK(module);
  return RunMlirModuleWithHostBuffers(*module, arguments);
}

}  // namespace gpu
}  // namespace xla
