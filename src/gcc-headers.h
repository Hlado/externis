#pragma once

//gcc headers have to be quarantined due to surprises like #define abort
//
//Avoid including it from headers

//Has to be included first
#include <gcc-plugin.h>

#include <cpplib.h>
#include <plugin-version.h>

#include <system.h>
#include <tree.h>
#include <tree-pass.h>

#undef abort