#pragma once

#include "utils.h"

#include <string>

//Always last
#include "gcc-headers.h"

namespace insight {

std::string getFunctionId(tree decl, Verbosity verbosity = Verbosity::Default);

} //namespace insight