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

    //TODO failbit
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
      registerCallback<&InstanceImpl::handlePluginFinish>(PLUGIN_FINISH);

      mStages.push(Stage{Clock::now(), getFullInputName(), {}});
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
  }

  template <void (InstanceImpl::*HandlerV)(void *)>
  static void handleCallback(void *gccData, void *userData)
  {
    auto obj = static_cast<InstanceImpl *>(userData);
    (obj->*HandlerV)(gccData);
  }

  const Options mOptions;
  std::stack<Stage> mStages;

  template <void (InstanceImpl::*HandlerV)(void *)>
  void registerCallback(int event) {
    register_callback(PLUGIN_NAME.data(), event, &::insight::handleCallback<&handleCallback<HandlerV>>, this);
  }

  void handlePluginFinish(void *)
  {
    assert(((void)"no stages at the end", !mStages.empty()));

    auto root = std::move(mStages.top());
     while(mStages.size() > 1)
    {
      mStages.pop();
      mStages.top().consume(root);
      root = std::move(mStages.top());
    };

    auto total = measure(root.start, Clock::now());
    root.records["Uncategorized"] = max(nanoseconds::zero(), nanoseconds{total - root.duration()});

    std::shared_ptr<std::ostream> individual = getIndividualStream();
    if(individual) {
      root.dump(*individual);
    }

    std::shared_ptr<std::ostream> combined = getCombinedStream();
    if(combined) {
       root.dump(*combined);
    }
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

        logInfo("dumping to '", dumpPath, "'");
        stream = std::make_shared<std::ofstream>(dumpPath);
        if(stream->fail()) {
          throw Error{"failed to open '", dumpPath, "'"};
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
        logInfo("dumping combined to '", mOptions.combined, "'");
        
        stream = std::make_shared<std::ofstream>(mOptions.combined, std::ios_base::app);
        if(stream->fail()) {
          throw Error{"failed to open '", mOptions.combined, "'"};
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