#include "parser-profiler.h"

#include "options.h"
#include "trace.h"

// Always last
#include "gcc-headers.h"

using namespace std::chrono;

namespace insight {

namespace internal {

class ParserProfilerImpl {
public:
  explicit ParserProfilerImpl(const Options &options, std::shared_ptr<Trace> trace)
  : mOptions{options}
  , mTrace{std::move(trace)}
  {
  }

  ParserProfilerImpl(const ParserProfilerImpl &) = delete;
  ParserProfilerImpl &operator=(const ParserProfilerImpl &) = delete;

private:
  const Options mOptions;
  std::shared_ptr<Trace> mTrace;
};

} // namespace internal

using internal::ParserProfilerImpl;

ParserProfiler::ParserProfiler(const Options &options, std::shared_ptr<Trace> trace)
: mImpl{std::make_unique<ParserProfilerImpl>(options, std::move(trace))}
{
}

ParserProfiler::~ParserProfiler() = default;

} // namespace insight
