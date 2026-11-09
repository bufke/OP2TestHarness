// Force-included when building TethysAPI missions with MinGW GCC. TethysAPI is written against MSVC,
// which pulls these in transitively and provides global min/max via <windows.h>.
#pragma once
#include <algorithm>
#include <cstring>
#include <utility>
using std::max;
using std::min;
