// Copyright 2020 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "absl/base/casts.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "pybind11/functional.h"
#include "pybind11/pybind11.h"
#include "pybind11/stl.h"
#include "xls/common/logging/logging.h"
#include "xls/common/status/status_macros.h"
#include "xls/common/status/statusor_pybind_caster.h"
#include "xls/dslx/ast.h"
#include "xls/dslx/bytecode.h"
#include "xls/dslx/bytecode_emitter.h"
#include "xls/dslx/bytecode_interpreter.h"
#include "xls/dslx/create_import_data.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/import_routines.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/interp_value_helpers.h"
#include "xls/dslx/ir_converter.h"
#include "xls/dslx/parse_and_typecheck.h"
#include "xls/dslx/parser.h"
#include "xls/dslx/python/errors.h"
#include "xls/dslx/symbolic_bindings.h"
#include "xls/dslx/typecheck.h"
#include "xls/ir/ir_parser.h"
#include "xls/ir/python/wrapper_types.h"

namespace py = pybind11;

namespace xls::dslx {

namespace {

absl::StatusOr<std::vector<InterpValue>> RunFunctionBatched(
    const dslx::Function* f, ImportData& import_data,
    const TypecheckedModule& tm,
    const std::vector<std::vector<InterpValue>>& args_batch) {
  std::vector<std::unique_ptr<ConcreteType>> params;
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<BytecodeFunction> bf,
                       BytecodeEmitter::Emit(&import_data, tm.type_info, f,
                                             /*caller_bindings=*/{}));
  XLS_ASSIGN_OR_RETURN(FunctionType * fn_type,
                       tm.type_info->GetItemAs<FunctionType>(f));
  std::vector<InterpValue> results;
  results.reserve(args_batch.size());
  for (const std::vector<InterpValue>& unsigned_args : args_batch) {
    XLS_ASSIGN_OR_RETURN(std::vector<InterpValue> args,
                         SignConvertArgs(*fn_type, unsigned_args));
    XLS_ASSIGN_OR_RETURN(InterpValue result, BytecodeInterpreter::Interpret(
                                                 &import_data, bf.get(), args));
    results.push_back(result);
  }
  return results;
}

absl::StatusOr<std::vector<InterpValue>> RunProc(
    dslx::Proc* proc, ImportData& import_data, const TypecheckedModule& tm,
    const std::vector<std::vector<InterpValue>>& channel_values,
    const std::vector<InterpValue>& proc_initial_values) {
  XLS_ASSIGN_OR_RETURN(dslx::TypeInfo * proc_type_info,
                       tm.type_info->GetTopLevelProcTypeInfo(proc));
  std::vector<ProcInstance> proc_instances;
  std::vector<InterpValue> config_args;
  // Positional indexes of the input/output channels in the config
  // function.
  std::vector<int64_t> in_chan_indexes, out_chan_indexes;

  for (int64_t index = 0; index < proc->config()->params().size(); ++index) {
    // Currently, only channels are supported as parameters to the config
    // function of a proc.
    ChannelTypeAnnotation* channel_type = dynamic_cast<ChannelTypeAnnotation*>(
        proc->config()->params().at(index)->type_annotation());
    XLS_CHECK_NE(channel_type, nullptr);
    config_args.push_back(InterpValue::MakeChannel());
    if (channel_type->direction() == ChannelTypeAnnotation::kIn) {
      in_chan_indexes.push_back(index);
    } else if (channel_type->direction() == ChannelTypeAnnotation::kOut) {
      out_chan_indexes.push_back(index);
    }
  }
  for (const std::vector<InterpValue>& values : channel_values) {
    XLS_CHECK_EQ(in_chan_indexes.size(), values.size())
        << "The input channel count should match the args count.";
    for (int64_t index = 0; index < values.size(); ++index) {
      config_args[in_chan_indexes[index]].GetChannelOrDie()->push_back(
          values[index]);
    }
  }
  XLS_RETURN_IF_ERROR(ProcConfigBytecodeInterpreter::EvalSpawn(
      &import_data, proc_type_info, absl::nullopt, absl::nullopt, proc,
      config_args, proc_initial_values, &proc_instances));
  // Currently a single proc is supported.
  XLS_CHECK_EQ(proc_instances.size(), 1);

  std::vector<InterpValue> results;
  for (const std::vector<InterpValue>& values : channel_values) {
    XLS_RETURN_IF_ERROR(proc_instances[0].Run());
    std::vector<InterpValue> channel_results;
    for (int64_t index = 0; index < out_chan_indexes.size(); ++index) {
      std::shared_ptr<InterpValue::Channel> channel =
          config_args[out_chan_indexes[index]].GetChannelOrDie();
      // TODO(vmirian): Consider adding a 'None' InterpValue.
      if (channel->empty()) {
        continue;
      }
      channel_results.push_back(channel->front());
      channel->pop_front();
    }
    // Ideally, the result should be a tuple containing two tuples. The first
    // entry is the result of the next function, the second is the results of
    // the output channels.
    // TODO(vmirian): Collect the result from the next function.
    InterpValue next_state_result = InterpValue::MakeTuple({});
    results.push_back(InterpValue::MakeTuple(
        {next_state_result, InterpValue::MakeTuple(channel_results)}));
  }
  return results;
}

}  // namespace

PYBIND11_MODULE(interpreter, m) {
  ImportStatusModule();

  m.def(
      "run_function_batched",
      [](std::string_view text, std::string_view top_name,
         const std::vector<std::vector<InterpValue>> args_batch,
         std::string_view dslx_stdlib_path)
          -> absl::StatusOr<std::vector<InterpValue>> {
        ImportData import_data(
            CreateImportData(std::string(dslx_stdlib_path),
                             /*additional_search_paths=*/{}));
        XLS_ASSIGN_OR_RETURN(
            TypecheckedModule tm,
            ParseAndTypecheck(text, "batched.x", "batched", &import_data));

        std::optional<ModuleMember*> module_member =
            tm.module->FindMemberWithName(top_name);
        XLS_CHECK(module_member.has_value());
        ModuleMember* member = module_member.value();
        XLS_CHECK(std::holds_alternative<dslx::Function*>(*member));
        dslx::Function* f = std::get<dslx::Function*>(*member);
        return RunFunctionBatched(f, import_data, tm, args_batch);
      },
      py::arg("text"), py::arg("top_name"), py::arg("args_batch"),
      py::arg("dslx_stdlib_path"));

  m.def(
      "run_proc",
      [](std::string_view text, std::string_view top_name,
         const std::vector<std::vector<InterpValue>> args_batch,
         const std::vector<InterpValue> proc_initial_values,
         std::string_view dslx_stdlib_path)
          -> absl::StatusOr<std::vector<InterpValue>> {
        ImportData import_data(
            CreateImportData(std::string(dslx_stdlib_path),
                             /*additional_search_paths=*/{}));
        XLS_ASSIGN_OR_RETURN(
            TypecheckedModule tm,
            ParseAndTypecheck(text, "batched.x", "batched", &import_data));

        std::optional<ModuleMember*> module_member =
            tm.module->FindMemberWithName(top_name);
        XLS_CHECK(module_member.has_value());
        ModuleMember* member = module_member.value();

        XLS_CHECK(std::holds_alternative<dslx::Proc*>(*member));
        dslx::Proc* proc = std::get<dslx::Proc*>(*member);
        return RunProc(proc, import_data, tm, args_batch, proc_initial_values);
      },
      py::arg("text"), py::arg("top_name"), py::arg("args_batch"),
      py::arg("proc_initial_values"), py::arg("dslx_stdlib_path"));
}

}  // namespace xls::dslx
