#include "instance.h"

#include "backend-profiler.h"
#include "insight.h"
#include "options.h"
#include "pp-profiler.h"
#include "trace.h"
#include "utils.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <numeric>
#include <optional>
#include <stack>
#include <string>
#include <type_traits>
#include <unordered_map>

// Always last
#include "gcc-headers.h"

using namespace std::chrono;

namespace insight {

namespace {

constexpr auto STAGE_FILE = std::size_t{1};
constexpr auto STAGE_STEP = std::size_t{2};

enum class Steps { Preprocessing, Parsing, Backend };

std::string getFullInputName()
{
  if (main_input_filename == nullptr || std::strcmp("", main_input_filename) == 0) {
    return std::string{"unnamed-"} + std::to_string(Clock::now().time_since_epoch().count());
  } else {
    return main_input_filename;
  }
}

} // unnamed namespace

namespace internal {

class InstanceImpl {
public:
  explicit InstanceImpl(const Options &options)
  : mOptions{options}
  {
    try {
      PLUGIN_START_PARSE_FUNCTION;
      PLUGIN_FINISH_PARSE_FUNCTION;
      PLUGIN_PASS_MANAGER_SETUP;
      PLUGIN_FINISH_TYPE;
      PLUGIN_FINISH_DECL;

      PLUGIN_ALL_PASSES_START;
      PLUGIN_ALL_PASSES_END;
      PLUGIN_ALL_IPA_PASSES_START;
      PLUGIN_ALL_IPA_PASSES_END;
      PLUGIN_OVERRIDE_GATE;
      PLUGIN_PASS_EXECUTION;
      PLUGIN_EARLY_GIMPLE_PASSES_START;
      PLUGIN_EARLY_GIMPLE_PASSES_END;


      registerCallback<&InstanceImpl::handlePluginFinish>(PLUGIN_FINISH);
      registerCallback<&InstanceImpl::handleParserCallback<&InstanceImpl::handleFinishDecl>>(PLUGIN_FINISH_DECL);
      registerCallback<&InstanceImpl::handleBackendCallback<&InstanceImpl::handlePluginPassExecution>>(
        PLUGIN_PASS_EXECUTION);

      //! Order
      mTrace->push(getFullInputName());
      mTrace->push("Preprocessor");
      mPpProfiler = PpProfiler{options, mTrace};
    } catch (const std::exception &e) {
      cleanup();
      throw;
    }
  }

  InstanceImpl(const InstanceImpl &) = delete;
  InstanceImpl &operator=(const InstanceImpl &) = delete;

  ~InstanceImpl()
  {
    cleanup();
  }

private:
  const Options mOptions;
  //! Order
  std::shared_ptr<Trace> mTrace{std::make_shared<Trace>()};
  std::optional<PpProfiler> mPpProfiler;
  std::optional<BackendProfiler> mBackendProfiler;
  Steps mStep{Steps::Preprocessing};


  static void unregisterCallback(int event) noexcept
  {
    unregister_callback(PLUGIN_NAME.data(), event);
  }

  static void unregisterCallbacks() noexcept
  {
    unregisterCallback(PLUGIN_FINISH);
    unregisterCallback(PLUGIN_PASS_EXECUTION);
  }

  template <void (InstanceImpl::*HandlerV)(void *)>
  static void handleCallback(void *gccData, void *userData)
  {
    auto obj = static_cast<InstanceImpl *>(userData);

    try {
      (obj->*HandlerV)(gccData);
    } catch (...) {
      // It is really hard to make this whole class exception safe, so we better stop on exception
      obj->cleanup();
      throw;
    }
  }

  void cleanup() noexcept
  {
    unregisterCallbacks();
  }

  template <void (InstanceImpl::*HandlerV)(void *)>
  void registerCallback(int event)
  {
    register_callback(PLUGIN_NAME.data(), event, &::insight::handleCallback<&handleCallback<HandlerV>>, this);
  }

  template <void (InstanceImpl::*HandlerV)(void *)>
  void handleParserCallback(void *gccData)
  {
    if (mStep == Steps::Preprocessing) {
      // Release preprocessor callbacks
      mPpProfiler.reset();
      mStep = Steps::Parsing;
      collapse(*mTrace, 1);
      mTrace->push("Parser");
    }
    if (mStep != Steps::Parsing) {
      throw Error{"parsing callback happened at the wrong step (",
                  static_cast<std::underlying_type_t<Steps>>(mStep), ")"};
    }

    (this->*HandlerV)(gccData);
  }

  template <void (InstanceImpl::*HandlerV)(void *)>
  void handleBackendCallback(void *gccData)
  {
    if (mStep == Steps::Parsing) {
      mStep = Steps::Backend;
      collapse(*mTrace, 1);
      mTrace->push("Backend");
      mBackendProfiler = BackendProfiler{mOptions, mTrace};
    } else {
      if (mStep != Steps::Backend) {
        throw Error{"backend callback happened at the wrong step (",
                    static_cast<std::underlying_type_t<Steps>>(mStep), ")"};
      }
    }

    (this->*HandlerV)(gccData);
  }

  void handleFinishDecl(void *)
  {
    // Just to test wrapper callbacks for now
  }

  void handlePluginPassExecution(void *gccData)
  {
    mBackendProfiler->handlePassExecution(gccData);
  }

  void handlePluginFinish(void *gccData)
  {
    mBackendProfiler->handlePluginFinish(gccData);
    mBackendProfiler.reset();

    collapse(*mTrace, 0);
    auto collapsed = mTrace->flatten();

    std::shared_ptr<std::ostream> individual = getIndividualStream();
    if (individual) {
      dump(collapsed, *individual);
      if (individual->fail()) {
        throw Error{"failed to dump individual trace"};
      }
    }

    std::shared_ptr<std::ostream> combined = getCombinedStream();
    if (combined) {
      dump(collapsed, *combined);
      if (combined->fail()) {
        throw Error{"failed to dump combined trace"};
      }
    }

    cleanup();
  }

  std::shared_ptr<std::ostream> getIndividualStream() const
  {
    std::shared_ptr<std::ostream> stream;

    if (!mOptions.noIndividual) {
      if (!std::filesystem::exists(main_input_filename)) {
        logInfo("input is not a file, dumping to cout");
        stream = std::shared_ptr<std::ostream>{&std::cout, [](auto &&) {}};
      } else {
        auto dumpPath = std::filesystem::path{dump_base_name};
        dumpPath += ".trace.collapsed";

        logInfo("dumping to '", dumpPath.string(), "'");
        stream = std::make_shared<std::ofstream>(dumpPath);
        if (stream->fail()) {
          throw Error{"failed to open '", dumpPath.string(), "'"};
        }
      }
    }

    return stream;
  }

  std::shared_ptr<std::ostream> getCombinedStream() const
  {
    std::shared_ptr<std::ostream> stream;

    if (!mOptions.noIndividual) {
      if (!mOptions.combined.empty()) {
        logInfo("dumping combined to '", mOptions.combined.string(), "'");

        stream = std::make_shared<std::ofstream>(mOptions.combined, std::ios_base::app);
        if (stream->fail()) {
          throw Error{"failed to open '", mOptions.combined.string(), "'"};
        }
      }
    }

    return stream;
  }
};

} // namespace internal

using internal::InstanceImpl;

Instance::Instance(const Options &options)
: mImpl{std::make_unique<InstanceImpl>(options)}
{
}

Instance::Instance(Instance &&) = default;
Instance &Instance::operator=(Instance &&) = default;
Instance::~Instance() = default;

} // namespace insight
