// RUN: genpybind-tool --xfail %s -- -std=c++17 -xc++ -D__GENPYBIND__ 2>&1 | FileCheck %s --strict-whitespace
#pragma once

#include "genpybind.h"

struct GENPYBIND(visible) Target {};

// CHECK: parsing-errors.h:[[# @LINE + 1]]:8: error: Invalid genpybind annotation: something
struct GENPYBIND(visible, something) Context {
  // CHECK: parsing-errors.h:[[# @LINE + 1]]:15: error: Invalid token in genpybind annotation: 123
  using alias GENPYBIND(visible 123) = Target;
};

// CHECK: parsing-errors.h:[[# @LINE + 1]]:8: error: Invalid token in genpybind annotation while looking for ')': "abc"
struct GENPYBIND(expose_as(uiae "abc")) Xyz {};
// CHECK: 3 errors generated.
