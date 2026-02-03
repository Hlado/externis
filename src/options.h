#pragma once

#include <filesystem>
#include <string_view>

struct plugin_name_args;

namespace insight {

struct Options {
  bool basicProfiling{false};
  bool noIndividual{false};
  std::filesystem::path combined;
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
         "                   measured"
         "  no-individual    Disable generation of per-file traces.\n"
         "  combined=<path>  Path to file used to accumulate a multi-file trace. Each\n"
         "                   individual trace is appended to the end of this file.\n";
}

} // namespace insight
