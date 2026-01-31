#include "instance.h"

#include "insight.h"
#include "options.h"
#include "utils.h"

#include <gcc-plugin.h>

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <functional>
#include <numeric>
#include <stack>
#include <string>
#include <unordered_map>

using namespace std::chrono;

namespace insight {

namespace {

constexpr auto STAGE_FILE = std::size_t{1};
constexpr auto STAGE_STEP = std::size_t{2};

enum class Steps {
  Preprocessing,
  Parsing,
  Backend
};

struct Stage {
  using Records = std::unordered_map<std::string, nanoseconds>;

  TimePoint start;
  std::string name;
  Records records;

  void consume(const Stage &other)
  {
    if(this == &other) {
      for(auto &[n, d] : records) {
        d *= 2;
      }
      return;
    }

    for(auto &&[n, d] : other.records) {
      std::string fullName{other.name + ";" + n};
      auto it = records.find(name);
      if(it == records.end()) {
        auto [newIt, inserted] = records.emplace(std::move(fullName), nanoseconds::zero());
        it = newIt;
      }
      it->second += d;
    }
  }

  nanoseconds duration() const
  {
    using Pair = Records::value_type;
    return std::accumulate(records.cbegin(), records.cend(), nanoseconds::zero(), [](nanoseconds acc, const Pair &r) { return acc + r.second; });
  }

  void dump(std::ostream &stream) const
  {
    for(auto &&[n, d] : records) {
      stream << name << ";" << n << " " << duration_cast<microseconds>(d).count() << "\n";
    }
    stream.flush();
  }
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
  {
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

      mStages.push(Stage{Clock::now(), getFullInputName(), {}});
      mStages.push(Stage{Clock::now(), "Preprocessing", {}});
    }
    catch(const std::exception& e)
    {
      unregisterCallbacks();
      throw;
    }
  }

  ~InstanceImpl()
  {
    unregisterCallbacks();
  }

  InstanceImpl(const InstanceImpl &) = delete;
  InstanceImpl &operator=(const InstanceImpl &) = delete;

private:
  static void unregisterCallbacks() noexcept
  {
    unregister_callback(PLUGIN_NAME.data(), PLUGIN_FINISH);
    unregister_callback(PLUGIN_NAME.data(), PLUGIN_FINISH_DECL);
    unregister_callback(PLUGIN_NAME.data(), PLUGIN_ALL_PASSES_START);
  }

  template <void (InstanceImpl::*HandlerV)(void *)>
  static void handleCallback(void *gccData, void *userData)
  {
    try {
      auto obj = static_cast<InstanceImpl *>(userData);
      (obj->*HandlerV)(gccData);
    } catch(...) {
      //It is really hard to make this whole class exception safe, so we better stop on exception
      unregisterCallbacks();
      throw;
    }
  }

  const Options mOptions;
  std::stack<Stage> mStages;
  Steps step{Steps::Preprocessing}; 

  template <void (InstanceImpl::*HandlerV)(void *)>
  void registerCallback(int event) {
    register_callback(PLUGIN_NAME.data(), event, &::insight::handleCallback<&handleCallback<HandlerV>>, this);
  }

  template <void (InstanceImpl::*HandlerV)(void *)>
  void handleParserCallback(void *gccData)
  {
    if(step == Steps::Preprocessing) {
      step = Steps::Parsing;
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
    assert(((void)"desired depth must not be greater than stack size", desiredDepth >= mStages.size()));

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