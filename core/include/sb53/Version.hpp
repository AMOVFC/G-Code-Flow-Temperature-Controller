#pragma once

#include <string_view>

namespace sb53 {

inline constexpr int kVersionMajor = 2;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 0;

inline constexpr std::string_view kVersion = "2.0.0";

// Includes "C++ Edition" so that output, window titles and log lines are never mistaken
// for the original Delphi application's. The two produce similar files and only one of
// them has been validated by real prints.
inline constexpr std::string_view kProductName =
    "G-Code Flow & Temperature Controller (C++ Edition)";

// Written into the header of every processed file, and matched on input to detect
// re-processing (ALGORITHM.md §9). Changing this string breaks that detection for files
// produced by earlier builds — treat it as a compatibility surface.
inline constexpr std::string_view kProcessedMarker = "; Processed by G-Code Flow & Temperature Controller";

} // namespace sb53
