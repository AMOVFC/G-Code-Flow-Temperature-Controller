#include "sb53/Diagnostics.hpp"

#include <algorithm>

namespace sb53 {

Diagnostic info(Code c, std::string message)
{
    return Diagnostic{Severity::Info, c, std::move(message), 0};
}

Diagnostic warning(Code c, std::string message)
{
    return Diagnostic{Severity::Warning, c, std::move(message), 0};
}

Diagnostic error(Code c, std::string message)
{
    return Diagnostic{Severity::Error, c, std::move(message), 0};
}

std::string_view toString(Severity s) noexcept
{
    switch (s) {
    case Severity::Info:    return "info";
    case Severity::Warning: return "warning";
    case Severity::Error:   return "error";
    }
    return "unknown";
}

std::string_view toString(Code c) noexcept
{
    switch (c) {
    case Code::Unknown:                      return "unknown";

    case Code::FileNotFound:                 return "file-not-found";
    case Code::FileUnreadable:               return "file-unreadable";
    case Code::FileEmpty:                    return "file-empty";

    case Code::AbsoluteExtrusionUnsupported: return "absolute-extrusion-unsupported";
    case Code::ExtrusionModeUnknown:         return "extrusion-mode-unknown";
    case Code::AlreadyProcessed:             return "already-processed";
    case Code::PrintBodyNotFound:            return "print-body-not-found";

    case Code::ProfileNotFound:              return "profile-not-found";
    case Code::ProfileInvalid:               return "profile-invalid";
    case Code::FlowPointsNotIncreasing:      return "flow-points-not-increasing";
    case Code::TemperaturePointsNotMonotonic:return "temperature-points-not-monotonic";
    case Code::PrinterConfigMissing:         return "printer-config-missing";
    case Code::PrinterConfigInvalid:         return "printer-config-invalid";
    case Code::PrinterConfigMismatch:        return "printer-config-mismatch";

    case Code::EstimatorNotFound:            return "estimator-not-found";
    case Code::EstimatorFailed:              return "estimator-failed";
    case Code::EstimatorOutputUnparsable:    return "estimator-output-unparsable";

    case Code::NoExtrusionFound:             return "no-extrusion-found";
    case Code::OutputWriteFailed:            return "output-write-failed";
    case Code::Cancelled:                    return "cancelled";
    }
    return "unknown";
}

bool DiagnosticList::hasErrors() const noexcept
{
    return std::any_of(m_items.begin(), m_items.end(),
                       [](const Diagnostic& d) { return d.isError(); });
}

} // namespace sb53
