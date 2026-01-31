#pragma once

//Weird thing, without that gcc 9 goes insane with compilation errors.
#if __GNUC__ == 9
#include <filesystem>
#endif

#include <memory>

namespace insight {

namespace internal {

class InstanceImpl;

} //namespace internal

struct Options;

class Instance {
public:
  explicit Instance(const Options &options);
  Instance(Instance &&);
  Instance &operator=(Instance &&);
  ~Instance();

private:
  std::unique_ptr<internal::InstanceImpl> mImpl;
};

} //namespace insight