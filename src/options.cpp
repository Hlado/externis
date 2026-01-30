#include "options.h"

#include "utils.h"

namespace insight {

Options parseOptions(const plugin_name_args &args)
{
  Options options;

  for(int i = 0; i < args.argc; ++i) {
    auto &&[name, value] = args.argv[i];
    if(std::strcmp("no-individual", name) == 0) {
      if(value != nullptr && std::strcmp("", value) != 0) {
        logWarn("'no-individual' option value '", value, "' will be ignored");
        continue;
      }
      options.noIndividual = true;
    } else if(std::strcmp("combined", name) == 0) {
      if(value == nullptr || std::strcmp("", value) == 0) {
        logWarn("'combined' option doesn't have value");
        continue;
      }
      options.combined = value;
    } else {
      logWarn("unrecognozed option: '", name, "'");
    }
  }

  return options;
}

} //namespace insight