#include "options.h"

#include "utils.h"

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

// Always last
#include "gcc-headers.h"

namespace insight {

namespace {

class Reader {
public:
  Reader() = default;
  Reader(const Reader &) = delete;
  Reader &operator=(const Reader &) = delete;

  void read(const plugin_name_args &args, Options &options)
  {
    for (int i = 0; i < args.argc; ++i) {
      auto [name, value] = args.argv[i];
      auto it = handlers.find(name);
      if (it == handlers.end()) {
        logWarn("unrecognozed option: '", name, "'");
        continue;
      }

      it->second(value == nullptr ? "" : value, options);
    }
  }

  template <typename T>
  void registerHandler(std::string name, T Options::*member, void (*handler)(std::string_view, std::string_view, T &))
  {
    handlers[name] = [name, member, handler](std::string_view value, Options &options) {
      (*handler)(name, value, options.*member);
    };
  }

private:
  std::unordered_map<std::string, std::function<void(std::string_view, Options &)>> handlers;
};

void flagOptionHandler(std::string_view name, std::string_view value, bool &option)
{
  if (value != "") {
    logWarn("'", name, "' option value '", value, "' will be ignored");
  }

  option = true;
}

void pathOptionHandler(std::string_view name, std::string_view value, std::filesystem::path &out)
{
  if (value == "") {
    logWarn("'", name, "' option doesn't have value");
    return;
  }

  out = value;
}

template <typename RepT, typename PeriodT>
void durationOptionHandler(std::string_view name, std::string_view value, std::chrono::duration<RepT, PeriodT> &out)
{
  if (value == "") {
    logWarn("'", name, "' option doesn't have value");
    return;
  }

  long long int numeric;
  try {
    numeric = std::stoll(std::string{value});
  } catch (const std::exception &e) {
    throw Error{"'", name, "' option parsing error: ", e.what()};
  }

  if (numeric < 0) {
    throw Error{"'", name, "' option value must be positive"};
  }

  out = std::chrono::duration<RepT, PeriodT>{numeric};

  logError(numeric);
}

} // unnamed namespace

Options parseOptions(const plugin_name_args &args)
{
  Options options;

  Reader reader;
  reader.registerHandler("basic-profiling", &Options::basicProfiling, flagOptionHandler);
  reader.registerHandler("combined", &Options::combined, pathOptionHandler);
  reader.registerHandler("duration-treshold", &Options::durationTreshold, durationOptionHandler);
  reader.registerHandler("no-backend", &Options::noBackend, flagOptionHandler);
  reader.registerHandler("no-individual", &Options::noIndividual, flagOptionHandler);
  reader.registerHandler("no-parser", &Options::noParser, flagOptionHandler);
  reader.registerHandler("no-preprocessor", &Options::noPreprocessor, flagOptionHandler);
  reader.registerHandler("no-unit", &Options::noUnit, flagOptionHandler);
  reader.registerHandler("with-macros", &Options::withMacros, flagOptionHandler);
  reader.read(args, options);

  return options;
}

} // namespace insight
