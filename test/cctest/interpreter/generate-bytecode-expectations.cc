// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstring>
#include <fstream>
#include <vector>

#include "test/cctest/interpreter/bytecode-expectations-printer.h"

#include "include/libplatform/libplatform.h"
#include "include/v8.h"

#include "src/base/logging.h"
#include "src/base/smart-pointers.h"
#include "src/compiler.h"
#include "src/interpreter/interpreter.h"

#ifdef V8_OS_POSIX
#include <dirent.h>
#endif

using v8::internal::interpreter::BytecodeExpectationsPrinter;

#define REPORT_ERROR(MESSAGE) (((std::cerr << "ERROR: ") << MESSAGE) << '\n')

namespace {

#ifdef V8_OS_POSIX
const char* kGoldenFilesPath = "test/cctest/interpreter/bytecode_expectations/";
#endif

class ProgramOptions final {
 public:
  static ProgramOptions FromCommandLine(int argc, char** argv);

  ProgramOptions()
      : parsing_failed_(false),
        print_help_(false),
        read_raw_js_snippet_(false),
        read_from_stdin_(false),
        rebaseline_(false),
        wrap_(true),
        execute_(true),
        top_level_(false),
        do_expressions_(false),
        ignition_generators_(false),
        verbose_(false),
        const_pool_type_(
            BytecodeExpectationsPrinter::ConstantPoolType::kMixed) {}

  bool Validate() const;
  void UpdateFromHeader(std::istream& stream);   // NOLINT
  void PrintHeader(std::ostream& stream) const;  // NOLINT

  bool parsing_failed() const { return parsing_failed_; }
  bool print_help() const { return print_help_; }
  bool read_raw_js_snippet() const { return read_raw_js_snippet_; }
  bool read_from_stdin() const { return read_from_stdin_; }
  bool write_to_stdout() const {
    return output_filename_.empty() && !rebaseline_;
  }
  bool rebaseline() const { return rebaseline_; }
  bool wrap() const { return wrap_; }
  bool execute() const { return execute_; }
  bool top_level() const { return top_level_; }
  bool do_expressions() const { return do_expressions_; }
  bool ignition_generators() const { return ignition_generators_; }
  bool verbose() const { return verbose_; }
  bool suppress_runtime_errors() const { return rebaseline_ && !verbose_; }
  BytecodeExpectationsPrinter::ConstantPoolType const_pool_type() const {
    return const_pool_type_;
  }
  std::vector<std::string> input_filenames() const { return input_filenames_; }
  std::string output_filename() const { return output_filename_; }
  std::string test_function_name() const { return test_function_name_; }

 private:
  bool parsing_failed_;
  bool print_help_;
  bool read_raw_js_snippet_;
  bool read_from_stdin_;
  bool rebaseline_;
  bool wrap_;
  bool execute_;
  bool top_level_;
  bool do_expressions_;
  bool ignition_generators_;
  bool verbose_;
  BytecodeExpectationsPrinter::ConstantPoolType const_pool_type_;
  std::vector<std::string> input_filenames_;
  std::string output_filename_;
  std::string test_function_name_;
};

class ArrayBufferAllocator final : public v8::ArrayBuffer::Allocator {
 public:
  void* Allocate(size_t length) override {
    void* data = AllocateUninitialized(length);
    if (data != nullptr) memset(data, 0, length);
    return data;
  }
  void* AllocateUninitialized(size_t length) override { return malloc(length); }
  void Free(void* data, size_t) override { free(data); }
};

class V8InitializationScope final {
 public:
  explicit V8InitializationScope(const char* exec_path);
  ~V8InitializationScope();

  v8::Platform* platform() const { return platform_.get(); }
  v8::Isolate* isolate() const { return isolate_; }

 private:
  v8::base::SmartPointer<v8::Platform> platform_;
  v8::Isolate* isolate_;

  DISALLOW_COPY_AND_ASSIGN(V8InitializationScope);
};

BytecodeExpectationsPrinter::ConstantPoolType ParseConstantPoolType(
    const char* type_string) {
  if (strcmp(type_string, "number") == 0) {
    return BytecodeExpectationsPrinter::ConstantPoolType::kNumber;
  } else if (strcmp(type_string, "string") == 0) {
    return BytecodeExpectationsPrinter::ConstantPoolType::kString;
  } else if (strcmp(type_string, "mixed") == 0) {
    return BytecodeExpectationsPrinter::ConstantPoolType::kMixed;
  }
  return BytecodeExpectationsPrinter::ConstantPoolType::kUnknown;
}

const char* ConstantPoolTypeToString(
    BytecodeExpectationsPrinter::ConstantPoolType type) {
  switch (type) {
    case BytecodeExpectationsPrinter::ConstantPoolType::kNumber:
      return "number";
    case BytecodeExpectationsPrinter::ConstantPoolType::kMixed:
      return "mixed";
    case BytecodeExpectationsPrinter::ConstantPoolType::kString:
      return "string";
    default:
      UNREACHABLE();
      return nullptr;
  }
}

bool ParseBoolean(const char* string) {
  if (strcmp(string, "yes") == 0) {
    return true;
  } else if (strcmp(string, "no") == 0) {
    return false;
  } else {
    UNREACHABLE();
    return false;
  }
}

const char* BooleanToString(bool value) { return value ? "yes" : "no"; }

#ifdef V8_OS_POSIX

bool StrEndsWith(const char* string, const char* suffix) {
  int string_size = i::StrLength(string);
  int suffix_size = i::StrLength(suffix);
  if (string_size < suffix_size) return false;

  return strcmp(string + (string_size - suffix_size), suffix) == 0;
}

bool CollectGoldenFiles(std::vector<std::string>* golden_file_list,
                        const char* directory_path) {
  DIR* directory = opendir(directory_path);
  if (!directory) return false;

  dirent entry_buffer;
  dirent* entry;

  while (readdir_r(directory, &entry_buffer, &entry) == 0 && entry) {
    if (StrEndsWith(entry->d_name, ".golden")) {
      std::string golden_filename(kGoldenFilesPath);
      golden_filename += entry->d_name;
      golden_file_list->push_back(golden_filename);
    }
  }

  closedir(directory);

  return true;
}

#endif  // V8_OS_POSIX

// static
ProgramOptions ProgramOptions::FromCommandLine(int argc, char** argv) {
  ProgramOptions options;

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--help") == 0) {
      options.print_help_ = true;
    } else if (strcmp(argv[i], "--raw-js") == 0) {
      options.read_raw_js_snippet_ = true;
    } else if (strncmp(argv[i], "--pool-type=", 12) == 0) {
      options.const_pool_type_ = ParseConstantPoolType(argv[i] + 12);
    } else if (strcmp(argv[i], "--stdin") == 0) {
      options.read_from_stdin_ = true;
    } else if (strcmp(argv[i], "--rebaseline") == 0) {
      options.rebaseline_ = true;
    } else if (strcmp(argv[i], "--no-wrap") == 0) {
      options.wrap_ = false;
    } else if (strcmp(argv[i], "--no-execute") == 0) {
      options.execute_ = false;
    } else if (strcmp(argv[i], "--top-level") == 0) {
      options.top_level_ = true;
    } else if (strcmp(argv[i], "--do-expressions") == 0) {
      options.do_expressions_ = true;
    } else if (strcmp(argv[i], "--ignition-generators") == 0) {
      options.ignition_generators_ = true;
    } else if (strcmp(argv[i], "--verbose") == 0) {
      options.verbose_ = true;
    } else if (strncmp(argv[i], "--output=", 9) == 0) {
      options.output_filename_ = argv[i] + 9;
    } else if (strncmp(argv[i], "--test-function-name=", 21) == 0) {
      options.test_function_name_ = argv[i] + 21;
    } else if (strncmp(argv[i], "--", 2) != 0) {  // It doesn't start with --
      options.input_filenames_.push_back(argv[i]);
    } else {
      REPORT_ERROR("Unknown option " << argv[i]);
      options.parsing_failed_ = true;
      break;
    }
  }

  if (options.rebaseline_ && options.input_filenames_.empty()) {
#ifdef V8_OS_POSIX
    if (options.verbose_) {
      std::cout << "Looking for golden files in " << kGoldenFilesPath << '\n';
    }
    if (!CollectGoldenFiles(&options.input_filenames_, kGoldenFilesPath)) {
      REPORT_ERROR("Golden files autodiscovery failed.");
      options.parsing_failed_ = true;
    }
#else
    REPORT_ERROR("Golden files autodiscovery requires a POSIX OS, sorry.");
    options.parsing_failed_ = true;
#endif
  }

  return options;
}

bool ProgramOptions::Validate() const {
  if (parsing_failed_) return false;
  if (print_help_) return true;

  if (const_pool_type_ ==
      BytecodeExpectationsPrinter::ConstantPoolType::kUnknown) {
    REPORT_ERROR("Unknown constant pool type.");
    return false;
  }

  if (!read_from_stdin_ && input_filenames_.empty()) {
    REPORT_ERROR("No input file specified.");
    return false;
  }

  if (read_from_stdin_ && !input_filenames_.empty()) {
    REPORT_ERROR("Reading from stdin, but input files supplied.");
    return false;
  }

  if (rebaseline_ && read_raw_js_snippet_) {
    REPORT_ERROR("Cannot use --rebaseline on a raw JS snippet.");
    return false;
  }

  if (rebaseline_ && !output_filename_.empty()) {
    REPORT_ERROR("Output file cannot be specified together with --rebaseline.");
    return false;
  }

  if (rebaseline_ && read_from_stdin_) {
    REPORT_ERROR("Cannot --rebaseline when input is --stdin.");
    return false;
  }

  if (input_filenames_.size() > 1 && !rebaseline_ && !read_raw_js_snippet()) {
    REPORT_ERROR(
        "Multiple input files, but no --rebaseline or --raw-js specified.");
    return false;
  }

  if (top_level_ && !test_function_name_.empty()) {
    REPORT_ERROR(
        "Test function name specified while processing top level code.");
    return false;
  }

  return true;
}

void ProgramOptions::UpdateFromHeader(std::istream& stream) {
  std::string line;

  // Skip to the beginning of the options header
  while (std::getline(stream, line)) {
    if (line == "---") break;
  }

  while (std::getline(stream, line)) {
    if (line.compare(0, 11, "pool type: ") == 0) {
      const_pool_type_ = ParseConstantPoolType(line.c_str() + 11);
    } else if (line.compare(0, 9, "execute: ") == 0) {
      execute_ = ParseBoolean(line.c_str() + 9);
    } else if (line.compare(0, 6, "wrap: ") == 0) {
      wrap_ = ParseBoolean(line.c_str() + 6);
    } else if (line.compare(0, 20, "test function name: ") == 0) {
      test_function_name_ = line.c_str() + 20;
    } else if (line.compare(0, 11, "top level: ") == 0) {
      top_level_ = ParseBoolean(line.c_str() + 11);
    } else if (line.compare(0, 16, "do expressions: ") == 0) {
      do_expressions_ = ParseBoolean(line.c_str() + 16);
    } else if (line.compare(0, 21, "ignition generators: ") == 0) {
      ignition_generators_ = ParseBoolean(line.c_str() + 21);
    } else if (line == "---") {
      break;
    } else if (line.empty()) {
      continue;
    } else {
      UNREACHABLE();
      return;
    }
  }
}

void ProgramOptions::PrintHeader(std::ostream& stream) const {  // NOLINT
  stream << "---"
            "\npool type: "
         << ConstantPoolTypeToString(const_pool_type_)
         << "\nexecute: " << BooleanToString(execute_)
         << "\nwrap: " << BooleanToString(wrap_);

  if (!test_function_name_.empty()) {
    stream << "\ntest function name: " << test_function_name_;
  }

  if (top_level_) stream << "\ntop level: yes";
  if (do_expressions_) stream << "\ndo expressions: yes";
  if (ignition_generators_) stream << "\nignition generators: yes";

  stream << "\n\n";
}

V8InitializationScope::V8InitializationScope(const char* exec_path)
    : platform_(v8::platform::CreateDefaultPlatform()) {
  i::FLAG_ignition = true;
  i::FLAG_always_opt = false;
  i::FLAG_allow_natives_syntax = true;

  v8::V8::InitializeICUDefaultLocation(exec_path);
  v8::V8::InitializeExternalStartupData(exec_path);
  v8::V8::InitializePlatform(platform_.get());
  v8::V8::Initialize();

  ArrayBufferAllocator allocator;
  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = &allocator;

  isolate_ = v8::Isolate::New(create_params);
}

V8InitializationScope::~V8InitializationScope() {
  isolate_->Dispose();
  v8::V8::Dispose();
  v8::V8::ShutdownPlatform();
}

std::string ReadRawJSSnippet(std::istream& stream) {  // NOLINT
  std::stringstream body_buffer;
  CHECK(body_buffer << stream.rdbuf());
  return body_buffer.str();
}

bool ReadNextSnippet(std::istream& stream, std::string* string_out) {  // NOLINT
  std::string line;
  bool found_begin_snippet = false;
  string_out->clear();
  while (std::getline(stream, line)) {
    if (line == "snippet: \"") {
      found_begin_snippet = true;
      continue;
    }
    if (!found_begin_snippet) continue;
    if (line == "\"") return true;
    CHECK_GE(line.size(), 2u);  // We should have the indent
    string_out->append(line.begin() + 2, line.end());
    *string_out += '\n';
  }
  return false;
}

std::string UnescapeString(const std::string& escaped_string) {
  std::string unescaped_string;
  bool previous_was_backslash = false;
  for (char c : escaped_string) {
    if (previous_was_backslash) {
      // If it was not an escape sequence, emit the previous backslash
      if (c != '\\' && c != '"') unescaped_string += '\\';
      unescaped_string += c;
      previous_was_backslash = false;
    } else {
      if (c == '\\') {
        previous_was_backslash = true;
        // Defer emission to the point where we can check if it was an escape.
      } else {
        unescaped_string += c;
      }
    }
  }
  return unescaped_string;
}

void ExtractSnippets(std::vector<std::string>* snippet_list,
                     std::istream& body_stream,  // NOLINT
                     bool read_raw_js_snippet) {
  if (read_raw_js_snippet) {
    snippet_list->push_back(ReadRawJSSnippet(body_stream));
  } else {
    std::string snippet;
    while (ReadNextSnippet(body_stream, &snippet)) {
      snippet_list->push_back(UnescapeString(snippet));
    }
  }
}

void GenerateExpectationsFile(std::ostream& stream,  // NOLINT
                              const std::vector<std::string>& snippet_list,
                              const V8InitializationScope& platform,
                              const ProgramOptions& options) {
  v8::Isolate::Scope isolate_scope(platform.isolate());
  v8::HandleScope handle_scope(platform.isolate());
  v8::Local<v8::Context> context = v8::Context::New(platform.isolate());
  v8::Context::Scope context_scope(context);

  BytecodeExpectationsPrinter printer(platform.isolate(),
                                      options.const_pool_type());
  printer.set_wrap(options.wrap());
  printer.set_execute(options.execute());
  printer.set_top_level(options.top_level());
  if (!options.test_function_name().empty()) {
    printer.set_test_function_name(options.test_function_name());
  }

  if (options.do_expressions()) i::FLAG_harmony_do_expressions = true;
  if (options.ignition_generators()) i::FLAG_ignition_generators = true;

  stream << "#\n# Autogenerated by generate-bytecode-expectations.\n#\n\n";
  options.PrintHeader(stream);
  for (const std::string& snippet : snippet_list) {
    printer.PrintExpectation(stream, snippet);
  }

  i::FLAG_harmony_do_expressions = false;
}

bool WriteExpectationsFile(const std::vector<std::string>& snippet_list,
                           const V8InitializationScope& platform,
                           const ProgramOptions& options,
                           const std::string& output_filename) {
  std::ofstream output_file_handle;
  if (!options.write_to_stdout()) {
    output_file_handle.open(output_filename.c_str());
    if (!output_file_handle.is_open()) {
      REPORT_ERROR("Could not open " << output_filename << " for writing.");
      return false;
    }
  }
  std::ostream& output_stream =
      options.write_to_stdout() ? std::cout : output_file_handle;

  GenerateExpectationsFile(output_stream, snippet_list, platform, options);

  return true;
}

void PrintMessage(v8::Local<v8::Message> message, v8::Local<v8::Value>) {
  std::cerr << "INFO: " << *v8::String::Utf8Value(message->Get()) << '\n';
}

void DiscardMessage(v8::Local<v8::Message>, v8::Local<v8::Value>) {}

void PrintUsage(const char* exec_path) {
  std::cerr
      << "\nUsage: " << exec_path
      << " [OPTIONS]... [INPUT FILES]...\n\n"
         "Options:\n"
         "  --help    Print this help message.\n"
         "  --verbose Emit messages about the progress of the tool.\n"
         "  --raw-js  Read raw JavaScript, instead of the output format.\n"
         "  --stdin   Read from standard input instead of file.\n"
         "  --rebaseline  Rebaseline input snippet file.\n"
         "  --no-wrap     Do not wrap the snippet in a function.\n"
         "  --no-execute  Do not execute after compilation.\n"
         "  --test-function-name=foo  "
         "Specify the name of the test function.\n"
         "  --top-level   Process top level code, not the top-level function.\n"
         "  --do-expressions  Enable harmony_do_expressions flag.\n"
         "  --ignition-generators  Enable ignition_generators flag.\n"
         "  --output=file.name\n"
         "      Specify the output file. If not specified, output goes to "
         "stdout.\n"
         "  --pool-type=(number|string|mixed)\n"
         "      Specify the type of the entries in the constant pool "
         "(default: mixed).\n"
         "\n"
         "When using --rebaseline, flags --no-wrap, --no-execute, "
         "--test-function-name\nand --pool-type will be overridden by the "
         "options specified in the input file\nheader.\n\n"
         "Each raw JavaScript file is interpreted as a single snippet.\n\n"
         "This tool is intended as a help in writing tests.\n"
         "Please, DO NOT blindly copy and paste the output "
         "into the test suite.\n";
}

}  // namespace

int main(int argc, char** argv) {
  ProgramOptions options = ProgramOptions::FromCommandLine(argc, argv);

  if (!options.Validate() || options.print_help()) {
    PrintUsage(argv[0]);
    return options.print_help() ? 0 : 1;
  }

  V8InitializationScope platform(argv[0]);
  platform.isolate()->AddMessageListener(
      options.suppress_runtime_errors() ? DiscardMessage : PrintMessage);

  std::vector<std::string> snippet_list;

  if (options.read_from_stdin()) {
    // Rebaseline will never get here, so we will always take the
    // GenerateExpectationsFile at the end of this function.
    DCHECK(!options.rebaseline());
    ExtractSnippets(&snippet_list, std::cin, options.read_raw_js_snippet());
  } else {
    for (const std::string& input_filename : options.input_filenames()) {
      if (options.verbose()) {
        std::cerr << "Processing " << input_filename << '\n';
      }

      std::ifstream input_stream(input_filename.c_str());
      if (!input_stream.is_open()) {
        REPORT_ERROR("Could not open " << input_filename << " for reading.");
        return 2;
      }

      ProgramOptions updated_options = options;
      if (options.rebaseline()) {
        updated_options.UpdateFromHeader(input_stream);
        CHECK(updated_options.Validate());
      }

      ExtractSnippets(&snippet_list, input_stream,
                      options.read_raw_js_snippet());

      if (options.rebaseline()) {
        if (!WriteExpectationsFile(snippet_list, platform, updated_options,
                                   input_filename)) {
          return 3;
        }
        snippet_list.clear();
      }
    }
  }

  if (!options.rebaseline()) {
    if (!WriteExpectationsFile(snippet_list, platform, options,
                               options.output_filename())) {
      return 3;
    }
  }
}
