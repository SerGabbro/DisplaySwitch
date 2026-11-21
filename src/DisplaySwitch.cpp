// DisplaySwitch - switches the Windows display layout between named profiles
// (e.g. desk monitors <-> a virtual display for game streaming).
//
// Usage:
//   DisplaySwitch.exe list                 monitors (ID, state, mode, position) and profiles
//   DisplaySwitch.exe apply <profile>      apply a profile from profiles.yaml (next to the exe)
//   DisplaySwitch.exe version              print the version
//   DisplaySwitch.exe help                 print the usage
//
// Monitors are identified by their hardware ID (EDID manufacturer + product code,
// e.g. DEL41A8). Unlike the CCD target id, the adapter LUID, the GDI name or the
// PnP instance path, it does not change across reboots, driver reinstalls or when
// the cable is moved to another port.
//
// Every result is appended to DisplaySwitch.log (named after the exe) next to the exe,
// because the tool is usually launched without a console (shortcuts, services, scripts).

#include <windows.h>
#include <wtsapi32.h>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <vector>
#include <string>
#include <algorithm>
#include <map>
#include <cctype>
#include <cwctype>
#include <ctime>
#include "version.h"

namespace fs = std::filesystem;

struct DisplayTargetConfig {
    std::string id;           // hardware ID shown by "list", e.g. "DEL41A8" (preferred)
    std::string name;         // part of the monitor name, e.g. "Dell" (fallback)
    bool primary = false;
    DWORD width = 0;
    DWORD height = 0;
    DWORD refresh_rate = 0;   // 0 = highest available for the resolution
    LONG x = 0;
    LONG y = 0;
};

// A physical or virtual monitor as seen by the CCD API (QueryDisplayConfig)
struct MonitorEntry {
    std::string hardwareId;   // e.g. DEL41A8, MTT1337; empty if Windows does not report it
    LUID adapterId;
    UINT32 targetId;
    std::wstring friendlyName;
    std::wstring gdiName;     // \\.\DISPLAYn, only valid while the monitor is active
    bool isActive;
};

// ---------------------------------------------------------------------------
// Utilities and logging
// ---------------------------------------------------------------------------

fs::path ExePath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        DWORD len = GetModuleFileNameW(NULL, &path[0], (DWORD)path.size());
        if (len == 0) return fs::path(L"DisplaySwitch.exe");
        if (len < path.size()) { path.resize(len); break; }
        path.resize(path.size() * 2);  // truncated: long path, grow and retry
    }
    return fs::path(path);
}

fs::path ExeDir() {
    fs::path dir = ExePath().parent_path();
    return dir.empty() ? fs::path(L".") : dir;
}

std::string ExeName() {
    return ExePath().filename().string();
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string ToUpper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::toupper(c); });
    return s;
}

bool SameLuid(const LUID& a, const LUID& b) {
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

void Log(const std::string& msg, bool error = false) {
    (error ? std::cerr : std::cout) << msg << "\n";

    fs::path logFile = ExeDir() / ExePath().stem().concat(L".log");
    std::error_code ec;
    // keep the log small: past 1 MB the old one becomes <name>.log.old
    // (removed first: renaming onto an existing file is not guaranteed to work on Windows)
    if (fs::exists(logFile, ec) && fs::file_size(logFile, ec) > 1024 * 1024) {
        fs::path old = fs::path(logFile).concat(L".old");
        fs::remove(old, ec);
        fs::rename(logFile, old, ec);
    }

    std::ofstream f(logFile, std::ios::app);
    if (!f.is_open()) return;
    char ts[32];
    time_t now = time(NULL);
    tm local = {};
    localtime_s(&local, &now);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &local);
    f << "[" << ts << "] " << (error ? "ERROR " : "") << msg << "\n";
}

std::string Trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    std::string s = str.substr(first, (last - first + 1));
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front())
        s = s.substr(1, s.size() - 2);
    return s;
}

// "#" starts a comment at the beginning of the line or after a space/tab
std::string StripComment(const std::string& line) {
    for (size_t i = 0; i < line.size(); i++) {
        if (line[i] == '#' && (i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t'))
            return line.substr(0, i);
    }
    return line;
}

// ---------------------------------------------------------------------------
// Profile parsing (minimal YAML subset: profiles -> name -> list of monitors)
// ---------------------------------------------------------------------------

std::map<std::string, std::vector<DisplayTargetConfig>> ParseYamlProfiles(const fs::path& filename) {
    std::map<std::string, std::vector<DisplayTargetConfig>> profiles;
    std::ifstream file(filename);
    if (!file.is_open()) {
        Log("Cannot open file: " + Narrow(filename.wstring()), true);
        return profiles;
    }

    std::string line;
    std::string currentProfile = "";
    DisplayTargetConfig currentConfig;
    bool parsingConfig = false;
    int lineNo = 0;

    auto flush = [&]() {
        if (parsingConfig && !currentProfile.empty())
            profiles[currentProfile].push_back(currentConfig);
        parsingConfig = false;
    };

    while (std::getline(file, line)) {
        lineNo++;
        line = StripComment(line);
        std::string trimmed = Trim(line);
        if (trimmed.empty()) continue;

        // profile names are the keys indented by two spaces under "profiles:"
        size_t indent = line.find_first_not_of(' ');
        if (indent == 2 && trimmed.back() == ':') {
            flush();
            currentProfile = trimmed.substr(0, trimmed.size() - 1);
            continue;
        }

        // each monitor starts with "- " (e.g. "- id: DEL41A8")
        if (trimmed.rfind("- ", 0) == 0) {
            flush();
            currentConfig = DisplayTargetConfig();
            parsingConfig = true;
            trimmed = Trim(trimmed.substr(2));
        }

        if (parsingConfig) {
            size_t colon = trimmed.find(':');
            if (colon == std::string::npos) continue;
            std::string key = Trim(trimmed.substr(0, colon));
            std::string val = Trim(trimmed.substr(colon + 1));

            try {
                if (key == "id") currentConfig.id = ToUpper(val);
                else if (key == "name") currentConfig.name = val;
                else if (key == "primary") currentConfig.primary = (ToLower(val) == "true" || val == "1");
                else if (key == "width") currentConfig.width = std::stoul(val);
                else if (key == "height") currentConfig.height = std::stoul(val);
                else if (key == "refresh_rate") currentConfig.refresh_rate = std::stoul(val);
                else if (key == "x") currentConfig.x = std::stol(val);
                else if (key == "y") currentConfig.y = std::stol(val);
                else Log("Unknown key '" + key + "' in profiles.yaml line " + std::to_string(lineNo) + ", ignored");
            } catch (...) {
                Log("Invalid value in profiles.yaml line " + std::to_string(lineNo) + ": '" + trimmed + "'", true);
            }
        }
    }
    flush();
    return profiles;
}

// ---------------------------------------------------------------------------
// Monitor enumeration with the CCD API
// ---------------------------------------------------------------------------

bool QueryConfig(UINT32 flags, std::vector<DISPLAYCONFIG_PATH_INFO>& paths, std::vector<DISPLAYCONFIG_MODE_INFO>& modes) {
    for (int attempt = 0; attempt < 5; attempt++) {
        UINT32 numPaths = 0, numModes = 0;
        if (GetDisplayConfigBufferSizes(flags, &numPaths, &numModes) != ERROR_SUCCESS) return false;
        paths.resize(numPaths);
        modes.resize(numModes);
        LONG r = QueryDisplayConfig(flags, &numPaths, paths.data(), &numModes, modes.data(), NULL);
        if (r == ERROR_INSUFFICIENT_BUFFER) continue;  // topology changed in the meantime, retry
        if (r != ERROR_SUCCESS) return false;
        paths.resize(numPaths);
        modes.resize(numModes);
        return true;
    }
    return false;
}

bool SameTarget(const DISPLAYCONFIG_PATH_INFO& p, const MonitorEntry& m) {
    return SameLuid(p.targetInfo.adapterId, m.adapterId) && p.targetInfo.id == m.targetId;
}

bool SameMonitor(const MonitorEntry& a, const MonitorEntry& b) {
    return SameLuid(a.adapterId, b.adapterId) && a.targetId == b.targetId;
}

// Hardware ID from the monitor device path, e.g.
// \\?\DISPLAY#DEL41A8#5&1a2b3c4d&0&UID4352#{e6f07b5f-...}  ->  DEL41A8
// Falls back to decoding the EDID IDs when the path is missing.
std::string HardwareIdOf(const DISPLAYCONFIG_TARGET_DEVICE_NAME& tn) {
    std::wstring path = tn.monitorDevicePath;
    std::wstring upper = path;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](wchar_t c) { return (wchar_t)std::towupper(c); });
    size_t start = upper.find(L"DISPLAY#");
    if (start != std::wstring::npos) {
        start += 8;
        size_t end = upper.find(L'#', start);
        if (end != std::wstring::npos && end > start)
            return Narrow(upper.substr(start, end - start));
    }

    if (tn.flags.edidIdsValid) {
        // the manufacturer ID is big-endian: three 5-bit letters, 'A' = 1
        UINT16 m = (UINT16)(((tn.edidManufactureId & 0xFF) << 8) | (tn.edidManufactureId >> 8));
        char id[8];
        id[0] = (char)('A' - 1 + ((m >> 10) & 0x1F));
        id[1] = (char)('A' - 1 + ((m >> 5) & 0x1F));
        id[2] = (char)('A' - 1 + (m & 0x1F));
        snprintf(id + 3, sizeof(id) - 3, "%04X", tn.edidProductCodeId);
        return id;
    }
    return "";
}

void ReadTargetInfo(const DISPLAYCONFIG_PATH_INFO& p, std::wstring& friendlyName, std::string& hardwareId) {
    DISPLAYCONFIG_TARGET_DEVICE_NAME tn = {};
    tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    tn.header.size = sizeof(tn);
    tn.header.adapterId = p.targetInfo.adapterId;
    tn.header.id = p.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&tn.header) != ERROR_SUCCESS) {
        friendlyName = L"Generic Display";
        hardwareId = "";
        return;
    }
    friendlyName = tn.monitorFriendlyDeviceName[0] ? tn.monitorFriendlyDeviceName : L"Generic Display";
    hardwareId = HardwareIdOf(tn);
}

std::wstring SourceGdiName(const DISPLAYCONFIG_PATH_INFO& p) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME sn = {};
    sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    sn.header.size = sizeof(sn);
    sn.header.adapterId = p.sourceInfo.adapterId;
    sn.header.id = p.sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&sn.header) == ERROR_SUCCESS)
        return sn.viewGdiDeviceName;
    return L"";
}

std::vector<MonitorEntry> EnumerateMonitors() {
    std::vector<MonitorEntry> monitors;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!QueryConfig(QDC_ALL_PATHS, paths, modes)) {
        Log("QueryDisplayConfig failed", true);
        return monitors;
    }

    // QDC_ALL_PATHS returns every source x monitor combination: keep a single
    // entry per connected monitor, preferring its active path
    for (const auto& p : paths) {
        if (!p.targetInfo.targetAvailable) continue;
        bool active = (p.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;

        auto it = std::find_if(monitors.begin(), monitors.end(), [&](const MonitorEntry& m) {
            return SameTarget(p, m);
        });
        if (it == monitors.end()) {
            MonitorEntry m = { "", p.targetInfo.adapterId, p.targetInfo.id, L"", active ? SourceGdiName(p) : L"", active };
            ReadTargetInfo(p, m.friendlyName, m.hardwareId);
            monitors.push_back(m);
        } else if (active && !it->isActive) {
            it->isActive = true;
            it->gdiName = SourceGdiName(p);
        }
    }
    return monitors;
}

std::string Describe(const DisplayTargetConfig& t) {
    return t.id.empty() ? "'" + t.name + "'" : t.id;
}

bool Matches(const MonitorEntry& m, const DisplayTargetConfig& t) {
    if (!t.id.empty() && m.hardwareId != t.id) return false;
    if (!t.name.empty() && ToLower(Narrow(m.friendlyName)).find(ToLower(t.name)) == std::string::npos) return false;
    return true;
}

// Finds the monitor for a profile entry. A match must be unique: with two
// monitors of the same model (same ID) the tool refuses instead of guessing,
// since the choice could swap between runs.
const MonitorEntry* FindMonitor(const std::vector<MonitorEntry>& monitors, const DisplayTargetConfig& t) {
    if (t.id.empty() && t.name.empty()) {
        Log("A monitor in the profile has neither 'id:' nor 'name:'", true);
        return nullptr;
    }
    const MonitorEntry* found = nullptr;
    int count = 0;
    for (const auto& m : monitors) {
        if (Matches(m, t)) { found = &m; count++; }
    }
    if (count == 0) {
        Log("Monitor " + Describe(t) + " not found or not connected (see '" + ExeName() + " list')", true);
        return nullptr;
    }
    if (count > 1) {
        Log("Monitor " + Describe(t) + " is ambiguous: " + std::to_string(count) +
            " connected monitors match it (see '" + ExeName() + " list')", true);
        return nullptr;
    }
    return found;
}

std::string Pad(const std::string& s, size_t width) {
    return s.size() >= width ? s + "  " : s + std::string(width - s.size(), ' ');
}

// Current mode of an active monitor; false if it is off or Windows does not answer
bool CurrentMode(const MonitorEntry& m, DEVMODEW& dm) {
    if (!m.isActive) return false;
    dm = {};
    dm.dmSize = sizeof(dm);
    return EnumDisplaySettingsW(m.gdiName.c_str(), ENUM_CURRENT_SETTINGS, &dm) != FALSE;
}

std::string DescribeTarget(const DisplayTargetConfig& t, bool primary) {
    std::string s = Describe(t);
    if (t.width && t.height) s += " " + std::to_string(t.width) + "x" + std::to_string(t.height);
    if (t.refresh_rate) s += (t.width && t.height ? "@" : " @") + std::to_string(t.refresh_rate);
    if (primary) s += " (primary)";
    return s;
}

// True if the active layout is exactly this profile: the same monitors on, each at
// the requested resolution/refresh rate, the primary at (0,0) and the others at x,y
bool IsCurrentLayout(const std::vector<MonitorEntry>& monitors, const std::vector<DisplayTargetConfig>& targets,
                     const std::vector<const MonitorEntry*>& matched, size_t primaryIdx) {
    size_t activeCount = std::count_if(monitors.begin(), monitors.end(), [](const MonitorEntry& m) { return m.isActive; });
    if (activeCount != targets.size()) return false;
    for (size_t i = 0; i < targets.size(); i++) {
        const auto& t = targets[i];
        DEVMODEW dm;
        if (!CurrentMode(*matched[i], dm)) return false;
        if (t.width && t.height && (dm.dmPelsWidth != t.width || dm.dmPelsHeight != t.height)) return false;
        if (t.refresh_rate && dm.dmDisplayFrequency != t.refresh_rate) return false;
        LONG x = (i == primaryIdx) ? 0 : t.x, y = (i == primaryIdx) ? 0 : t.y;
        if (dm.dmPosition.x != x || dm.dmPosition.y != y) return false;
    }
    return true;
}

void ListMonitors() {
    auto monitors = EnumerateMonitors();

    // --- monitors ---
    size_t nameWidth = 6;
    for (const auto& m : monitors) nameWidth = std::max(nameWidth, Narrow(m.friendlyName).size() + 2);

    std::cout << "\nMonitors\n\n";
    std::cout << "  " << Pad("ID", 10) << Pad("Name", nameWidth) << Pad("State", 8) << Pad("Mode", 18) << "Position\n";
    for (const auto& m : monitors) {
        DEVMODEW dm;
        bool on = CurrentMode(m, dm);
        bool primary = on && dm.dmPosition.x == 0 && dm.dmPosition.y == 0;
        std::string mode = on ? std::to_string(dm.dmPelsWidth) + "x" + std::to_string(dm.dmPelsHeight) +
                                " @" + std::to_string(dm.dmDisplayFrequency) + "Hz" : "-";
        std::string pos = on ? std::to_string(dm.dmPosition.x) + ", " + std::to_string(dm.dmPosition.y) : "-";

        std::cout << "  " << Pad(m.hardwareId.empty() ? "(no ID)" : m.hardwareId, 10)
                  << Pad(Narrow(m.friendlyName), nameWidth)
                  << Pad(m.isActive ? (primary ? "ON *" : "ON") : "OFF", 8)
                  << Pad(mode, 18) << pos;
        // two monitors of the same model cannot be told apart by ID
        if (!m.hardwareId.empty() &&
            std::count_if(monitors.begin(), monitors.end(), [&](const MonitorEntry& o) { return o.hardwareId == m.hardwareId; }) > 1)
            std::cout << "   (same ID as another monitor: add 'name:' too)";
        std::cout << "\n";
    }
    if (monitors.empty()) std::cout << "  (none detected)\n";
    std::cout << "\n  * = primary\n";

    // --- profiles, checked against the monitors connected right now ---
    fs::path yaml = ExeDir() / "profiles.yaml";
    std::cout << "\nProfiles (" << Narrow(yaml.wstring()) << ")\n\n";
    auto profiles = ParseYamlProfiles(yaml);
    if (profiles.empty()) {
        std::cout << "  (no profiles)\n\n";
        return;
    }

    struct Row { std::string name, status, details; bool current; };
    std::vector<Row> rows;

    for (const auto& p : profiles) {
        const auto& targets = p.second;
        auto firstPrimary = std::find_if(targets.begin(), targets.end(), [](const DisplayTargetConfig& t) { return t.primary; });
        size_t primaryIdx = firstPrimary == targets.end() ? 0 : (size_t)(firstPrimary - targets.begin());

        std::string status = "ready";
        std::vector<const MonitorEntry*> matched;
        std::string details;
        for (size_t i = 0; i < targets.size(); i++) {
            const auto& t = targets[i];
            const MonitorEntry* found = nullptr;
            int count = 0;
            for (const auto& m : monitors)
                if (Matches(m, t)) { found = &m; count++; }
            if (count == 0 && status == "ready") status = "MISSING " + Describe(t);
            if (count > 1 && status == "ready") status = "AMBIGUOUS " + Describe(t);
            matched.push_back(count == 1 ? found : nullptr);
            details += (i ? ", " : "") + DescribeTarget(t, i == primaryIdx);
        }
        bool current = status == "ready" && !targets.empty() && IsCurrentLayout(monitors, targets, matched, primaryIdx);
        rows.push_back({ p.first, status, details, current });
    }

    // columns sized on the content ("MISSING <id>" is wider than "ready")
    size_t nameW = 8, statusW = 8;
    for (const auto& r : rows) {
        nameW = std::max(nameW, r.name.size() + 2);
        statusW = std::max(statusW, r.status.size() + 2);
    }
    for (const auto& r : rows)
        std::cout << "  " << Pad(r.name, nameW) << Pad(r.status, statusW) << r.details
                  << (r.current ? "   <- current" : "") << "\n";
    std::cout << "\nApply one with: " << ExeName() << " apply <profile>\n\n";
}

// ---------------------------------------------------------------------------
// Applying a profile
// ---------------------------------------------------------------------------

// STEP A: turn on only the profile's monitors and turn off all the others with
// SetDisplayConfig (ChangeDisplaySettingsEx cannot enable inactive or virtual displays).
// Returns the SetDisplayConfig result code.
LONG ApplyTopology(const std::vector<const MonitorEntry*>& wanted) {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!QueryConfig(QDC_ALL_PATHS, paths, modes)) {
        Log("QueryDisplayConfig failed", true);
        return ERROR_GEN_FAILURE;
    }

    // monitors that are already on keep their current path (and source), so handle
    // them first: otherwise an inactive monitor could grab their source
    std::vector<const MonitorEntry*> order = wanted;
    std::stable_partition(order.begin(), order.end(), [](const MonitorEntry* m) { return m->isActive; });

    std::vector<DISPLAYCONFIG_PATH_INFO> chosen;
    for (const MonitorEntry* m : order) {
        const DISPLAYCONFIG_PATH_INFO* pick = nullptr;
        for (const auto& p : paths) {
            if (!SameTarget(p, *m)) continue;
            // each source can drive only one monitor on the same adapter
            bool sourceUsed = std::any_of(chosen.begin(), chosen.end(), [&](const DISPLAYCONFIG_PATH_INFO& c) {
                return SameLuid(c.sourceInfo.adapterId, p.sourceInfo.adapterId) && c.sourceInfo.id == p.sourceInfo.id;
            });
            if (sourceUsed) continue;
            bool active = (p.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
            if (!pick || active) pick = &p;
            if (active) break;
        }
        if (!pick) {
            Log("No available path for " + m->hardwareId + " (" + Narrow(m->friendlyName) + ")", true);
            return ERROR_GEN_FAILURE;
        }
        DISPLAYCONFIG_PATH_INFO p = *pick;
        p.flags = DISPLAYCONFIG_PATH_ACTIVE;
        // let Windows pick the modes here, they are set precisely in step B
        p.sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        p.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        chosen.push_back(p);
    }

    return SetDisplayConfig((UINT32)chosen.size(), chosen.data(), 0, NULL,
                            SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
}

// SetDisplayConfig returns ERROR_ACCESS_DENIED (5) while the input desktop is not
// the user's one: lock screen, UAC prompt, or the moment a remote/streaming session
// connects or disconnects. Retry for a while instead of failing at once.
bool ApplyTopologyWithRetry(const std::vector<const MonitorEntry*>& wanted) {
    const int maxTries = 10;
    for (int i = 1; i <= maxTries; i++) {
        LONG r = ApplyTopology(wanted);
        if (r == ERROR_SUCCESS) {
            if (i > 1) Log("Topology applied at attempt " + std::to_string(i));
            return true;
        }
        if (r != ERROR_ACCESS_DENIED) {
            Log("SetDisplayConfig failed (code " + std::to_string(r) + ")", true);
            return false;
        }
        if (i == maxTries) break;
        Sleep(1000);
    }
    Log("SetDisplayConfig kept failing with access denied (screen locked or secure desktop shown?)", true);
    return false;
}

// Finds the mode matching the requested resolution: the exact refresh rate if
// available, otherwise the highest one (interlaced modes are never picked)
bool ResolveMode(const std::wstring& gdi, const DisplayTargetConfig& t, DEVMODEW& out) {
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    bool found = false;
    for (DWORD i = 0; EnumDisplaySettingsW(gdi.c_str(), i, &dm); i++) {
        if (dm.dmPelsWidth != t.width || dm.dmPelsHeight != t.height) continue;
        if (dm.dmDisplayFlags & DM_INTERLACED) continue;
        if (t.refresh_rate && dm.dmDisplayFrequency == t.refresh_rate) { out = dm; return true; }
        if (!found || dm.dmDisplayFrequency > out.dmDisplayFrequency) { out = dm; found = true; }
    }
    return found;
}

// STEP B: resolution, refresh rate, position and primary on the now-active monitors
bool ApplyModes(const std::vector<DisplayTargetConfig>& targets, const std::vector<const MonitorEntry*>& wanted) {
    auto monitors = EnumerateMonitors();
    bool ok = true;

    // the primary goes first and always at (0, 0)
    std::vector<size_t> order(targets.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return targets[a].primary > targets[b].primary; });

    for (size_t i : order) {
        const auto& t = targets[i];
        auto it = std::find_if(monitors.begin(), monitors.end(), [&](const MonitorEntry& m) {
            return SameMonitor(m, *wanted[i]);
        });
        if (it == monitors.end() || !it->isActive) {
            Log("Monitor " + Describe(t) + " is not active after the topology change", true);
            ok = false;
            continue;
        }

        DEVMODEW dm = {};
        dm.dmSize = sizeof(dm);
        dm.dmFields = DM_POSITION;

        // refresh_rate without a resolution: keep the current resolution, change only the rate
        DisplayTargetConfig want = t;
        if (!(want.width > 0 && want.height > 0) && want.refresh_rate) {
            DEVMODEW cur = {};
            cur.dmSize = sizeof(cur);
            if (EnumDisplaySettingsW(it->gdiName.c_str(), ENUM_CURRENT_SETTINGS, &cur)) {
                want.width = cur.dmPelsWidth;
                want.height = cur.dmPelsHeight;
            }
        }

        if (want.width > 0 && want.height > 0) {
            DEVMODEW mode = {};
            if (ResolveMode(it->gdiName, want, mode)) {
                if (t.refresh_rate && mode.dmDisplayFrequency != t.refresh_rate)
                    Log(std::to_string(t.refresh_rate) + "Hz not available on " + Describe(t) +
                        ", using " + std::to_string(mode.dmDisplayFrequency) + "Hz");
                dm.dmFields |= DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
                dm.dmPelsWidth = mode.dmPelsWidth;
                dm.dmPelsHeight = mode.dmPelsHeight;
                dm.dmDisplayFrequency = mode.dmDisplayFrequency;
            } else {
                // the profile is not what was asked for: report it as a failure
                // (a virtual display may also publish its modes a moment late,
                // which the retry in ApplyProfile covers)
                Log("Resolution " + std::to_string(want.width) + "x" + std::to_string(want.height) +
                    " not supported by " + Describe(t) + ", keeping the current one", true);
                ok = false;
            }
        }
        dm.dmPosition.x = t.primary ? 0 : t.x;
        dm.dmPosition.y = t.primary ? 0 : t.y;

        DWORD flags = CDS_UPDATEREGISTRY | CDS_NORESET | (t.primary ? CDS_SET_PRIMARY : 0);
        LONG r = ChangeDisplaySettingsExW(it->gdiName.c_str(), &dm, NULL, flags, NULL);
        if (r != DISP_CHANGE_SUCCESSFUL) {
            Log("ChangeDisplaySettingsEx failed for " + Describe(t) + " (code " + std::to_string(r) + ")", true);
            ok = false;
        }
    }

    // commit all the changes queued with CDS_NORESET in one go
    LONG r = ChangeDisplaySettingsExW(NULL, NULL, NULL, 0, NULL);
    if (r != DISP_CHANGE_SUCCESSFUL) {
        Log("Final apply failed (code " + std::to_string(r) + ")", true);
        ok = false;
    }
    return ok;
}

bool WaitForActive(const std::vector<const MonitorEntry*>& wanted, DWORD timeoutMs) {
    for (DWORD waited = 0; waited < timeoutMs; waited += 500) {
        Sleep(500);
        auto monitors = EnumerateMonitors();
        bool allActive = std::all_of(wanted.begin(), wanted.end(), [&](const MonitorEntry* w) {
            return std::any_of(monitors.begin(), monitors.end(), [&](const MonitorEntry& m) {
                return m.isActive && SameMonitor(m, *w);
            });
        });
        if (allActive) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Rollback: the layout active before the switch, restored if the profile fails
// halfway (e.g. physical monitors already off but the virtual display stuck at a
// wrong resolution), so the user is never left without a usable screen
// ---------------------------------------------------------------------------

struct SavedLayout {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    bool valid = false;
};

SavedLayout SaveLayout() {
    SavedLayout s;
    s.valid = QueryConfig(QDC_ONLY_ACTIVE_PATHS, s.paths, s.modes) && !s.paths.empty();
    if (!s.valid) Log("Could not save the current layout: no rollback available if the switch fails", true);
    return s;
}

bool RestoreLayout(SavedLayout& s) {
    if (!s.valid) return false;
    for (int i = 1; i <= 10; i++) {
        LONG r = SetDisplayConfig((UINT32)s.paths.size(), s.paths.data(), (UINT32)s.modes.size(), s.modes.data(),
                                  SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
        if (r == ERROR_SUCCESS) {
            Log("Previous layout restored");
            return true;
        }
        if (r != ERROR_ACCESS_DENIED || i == 10) {
            Log("Could not restore the previous layout (code " + std::to_string(r) + ")", true);
            return false;
        }
        Sleep(1000);
    }
    return false;
}

bool ApplyProfile(std::vector<DisplayTargetConfig> targets) {
    auto monitors = EnumerateMonitors();
    if (monitors.empty()) {
        Log("No monitors detected", true);
        return false;
    }
    if (targets.empty()) {
        Log("The profile has no monitors", true);
        return false;
    }

    // exactly one primary: the first one marked, or the first monitor if none is
    auto firstPrimary = std::find_if(targets.begin(), targets.end(), [](const DisplayTargetConfig& t) { return t.primary; });
    if (firstPrimary == targets.end()) firstPrimary = targets.begin();
    for (auto it = targets.begin(); it != targets.end(); ++it) it->primary = (it == firstPrimary);

    std::vector<const MonitorEntry*> wanted;
    for (const auto& t : targets) {
        const MonitorEntry* m = FindMonitor(monitors, t);
        if (!m) return false;
        if (std::find(wanted.begin(), wanted.end(), m) != wanted.end()) {
            Log("Monitor " + Describe(t) + " is listed twice in the profile", true);
            return false;
        }
        wanted.push_back(m);
    }

    SavedLayout previous = SaveLayout();
    auto fail = [&]() {
        Log("Rolling back to the previous layout", true);
        RestoreLayout(previous);
        return false;
    };

    // physical monitors (especially high refresh ones) can take seconds to wake up:
    // wait until they are all active, and retry the topology change once if needed
    bool active = false;
    for (int attempt = 1; attempt <= 2 && !active; attempt++) {
        if (!ApplyTopologyWithRetry(wanted)) return fail();
        active = WaitForActive(wanted, 10000);
        if (!active) Log("Monitors still not active after attempt " + std::to_string(attempt), true);
    }

    // a mode change right after a topology change can fail transiently
    // (DISP_CHANGE_FAILED while the driver is still settling): retry once
    if (ApplyModes(targets, wanted)) return true;
    Log("Retrying the mode change in 2 seconds");
    Sleep(2000);
    if (ApplyModes(targets, wanted)) return true;
    return fail();
}

// ---------------------------------------------------------------------------
// Relaunch in the console session
// A Windows service runs as SYSTEM in session 0, which has no monitors: display
// changes made there have no effect, so when started by a service the tool relaunches itself
// in the session shown on screen (the logged-in user, or the logon screen if
// nobody is logged in)
// ---------------------------------------------------------------------------

bool InServiceSession() {
    DWORD session = 0;
    return ProcessIdToSessionId(GetCurrentProcessId(), &session) && session == 0;
}

int RelaunchInConsoleSession() {
    DWORD session = WTSGetActiveConsoleSessionId();
    // 0xFFFFFFFF = no console session (e.g. during a session switch); 0 would make
    // the child run in session 0 again and relaunch itself forever
    if (session == 0xFFFFFFFF || session == 0) {
        Log("No active console session to relaunch into", true);
        return 1;
    }

    HANDLE token = NULL;
    const wchar_t* desktop = L"winsta0\\default";
    if (WTSQueryUserToken(session, &token)) {
        Log("Session 0: relaunching as the logged-in user in session " + std::to_string(session));
    } else {
        // nobody logged in: a copy of the SYSTEM token moved to the logon screen session
        HANDLE self = NULL;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE, &self) ||
            !DuplicateTokenEx(self, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenPrimary, &token) ||
            !SetTokenInformation(token, TokenSessionId, &session, sizeof(session))) {
            Log("Cannot prepare the token for session " + std::to_string(session) +
                " (error " + std::to_string(GetLastError()) + ")", true);
            if (self) CloseHandle(self);
            if (token) CloseHandle(token);
            return 1;
        }
        CloseHandle(self);
        desktop = L"winsta0\\winlogon";
        Log("Session 0: nobody logged in, relaunching on the logon screen (session " + std::to_string(session) + ")");
    }

    std::wstring cmd = GetCommandLineW();
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.lpDesktop = const_cast<LPWSTR>(desktop);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessAsUserW(token, NULL, &cmd[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        Log("CreateProcessAsUser failed (error " + std::to_string(GetLastError()) + ")", true);
        CloseHandle(token);
        return 1;
    }

    // the child may wait up to 60 s for the lock, ~10 s for access-denied retries
    // and ~25 s for the switch itself
    DWORD exitCode = 1;
    if (WaitForSingleObject(pi.hProcess, 150000) == WAIT_OBJECT_0)
        GetExitCodeProcess(pi.hProcess, &exitCode);
    else
        Log("The relaunched process did not finish within 150 seconds", true);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(token);
    return (int)exitCode;
}

void PrintUsage() {
    std::cout << DS_PRODUCT_NAME " " DS_VERSION_STRING " - " DS_DESCRIPTION "\n\n";
    std::cout << "Usage:\n";
    std::cout << "  " << ExeName() << " list                 monitors and profiles\n";
    std::cout << "  " << ExeName() << " apply <profile>      switch to a profile from profiles.yaml\n";
    std::cout << "  " << ExeName() << " version              print the version\n";
    std::cout << "  " << ExeName() << " help                 print this help\n\n";
    std::cout << "Profiles are read from " << Narrow((ExeDir() / "profiles.yaml").wstring()) << "\n";
}

int main(int argc, char* argv[]) {
    // monitor names are printed as UTF-8
    SetConsoleOutputCP(CP_UTF8);

    if (argc < 2) {
        PrintUsage();
        return 1;
    }

    std::string command = ToLower(argv[1]);

    if (command == "help" || command == "--help" || command == "-h" || command == "/?") {
        PrintUsage();
        return 0;
    }

    if (command == "version" || command == "--version" || command == "-v") {
        std::cout << DS_PRODUCT_NAME " " DS_VERSION_STRING "\n";
        return 0;
    }

    if (command == "list") {
        ListMonitors();
        return 0;
    }

    if (command == "apply") {
        if (InServiceSession()) return RelaunchInConsoleSession();

        if (argc < 3) {
            Log("Specify a profile name (e.g. desk, stream).", true);
            return 1;
        }

        std::string profileName = argv[2];
        // profiles.yaml lives next to the exe, not in the current directory
        // (shortcuts, services and automation tools often start it from elsewhere)
        auto profiles = ParseYamlProfiles(ExeDir() / "profiles.yaml");

        if (profiles.find(profileName) == profiles.end()) {
            Log("Profile '" + profileName + "' not found in profiles.yaml.", true);
            return 1;
        }

        // one switch at a time: two quick clicks, or two tools firing together, would
        // otherwise fight over the topology. A relaunched child always runs in the
        // console session, so a session-local mutex is enough
        HANDLE lock = CreateMutexW(NULL, FALSE, L"Local\\DisplaySwitchApply");
        DWORD wait = lock ? WaitForSingleObject(lock, 60000) : WAIT_FAILED;
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
            Log("Another profile switch is still running, giving up on '" + profileName + "'.", true);
            if (lock) CloseHandle(lock);
            return 1;
        }

        bool ok = ApplyProfile(profiles[profileName]);
        if (ok)
            Log("Profile '" + profileName + "' applied successfully.");
        else
            Log("Failed to apply profile '" + profileName + "'.", true);

        ReleaseMutex(lock);
        CloseHandle(lock);
        return ok ? 0 : 1;
    }

    Log("Unknown command '" + command + "'.", true);
    PrintUsage();
    return 1;
}
