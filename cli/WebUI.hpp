#pragma once

#include <filesystem>
#include <functional>

namespace sb53::web {

// Serves the local UI on 127.0.0.1:<port> until interrupted. Returns a process exit code.
//
// `findEstimator` is injected rather than duplicated so the web UI and the CLI locate the
// estimator by exactly the same rule.
int runServe(unsigned short port, const std::filesystem::path& exeDir,
             const std::function<std::filesystem::path(const std::filesystem::path&)>&
                 findEstimator);

} // namespace sb53::web
