#include "instance.h"

#include "insight.h"
#include "options.h"
#include "stage.h"
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

//Always last
#include "gcc-headers.h"

using namespace std::chrono;

extern struct cpp_reader* parse_in;

namespace insight {

namespace {

constexpr auto STAGE_FILE = std::size_t{1};
constexpr auto STAGE_STEP = std::size_t{2};

enum class Steps {
  Preprocessing,
  Parsing,
  Backend
};

nanoseconds measure(TimePoint then, TimePoint now)
{
  //Clock isn't guaranteed to be steady
  return max(nanoseconds::zero(), nanoseconds{now - then});
}

std::string getFullInputName()
{
  if(main_input_filename == nullptr || std::strcmp("", main_input_filename) == 0) {
    return std::string{"unnamed-"} + std::to_string(Clock::now().time_since_epoch().count());
  } else {
    return main_input_filename;
  }
}

} //unnamed namespace

namespace internal {

class InstanceImpl {
public:
  explicit InstanceImpl(const Options &options)
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
      registerCallback<&InstanceImpl::handleBackendCallback<&InstanceImpl::handleAllPassesStart>>(PLUGIN_ALL_PASSES_START);

      mPpCallbacks->file_change = &handlePpCallback<&InstanceImpl::handlePpFileChange, void, cpp_reader *, const line_map_ordinary *>;
      mPpCallbacks->used = &handlePpCallback<&InstanceImpl::handlePpMacroUsed, void, cpp_reader *, location_t, cpp_hashnode *>;

      mStages.push(Stage{Clock::now(), getFullInputName(), {}});
      mStages.push(Stage{Clock::now(), "Preprocessing", {}});
    }
    catch(const std::exception& e)
    {
      cleanup();
      throw;
    }
  }

  ~InstanceImpl()
  {
    cleanup();
  }

  InstanceImpl(const InstanceImpl &) = delete;
  InstanceImpl &operator=(const InstanceImpl &) = delete;

private:
  static inline std::unordered_map<cpp_reader *, InstanceImpl *> mReadersMapping;

  const Options mOptions;
  std::optional<cpp_callbacks> mPpCallbacksChain;
  cpp_reader *mReader{nullptr};
  cpp_callbacks *mPpCallbacks{nullptr};
  std::stack<Stage> mStages;
  std::unordered_map<std::string, std::size_t> mHeadersVisits;
  Steps step{Steps::Preprocessing};


  static void unregisterCallbacks() noexcept
  {
    unregister_callback(PLUGIN_NAME.data(), PLUGIN_FINISH);
    unregister_callback(PLUGIN_NAME.data(), PLUGIN_FINISH_DECL);
    unregister_callback(PLUGIN_NAME.data(), PLUGIN_ALL_PASSES_START);
  }

  template <void (InstanceImpl::*HandlerV)(void *)>
  static void handleCallback(void *gccData, void *userData)
  {
    auto obj = static_cast<InstanceImpl *>(userData);

    try {
      (obj->*HandlerV)(gccData);
    } catch(...) {
      //It is really hard to make this whole class exception safe, so we better stop on exception
      obj->cleanup();
      throw;
    }
  }

  template <auto HandlerV, typename RetT, typename... ArgsT>
  static RetT handlePpCallback(ArgsT... args)
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

  void cleanup() noexcept {
    unregisterCallbacks();
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

    if(mPpCallbacks->file_change != &handlePpCallback<&InstanceImpl::handlePpFileChange, void, cpp_reader *, const line_map_ordinary *>) {
      logError("fatal error: preprocessor callbacks, abnormal termination...");
      std::abort();
    }
    mPpCallbacks->file_change = mPpCallbacksChain->file_change;

    if(mPpCallbacks->used != &handlePpCallback<&InstanceImpl::handlePpMacroUsed, void, cpp_reader *, location_t, cpp_hashnode *>) {
      logError("fatal error: preprocessor callbacks, abnormal termination...");
      std::abort();
    }
    mPpCallbacks->used = mPpCallbacksChain->used;

    mReadersMapping.erase(mReader);
    mReader = nullptr;
    mPpCallbacks = nullptr;
  }

  void handlePpMacroUsed(cpp_reader *, location_t loc, cpp_hashnode *node)
  {
    expanded_location xloc = expand_location(loc);

    logInfo("macro '", NODE_NAME(node), "' at '", xloc.file, ":", xloc.line, ":", xloc.column, "'");
  }

  void handlePpFileChange(cpp_reader *reader, const line_map_ordinary *lineMap)
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
        assert(((void)"enter/leave file preprocessor callback mismatch", mStages.size() > STAGE_STEP));

        auto fileName = mStages.top().name;
        if(fileName == UNNAMED) {
          collapseStages(mStages.size() - 1);
        } else {
          auto it = mHeadersVisits.find(fileName);
          assert(((void)"enter/leave file preprocessor callback mismatch", it != mHeadersVisits.end()));

          if(it->second == 0) {
            //We assume that every file included only once (effectively) and if we already measured it, we just skip repeated appearance
            mStages.pop();
          } else if(it->second == 1) {
            //This means it is first (and last) time we got chance to measure
            collapseStages(mStages.size() - 1);
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

  template <void (InstanceImpl::*HandlerV)(void *)>
  void registerCallback(int event) {
    register_callback(PLUGIN_NAME.data(), event, &::insight::handleCallback<&handleCallback<HandlerV>>, this);
  }

  template <void (InstanceImpl::*HandlerV)(void *)>
  void handleParserCallback(void *gccData)
  {
    if(step == Steps::Preprocessing) {
      step = Steps::Parsing;
      restorePpCallbacks();
      collapseStages(STAGE_FILE);
      mStages.push(Stage{Clock::now(), "Parsing", {}});
    } else {
      if(step != Steps::Parsing) {
        throw Error{"parsing callback happened at the wrong step (", static_cast<std::underlying_type_t<Steps>>(step), ")"};
      }
    }

    (this->*HandlerV)(gccData);
  }

  template <void (InstanceImpl::*HandlerV)(void *)>
  void handleBackendCallback(void *gccData)
  {
    if(step == Steps::Parsing) {
      step = Steps::Backend;
      collapseStages(STAGE_FILE);
      mStages.push(Stage{Clock::now(), "Backend", {}});
    } else {
      if(step != Steps::Backend) {
        throw Error{"backend callback happened at the wrong step (", static_cast<std::underlying_type_t<Steps>>(step), ")"};
      }
    }

    (this->*HandlerV)(gccData);
  }

  void handleFinishDecl(void *) {
    //Just to test wrapper callbacks for now
  }

  void handleAllPassesStart(void *) {
    //Just to test wrapper callbacks for now
  }

  void handlePluginFinish(void *)
  {
    assert(((void)"no stages at the end", !mStages.empty()));

    collapseStages(STAGE_FILE);
    auto &root = mStages.top();

    auto total = measure(root.start, Clock::now());
    root.records["Uncategorized"] = max(nanoseconds::zero(), nanoseconds{total - root.duration()});

    std::shared_ptr<std::ostream> individual = getIndividualStream();
    if(individual) {
      root.dump(*individual);
      if(individual->fail()) {
        throw Error{"failed to dump individual trace"};
      }
    }

    std::shared_ptr<std::ostream> combined = getCombinedStream();
    if(combined) {
       root.dump(*combined);
       if(combined->fail()) {
        throw Error{"failed to dump combined trace"};
      }
    }
  }

  

  void collapseStages(std::size_t desiredDepth) {
    assert(((void)"desired depth must be positive", desiredDepth > 0));
    assert(((void)"desired depth must not be greater than stack size", desiredDepth <= mStages.size()));

    auto now = Clock::now();

    while(mStages.size() > desiredDepth)
    {
      auto stage = std::move(mStages.top());
      mStages.pop();

      auto total = measure(stage.start, now);
      stage.records["Uncategorized"] = max(nanoseconds::zero(), nanoseconds{total - stage.duration()});

      mStages.top().consume(stage);
    };
  }
  std::shared_ptr<std::ostream> getIndividualStream() const
  {
    std::shared_ptr<std::ostream> stream;

    if(!mOptions.noIndividual) {
      if(!std::filesystem::exists(main_input_filename)) {
        logInfo("input is not a file, dumping to cout");
        stream = std::shared_ptr<std::ostream>{&std::cout, [](auto &&){}};
      } else {
        auto dumpPath = std::filesystem::path{dump_base_name};
        dumpPath += ".trace.collapsed";

        logInfo("dumping to '", dumpPath.string(), "'");
        stream = std::make_shared<std::ofstream>(dumpPath);
        if(stream->fail()) {
          throw Error{"failed to open '", dumpPath.string(), "'"};
        }
      }
    }

    return stream;
  }

  std::shared_ptr<std::ostream> getCombinedStream() const
  {
    std::shared_ptr<std::ostream> stream;

    if(!mOptions.noIndividual) {
      if(!mOptions.combined.empty()) {
        logInfo("dumping combined to '", mOptions.combined.string(), "'");
        
        stream = std::make_shared<std::ofstream>(mOptions.combined, std::ios_base::app);
        if(stream->fail()) {
          throw Error{"failed to open '", mOptions.combined.string(), "'"};
        }
      }
    }

    return stream;
  }
};

} //namespace internal

using internal::InstanceImpl;

Instance::Instance(const Options &options)
  : mImpl{std::make_unique<InstanceImpl>(options)}
{

}

Instance::Instance(Instance &&) = default;
Instance &Instance::operator=(Instance &&) = default;
Instance::~Instance() = default;

} //namespace insight