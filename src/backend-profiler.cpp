#include "backend-profiler.h"

#include "options.h"
#include "stage.h"

#include <chrono>

//Always last
#include "gcc-headers.h"

using namespace std::chrono;

namespace insight {

namespace internal {

class BackendProfilerImpl
{
public:
  explicit BackendProfilerImpl(const Options &options)
    : mOptions(options)
  {
    try
    {
      //It is very important question - are those callback being overwritten or appended or ignored?
      registerCallback<&BackendProfilerImpl::handleCallback<&BackendProfilerImpl::handlePassExecution>>(PLUGIN_PASS_EXECUTION);
      //registerCallback<&BackendProfilerImpl::handleCallback<&BackendProfilerImpl::handleAllPassesEnd>>(PLUGIN_ALL_PASSES_END);

      mStages.push(Stage{Clock::now(), "Backend", {}});
    }
    catch(const std::exception& e)
    {
      cleanup();
      throw;
    }
  }

  BackendProfilerImpl(const BackendProfilerImpl &) = delete;
  BackendProfilerImpl &operator=(const BackendProfilerImpl &) = delete;

  ~BackendProfilerImpl()
  {
    cleanup();
  }

  void dump(Stage &sink) const
  {
    //TODO: probably need more efficient solution - && overload?
    auto tmp = mStages;
    Stage::collapse(tmp, 1);
    sink.consume(tmp.top());
  }

private:
  const Options mOptions;
  std::stack<Stage> mStages;
  std::string activePass;
  TimePoint mLastEventTimestamp = Clock::now();

  
  static void unregisterCallback(int event) noexcept
  {
    unregister_callback(PLUGIN_NAME.data(), event);
  }

  static void unregisterCallbacks() noexcept
  {
    unregisterCallback(PLUGIN_PASS_EXECUTION);
    unregisterCallback(PLUGIN_ALL_PASSES_END);
  }

  template <void (BackendProfilerImpl::*HandlerV)(void *)>
  static void dispatchCallback(void *gccData, void *userData)
  {
    auto obj = static_cast<BackendProfilerImpl *>(userData);

    try {
      (obj->*HandlerV)(gccData);
    } catch(...) {
      //It is really hard to make this whole class exception safe, so we better stop on exception
      obj->cleanup();
      throw;
    }
  }

  template <void (BackendProfilerImpl::*HandlerV)(void *)>
  void handleCallback(void *gccData)
  {
    if(!activePass.empty()) {
      mStages.top().records[activePass] = measure(mLastEventTimestamp, Clock::now());
      activePass.clear();
    }

    (this->*HandlerV)(gccData);

    mLastEventTimestamp = Clock::now();
  }

  void handlePassExecution(void *gccData)
  {
    auto pass = (opt_pass *)gccData;
  
    if(pass->type == opt_pass_type::GIMPLE_PASS || pass->type == opt_pass_type::RTL_PASS) {
      auto fndecl = cfun->decl;
      location_t loc = DECL_SOURCE_LOCATION(fndecl);
      unsigned int line = 0;
      unsigned int column = 0;
      if (loc != UNKNOWN_LOCATION) {
        line = LOCATION_LINE(loc);
        column = LOCATION_COLUMN(loc);
      }
      auto funcName = fndecl ? IDENTIFIER_POINTER(DECL_NAME(fndecl)) : std::string("<anonymous>:") + std::to_string(line) + ":" + std::to_string(column);
      activePass = funcName;
      activePass += ";" + (pass->type == opt_pass_type::GIMPLE_PASS ? std::string{"GIMPLE;"} : std::string{"RTL;"});
      activePass += pass->name;
    } else if(pass->type == opt_pass_type::IPA_PASS || pass->type == opt_pass_type::SIMPLE_IPA_PASS) {
      activePass = "IPA;";
      activePass += pass->name;
    } else {
      logWarn("unknown pass type (", pass->type, ")");
    }
  }

  void handleAllPassesEnd(void *gccData)
  {
    
  }

  void cleanup() noexcept {
    unregisterCallbacks();
  }

  template <void (BackendProfilerImpl::*HandlerV)(void *)>
  void registerCallback(int event) {
    register_callback(PLUGIN_NAME.data(), event, &::insight::handleCallback<&dispatchCallback<HandlerV>>, this);
  }
};

} //namespace internal

using internal::BackendProfilerImpl;

BackendProfiler::BackendProfiler(const Options &options)
  : mImpl{std::make_unique<BackendProfilerImpl>(options)}
{

}

BackendProfiler::BackendProfiler(BackendProfiler &&) = default;
BackendProfiler &BackendProfiler::operator=(BackendProfiler &&) = default;
BackendProfiler::~BackendProfiler() = default;

void BackendProfiler::dump(Stage &sink) const
{
  return mImpl->dump(sink);
}

} //namespace insight