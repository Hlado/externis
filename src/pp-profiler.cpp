#include "pp-profiler.h"

#include "options.h"
#include "stage.h"
#include "utils.h"

#include <cassert>
#include <stack>
#include <unordered_map>

//Always last
#include "gcc-headers.h"

using namespace std::chrono;

extern struct cpp_reader* parse_in;

namespace insight {

namespace internal {

class PpProfilerImpl
{
public:
  explicit PpProfilerImpl(const Options &options)
    : mOptions{options}
    , mReader{parse_in}
  {
    if(mReader == nullptr) {
      throw Error{"reader is not set"};
    }
    mPpCallbacks = cpp_get_callbacks(mReader);
    if(mPpCallbacks == nullptr) {
      throw Error{"preprocessor callbacks are empty"};
    }
    mPpCallbacksChain = *mPpCallbacks;
    mReadersMapping[mReader] = this;

    try
    {
      mPpCallbacks->file_change = &handleCallback<&PpProfilerImpl::handleFileChange, void, cpp_reader *, const line_map_ordinary *>;
      mPpCallbacks->used = &handleCallback<&PpProfilerImpl::handleMacroUsed, void, cpp_reader *, location_t, cpp_hashnode *>;

      mStages.push(Stage{Clock::now(), "Preprocessing", {}});
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

  void dump(Stage &sink) const
  {
    //TODO: probably need more efficient solution - && overload?
    auto tmp = mStages;
    Stage::collapse(tmp, 1);
    sink.consume(tmp.top());
  }

private:
  static inline std::unordered_map<cpp_reader *, PpProfilerImpl *> mReadersMapping;

  const Options mOptions;
  std::optional<cpp_callbacks> mPpCallbacksChain;
  cpp_reader *mReader{nullptr};
  cpp_callbacks *mPpCallbacks{nullptr};
  std::stack<Stage> mStages;
  std::unordered_map<std::string, std::size_t> mHeadersVisits;


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
    if(mPpCallbacks != ppCallbacks) {
      logError("fatal error: preprocessor callbacks, abnormal termination...");
      std::abort();
    }

    if(mPpCallbacks->file_change != &handleCallback<&PpProfilerImpl::handleFileChange, void, cpp_reader *, const line_map_ordinary *>) {
      logError("fatal error: preprocessor callbacks, abnormal termination...");
      std::abort();
    }
    mPpCallbacks->file_change = mPpCallbacksChain->file_change;

    if(mPpCallbacks->used != &handleCallback<&PpProfilerImpl::handleMacroUsed, void, cpp_reader *, location_t, cpp_hashnode *>) {
      logError("fatal error: preprocessor callbacks, abnormal termination...");
      std::abort();
    }
    mPpCallbacks->used = mPpCallbacksChain->used;

    mReadersMapping.erase(mReader);
    mReader = nullptr;
    mPpCallbacks = nullptr;
  }

  void handleMacroUsed(cpp_reader *, location_t loc, cpp_hashnode *node)
  {
    expanded_location xloc = expand_location(loc);

    logInfo("macro '", NODE_NAME(node), "' at '", xloc.file, ":", xloc.line, ":", xloc.column, "'");
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

        mStages.push(Stage{Clock::now(), fileName, {}});
      } else if(lineMap->reason == LC_LEAVE) {
        assert(((void)"enter/leave file preprocessor callback mismatch", mStages.size() > 0));

        auto fileName = mStages.top().name;
        if(fileName == UNNAMED) {
          Stage::collapse(mStages, mStages.size() - 1);
        } else {
          auto it = mHeadersVisits.find(fileName);
          assert(((void)"enter/leave file preprocessor callback mismatch", it != mHeadersVisits.end()));

          if(it->second == 0) {
            //We assume that every file included only once (effectively) and if we already measured it, we just skip repeated appearance
            mStages.pop();
          } else if(it->second == 1) {
            //This means it is first (and last) time we got chance to measure
            Stage::collapse(mStages, mStages.size() - 1);
            it->second = 0;
          } else {
            //This means we encounter recursive include and in assumption of include guards we just skip nested ones
            mStages.pop();
            it->second -= 1;
          }
        }
      }
    }

    if(mPpCallbacksChain->file_change != nullptr) {
      (*mPpCallbacksChain->file_change)(reader, lineMap);
    }
  }

  template <auto HandlerV, typename RetT, typename... ArgsT>
  static RetT handleCallback(ArgsT... args)
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
      if constexpr(std::is_same_v<void, RetT>) {
        (obj->*HandlerV)(args...);
      } else {
        return (obj->*HandlerV)(args...);
      }
    } catch(...) {
      //It is really hard to make this whole class exception safe, so we better stop on exception
      obj->cleanup();
      throw;
    }
  }
};

} //namespace internal

using internal::PpProfilerImpl;

PpProfiler::PpProfiler(const Options &options)
  : mImpl{std::make_unique<PpProfilerImpl>(options)}
{

}

PpProfiler::PpProfiler(PpProfiler &&) = default;
PpProfiler &PpProfiler::operator=(PpProfiler &&) = default;
PpProfiler::~PpProfiler() = default;

void PpProfiler::dump(Stage &sink) const
{
  return mImpl->dump(sink);
}

} //namespace insight