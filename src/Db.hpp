#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

struct Location {
    std::int64_t id = 0;
    std::string  name;
    std::string  zone;
    std::string  notes;
    int          sort_order = 0;
};

struct Snippet {
    std::int64_t              id = 0;
    std::string               label;
    std::string               content;
    std::string               category;
    std::vector<std::int64_t> location_ids;
    std::int64_t              created_at = 0;
};

class Db {
public:
    static constexpr std::size_t MAX_LABEL         = 200;
    static constexpr std::size_t MAX_CONTENT       = 64 * 1024;
    static constexpr std::size_t MAX_CATEGORY      = 64;
    static constexpr std::size_t MAX_LOCATION_NAME = 64;
    static constexpr std::size_t MAX_NOTES         = 1 << 20;
    static constexpr std::size_t MAX_TZ_LEN        = 64;
    static constexpr const char* DEFAULT_CATEGORY  = "General";

    explicit Db(const std::filesystem::path& file);
    ~Db();
    Db(const Db&)            = delete;
    Db& operator=(const Db&) = delete;

    std::filesystem::path path() const { return path_; }

    // key/value settings
    std::string get_setting(const std::string& key) const;
    void        set_setting(const std::string& key, const std::string& value);
    void        delete_setting(const std::string& key);

    // favorites (list of zone strings)
    std::vector<std::string> favorites() const;
    bool is_favorite(const std::string& zone) const;
    void add_favorite(const std::string& zone);
    void remove_favorite(const std::string& zone);

    // locations
    std::vector<Location> locations() const;
    Location              location_by_id(std::int64_t id) const;
    std::int64_t          add_location(const std::string& name, const std::string& zone);
    void                  remove_location(std::int64_t id);
    void                  update_location_notes(std::int64_t id, const std::string& notes);

    // categories
    std::vector<std::string> categories() const;
    void add_category(const std::string& name);
    void rename_category(const std::string& old_name, const std::string& new_name);
    void remove_category(const std::string& name);
    bool has_category(const std::string& name) const;

    // snippets
    std::vector<Snippet> snippets(std::int64_t location_filter_id = 0,
                                  const std::string& category_filter = "") const;
    std::vector<Snippet> snippets_untagged(const std::string& category_filter = "") const;
    std::int64_t         add_snippet(const std::string& label,
                                     const std::string& content,
                                     const std::string& category,
                                     const std::vector<std::int64_t>& location_ids);
    void                 update_snippet(std::int64_t id,
                                        const std::string& label,
                                        const std::string& content,
                                        const std::string& category,
                                        const std::vector<std::int64_t>& location_ids);
    void                 remove_snippet(std::int64_t id);
    Snippet              snippet_by_id(std::int64_t id) const;

private:
    std::filesystem::path path_;
    sqlite3*              db_ = nullptr;

    void exec(const char* sql);
    void init_schema();
};
