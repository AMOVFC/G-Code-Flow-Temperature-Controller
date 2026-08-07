// Extruder and filament profiles.
//
// These are the user's calibration data, not constants. ALGORITHM.md §3 documents the
// visual calibration procedure users follow to derive their own values; we ship none.
//
// Field names and semantics mirror the legacy SQLite schema so existing databases can be
// read without migration (ADR-0003) -- that data represents hours of test prints per
// filament and is the only artefact of the old project that took real human effort.

#pragma once

#include "sb53/Diagnostics.hpp"
#include "sb53/Model.hpp"

#include <string>

namespace sb53 {

// What the hotend can physically do, plus how it is driven.
struct ExtruderProfile {
    std::string name;

    // Heating and cooling rates, stored as "N degrees over M seconds" exactly as the
    // legacy schema does, because that is how users measure them.
    //
    // They are NOT symmetric: heating is actively driven by the cartridge, cooling is
    // passive and depends on the part fan and ambient air. Cooling is typically much
    // slower, and the plan must respect that or it will command temperatures the hotend
    // never reaches.
    Celsius tempRise     = 10.0;
    Seconds tempRiseTime = 1.0;
    Celsius tempFall     = 5.0;
    Seconds tempFallTime = 1.0;

    // Width, in seconds, of the centered moving average applied to the flow signal.
    // README recommends 10-30. Larger means smoother temperature and fewer swings, at
    // the cost of tracking the flow less closely.
    int smoothingWindow = 20;

    // Klipper-style start macro rewriting (ALGORITHM.md §9). `startMacro` is the line
    // prefix to find (e.g. "PRINT_START") and `temperatureToken` the parameter whose
    // value is replaced (e.g. "EXTRUDER_TEMP"). Empty startMacro disables this path, in
    // which case the M109 route is used instead.
    std::string startMacro;
    std::string temperatureToken;

    [[nodiscard]] double riseRatePerSecond() const noexcept
    {
        return tempRiseTime > 0.0 ? tempRise / tempRiseTime : 0.0;
    }
    [[nodiscard]] double fallRatePerSecond() const noexcept
    {
        return tempFallTime > 0.0 ? tempFall / tempFallTime : 0.0;
    }

    void validate(DiagnosticList& diagnostics) const;
};

// The filament's flow/temperature calibration: three points, linearly interpolated.
struct FilamentProfile {
    std::string name;
    std::string type;   // PLA, PETG, ABS, ...

    // Three calibration points, low to high. ALGORITHM.md §3.
    CubicMmPerSec lowFlow  = 1.0;
    CubicMmPerSec midFlow  = 10.0;
    CubicMmPerSec highFlow = 20.0;

    Celsius lowTemp  = 195.0;
    Celsius midTemp  = 215.0;
    Celsius highTemp = 235.0;

    // Pressure advance at each of the same three points. Klipper only, and applied only
    // in concealed features -- see allowsPressureAdvanceChange() in Model.hpp.
    double lowPressureAdvance  = 0.0;
    double midPressureAdvance  = 0.0;
    double highPressureAdvance = 0.0;
    bool   adjustPressureAdvance = false;

    // 0 = Quality (track the per-second average, smoother temperature, fewer swings)
    // 10 = Speed  (track the per-second maximum, higher temperature, shorter print)
    //
    // Direction chosen to match the README's described semantics; the legacy's own
    // formula is undocumented and deliberately not reproduced (ADR-0006).
    int speedQualityBias = 5;

    [[nodiscard]] double biasFraction() const noexcept;

    void validate(DiagnosticList& diagnostics) const;
};

} // namespace sb53
