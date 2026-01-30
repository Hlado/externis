#pragma once

#include "options.h"

#include <memory>

namespace insight {

namespace internal {

class InstanceImpl;

} //namespace internal

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