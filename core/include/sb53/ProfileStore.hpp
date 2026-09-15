// Saved printers and their filaments.
//
// Lives in core, not the web layer, so the planned OrcaSlicer plugin inherits it rather
// than reimplementing it (ADR-0002, ADR-0003).
//
// Deliberately NOT written to the legacy Config.sdb. That file is irreplaceable user
// calibration data and the README tells people to carry it across upgrades; we read it
// for import and never write to it.

#pragma once

#include "sb53/Diagnostics.hpp"
#include "sb53/Profiles.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace sb53 {

// A filament belongs to a printer and carries only what differs from it.
//
// That containment is the point of the feature: "keep most settings, change one" means
// duplicating a filament under the same printer and editing a field, not re-entering
// every hotend and machine value.
struct SavedFilament {
    std::string name;
    std::string type = "PLA";

    CubicMmPerSec lowFlow = 1.0, midFlow = 80.0, highFlow = 105.0;
    Celsius lowTemp = 220.0, midTemp = 280.0, highTemp = 310.0;
    int bias = 7;

    CubicMmPerSec minFlow = 0.0, maxFlow = 0.0;
    bool hardFlowLimit = false;
    bool adjustPressureAdvance = false;
};

// Everything that stays the same when you switch filament on one machine.
struct SavedPrinter {
    std::string name;

    // Hotend and planner behaviour.
    Celsius rise = 5.0, fall = 1.0;
    int smoothing = 20;
    Seconds coolingLayerTime = 5.0;
    Celsius coolingMaxDrop = 0.0;

    std::string startMacro = "PRINT_START";
    std::string temperatureToken = "EXTRUDER_TEMP";

    // Machine limits. Zero means "leave config.json alone".
    double maxVelocity = 0.0, maxAcceleration = 0.0, squareCornerVelocity = 0.0;
    double zVelocity = 0.0, zAcceleration = 0.0;

    std::vector<SavedFilament> filaments;

    [[nodiscard]] const SavedFilament* findFilament(std::string_view name) const;
};

class ProfileStore {
public:
    // %LOCALAPPDATA%\flowtemp\profiles.json on Windows; ~/.config/flowtemp elsewhere.
    [[nodiscard]] static std::filesystem::path defaultPath();

    // A missing file is not an error -- it is a first run, and yields an empty store.
    [[nodiscard]] static ProfileStore load(const std::filesystem::path&,
                                           DiagnosticList&);

    // Written to a temporary file and moved into place, so an interrupted save cannot
    // destroy existing profiles.
    bool save(const std::filesystem::path&, DiagnosticList&) const;

    [[nodiscard]] const std::vector<SavedPrinter>& printers() const { return m_printers; }
    [[nodiscard]] SavedPrinter* find(std::string_view name);

    // Replaces a printer of the same name, otherwise appends.
    void upsert(SavedPrinter);
    bool remove(std::string_view name);

    [[nodiscard]] std::string toJson() const;
    [[nodiscard]] static ProfileStore fromJson(std::string_view, DiagnosticList&);

private:
    std::vector<SavedPrinter> m_printers;
};

} // namespace sb53
