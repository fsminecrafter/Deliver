#pragma once

// DLR_VERSION is injected by CMake via add_compile_definitions(DLR_VERSION="${PROJECT_VERSION}")
// Never hard-code a version here — always driven by CMakeLists.txt
#ifndef DLR_VERSION
#  define DLR_VERSION "unknown"
#endif

namespace dlr {
    constexpr const char* VERSION = DLR_VERSION;
} // namespace dlr