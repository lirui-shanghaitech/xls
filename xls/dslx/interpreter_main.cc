// Copyright 2021 The XLS Authors
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

#include <time.h>
#include <unistd.h>

#include "absl/flags/flag.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "xls/common/file/filesystem.h"
#include "xls/common/init_xls.h"
#include "xls/dslx/builtins.h"
#include "xls/dslx/command_line_utils.h"
#include "xls/dslx/error_printer.h"
#include "xls/dslx/interpreter.h"
#include "xls/dslx/ir_converter.h"
#include "xls/dslx/parse_and_typecheck.h"
#include "xls/dslx/parser.h"
#include "xls/dslx/scanner.h"
#include "xls/dslx/typecheck.h"
#include "xls/jit/ir_jit.h"

ABSL_FLAG(std::string, dslx_path, "",
          "Additional paths to search for modules (colon delimited).");
ABSL_FLAG(bool, trace_all, false, "Trace every expression.");
ABSL_FLAG(bool, compare_jit, true,
          "Compare interpreted and JIT execution of each function.");
ABSL_FLAG(
    int64_t, seed, 0,
    "Seed for quickcheck random stimulus; 0 for an nondetermistic value.");
// TODO(leary): 2021-01-19 allow filters with wildcards.
ABSL_FLAG(std::string, test_filter, "",
          "Target (currently *single*) test name to run.");

namespace xls::dslx {
namespace {

const char* kUsage = R"(
Parses, typechecks, and executes all tests inside of a DSLX module.
)";

bool TestMatchesFilter(absl::string_view test_name,
                       absl::optional<absl::string_view> test_filter) {
  if (!test_filter.has_value()) {
    return true;
  }
  // TODO(leary): 2019-08-28 Implement wildcards.
  return test_name == *test_filter;
}

absl::Status RunQuickCheck(Interpreter* interp, Package* ir_package,
                           QuickCheck* quickcheck, int64_t seed) {
  Function* fn = quickcheck->f();
  XLS_ASSIGN_OR_RETURN(
      std::string ir_name,
      MangleDslxName(fn->identifier(), fn->GetFreeParametricKeySet(),
                     interp->entry_module()));
  XLS_ASSIGN_OR_RETURN(xls::Function * ir_function,
                       ir_package->GetFunction(ir_name));

  using ResultT =
      std::pair<std::vector<std::vector<Value>>, std::vector<Value>>;
  XLS_ASSIGN_OR_RETURN(
      ResultT result,
      CreateAndQuickCheck(ir_function, seed, quickcheck->test_count()));
  const auto& [argsets, results] = result;
  XLS_ASSIGN_OR_RETURN(Bits last_result, results.back().GetBitsWithStatus());
  if (!last_result.IsZero()) {
    // Did not find a falsifying example.
    return absl::OkStatus();
  }

  XLS_RET_CHECK_EQ(interp->current_type_info()->module(),
                   interp->entry_module());
  const std::vector<Value>& last_argset = argsets.back();
  XLS_ASSIGN_OR_RETURN(
      FunctionType * fn_type,
      interp->current_type_info()->GetItemAs<FunctionType>(fn));
  const std::vector<std::unique_ptr<ConcreteType>>& params = fn_type->params();

  std::vector<InterpValue> dslx_argset;
  for (int64_t i = 0; i < params.size(); ++i) {
    const ConcreteType& arg_type = *params[i];
    const Value& value = last_argset[i];
    XLS_ASSIGN_OR_RETURN(InterpValue interp_value,
                         ValueToInterpValue(value, &arg_type));
    dslx_argset.push_back(interp_value);
  }
  std::string dslx_argset_str = absl::StrJoin(
      dslx_argset, ", ", [](std::string* out, const InterpValue& v) {
        absl::StrAppend(out, v.ToString());
      });
  return FailureErrorStatus(
      fn->span(),
      absl::StrFormat("Found falsifying example after %d tests: [%s]",
                      results.size(), dslx_argset_str));
}

// Parses program and run all tests contained inside.
//
// Args:
//   program: The program text to parse.
//   module_name: Name for the module.
//   filename: The filename from which "program" text originates.
//   dslx_paths: Additional paths at which we search for imported module files.
//   test_filter: Test filter specification (e.g. as passed from bazel test
//     environment).
//   trace_all: Whether or not to trace all expressions.
//   compare_jit: Whether or not to assert equality between interpreted and
//     JIT'd function return values.
//   seed: Seed for QuickCheck random input stimulus.
//
// Returns:
//   Whether any test failed (as a boolean).
absl::StatusOr<bool> ParseAndTest(
    absl::string_view program, absl::string_view module_name,
    absl::string_view filename, absl::Span<const std::string> dslx_paths,
    absl::optional<absl::string_view> test_filter = absl::nullopt,
    bool trace_all = false, bool compare_jit = true,
    absl::optional<int64_t> seed = absl::nullopt) {
  int64_t ran = 0;
  int64_t failed = 0;
  int64_t skipped = 0;

  constexpr int kUnitSpaces = 7;
  constexpr int kQuickcheckSpaces = 15;
  auto handle_error = [&](const absl::Status& status,
                          absl::string_view test_name, bool is_quickcheck) {
    XLS_VLOG(1) << "Handling error; status: " << status
                << " test_name: " << test_name;
    absl::StatusOr<PositionalErrorData> data_or =
        GetPositionalErrorData(status);
    std::string suffix;
    if (data_or.ok()) {
      const auto& data = data_or.value();
      XLS_CHECK_OK(PrintPositionalError(data.span, data.GetMessageWithType(),
                                        std::cerr));
    } else {
      // If we can't extract positional data we log the error and put the error
      // status into the "failed" prompted.
      XLS_LOG(ERROR) << "Internal error: " << status;
      suffix = absl::StrCat(": internal error: ", status.ToString());
    }
    std::string spaces((is_quickcheck ? kQuickcheckSpaces : kUnitSpaces), ' ');
    std::cerr << absl::StreamFormat("[ %sFAILED ] %s%s", spaces, test_name,
                                    suffix)
              << std::endl;
    failed += 1;
  };

  ImportData import_data;
  absl::StatusOr<TypecheckedModule> tm_or = ParseAndTypecheck(
      program, filename, module_name, &import_data, dslx_paths);
  if (!tm_or.ok()) {
    if (TryPrintError(tm_or.status())) {
      return true;
    }
    return tm_or.status();
  }
  Module* entry_module = tm_or.value().module;

  std::unique_ptr<Package> ir_package;
  if (compare_jit) {
    XLS_ASSIGN_OR_RETURN(ir_package,
                         ConvertModuleToPackage(entry_module, &import_data,
                                                /*emit_positions=*/true,
                                                /*traverse_tests=*/true));
  }

  auto typecheck_callback = [&import_data, &dslx_paths](Module* module) {
    return CheckModule(module, &import_data, dslx_paths);
  };

  Interpreter interpreter(entry_module, typecheck_callback, dslx_paths,
                          &import_data, /*trace_all=*/trace_all,
                          /*ir_package=*/ir_package.get());

  // Run unit tests.
  for (const std::string& test_name : entry_module->GetTestNames()) {
    if (!TestMatchesFilter(test_name, test_filter)) {
      skipped += 1;
      continue;
    }

    ran += 1;
    std::cerr << "[ RUN UNITTEST  ] " << test_name << std::endl;
    absl::Status status = interpreter.RunTest(test_name);
    if (status.ok()) {
      std::cerr << "[            OK ]" << std::endl;
    } else {
      handle_error(status, test_name, /*is_quickcheck=*/false);
    }
  }

  std::cerr << absl::StreamFormat(
                   "[===============] %d test(s) ran; %d failed; %d skipped.",
                   ran, failed, skipped)
            << std::endl;

  // Run quickchecks.
  if (ir_package != nullptr && !entry_module->GetQuickChecks().empty()) {
    if (!seed.has_value()) {
      // Note: we *want* to *provide* non-determinism by default. See
      // https://abseil.io/docs/cpp/guides/random#stability-of-generated-sequences
      // for rationale.
      seed =
          static_cast<int64_t>(getpid()) * static_cast<int64_t>(time(nullptr));
    }
    std::cerr << absl::StreamFormat("[ SEED %*d ]", kQuickcheckSpaces + 1,
                                    *seed)
              << std::endl;
    for (QuickCheck* quickcheck : entry_module->GetQuickChecks()) {
      const std::string& test_name = quickcheck->identifier();
      std::cerr << "[ RUN QUICKCHECK        ] " << test_name
                << " count: " << quickcheck->test_count() << std::endl;
      absl::Status status =
          RunQuickCheck(&interpreter, ir_package.get(), quickcheck, *seed);
      if (!status.ok()) {
        handle_error(status, test_name, /*is_quickcheck=*/true);
      } else {
        std::cerr << "[                    OK ] " << test_name << std::endl;
      }
    }
    std::cerr << absl::StreamFormat(
                     "[=======================] %d quickcheck(s) ran.",
                     entry_module->GetQuickChecks().size())
              << std::endl;
  }

  return failed != 0;
}

absl::Status RealMain(absl::string_view entry_module_path,
                      absl::Span<const std::string> dslx_paths,
                      absl::optional<std::string> test_filter, bool trace_all,
                      bool compare_jit, absl::optional<int64_t> seed,
                      bool* printed_error) {
  XLS_ASSIGN_OR_RETURN(std::string program, GetFileContents(entry_module_path));
  XLS_ASSIGN_OR_RETURN(std::string module_name, PathToName(entry_module_path));
  XLS_ASSIGN_OR_RETURN(
      *printed_error,
      ParseAndTest(program, module_name, entry_module_path, dslx_paths,
                   test_filter, trace_all, compare_jit, seed));
  return absl::OkStatus();
}

}  // namespace
}  // namespace xls::dslx

int main(int argc, char* argv[]) {
  std::vector<absl::string_view> args =
      xls::InitXls(xls::dslx::kUsage, argc, argv);
  if (args.empty()) {
    XLS_LOG(QFATAL) << "Wrong number of command-line arguments; got "
                    << args.size() << ": `" << absl::StrJoin(args, " ")
                    << "`; want " << argv[0] << " <input-file>";
  }
  std::string dslx_path = absl::GetFlag(FLAGS_dslx_path);
  std::vector<std::string> dslx_paths = absl::StrSplit(dslx_path, ':');

  bool trace_all = absl::GetFlag(FLAGS_trace_all);
  bool compare_jit = absl::GetFlag(FLAGS_compare_jit);

  // Optional seed value.
  absl::optional<int64_t> seed;
  if (int64_t seed_flag_value = absl::GetFlag(FLAGS_seed);
      seed_flag_value != 0) {
    seed = seed_flag_value;
  }

  // Optional test filter.
  absl::optional<std::string> test_filter;
  if (std::string flag = absl::GetFlag(FLAGS_test_filter); !flag.empty()) {
    test_filter = std::move(flag);
  }

  bool printed_error = false;
  absl::Status status =
      xls::dslx::RealMain(args[0], dslx_paths, test_filter, trace_all,
                          compare_jit, seed, &printed_error);
  if (printed_error) {
    return EXIT_FAILURE;
  }
  XLS_QCHECK_OK(status);
  return EXIT_SUCCESS;
}
