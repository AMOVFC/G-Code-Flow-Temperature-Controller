// The real IProcessRunner.
//
// This is the only header in core that fronts OS-specific code; the implementation
// lives in core/src/platform/. Everything else in core sees only the IProcessRunner
// interface, so tests substitute a fake and never spawn anything (ADR-0002).

#pragma once

#include "sb53/Ports.hpp"

namespace sb53 {

class SubprocessRunner final : public IProcessRunner {
public:
    ProcessResult run(const std::filesystem::path& executable,
                      const std::vector<std::string>& arguments,
                      std::chrono::milliseconds timeout) override;
};

} // namespace sb53
