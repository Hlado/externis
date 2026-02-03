#include "pp-profiler.h"

#include "options.h"
#include "trace.h"
#include "utils.h"

#include <array>
#include <cassert>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>

// Always last
#include "gcc-headers.h"

using namespace std::chrono;

extern struct cpp_reader *parse_in;

namespace insight {

namespace {

enum class CallbackType { Other, Used };

struct UsedInfo {
  std::string name;
};

using CallbackInfo = std::variant<std::monostate, UsedInfo>;

class HeadersTracker {
public:
  explicit HeadersTracker(std::shared_ptr<Trace> trace)
  : mTrace{std::move(trace)}
  , initialDepth{mTrace->depth()}
  {
  }

  void handleFileChange(cpp_reader *reader, const line_map_ordinary *lineMap)
  {
    if (lineMap == nullptr) {
      return;
    }

    if (lineMap->reason == LC_ENTER) {
      handleEnter(lineMap);
    } else if (lineMap->reason == LC_LEAVE) {
      handleLeave(lineMap);
    }
  }

private:
  static constexpr auto UNNAMED_FILE_NAME = "<unnamed>";

  std::shared_ptr<Trace> mTrace;
  std::unordered_map<std::string, std::size_t> mHeadersVisits;
  std::size_t initialDepth;

  void handleEnter(const line_map_ordinary *lineMap)
  {
    auto rawName = ORDINARY_MAP_FILE_NAME(lineMap);
    auto name = std::string{UNNAMED_FILE_NAME};

    // Named files require bit of a processing
    if (rawName != nullptr) {
      name = rawName;
      visit(name);
    }

    mTrace->push(name);
  }

  // We assume that every file has include guard so all occurences except first is instant and we skip them
  void visit(const std::string &name)
  {
    auto [it, inserted] = mHeadersVisits.insert(std::make_pair(name, 1));
    // We don't touch zeroes because it means that file was once processed already
    if (!inserted && it->second > 0) {
      it->second += 1;
    }
  }

  void handleLeave(const line_map_ordinary *lineMap)
  {
    assert(mTrace->depth() > initialDepth);

    auto name = mTrace->name();
    if (name != UNNAMED_FILE_NAME) {
      auto it = mHeadersVisits.find(name);
      assert(it != mHeadersVisits.end());

      if (it->second == 0) { // Skip already processed files
        mTrace->drop();
      } else if (it->second == 1) { // Leaving header on first occurence
        collapse(*mTrace, mTrace->depth() - 1);
        it->second = 0;
      } else { // Recursive includes on first encounter, skipping nested
        mTrace->drop();
        it->second -= 1;
      }
    } else {
      collapse(*mTrace, mTrace->depth() - 1);
    }
  }
};

class MacroTracker {
public:
  explicit MacroTracker(std::shared_ptr<Trace> trace)
  : mTrace{std::move(trace)}
  {
  }

  void handleUsedKind(cpp_reader *reader, location_t loc, cpp_hashnode *node)
  {
    handleLastCallback();

    auto name = reinterpret_cast<const char *>(NODE_NAME(node));
    assert(((void)"node name is null", name != nullptr));

    mLastCallback = UsedInfo{name};

    uptateTimestamps();
  }

  void handleLineChange(cpp_reader *, const cpp_token *, int)
  {
    handleLastCallback();
    uptateTimestamps();
  }

private:
  std::shared_ptr<Trace> mTrace;
  std::array<TimePoint, 2> mTimestamps{Clock::now(), Clock::now()};
  CallbackInfo mLastCallback;

  void handleLastCallback()
  {
    auto now = Clock::now();
    if (std::holds_alternative<UsedInfo>(mLastCallback)) {
      auto &usedInfo = std::get<UsedInfo>(mLastCallback);
      mTrace->add(Event{usedInfo.name, measure(mTimestamps[0], now)});
    }
  }

  void uptateTimestamps()
  {
    mTimestamps[0] = mTimestamps[1];
    mTimestamps[1] = Clock::now();
  }
};

} // unnamed namespace

namespace internal {

class PpProfilerImpl {
public:
  PpProfilerImpl(const Options &options, std::shared_ptr<Trace> trace, std::function<void()> finishHandler)
  : mOptions{options}
  , mReader{parse_in}
  , mHeadersTracker{trace}
  , mMacroTracker{trace}
  , mFinishHandler{finishHandler}
  {
    if (mReader == nullptr) {
      throw Error{"reader is not set"};
    }
    mReaderCallbacks = cpp_get_callbacks(mReader);
    if (mReaderCallbacks == nullptr) {
      throw Error{"preprocessor callbacks are empty"};
    }
    mOriginalCallbacks = *mReaderCallbacks;
    mOurCallbacks = mOriginalCallbacks;
    mReadersMapping[mReader] = this;

    try {
#define INSIGHT_PPP_SET_CALLBACK(handler, callback, ...)                                                                 \
  mReaderCallbacks->callback =                                                                                           \
    &dispatchCallback<&PpProfilerImpl::handleCallback<&PpProfilerImpl::handler, &cpp_callbacks::callback, __VA_ARGS__>>; \
  mOurCallbacks.callback = mReaderCallbacks->callback;

      INSIGHT_PPP_SET_CALLBACK(handleFileChange, file_change, cpp_reader *, const line_map_ordinary *);
      INSIGHT_PPP_SET_CALLBACK(handleLineChange, line_change, cpp_reader *, const cpp_token *, int);
      INSIGHT_PPP_SET_CALLBACK(handleUsed, used, cpp_reader *, location_t, cpp_hashnode *);
      INSIGHT_PPP_SET_CALLBACK(handleUsedDefine, used_define, cpp_reader *, location_t, cpp_hashnode *);

#undef INSIGHT_PPP_SET_CALLBACK
    } catch (const std::exception &e) {
      cleanup();
      throw;
    }
  }

  PpProfilerImpl(const PpProfilerImpl &) = delete;
  PpProfilerImpl &operator=(const PpProfilerImpl &) = delete;

  ~PpProfilerImpl()
  {
    cleanup();
  }

private:
  static inline std::unordered_map<cpp_reader *, PpProfilerImpl *> mReadersMapping;

  const Options mOptions;
  cpp_reader *mReader{nullptr};
  cpp_callbacks *mReaderCallbacks{nullptr};
  cpp_callbacks mOriginalCallbacks;
  cpp_callbacks mOurCallbacks;
  HeadersTracker mHeadersTracker;
  MacroTracker mMacroTracker;
  std::function<void()> mFinishHandler;


  void handleLineChange(cpp_reader *reader, const cpp_token *token, int line)
  {
    mMacroTracker.handleLineChange(reader, token, line);
  }

  void handleUsed(cpp_reader *reader, location_t loc, cpp_hashnode *node)
  {
    mMacroTracker.handleUsedKind(reader, loc, node);
  }

  void handleUsedDefine(cpp_reader *reader, location_t loc, cpp_hashnode *node)
  {
    mMacroTracker.handleUsedKind(reader, loc, node);
  }

  void handleFileChange(cpp_reader *reader, const line_map_ordinary *lineMap)
  {
    mHeadersTracker.handleFileChange(reader, lineMap);

    // nullptr means finishing of preprocessing
    if (lineMap == nullptr) {
      cleanup();
      if (mFinishHandler) {
        mFinishHandler();
      }
    }
  }

  void cleanup() noexcept
  {
    restorePpCallbacks();

    mReadersMapping.erase(mReader);
    mReader = nullptr;
    mReaderCallbacks = nullptr;
  }

  void restorePpCallbacks() noexcept
  {
    if (mReader == nullptr) {
      return;
    }

    if (mReader != parse_in) {
      logError("fatal error: reader changed, abnormal termination...");
      std::abort();
    }

    if (mReaderCallbacks != cpp_get_callbacks(mReader)) {
      logError("fatal error: reader callbacks pointer changed, abnormal termination...");
      std::abort();
    }

    restoreCallback<&cpp_callbacks::file_change>();
    restoreCallback<&cpp_callbacks::line_change>();
    restoreCallback<&cpp_callbacks::used>();
    restoreCallback<&cpp_callbacks::used_define>();
  }

  template <auto CallbackV>
  void restoreCallback() noexcept
  {
    auto ppCallbacks = cpp_get_callbacks(mReader);

    if (ppCallbacks->*CallbackV != mOurCallbacks.*CallbackV) {
      logError("fatal error: reader callback was overwritten, abnormal termination...");
      std::abort();
    }
    ppCallbacks->*CallbackV = mOriginalCallbacks.*CallbackV;
  }

  // We do not need non-void callbacks for now and it would complicate code a fair bit,
  // so we do not support that case yet
  template <auto HandlerV, typename... ArgsT>
  static void dispatchCallback(ArgsT... args)
  {
    if (parse_in == nullptr) {
      logError("fatal error: reader is not set, abnormal termination...");
      std::abort();
    }

    auto it = mReadersMapping.find(parse_in);
    if (it == mReadersMapping.end()) {
      logError("fatal error: reader is not found, abnormal termination...");
      std::abort();
    }

    auto obj = it->second;
    try {
      (obj->*HandlerV)(args...);
    } catch (...) {
      // It is really hard to make this whole class exception safe, so we better stop on exception
      obj->cleanup();
      throw;
    }
  }

  template <auto HandlerV, auto OriginalHandlerV, typename... ArgsT>
  void handleCallback(ArgsT... args)
  {
    (this->*HandlerV)(args...);

    auto oh = mOriginalCallbacks.*OriginalHandlerV;
    if (oh != nullptr) {
      oh(args...);
    }
  }
};

} // namespace internal

using internal::PpProfilerImpl;

PpProfiler::PpProfiler(const Options &options, std::shared_ptr<Trace> trace, std::function<void()> finishHandler)
: mImpl{std::make_unique<PpProfilerImpl>(options, std::move(trace), finishHandler)}
{
}

PpProfiler::~PpProfiler() = default;

} // namespace insight
