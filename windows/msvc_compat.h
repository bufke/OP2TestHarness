// Build shims for compiling harness.cpp with MSVC (it is written for MinGW GCC; see its mingw_compat.h).
#pragma once
#define NOMINMAX
#include <winsock2.h>
#include <intrin.h>
#include <algorithm>
#include <cstring>
#include <utility>
using std::max;
using std::min;
#pragma intrinsic(_ReturnAddress)
#define __builtin_return_address(level) _ReturnAddress()
#include <windows.h>
#undef small
