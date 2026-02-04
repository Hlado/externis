#include "instance.h"

#include "backend-profiler.h"
#include "insight.h"
#include "options.h"
#include "parser-profiler.h"
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
      registerCallback<&InstanceImpl::handleParserCallback<&InstanceImpl::handleStartParseFunction>>(
        PLUGIN_START_PARSE_FUNCTION);
      registerCallback<&InstanceImpl::handleParserCallback<&InstanceImpl::handleFinishParseFunction>>(
        PLUGIN_FINISH_PARSE_FUNCTION);
      registerCallback<&InstanceImpl::handleParserCallback<&InstanceImpl::handleFinishDecl>>(PLUGIN_FINISH_DECL);
      registerCallback<&InstanceImpl::handleParserCallback<&InstanceImpl::handleFinishType>>(PLUGIN_FINISH_TYPE);
      registerCallback<&InstanceImpl::handleBackendCallback<&InstanceImpl::handlePluginPassExecution>>(
        PLUGIN_PASS_EXECUTION);
      registerCallback<&InstanceImpl::handleBackendCallback<&InstanceImpl::handleFinishUnit>>(PLUGIN_FINISH_UNIT);
      // Although it's called PLUGIN_FINISH, for multi input case it is called for each translation unit
      registerCallback<&InstanceImpl::handleFinish>(PLUGIN_FINISH);

      if (!mOptions.noUnit) {
        mTraces.global.push(getFullInputName());
      }

      mTraces.preprocessor = std::make_unique<Trace>();
      mTraces.preprocessor->push("Preprocessor");
      mPpProfiler.emplace(options, mTraces.preprocessor,
                          std::bind(&InstanceImpl::handlePreprocessingFinish, this));
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
  // TODO: partial traces likely have to be moved into profilers
  struct Traces {
    Trace global;
    std::shared_ptr<Trace> preprocessor;
    std::shared_ptr<Trace> parser;
    std::shared_ptr<Trace> backend;
  };

  const Options mOptions;
  Traces mTraces;
  std::optional<PpProfiler> mPpProfiler;
  std::optional<ParserProfiler> mParserProfiler;
  std::optional<BackendProfiler> mBackendProfiler;

  static void unregisterCallback(int event) noexcept
  {
    unregister_callback(PLUGIN_NAME.data(), event);
  }

  void unregisterCallbacks() noexcept
  {
    unregisterCallback(PLUGIN_START_PARSE_FUNCTION);
    unregisterCallback(PLUGIN_FINISH_PARSE_FUNCTION);
    unregisterCallback(PLUGIN_FINISH_DECL);
    unregisterCallback(PLUGIN_FINISH_TYPE);
    unregisterCallback(PLUGIN_PASS_EXECUTION);
    unregisterCallback(PLUGIN_FINISH_UNIT);
    unregisterCallback(PLUGIN_FINISH);
  }

  void cleanup() noexcept
  {
    mPpProfiler.reset();
    mParserProfiler.reset();
    mBackendProfiler.reset();
    unregisterCallbacks();
  }

  template <void (InstanceImpl::*HandlerV)(void *)>
  void registerCallback(int event)
  {
    register_callback(PLUGIN_NAME.data(), event, &::insight::handleCallback<&dispatchCallback<HandlerV>>, this);
  }

  void handlePreprocessingFinish()
  {
    collapse(*mTraces.preprocessor);
    // We start the next stage here because parser callbacks usually occur after an action has
    // completed. Starting later would cause us to miss the first measure, while starting earlier
    // would measure more work than necessary. For now, we prefer the latter.
    mTraces.parser = std::make_unique<Trace>();
    mTraces.parser->push("Parser");
    mParserProfiler.emplace(mOptions, mTraces.parser);
  }

  void handleStartParseFunction(void *gccData)
  {
  }

  void handleFinishParseFunction(void *gccData)
  {
  }

  void handleFinishDecl(void *gccData)
  {
  }

  void handleFinishType(void *gccData)
  {
  }

  template <void (InstanceImpl::*HandlerV)(void *)>
  void handleParserCallback(void *gccData)
  {
    // We delete the preprocessing profiler here because deleting it from within its own callback
    // would be unsafe. We also have no other reliable way to detect when preprocessing has finished
    // except through the profiler itself, so we need to split process a little.
    if (!mParserProfiler) {
      mPpProfiler.reset();
    }

    if (!mOptions.basicProfiling) {
      (this->*HandlerV)(gccData);
    }
  }

  void handlePluginPassExecution(void *gccData)
  {
    assert(mBackendProfiler);

    mBackendProfiler->handlePassExecution(gccData);
  }

  void handleFinishUnit(void *gccData)
  {
    assert(mBackendProfiler);

    mBackendProfiler->handleFinishUnit(gccData);
    mBackendProfiler.reset();
  }

  template <void (InstanceImpl::*HandlerV)(void *)>
  void handleBackendCallback(void *gccData)
  {
    if (!mBackendProfiler) {
      mParserProfiler.reset();
      collapse(*mTraces.parser);
      mTraces.backend = std::make_shared<Trace>();
      mTraces.backend->push("Backend");
      mBackendProfiler.emplace(mOptions, mTraces.backend);
    }

    if (!mOptions.basicProfiling) {
      (this->*HandlerV)(gccData);
    }
  }

  void handleFinish(void *gccData)
  {
    // We can't do it in FINISH_UNIT callback because it can be disabled on basic only profiling
    collapse(*mTraces.backend);

    mTraces.global.consume(std::move(*mTraces.preprocessor));
    mTraces.global.consume(std::move(*mTraces.parser));
    mTraces.global.consume(std::move(*mTraces.backend));
    mTraces.global.collapse();
    auto collapsed = mTraces.global.flatten();

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

  template <void (InstanceImpl::*HandlerV)(void *)>
  static void dispatchCallback(void *gccData, void *userData)
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
};

} // namespace internal

using internal::InstanceImpl;

Instance::Instance(const Options &options)
: mImpl{std::make_unique<InstanceImpl>(options)}
{
}

Instance::~Instance() = default;

} // namespace insight
