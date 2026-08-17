// Fuzz target for the input parsers.
//
// Both consume untrusted data. A G-code file arrives from a slicer, but people download
// and share them, and this tool is pointed at whatever the user picked. The move dump
// crosses a subprocess boundary. Neither should be able to crash the process, read out
// of bounds, or hang.
//
// Build: cmake -DSB53_FUZZ=ON with clang. Run: ./fuzz_scanner corpus/

#include "sb53/FlowAnalysis.hpp"
#include "sb53/GcodeScanner.hpp"
#include "sb53/ProfileStore.hpp"

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0 || size > (1u << 20)) {
        return 0;
    }

    const std::string input(reinterpret_cast<const char*>(data), size);

    // The scanner: markers, body bounds, extrusion mode, slicer identity.
    {
        std::istringstream in{input};
        sb53::DiagnosticList diagnostics;
        const auto scan = sb53::GcodeScanner::scan(in, diagnostics);

        // Whatever the input, the reported bounds must be self-consistent -- the rewriter
        // indexes lines with them and would read past the end otherwise.
        if (scan.hasPrintBody() && scan.bodyLastLine < scan.bodyFirstLine) {
            __builtin_trap();
        }
        for (const auto& layer : scan.layers) {
            if (layer.usedFilament < 0.0) {
                __builtin_trap();   // filament use can never run backwards
            }
        }
    }

    // The estimator's output, treated as hostile.
    {
        std::istringstream in{input};
        sb53::DiagnosticList diagnostics;
        const auto moves = sb53::MoveDumpParser::parse(in, diagnostics);

        const auto analysis = sb53::analyseFlow(moves);

        // Cumulative filament is the coordinate the temperature plan is indexed by, so
        // it must be monotonic no matter how malformed the input was.
        double previous = -1.0;
        for (const auto& second : analysis.seconds) {
            if (second.usedFilament < previous) {
                __builtin_trap();
            }
            previous = second.usedFilament;
        }
    }

    // Saved profiles: written by this program, but a user may hand-edit them, and a
    // corrupt file must not take the application down on startup.
    {
        sb53::DiagnosticList diagnostics;
        (void)sb53::ProfileStore::fromJson(input, diagnostics);
    }

    return 0;
}
