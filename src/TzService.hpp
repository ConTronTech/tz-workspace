#pragma once

#include <string>
#include <vector>

class Db;

struct ZoneAlias {
    std::string display;   // e.g. "Memphis, Tennessee"
    std::string zone;      // e.g. "America/Chicago"
};

class TzService {
public:
    explicit TzService(Db& db);

    const std::vector<std::string>& all_zones() const { return all_zones_; }
    const std::vector<ZoneAlias>&   aliases()   const { return aliases_; }

    // Reads /etc/localtime via timedatectl each call.
    std::string current_zone() const;

    // Strict charset + whitelist membership check.
    bool is_valid_zone(const std::string& tz) const;

    // Runs `timedatectl set-timezone <tz>` synchronously (polkit prompts).
    // Returns true on exit 0; on failure, err_out contains stderr.
    bool apply(const std::string& tz, std::string& err_out);

    // Name validation for locations/labels/etc — no control chars, length bounded.
    static bool is_display_name_safe(const std::string& s, std::size_t max_len);

    // Import any pre-existing data from the legacy time-zone-changer config dir.
    // Safe to call every launch; skips rows that already exist.
    void import_legacy_config();

private:
    Db&                      db_;
    std::vector<std::string> all_zones_;
    std::vector<ZoneAlias>   aliases_;

    void load_all_zones();
    void load_aliases();
};
