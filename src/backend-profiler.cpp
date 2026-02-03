#include "backend-profiler.h"

#include "gcc-utils.h"
#include "options.h"
#include "trace.h"

#include <chrono>

// Always last
#include "gcc-headers.h"

using namespace std::chrono;

namespace insight {

namespace {

const std::string &getPassTypeName(opt_pass_type passType)
{
  static const auto GIMPLE = std::string{"GIMPLE"};
  static const auto RTL = std::string{"RTL"};
  static const auto IPA = std::string{"IPA"};
  static const auto UNKNOWN = std::string{"__UNKNOWN"};

  switch (passType) {
  case opt_pass_type::GIMPLE_PASS:
    return GIMPLE;
  case opt_pass_type::RTL_PASS:
    return RTL;
  case opt_pass_type::IPA_PASS:
    [[fallthrough]];
  case opt_pass_type::SIMPLE_IPA_PASS:
    return IPA;
  default:
    return UNKNOWN;
  };
}

} // unnamed namespace

namespace internal {

class BackendProfilerImpl {
public:
  explicit BackendProfilerImpl(const Options &options, std::shared_ptr<Trace> trace)
  : mOptions{options}
  , mTrace{std::move(trace)}
  {
  }

  BackendProfilerImpl(const BackendProfilerImpl &) = delete;
  BackendProfilerImpl &operator=(const BackendProfilerImpl &) = delete;

  void handlePassExecution(void *gccData)
  {
    handleLastPass();

    auto &pass = *static_cast<opt_pass *>(gccData);

    if (isFunctionPass(pass.type)) {
      handleFunctionPass(pass);
    } else if (isIpaPass(pass.type)) {
      handleIpaPass(pass);
    } else {
      logWarn("unknown pass type (", pass.type, ")");
    }

    mTimestamp = Clock::now();
  }

  void handlePluginFinish(void *gccData)
  {
    handleLastPass();
  }

private:
  const Options mOptions;
  std::shared_ptr<Trace> mTrace;
  std::string mLastPass;
  TimePoint mTimestamp = Clock::now();

  void handleFunctionPass(opt_pass &pass)
  {
    assert(cfun != nullptr);

    mLastPass =
      squashLevels({"Function passes", getFunctionId(cfun->decl), getPassTypeName(pass.type), pass.name});
  }

  void handleIpaPass(opt_pass &pass)
  {
    mLastPass = squashLevels({"IPA passes", pass.name});
  }

  void handleLastPass()
  {
    if (!mLastPass.empty()) {
      mTrace->add(Event{mLastPass, measure(mTimestamp, Clock::now())});
      mLastPass.clear();
    }
  }

  bool isFunctionPass(opt_pass_type &type)
  {
    return type == opt_pass_type::GIMPLE_PASS || type == opt_pass_type::RTL_PASS;
  }

  bool isIpaPass(opt_pass_type &type)
  {
    return type == opt_pass_type::IPA_PASS || type == opt_pass_type::SIMPLE_IPA_PASS;
  }
};

} // namespace internal

using internal::BackendProfilerImpl;

BackendProfiler::BackendProfiler(const Options &options, std::shared_ptr<Trace> trace)
: mImpl{std::make_unique<BackendProfilerImpl>(options, std::move(trace))}
{
}

BackendProfiler::~BackendProfiler() = default;

void BackendProfiler::handlePassExecution(void *gccData)
{
  mImpl->handlePassExecution(gccData);
}

void BackendProfiler::handlePluginFinish(void *gccData)
{
  mImpl->handlePluginFinish(gccData);
}

} // namespace insight
