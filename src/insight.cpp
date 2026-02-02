#include "insight.h"
#include "instance.h"
#include "options.h"
#include "utils.h"

#include <optional>

// Always last
#include "gcc-headers.h"

static_assert(__GNUC__ >= 9 && __GNUC__ <= 12, "gcc version is not supported");

namespace insight {

namespace {

Options options;
std::optional<Instance> instance;

void handleStartUnit(void *, void *)
{
  instance = std::make_optional<Instance>(options);
}

} // namespace

} // namespace insight

using namespace insight;

// Has to be defined
int plugin_is_GPL_compatible = 1;

int plugin_init(plugin_name_args *args, plugin_gcc_version *runtimeGccVersion)
{
  static auto info = plugin_info{"0.1", getHelpText().data()};
  register_callback(PLUGIN_NAME.data(), PLUGIN_INFO, nullptr, &info);

  if (!plugin_default_version_check(runtimeGccVersion, &gcc_version)) {
    logError("plugin compiled for different gcc version");
    return 1;
  }

  try {
    options = parseOptions(*args);
    register_callback(PLUGIN_NAME.data(), PLUGIN_START_UNIT, &handleCallback<&handleStartUnit>, nullptr);
  } catch (const std::exception &e) {
    logError(e.what());
    return 1;
  } catch (...) {
    logError("unknown error");
    return 1;
  }

  return 0;
}
