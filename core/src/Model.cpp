#include "sb53/Model.hpp"

#include <algorithm>

namespace sb53 {

bool allowsPressureAdvanceChange(FeatureType f) noexcept
{
    // ALGORITHM.md §8. Changing pressure advance mid-print can cause bubbles in the
    // extrudate, so it is permitted only in features concealed inside the part.
    // This list is a documented correctness requirement, not a tuning knob.
    switch (f) {
    case FeatureType::SparseInfill:
    case FeatureType::InternalSolidInfill:
    case FeatureType::InternalBridge:
    case FeatureType::Support:
        return true;

    case FeatureType::Unknown:
    case FeatureType::OuterWall:
    case FeatureType::InnerWall:
    case FeatureType::TopSurface:
    case FeatureType::Bridge:
    case FeatureType::Overhang:
    case FeatureType::Skirt:
        return false;
    }
    return false;   // unreachable for valid enumerators; defensive
}

std::optional<Celsius> TemperaturePlan::temperatureAtFilament(
    const std::vector<FlowSecond>& seconds, Millimetres usedFilament) const
{
    // The plan is indexed by cumulative extruded filament rather than by time or line
    // number, because filament consumption is invariant under the speed rewriting we
    // are about to perform. See ALGORITHM.md §6.
    //
    // `seconds` is monotonically non-decreasing in usedFilament, so a binary search is
    // valid. Every boundary case below returns an explicit value: the legacy's
    // equivalent left its result uninitialised on three of these paths
    // (legacy/known-bugs.md #3).

    if (seconds.empty() || achievableTemperature.empty()) {
        return std::nullopt;
    }

    // Guard against a malformed plan rather than reading out of bounds.
    const std::size_t n = std::min(seconds.size(), achievableTemperature.size());
    if (n == 0) {
        return std::nullopt;
    }

    // Before the first sample: the print has not reached flow-derived control yet.
    // The initial temperature applies, which the caller handles separately.
    if (usedFilament <= seconds.front().usedFilament) {
        return achievableTemperature.front();
    }

    // At or past the end: hold the final planned temperature.
    if (usedFilament >= seconds[n - 1].usedFilament) {
        return achievableTemperature[n - 1];
    }

    // First bucket whose cumulative filament reaches the requested position.
    const auto it = std::lower_bound(
        seconds.begin(), seconds.begin() + static_cast<std::ptrdiff_t>(n), usedFilament,
        [](const FlowSecond& s, Millimetres value) { return s.usedFilament < value; });

    const auto index = static_cast<std::size_t>(std::distance(seconds.begin(), it));
    return achievableTemperature[std::min(index, n - 1)];
}

} // namespace sb53
