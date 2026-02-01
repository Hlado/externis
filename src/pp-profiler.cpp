#include "pp-profiler.h"

#include "options.h"
#include "trace.h"
#include "utils.h"

#include <cassert>
#include <cstring>
#include <memory>
#include <stack>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <variant>

//Always last
#include "gcc-headers.h"

using namespace std::chrono;

extern struct cpp_reader* parse_in;

namespace insight {

namespace {

enum class CallbackType
{
  Other,
  Used
};

struct UsedInfo
{
  std::string name;
};

using CallbackInfo = std::variant<std::monostate, UsedInfo>;

} //unnamed namespace

namespace internal {

class PpProfilerImpl
{
public:
  PpProfilerImpl(const Options &options, std::shared_ptr<Trace> trace)
    : mOptions{options}
    , mReader{parse_in}
    , mTrace{std::move(trace)}
  {
    if(mReader == nullptr) {
      throw Error{"reader is not set"};
    }
    mReaderCallbacksAddress = cpp_get_callbacks(mReader);
    if(mReaderCallbacksAddress == nullptr) {
      throw Error{"preprocessor callbacks are empty"};
    }
    mOriginalCallbacks = *mReaderCallbacksAddress;
    mOurCallbacks = mOriginalCallbacks;
    mReadersMapping[mReader] = this;

    try
    {
      mReaderCallbacksAddress->file_change = &dispatchCallback<&PpProfilerImpl::handleFileChange, cpp_reader *, const line_map_ordinary *>;
      mOurCallbacks.file_change = mReaderCallbacksAddress->file_change;

      mReaderCallbacksAddress->used = &dispatchCallback<&PpProfilerImpl::handleMacroUsed, cpp_reader *, location_t, cpp_hashnode *>;
      mOurCallbacks.used = mReaderCallbacksAddress->used;

      mReaderCallbacksAddress->used_define = &dispatchCallback<&PpProfilerImpl::handleMacroUsed, cpp_reader *, location_t, cpp_hashnode *>;
      mOurCallbacks.used_define = mReaderCallbacksAddress->used_define;
      

      if(mOriginalCallbacks.line_change != nullptr) {
        mReaderCallbacksAddress->line_change = &dispatchCallback<&PpProfilerImpl::proxyCallback<&cpp_callbacks::line_change, cpp_reader *, const cpp_token *, int>, cpp_reader *, const cpp_token *, int>;
        mOurCallbacks.line_change = mReaderCallbacksAddress->line_change;
      }

      mStart = Clock::now();
      mTrace->push("Preprocessing");
    }
    catch(const std::exception& e)
    {
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
  cpp_callbacks mOriginalCallbacks;
  cpp_callbacks mOurCallbacks;
  cpp_reader *mReader{nullptr};
  cpp_callbacks *mReaderCallbacksAddress{nullptr};
  std::shared_ptr<Trace> mTrace;
  TimePoint mStart{Clock::now()};
  std::unordered_map<std::string, std::size_t> mHeadersVisits;
  TimePoint mLastCallbackTimestamp{Clock::now()};
  TimePoint mSecondToLastCallbackTimestamp{Clock::now()};
  CallbackInfo mLastCallbackInfo;

  //We do not need non-void callbacks for now and it would complicate code a fair bit,
  //so we do not support that case yet
  template <auto HandlerV, typename... ArgsT>
  static void dispatchCallback(ArgsT... args)
  {
    if(parse_in == nullptr) {
      logError("fatal error: reader is not set, abnormal termination...");
      std::abort();
    }

    auto it = mReadersMapping.find(parse_in);
    if(it == mReadersMapping.end()) {
      logError("fatal error: reader is not found, abnormal termination...");
      std::abort();
    }

    auto obj = it->second;
    try {
      obj->handleCallback<HandlerV>(args...);
    } catch(...) {
      //It is really hard to make this whole class exception safe, so we better stop on exception
      obj->cleanup();
      throw;
    }
  }

  template <auto HandlerV, typename... ArgsT>
  void handleCallback(ArgsT... args)
  {
    auto now = Clock::now();
    if(std::holds_alternative<UsedInfo>(mLastCallbackInfo)) {
      auto &usedInfo = std::get<UsedInfo>(mLastCallbackInfo);
      mTrace->add(Event{usedInfo.name, measure(mSecondToLastCallbackTimestamp, now)});
    }

    (this->*HandlerV)(args...);
    mSecondToLastCallbackTimestamp = mLastCallbackTimestamp;
    mLastCallbackTimestamp = Clock::now();
  }

  template <auto HandlerV, typename... ArgsT>
  void proxyCallback(ArgsT... args)
  {
    if((mOriginalCallbacks.*HandlerV) != nullptr) {
      (mOriginalCallbacks.*HandlerV)(args...);
    }
    
    mLastCallbackInfo = std::monostate{};
  }


  void cleanup() noexcept {
    restorePpCallbacks();
  }

  void restorePpCallbacks() noexcept
  {
    if(mReader == nullptr) {
      return;
    }
    
    if(mReader != parse_in) {
      logError("fatal error: reader changed, abnormal termination...");
      std::abort();
    }

    auto ppCallbacks = cpp_get_callbacks(mReader);
    if(mReaderCallbacksAddress != ppCallbacks) {
      logError("fatal error: reader callbacks pointer changed, abnormal termination...");
      std::abort();
    }

    if(ppCallbacks->file_change != mOurCallbacks.file_change) {
      logError("fatal error: reader callbacks were overriden, abnormal termination...");
      std::abort();
    }

    if(ppCallbacks->used != mOurCallbacks.used) {
      logError("fatal error: reader callbacks were overriden, abnormal termination...");
      std::abort();
    }

    if(ppCallbacks->used_define != mOurCallbacks.used_define) {
      logError("fatal error: reader callbacks were overriden, abnormal termination...");
      std::abort();
    }

    if(ppCallbacks->line_change != mOurCallbacks.line_change) {
      logError("fatal error: reader callbacks were overriden, abnormal termination...");
      std::abort();
    }

    ppCallbacks->file_change = mOriginalCallbacks.file_change;
    ppCallbacks->used = mOriginalCallbacks.used;
    ppCallbacks->used_define = mOriginalCallbacks.used_define;
    ppCallbacks->line_change = mOriginalCallbacks.line_change;

    mReadersMapping.erase(mReader);
    mReader = nullptr;
    mReaderCallbacksAddress = nullptr;
  }

  void handleMacroUsed(cpp_reader *, location_t, cpp_hashnode *node)
  {
    auto name = reinterpret_cast<const char *>(NODE_NAME(node));
    assert(((void)"macro name is null", name != nullptr));

    mLastCallbackInfo = UsedInfo{name};
  }

  void handleFileChange(cpp_reader *reader, const line_map_ordinary *lineMap)
  {
    static constexpr auto  UNNAMED = "<unnamed>";
    //It seems map is null in the end when preprocessor returns to main file
    if(lineMap != nullptr) {
      if(lineMap->reason == LC_ENTER) {
        auto fileNameRaw = ORDINARY_MAP_FILE_NAME(lineMap);
        auto fileName = std::string{UNNAMED};
        
        if(fileNameRaw != nullptr) {
          fileName = fileNameRaw;
          auto it = mHeadersVisits.find(fileName);
          if(it == mHeadersVisits.end()) {
            mHeadersVisits[fileName] = 1;
          } else {
            //Once visit number hit zero it becomes unchangeable (or at least that's the idea)
            if(it->second > 0) {
              it->second += 1;
            }
          }
        }

        mTrace->push(fileName);
      } else if(lineMap->reason == LC_LEAVE) {
        //We either need to track initial depth or part ways with this assertion
        //assert(((void)"enter/leave file preprocessor callback mismatch", mTrace->depth() > 1 mStages.size() > 0));

        auto fileName = mTrace->name();
        if(fileName == UNNAMED) {
          collapse(*mTrace, mTrace->depth() - 1);
        } else {
          auto it = mHeadersVisits.find(fileName);
          assert(((void)"enter/leave file preprocessor callback mismatch", it != mHeadersVisits.end()));

          if(it->second == 0) {
            //We assume that every file included only once (effectively) and if we already measured it, we just skip repeated appearance
            mTrace->drop();
          } else if(it->second == 1) {
            //This means it is first (and last) time we got chance to measure
            collapse(*mTrace, mTrace->depth() - 1);
            it->second = 0;
          } else {
            //This means we encounter recursive include and in assumption of include guards we just skip nested ones
            mTrace->drop();
            it->second -= 1;
          }
        }
      }
    }

    if(mOriginalCallbacks.file_change != nullptr) {
      (mOriginalCallbacks.file_change)(reader, lineMap);
    }

    mLastCallbackInfo = std::monostate{};

    if(lineMap == nullptr) {
      cleanup();
    }
  }
};

} //namespace internal

using internal::PpProfilerImpl;

PpProfiler::PpProfiler(const Options &options, std::shared_ptr<Trace> trace)
  : mImpl{std::make_unique<PpProfilerImpl>(options, std::move(trace))}
{

}

PpProfiler::PpProfiler(PpProfiler &&) = default;
PpProfiler &PpProfiler::operator=(PpProfiler &&) = default;
PpProfiler::~PpProfiler() = default;

} //namespace insight