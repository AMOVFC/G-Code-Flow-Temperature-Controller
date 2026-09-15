#include "sb53/ProfileStore.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace sb53 {
namespace {

// --- minimal JSON emission --------------------------------------------------

std::string quote(std::string_view s)
{
    std::string out = "\"";
    for (const char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) >= 0x20) { out += c; }
        }
    }
    return out + "\"";
}

std::string num(double v)
{
    std::ostringstream os;
    os.imbue(std::locale::classic());   // never a comma separator, whatever the locale
    os << v;
    return os.str();
}

// --- minimal JSON reading ---------------------------------------------------
//
// The file is written by this program and its shape is fixed, so a targeted reader is
// enough. Tolerant by design: an unknown or missing key leaves the default in place
// rather than failing the load, so an older profiles.json still opens.

std::size_t matchBrace(std::string_view s, std::size_t open)
{
    const char c = s[open];
    const char close = (c == '{') ? '}' : ']';
    int depth = 0;
    bool inString = false;
    for (std::size_t i = open; i < s.size(); ++i) {
        if (s[i] == '"' && (i == 0 || s[i - 1] != '\\')) { inString = !inString; }
        if (inString) { continue; }
        if (s[i] == c) { ++depth; }
        else if (s[i] == close && --depth == 0) { return i; }
    }
    return std::string_view::npos;
}

std::size_t keyAt(std::string_view s, std::string_view key, std::size_t from,
                  std::size_t to)
{
    const std::string needle = "\"" + std::string(key) + "\"";
    const auto at = s.find(needle, from);
    if (at == std::string_view::npos || at >= to) { return std::string_view::npos; }
    const auto colon = s.find(':', at + needle.size());
    if (colon == std::string_view::npos) { return std::string_view::npos; }
    return s.find_first_not_of(" \t\r\n", colon + 1);
}

void readNum(std::string_view s, std::string_view key, double& out, std::size_t from,
             std::size_t to)
{
    const auto at = keyAt(s, key, from, to);
    if (at == std::string_view::npos) { return; }
    double v = 0.0;
    if (std::from_chars(s.data() + at, s.data() + s.size(), v).ec == std::errc{}) {
        out = v;
    }
}

void readInt(std::string_view s, std::string_view key, int& out, std::size_t from,
             std::size_t to)
{
    double v = out;
    readNum(s, key, v, from, to);
    out = static_cast<int>(v);
}

void readBool(std::string_view s, std::string_view key, bool& out, std::size_t from,
              std::size_t to)
{
    const auto at = keyAt(s, key, from, to);
    if (at != std::string_view::npos) { out = s.compare(at, 4, "true") == 0; }
}

void readStr(std::string_view s, std::string_view key, std::string& out,
             std::size_t from, std::size_t to)
{
    const auto at = keyAt(s, key, from, to);
    if (at == std::string_view::npos || s[at] != '"') { return; }
    std::string value;

    // A while loop, not a for loop: the escape-sequence branch below needs to advance by
    // 2 characters (the backslash and the character it escapes) while the plain branch
    // advances by 1, and a for-loop's fixed `++i` header cannot express that. An earlier
    // version used a for-loop with `++i` in the body to reach the escaped character before
    // reading it, which CodeQL's cpp/loop-variable-changed correctly flagged as hard to
    // follow -- not because it was wrong (it was traced correct and is covered by
    // test_profilestore.cpp), but because a reader has to notice the body mutates the same
    // variable the header increments. Reading `s[i + 1]` directly, without mutating `i`
    // first, is also clearer on its own: the escaped character is looked at before any
    // index changes, rather than after.
    std::size_t i = at + 1;
    while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char escaped = s[i + 1];
            switch (escaped) {
            case 'n': value += '\n'; break;
            case 't': value += '\t'; break;
            case 'r': value += '\r'; break;
            default:  value += escaped;
            }
            i += 2;   // the backslash and the character it escaped
        } else {
            value += s[i];
            ++i;
        }
    }
    out = value;
}

} // namespace

const SavedFilament* SavedPrinter::findFilament(std::string_view wanted) const
{
    const auto it = std::find_if(filaments.begin(), filaments.end(),
                                 [&](const SavedFilament& f) { return f.name == wanted; });
    return it == filaments.end() ? nullptr : &*it;
}

std::filesystem::path ProfileStore::defaultPath()
{
    // std::getenv is deprecated by MSVC in favour of _dupenv_s. Reading an environment
    // variable we do not modify is not the hazard that warning is about, and the
    // alternative is not portable, so it is silenced locally rather than project-wide.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
#ifdef _WIN32
    if (const char* local = std::getenv("LOCALAPPDATA")) {
        return std::filesystem::path(local) / "flowtemp" / "profiles.json";
    }
#else
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path(home) / ".config" / "flowtemp" / "profiles.json";
    }
#endif
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    return std::filesystem::path("profiles.json");
}

SavedPrinter* ProfileStore::find(std::string_view name)
{
    const auto it = std::find_if(m_printers.begin(), m_printers.end(),
                                 [&](const SavedPrinter& p) { return p.name == name; });
    return it == m_printers.end() ? nullptr : &*it;
}

void ProfileStore::upsert(SavedPrinter printer)
{
    if (auto* existing = find(printer.name)) {
        *existing = std::move(printer);
        return;
    }
    m_printers.push_back(std::move(printer));
}

bool ProfileStore::remove(std::string_view name)
{
    const auto it = std::remove_if(m_printers.begin(), m_printers.end(),
                                   [&](const SavedPrinter& p) { return p.name == name; });
    if (it == m_printers.end()) { return false; }
    m_printers.erase(it, m_printers.end());
    return true;
}

std::string ProfileStore::toJson() const
{
    std::string out = "{\n  \"printers\": [\n";
    for (std::size_t i = 0; i < m_printers.size(); ++i) {
        const auto& p = m_printers[i];
        out += "    {\n";
        out += "      \"name\": " + quote(p.name) + ",\n";
        out += "      \"rise\": " + num(p.rise) + ",\n";
        out += "      \"fall\": " + num(p.fall) + ",\n";
        out += "      \"smoothing\": " + num(p.smoothing) + ",\n";
        out += "      \"coolingLayerTime\": " + num(p.coolingLayerTime) + ",\n";
        out += "      \"coolingMaxDrop\": " + num(p.coolingMaxDrop) + ",\n";
        out += "      \"startMacro\": " + quote(p.startMacro) + ",\n";
        out += "      \"temperatureToken\": " + quote(p.temperatureToken) + ",\n";
        out += "      \"maxVelocity\": " + num(p.maxVelocity) + ",\n";
        out += "      \"maxAcceleration\": " + num(p.maxAcceleration) + ",\n";
        out += "      \"squareCornerVelocity\": " + num(p.squareCornerVelocity) + ",\n";
        out += "      \"zVelocity\": " + num(p.zVelocity) + ",\n";
        out += "      \"zAcceleration\": " + num(p.zAcceleration) + ",\n";
        out += "      \"filaments\": [\n";
        for (std::size_t j = 0; j < p.filaments.size(); ++j) {
            const auto& f = p.filaments[j];
            out += "        {";
            out += "\"name\": " + quote(f.name);
            out += ", \"type\": " + quote(f.type);
            out += ", \"lowFlow\": " + num(f.lowFlow);
            out += ", \"midFlow\": " + num(f.midFlow);
            out += ", \"highFlow\": " + num(f.highFlow);
            out += ", \"lowTemp\": " + num(f.lowTemp);
            out += ", \"midTemp\": " + num(f.midTemp);
            out += ", \"highTemp\": " + num(f.highTemp);
            out += ", \"bias\": " + num(f.bias);
            out += ", \"minFlow\": " + num(f.minFlow);
            out += ", \"maxFlow\": " + num(f.maxFlow);
            out += ", \"hardFlowLimit\": " + std::string(f.hardFlowLimit ? "true" : "false");
            out += ", \"adjustPressureAdvance\": " +
                   std::string(f.adjustPressureAdvance ? "true" : "false");
            out += "}";
            if (j + 1 < p.filaments.size()) { out += ","; }
            out += "\n";
        }
        out += "      ]\n    }";
        if (i + 1 < m_printers.size()) { out += ","; }
        out += "\n";
    }
    out += "  ]\n}\n";
    return out;
}

ProfileStore ProfileStore::fromJson(std::string_view json, DiagnosticList& diagnostics)
{
    ProfileStore store;

    const auto printersAt = keyAt(json, "printers", 0, json.size());
    if (printersAt == std::string_view::npos || json[printersAt] != '[') {
        if (!json.empty()) {
            diagnostics.add(warning(Code::ProfileInvalid,
                                    "profiles.json has no printers list; starting empty."));
        }
        return store;
    }
    const auto printersEnd = matchBrace(json, printersAt);

    std::size_t cursor = printersAt + 1;
    while (cursor < printersEnd) {
        const auto open = json.find('{', cursor);
        if (open == std::string_view::npos || open >= printersEnd) { break; }
        const auto close = matchBrace(json, open);
        if (close == std::string_view::npos) { break; }

        SavedPrinter p;
        // Bound reads to this printer, but stop before its filament list so a filament's
        // "name" is not mistaken for the printer's.
        const auto filamentsAt = keyAt(json, "filaments", open, close);
        const auto scalarEnd = filamentsAt == std::string_view::npos ? close : filamentsAt;

        readStr(json, "name", p.name, open, scalarEnd);
        readNum(json, "rise", p.rise, open, scalarEnd);
        readNum(json, "fall", p.fall, open, scalarEnd);
        readInt(json, "smoothing", p.smoothing, open, scalarEnd);
        readNum(json, "coolingLayerTime", p.coolingLayerTime, open, scalarEnd);
        readNum(json, "coolingMaxDrop", p.coolingMaxDrop, open, scalarEnd);
        readStr(json, "startMacro", p.startMacro, open, scalarEnd);
        readStr(json, "temperatureToken", p.temperatureToken, open, scalarEnd);
        readNum(json, "maxVelocity", p.maxVelocity, open, scalarEnd);
        readNum(json, "maxAcceleration", p.maxAcceleration, open, scalarEnd);
        readNum(json, "squareCornerVelocity", p.squareCornerVelocity, open, scalarEnd);
        readNum(json, "zVelocity", p.zVelocity, open, scalarEnd);
        readNum(json, "zAcceleration", p.zAcceleration, open, scalarEnd);

        if (filamentsAt != std::string_view::npos && json[filamentsAt] == '[') {
            const auto fEnd = matchBrace(json, filamentsAt);
            std::size_t fc = filamentsAt + 1;
            while (fc < fEnd) {
                const auto fo = json.find('{', fc);
                if (fo == std::string_view::npos || fo >= fEnd) { break; }
                const auto fcl = matchBrace(json, fo);
                if (fcl == std::string_view::npos) { break; }

                SavedFilament f;
                readStr(json, "name", f.name, fo, fcl);
                readStr(json, "type", f.type, fo, fcl);
                readNum(json, "lowFlow", f.lowFlow, fo, fcl);
                readNum(json, "midFlow", f.midFlow, fo, fcl);
                readNum(json, "highFlow", f.highFlow, fo, fcl);
                readNum(json, "lowTemp", f.lowTemp, fo, fcl);
                readNum(json, "midTemp", f.midTemp, fo, fcl);
                readNum(json, "highTemp", f.highTemp, fo, fcl);
                readInt(json, "bias", f.bias, fo, fcl);
                readNum(json, "minFlow", f.minFlow, fo, fcl);
                readNum(json, "maxFlow", f.maxFlow, fo, fcl);
                readBool(json, "hardFlowLimit", f.hardFlowLimit, fo, fcl);
                readBool(json, "adjustPressureAdvance", f.adjustPressureAdvance, fo, fcl);
                if (!f.name.empty()) { p.filaments.push_back(std::move(f)); }
                fc = fcl + 1;
            }
        }

        if (!p.name.empty()) { store.m_printers.push_back(std::move(p)); }
        cursor = close + 1;
    }
    return store;
}

ProfileStore ProfileStore::load(const std::filesystem::path& path,
                                DiagnosticList& diagnostics)
{
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return {};   // first run, not an error
    }
    const std::string json((std::istreambuf_iterator<char>(in)), {});
    return fromJson(json, diagnostics);
}

bool ProfileStore::save(const std::filesystem::path& path,
                        DiagnosticList& diagnostics) const
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    // Write beside the target then move, so an interrupted save cannot leave the user
    // with a truncated profiles file.
    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream out{temporary, std::ios::binary};
        if (!out) {
            diagnostics.add(error(Code::OutputWriteFailed,
                                  "Could not write profiles to " + temporary));
            return false;
        }
        out << toJson();
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::filesystem::copy_file(
            temporary, path, std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(temporary, ec);
    }
    return true;
}

} // namespace sb53
