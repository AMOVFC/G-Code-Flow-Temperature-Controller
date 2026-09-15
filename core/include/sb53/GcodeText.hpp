// Low-level G-code text handling, shared by the scanner and the rewriter.
//
// Extracted so both passes agree on what a "word" is and what counts as a comment.
// They must: the scanner establishes the filament coordinate that the rewriter later
// walks, and any disagreement between them silently misaligns the temperature plan.
// That exact class of bug has already occurred once here -- see legacy/known-bugs.md and
// the unretract regression test.

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace sb53::detail {

[[nodiscard]] bool startsWith(std::string_view s, std::string_view prefix) noexcept;

[[nodiscard]] std::string_view trim(std::string_view s) noexcept;

// Everything before a ';' is the command; the rest is a comment.
[[nodiscard]] std::string_view command(std::string_view line) noexcept;

// Extracts a G-code word (e.g. 'F', 'E') from a line, ignoring any trailing comment.
// The letter must start a token, so the 'E' inside EXTRUDER_TEMP does not match.
[[nodiscard]] std::optional<double> word(std::string_view line, char letter) noexcept;

// True when the line is an extruding move: a G0/G1 that BOTH travels (has X or Y) and
// pushes filament (positive E).
//
// The travel requirement is what excludes unretracts and primes, which push filament
// into the nozzle without laying any down. The flow analyser zeroes those, so anything
// tracking filament position must exclude them too or the two coordinates drift apart.
[[nodiscard]] bool isExtrudingMove(std::string_view line) noexcept;

// Formats a number the way G-code expects: locale-invariant, no trailing zeros.
[[nodiscard]] std::string formatNumber(double value, int maxDecimals);

} // namespace sb53::detail
