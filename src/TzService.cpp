#include "TzService.hpp"
#include "Db.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <glibmm/spawn.h>

namespace fs = std::filesystem;

static bool is_tz_name_safe(const std::string& s) {
    if (s.empty() || s.size() > Db::MAX_TZ_LEN) return false;
    if (s.front() == '/' || s.back() == '/')    return false;
    bool prev_slash = false;
    for (char c : s) {
        if (c == '/') {
            if (prev_slash) return false;
            prev_slash = true;
            continue;
        }
        prev_slash = false;
        const bool ok =
            (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '_' || c == '-' || c == '+';
        if (!ok) return false;
    }
    return true;
}

bool TzService::is_display_name_safe(const std::string& s, std::size_t max_len) {
    if (s.empty() || s.size() > max_len) return false;
    for (unsigned char c : s) {
        if (c == '\t' || c == '\n' || c == '\r') return false;
        if (c < 0x20) return false;
    }
    return true;
}

static std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) out.push_back(std::move(line));
    }
    return out;
}

static bool spawn_capture(const std::vector<std::string>& argv,
                          std::string& stdout_out,
                          std::string& stderr_out,
                          int& exit_status) {
    try {
        Glib::spawn_sync(
            std::string(),
            argv,
            Glib::SpawnFlags::SEARCH_PATH,
            sigc::slot<void()>(),
            &stdout_out,
            &stderr_out,
            &exit_status);
        return true;
    } catch (const Glib::Error& e) {
        stderr_out  = e.what();
        exit_status = -1;
        return false;
    } catch (const std::exception& e) {
        stderr_out  = e.what();
        exit_status = -1;
        return false;
    }
}

TzService::TzService(Db& db) : db_(db) {
    load_all_zones();
    load_aliases();
}

void TzService::load_aliases() {
    // Curated US state + major-city aliases that IANA doesn't surface directly.
    // Each alias points at an existing IANA zone; we drop entries whose target
    // isn't on this system (old tzdata, chroot, etc.) so the list never lies.
    struct Pair { const char* display; const char* zone; };
    static const Pair kAliases[] = {
        // --- US states (picks the most populous time zone for states that span) ---
        {"Alabama",              "America/Chicago"},
        {"Alaska",               "America/Anchorage"},
        {"Arizona",              "America/Phoenix"},
        {"Arkansas",             "America/Chicago"},
        {"California",           "America/Los_Angeles"},
        {"Colorado",             "America/Denver"},
        {"Connecticut",          "America/New_York"},
        {"Delaware",             "America/New_York"},
        {"Florida",              "America/New_York"},
        {"Georgia",              "America/New_York"},
        {"Hawaii",               "Pacific/Honolulu"},
        {"Idaho",                "America/Boise"},
        {"Illinois",             "America/Chicago"},
        {"Indiana",              "America/Indiana/Indianapolis"},
        {"Iowa",                 "America/Chicago"},
        {"Kansas",               "America/Chicago"},
        {"Kentucky",             "America/Kentucky/Louisville"},
        {"Louisiana",            "America/Chicago"},
        {"Maine",                "America/New_York"},
        {"Maryland",             "America/New_York"},
        {"Massachusetts",        "America/New_York"},
        {"Michigan",             "America/Detroit"},
        {"Minnesota",            "America/Chicago"},
        {"Mississippi",          "America/Chicago"},
        {"Missouri",             "America/Chicago"},
        {"Montana",              "America/Denver"},
        {"Nebraska",             "America/Chicago"},
        {"Nevada",               "America/Los_Angeles"},
        {"New Hampshire",        "America/New_York"},
        {"New Jersey",           "America/New_York"},
        {"New Mexico",           "America/Denver"},
        {"North Carolina",       "America/New_York"},
        {"North Dakota",         "America/Chicago"},
        {"Ohio",                 "America/New_York"},
        {"Oklahoma",             "America/Chicago"},
        {"Oregon",               "America/Los_Angeles"},
        {"Pennsylvania",         "America/New_York"},
        {"Rhode Island",         "America/New_York"},
        {"South Carolina",       "America/New_York"},
        {"South Dakota",         "America/Chicago"},
        {"Tennessee (Central)",  "America/Chicago"},
        {"Tennessee (Eastern)",  "America/New_York"},
        {"Texas",                "America/Chicago"},
        {"Utah",                 "America/Denver"},
        {"Vermont",              "America/New_York"},
        {"Virginia",             "America/New_York"},
        {"Washington (State)",   "America/Los_Angeles"},
        {"West Virginia",        "America/New_York"},
        {"Wisconsin",            "America/Chicago"},
        {"Wyoming",              "America/Denver"},

        // --- US cities that aren't canonical IANA names ---
        {"Memphis, Tennessee",       "America/Chicago"},
        {"Nashville, Tennessee",     "America/Chicago"},
        {"Knoxville, Tennessee",     "America/New_York"},
        {"Chattanooga, Tennessee",   "America/New_York"},
        {"Atlanta, Georgia",         "America/New_York"},
        {"Miami, Florida",           "America/New_York"},
        {"Orlando, Florida",         "America/New_York"},
        {"Tampa, Florida",           "America/New_York"},
        {"Jacksonville, Florida",    "America/New_York"},
        {"Charlotte, North Carolina","America/New_York"},
        {"Raleigh, North Carolina",  "America/New_York"},
        {"Dallas, Texas",            "America/Chicago"},
        {"Houston, Texas",           "America/Chicago"},
        {"Austin, Texas",            "America/Chicago"},
        {"San Antonio, Texas",       "America/Chicago"},
        {"Philadelphia",             "America/New_York"},
        {"Boston, Massachusetts",    "America/New_York"},
        {"Washington, D.C.",         "America/New_York"},
        {"Baltimore",                "America/New_York"},
        {"Cleveland, Ohio",          "America/New_York"},
        {"Cincinnati, Ohio",         "America/New_York"},
        {"Columbus, Ohio",           "America/New_York"},
        {"Pittsburgh",               "America/New_York"},
        {"Milwaukee",                "America/Chicago"},
        {"Minneapolis",              "America/Chicago"},
        {"St. Louis",                "America/Chicago"},
        {"Kansas City",              "America/Chicago"},
        {"New Orleans",              "America/Chicago"},
        {"Las Vegas",                "America/Los_Angeles"},
        {"San Francisco",            "America/Los_Angeles"},
        {"San Diego",                "America/Los_Angeles"},
        {"Seattle, Washington",      "America/Los_Angeles"},
        {"Portland, Oregon",         "America/Los_Angeles"},
        {"Salt Lake City",           "America/Denver"},
        {"Albuquerque",              "America/Denver"},

        // --- Friendly US zone names ---
        {"Eastern Time (US)",   "America/New_York"},
        {"Central Time (US)",   "America/Chicago"},
        {"Mountain Time (US)",  "America/Denver"},
        {"Pacific Time (US)",   "America/Los_Angeles"},
    };
    aliases_.clear();
    aliases_.reserve(sizeof(kAliases) / sizeof(kAliases[0]));
    for (const auto& a : kAliases) {
        if (!is_display_name_safe(a.display, 64)) continue;      // defence in depth
        if (!is_tz_name_safe(a.zone)) continue;
        if (!std::binary_search(all_zones_.begin(), all_zones_.end(), a.zone))
            continue;  // zone not on this system — skip to avoid a dead row
        aliases_.push_back({a.display, a.zone});
    }
}

void TzService::load_all_zones() {
    std::string out, err;
    int status = 0;
    if (!spawn_capture({"timedatectl", "list-timezones"}, out, err, status) ||
        status != 0) {
        return;
    }
    all_zones_ = split_lines(out);

    // timedatectl filters out the Etc/* aliases (UTC, GMT, GMT+1..+14, etc.).
    // They are real, accepted tzdata entries, and users ask for them when they
    // need a fixed UTC offset. Pick them up straight off disk.
    std::error_code ec;
    const fs::path etc_dir = "/usr/share/zoneinfo/Etc";
    if (fs::is_directory(etc_dir, ec)) {
        for (const auto& entry : fs::directory_iterator(etc_dir, ec)) {
            if (ec) break;
            if (entry.is_directory()) continue;
            std::string name = "Etc/" + entry.path().filename().string();
            if (!is_tz_name_safe(name)) continue;
            if (std::find(all_zones_.begin(), all_zones_.end(), name) ==
                all_zones_.end()) {
                all_zones_.push_back(std::move(name));
            }
        }
    }

    // Top-level aliases like "UTC" and "GMT" live directly under zoneinfo/.
    // Only pick up a tiny whitelist — enumerating the whole dir would sweep
    // up region subdirs and legacy/posix trees.
    static const char* kTopAliases[] = {"UTC", "GMT", "Universal", "Zulu"};
    const fs::path zone_root = "/usr/share/zoneinfo";
    for (const char* a : kTopAliases) {
        if (fs::exists(zone_root / a, ec) &&
            std::find(all_zones_.begin(), all_zones_.end(), a) ==
                all_zones_.end()) {
            all_zones_.push_back(a);
        }
    }

    std::sort(all_zones_.begin(), all_zones_.end());
}

std::string TzService::current_zone() const {
    std::string out, err;
    int status = 0;
    if (!spawn_capture({"timedatectl", "show", "--property=Timezone", "--value"},
                       out, err, status) || status != 0) {
        return "(unknown)";
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    return out;
}

bool TzService::is_valid_zone(const std::string& tz) const {
    if (!is_tz_name_safe(tz)) return false;
    return std::binary_search(all_zones_.begin(), all_zones_.end(), tz);
}

bool TzService::apply(const std::string& tz, std::string& err_out) {
    if (!is_valid_zone(tz)) {
        err_out = "That timezone wasn't recognized by your system.";
        return false;
    }
    std::string out, err;
    int status = 0;
    spawn_capture({"timedatectl", "set-timezone", tz}, out, err, status);
    if (status == 0) return true;
    err_out = err.empty() ? ("timedatectl exited " + std::to_string(status)) : err;
    while (!err_out.empty() && (err_out.back() == '\n' || err_out.back() == '\r'))
        err_out.pop_back();
    return false;
}

void TzService::import_legacy_config() {
    // Guard: only run the import once per install.
    if (db_.get_setting("legacy_imported") == "1") return;

    const char* home = std::getenv("HOME");
    if (!home || !*home || !fs::path(home).is_absolute()) {
        db_.set_setting("legacy_imported", "1"); return;
    }
    fs::path cfg = fs::path(home) / ".config" / "time-zone-changer";

    // favorites.txt: one zone per line
    {
        std::ifstream in(cfg / "favorites.txt");
        std::string line;
        while (in && std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            if (is_valid_zone(line)) db_.add_favorite(line);
        }
    }

    // home.txt: single zone
    {
        std::ifstream in(cfg / "home.txt");
        std::string line;
        if (in && std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (is_valid_zone(line) && db_.get_setting("home_zone").empty()) {
                db_.set_setting("home_zone", line);
            }
        }
    }

    // locations.tsv: "name\tzone"
    {
        std::ifstream in(cfg / "locations.tsv");
        std::string line;
        // Build a set of existing names so we don't dup.
        std::vector<std::string> existing;
        for (const auto& l : db_.locations()) existing.push_back(l.name);
        while (in && std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            auto tab = line.find('\t');
            if (tab == std::string::npos) continue;
            std::string name = line.substr(0, tab);
            std::string zone = line.substr(tab + 1);
            if (!is_display_name_safe(name, Db::MAX_LOCATION_NAME)) continue;
            if (!is_valid_zone(zone)) continue;
            if (std::find(existing.begin(), existing.end(), name) != existing.end()) continue;
            try { db_.add_location(name, zone); existing.push_back(name); }
            catch (...) {}
        }
    }

    db_.set_setting("legacy_imported", "1");
}
