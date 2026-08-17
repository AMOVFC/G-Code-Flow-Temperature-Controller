#include "sb53/GcodeText.hpp"

#include <charconv>
#include <system_error>

namespace sb53::detail {

bool startsWith(std::string_view s, std::string_view prefix) noexcept
{
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

std::string_view trim(std::string_view s) noexcept
{
    const auto a = s.find_first_not_of(" \t\r");
    if (a == std::string_view::npos) {
        return {};
    }
    return s.substr(a, s.find_last_not_of(" \t\r") - a + 1);
}

std::string_view command(std::string_view line) noexcept
{
    const auto semi = line.find(';');
    return trim(semi == std::string_view::npos ? line : line.substr(0, semi));
}

std::optional<double> word(std::string_view line, char letter) noexcept
{
    const std::string_view cmd = command(line);
    for (std::size_t i = 0; i < cmd.size(); ++i) {
        if (cmd[i] != letter) {
            continue;
        }
        // Must start a token, or the 'E' in EXTRUDER_TEMP would match.
        if (i > 0 && cmd[i - 1] != ' ' && cmd[i - 1] != '\t') {
            continue;
        }
        const std::string_view rest = cmd.substr(i + 1);
        double value = 0.0;
        const auto* begin = rest.data();
        const auto* end = rest.data() + rest.size();
        const auto [ptr, ec] = std::from_chars(begin, end, value);
        if (ec == std::errc{} && ptr != begin) {
            return value;
        }
    }
    return std::nullopt;
}

bool isExtrudingMove(std::string_view line) noexcept
{
    const std::string_view cmd = command(line);
    if (!startsWith(cmd, "G1") && !startsWith(cmd, "G0")) {
        return false;
    }
    const auto e = word(line, 'E');
    if (!e.has_value() || !(*e > 0.0)) {
        return false;
    }
    return word(line, 'X').has_value() || word(line, 'Y').has_value();
}

std::string formatNumber(double value, int maxDecimals)
{
    // Locale-invariant, and trailing zeros removed so output reads like the slicer's own
    // (M104 S213, not M104 S213.000). Relying on locale is what broke the legacy
    // (known-bugs.md #5).
    std::string out(32, '\0');
    auto [ptr, ec] = std::to_chars(out.data(), out.data() + out.size(), value,
                                   std::chars_format::fixed, maxDecimals);
    if (ec != std::errc{}) {
        return "0";
    }
    out.resize(static_cast<std::size_t>(ptr - out.data()));

    if (out.find('.') != std::string::npos) {
        out.erase(out.find_last_not_of('0') + 1);
        if (!out.empty() && out.back() == '.') {
            out.pop_back();
        }
    }
    return out.empty() ? "0" : out;
}

} // namespace sb53::detail
