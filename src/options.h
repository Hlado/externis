#pragma once

#include <chrono>
#include <filesystem>
#include <string_view>

struct plugin_name_args;

namespace insight {

struct Options {
  std::filesystem::path combined;
  std::chrono::microseconds durationTreshold{1000};
  bool basicProfiling{false};
  bool noBackend{false};
  bool noIndividual{false};
  bool noParser{false};
  bool noPreprocessor{false};
  bool noUnit{false};
  bool withMacros{false};
};


Options parseOptions(const plugin_name_args &args);

constexpr std::string_view getHelpText()
{
  return "Plugin to allow profiling compilation.\n"
         "\n"
         "Usage:\n"
         "  gcc -fplugin=insight [-fplugin-arg-insight-<option>...] <gcc-args>\n"
         "\n"
         "Options:\n"
         "  basic-profiling  Disable detailed profiling, only top level stages are"
         "                   measured\n"
         "  combined=<path>  Path to file used to accumulate a multi-file trace. Each\n"
         "                   individual trace is appended to the end of this file.\n"
         "  duration-treshold=<microseconds>\n"
         "                   Minimal event duration to be included separately (1000 by\n"
         "                   default). Zero value disables this filter.\n"
         "  no-backend       Disable backend profiling.\n"
         "  no-individual    Disable generation of per-file traces.\n"
         "  no-parser        Disable parser profiling.\n"
         "  no-preprocessor  Disable preprocessor profiling.\n"
         "  no-unit          Exclude topmost translation unit level from trace.\n"
         "  with-macros      Enable macros profiling.\n";
}

} // namespace insight
