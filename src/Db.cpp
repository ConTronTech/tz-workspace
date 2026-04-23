#include "Db.hpp"

#include <sqlite3.h>

#include <cstring>
#include <stdexcept>
#include <string>

#include <sys/stat.h>

namespace fs = std::filesystem;

static void check_ok(int rc, sqlite3* db, const char* where) {
    if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW) {
        std::string msg = std::string(where) + ": ";
        msg += db ? sqlite3_errmsg(db) : "(no db)";
        throw std::runtime_error(msg);
    }
}

// Reject NUL bytes and disallowed control chars; enforce length cap.
// For single-line fields (names, labels, categories) no control chars at all.
// For multi-line fields (notes, snippet content) tab/LF/CR are allowed.
static void require_clean_text(const std::string& s, std::size_t max_len,
                               bool allow_multiline, const char* field) {
    if (s.size() > max_len) {
        throw std::runtime_error(std::string(field) + ": exceeds maximum length");
    }
    for (unsigned char c : s) {
        if (c == 0) {
            throw std::runtime_error(std::string(field) + ": contains NUL byte");
        }
        if (c < 0x20) {
            if (allow_multiline && (c == '\t' || c == '\n' || c == '\r')) continue;
            throw std::runtime_error(std::string(field) + ": contains control character");
        }
    }
}

// RAII wrapper for prepared statements.
namespace {
struct Stmt {
    sqlite3_stmt* p = nullptr;
    sqlite3*      db = nullptr;

    Stmt(sqlite3* d, const char* sql) : db(d) {
        int rc = sqlite3_prepare_v2(db, sql, -1, &p, nullptr);
        check_ok(rc, db, "prepare");
    }
    ~Stmt() { if (p) sqlite3_finalize(p); }
    Stmt(const Stmt&)            = delete;
    Stmt& operator=(const Stmt&) = delete;

    void bind_text(int i, const std::string& s) {
        int rc = sqlite3_bind_text(p, i, s.data(),
                                   static_cast<int>(s.size()), SQLITE_TRANSIENT);
        check_ok(rc, db, "bind_text");
    }
    void bind_int64(int i, std::int64_t v) {
        int rc = sqlite3_bind_int64(p, i, v);
        check_ok(rc, db, "bind_int64");
    }
    int step() {
        int rc = sqlite3_step(p);
        if (rc != SQLITE_ROW && rc != SQLITE_DONE) check_ok(rc, db, "step");
        return rc;
    }
    std::string col_text(int i) const {
        const unsigned char* t = sqlite3_column_text(p, i);
        int n = sqlite3_column_bytes(p, i);
        if (!t || n <= 0) return {};
        return std::string(reinterpret_cast<const char*>(t),
                           static_cast<std::size_t>(n));
    }
    std::int64_t col_int64(int i) const { return sqlite3_column_int64(p, i); }
};
}  // namespace

Db::Db(const fs::path& file) : path_(file) {
    std::error_code ec;
    fs::create_directories(path_.parent_path(), ec);
    ::chmod(path_.parent_path().c_str(), 0700);

    int rc = sqlite3_open_v2(path_.c_str(), &db_,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                             SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        std::string msg = "open sqlite db: ";
        msg += db_ ? sqlite3_errmsg(db_) : "(no db)";
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error(msg);
    }
    ::chmod(path_.c_str(), 0600);

    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA foreign_keys=ON;");
    exec("PRAGMA synchronous=NORMAL;");
    // Defensive hardening: block writable_schema exploits, disallow
    // untrusted function calls in tampered schema, enforce cell-size sanity.
    exec("PRAGMA trusted_schema=OFF;");
    exec("PRAGMA cell_size_check=ON;");
    init_schema();

    // WAL/SHM sidecars are created by SQLite with default perms; tighten to 0600.
    fs::path wal = path_; wal += "-wal";
    fs::path shm = path_; shm += "-shm";
    ::chmod(wal.c_str(), 0600);
    ::chmod(shm.c_str(), 0600);
}

Db::~Db() { if (db_) sqlite3_close(db_); }

void Db::exec(const char* sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = "exec: ";
        msg += err ? err : "(no msg)";
        if (err) sqlite3_free(err);
        throw std::runtime_error(msg);
    }
}

void Db::init_schema() {
    exec(R"(
      CREATE TABLE IF NOT EXISTS settings (
        key   TEXT PRIMARY KEY,
        value TEXT NOT NULL
      );

      CREATE TABLE IF NOT EXISTS favorites (
        zone     TEXT PRIMARY KEY,
        added_at INTEGER NOT NULL
      );

      CREATE TABLE IF NOT EXISTS locations (
        id         INTEGER PRIMARY KEY AUTOINCREMENT,
        name       TEXT UNIQUE NOT NULL,
        zone       TEXT NOT NULL,
        notes      TEXT NOT NULL DEFAULT '',
        sort_order INTEGER NOT NULL DEFAULT 0
      );

      CREATE TABLE IF NOT EXISTS categories (
        name       TEXT PRIMARY KEY,
        sort_order INTEGER NOT NULL DEFAULT 0
      );

      CREATE TABLE IF NOT EXISTS snippets (
        id         INTEGER PRIMARY KEY AUTOINCREMENT,
        label      TEXT NOT NULL,
        content    TEXT NOT NULL,
        category   TEXT NOT NULL DEFAULT 'General',
        created_at INTEGER NOT NULL
      );

      CREATE TABLE IF NOT EXISTS snippet_locations (
        snippet_id  INTEGER NOT NULL REFERENCES snippets(id) ON DELETE CASCADE,
        location_id INTEGER NOT NULL REFERENCES locations(id) ON DELETE CASCADE,
        PRIMARY KEY (snippet_id, location_id)
      );

      CREATE INDEX IF NOT EXISTS idx_snippets_cat  ON snippets(category);
      CREATE INDEX IF NOT EXISTS idx_snl_location  ON snippet_locations(location_id);
      CREATE INDEX IF NOT EXISTS idx_snl_snippet   ON snippet_locations(snippet_id);
    )");

    // Ensure default category exists.
    {
        Stmt s(db_, "INSERT OR IGNORE INTO categories(name, sort_order) VALUES (?, 0);");
        s.bind_text(1, DEFAULT_CATEGORY);
        s.step();
    }
}

// ---- settings ----

std::string Db::get_setting(const std::string& key) const {
    Stmt s(db_, "SELECT value FROM settings WHERE key = ?;");
    s.bind_text(1, key);
    if (s.step() == SQLITE_ROW) return s.col_text(0);
    return {};
}

void Db::set_setting(const std::string& key, const std::string& value) {
    Stmt s(db_,
        "INSERT INTO settings(key, value) VALUES (?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value;");
    s.bind_text(1, key);
    s.bind_text(2, value);
    s.step();
}

void Db::delete_setting(const std::string& key) {
    Stmt s(db_, "DELETE FROM settings WHERE key = ?;");
    s.bind_text(1, key);
    s.step();
}

// ---- favorites ----

std::vector<std::string> Db::favorites() const {
    std::vector<std::string> out;
    Stmt s(db_, "SELECT zone FROM favorites ORDER BY added_at ASC;");
    while (s.step() == SQLITE_ROW) out.push_back(s.col_text(0));
    return out;
}

bool Db::is_favorite(const std::string& zone) const {
    Stmt s(db_, "SELECT 1 FROM favorites WHERE zone = ? LIMIT 1;");
    s.bind_text(1, zone);
    return s.step() == SQLITE_ROW;
}

void Db::add_favorite(const std::string& zone) {
    Stmt s(db_,
        "INSERT OR IGNORE INTO favorites(zone, added_at) "
        "VALUES (?, strftime('%s','now'));");
    s.bind_text(1, zone);
    s.step();
}

void Db::remove_favorite(const std::string& zone) {
    Stmt s(db_, "DELETE FROM favorites WHERE zone = ?;");
    s.bind_text(1, zone);
    s.step();
}

// ---- locations ----

std::vector<Location> Db::locations() const {
    std::vector<Location> out;
    Stmt s(db_, "SELECT id, name, zone, notes, sort_order FROM locations "
                "ORDER BY sort_order, name;");
    while (s.step() == SQLITE_ROW) {
        Location l;
        l.id         = s.col_int64(0);
        l.name       = s.col_text(1);
        l.zone       = s.col_text(2);
        l.notes      = s.col_text(3);
        l.sort_order = static_cast<int>(s.col_int64(4));
        out.push_back(std::move(l));
    }
    return out;
}

Location Db::location_by_id(std::int64_t id) const {
    Stmt s(db_, "SELECT id, name, zone, notes, sort_order FROM locations WHERE id = ?;");
    s.bind_int64(1, id);
    Location l;
    if (s.step() == SQLITE_ROW) {
        l.id         = s.col_int64(0);
        l.name       = s.col_text(1);
        l.zone       = s.col_text(2);
        l.notes      = s.col_text(3);
        l.sort_order = static_cast<int>(s.col_int64(4));
    }
    return l;
}

std::int64_t Db::add_location(const std::string& name, const std::string& zone) {
    require_clean_text(name, MAX_LOCATION_NAME, false, "location.name");
    require_clean_text(zone, MAX_TZ_LEN,        false, "location.zone");
    Stmt s(db_, "INSERT INTO locations(name, zone) VALUES (?, ?);");
    s.bind_text(1, name);
    s.bind_text(2, zone);
    s.step();
    return sqlite3_last_insert_rowid(db_);
}

void Db::remove_location(std::int64_t id) {
    Stmt s(db_, "DELETE FROM locations WHERE id = ?;");
    s.bind_int64(1, id);
    s.step();
}

void Db::update_location_notes(std::int64_t id, const std::string& notes) {
    require_clean_text(notes, MAX_NOTES, true, "location.notes");
    Stmt s(db_, "UPDATE locations SET notes = ? WHERE id = ?;");
    s.bind_text(1, notes);
    s.bind_int64(2, id);
    s.step();
}

// ---- categories ----

std::vector<std::string> Db::categories() const {
    std::vector<std::string> out;
    Stmt s(db_, "SELECT name FROM categories ORDER BY "
                "CASE name WHEN 'General' THEN 0 ELSE 1 END, sort_order, name;");
    while (s.step() == SQLITE_ROW) out.push_back(s.col_text(0));
    return out;
}

bool Db::has_category(const std::string& name) const {
    Stmt s(db_, "SELECT 1 FROM categories WHERE name = ? LIMIT 1;");
    s.bind_text(1, name);
    return s.step() == SQLITE_ROW;
}

void Db::add_category(const std::string& name) {
    require_clean_text(name, MAX_CATEGORY, false, "category.name");
    Stmt s(db_, "INSERT OR IGNORE INTO categories(name, sort_order) VALUES (?, 0);");
    s.bind_text(1, name);
    s.step();
}

void Db::rename_category(const std::string& old_name, const std::string& new_name) {
    require_clean_text(old_name, MAX_CATEGORY, false, "category.old_name");
    require_clean_text(new_name, MAX_CATEGORY, false, "category.new_name");
    exec("BEGIN;");
    try {
        {
            Stmt s(db_, "INSERT OR IGNORE INTO categories(name, sort_order) "
                        "SELECT ?, sort_order FROM categories WHERE name = ?;");
            s.bind_text(1, new_name);
            s.bind_text(2, old_name);
            s.step();
        }
        {
            Stmt s(db_, "UPDATE snippets SET category = ? WHERE category = ?;");
            s.bind_text(1, new_name);
            s.bind_text(2, old_name);
            s.step();
        }
        {
            Stmt s(db_, "DELETE FROM categories WHERE name = ?;");
            s.bind_text(1, old_name);
            s.step();
        }
        exec("COMMIT;");
    } catch (...) {
        exec("ROLLBACK;");
        throw;
    }
}

void Db::remove_category(const std::string& name) {
    exec("BEGIN;");
    try {
        {
            Stmt s(db_, "UPDATE snippets SET category = 'General' WHERE category = ?;");
            s.bind_text(1, name);
            s.step();
        }
        {
            Stmt s(db_, "DELETE FROM categories WHERE name = ?;");
            s.bind_text(1, name);
            s.step();
        }
        exec("COMMIT;");
    } catch (...) {
        exec("ROLLBACK;");
        throw;
    }
}

// ---- snippets ----

static std::vector<std::int64_t> load_snippet_locations(sqlite3* db, std::int64_t snippet_id) {
    std::vector<std::int64_t> out;
    Stmt s(db, "SELECT location_id FROM snippet_locations WHERE snippet_id = ?;");
    s.bind_int64(1, snippet_id);
    while (s.step() == SQLITE_ROW) out.push_back(s.col_int64(0));
    return out;
}

std::vector<Snippet> Db::snippets(std::int64_t location_filter_id,
                                  const std::string& category_filter) const {
    std::vector<Snippet> out;
    std::string sql;
    if (location_filter_id > 0) {
        sql = "SELECT DISTINCT s.id, s.label, s.content, s.category, s.created_at "
              "FROM snippets s "
              "JOIN snippet_locations sl ON sl.snippet_id = s.id "
              "WHERE sl.location_id = ?";
        if (!category_filter.empty()) sql += " AND s.category = ?";
        sql += " ORDER BY s.created_at DESC;";
    } else {
        sql = "SELECT id, label, content, category, created_at FROM snippets";
        if (!category_filter.empty()) sql += " WHERE category = ?";
        sql += " ORDER BY created_at DESC;";
    }
    Stmt s(db_, sql.c_str());
    int idx = 1;
    if (location_filter_id > 0) s.bind_int64(idx++, location_filter_id);
    if (!category_filter.empty()) s.bind_text(idx++, category_filter);
    while (s.step() == SQLITE_ROW) {
        Snippet sn;
        sn.id         = s.col_int64(0);
        sn.label      = s.col_text(1);
        sn.content    = s.col_text(2);
        sn.category   = s.col_text(3);
        sn.created_at = s.col_int64(4);
        out.push_back(std::move(sn));
    }
    for (auto& sn : out) sn.location_ids = load_snippet_locations(db_, sn.id);
    return out;
}

std::vector<Snippet> Db::snippets_untagged(const std::string& category_filter) const {
    std::vector<Snippet> out;
    std::string sql =
        "SELECT id, label, content, category, created_at FROM snippets s "
        "WHERE NOT EXISTS (SELECT 1 FROM snippet_locations sl WHERE sl.snippet_id = s.id)";
    if (!category_filter.empty()) sql += " AND category = ?";
    sql += " ORDER BY created_at DESC;";
    Stmt s(db_, sql.c_str());
    if (!category_filter.empty()) s.bind_text(1, category_filter);
    while (s.step() == SQLITE_ROW) {
        Snippet sn;
        sn.id         = s.col_int64(0);
        sn.label      = s.col_text(1);
        sn.content    = s.col_text(2);
        sn.category   = s.col_text(3);
        sn.created_at = s.col_int64(4);
        out.push_back(std::move(sn));
    }
    return out;
}

Snippet Db::snippet_by_id(std::int64_t id) const {
    Stmt s(db_, "SELECT id, label, content, category, created_at FROM snippets WHERE id = ?;");
    s.bind_int64(1, id);
    Snippet sn;
    if (s.step() == SQLITE_ROW) {
        sn.id         = s.col_int64(0);
        sn.label      = s.col_text(1);
        sn.content    = s.col_text(2);
        sn.category   = s.col_text(3);
        sn.created_at = s.col_int64(4);
        sn.location_ids = load_snippet_locations(db_, sn.id);
    }
    return sn;
}

std::int64_t Db::add_snippet(const std::string& label,
                             const std::string& content,
                             const std::string& category,
                             const std::vector<std::int64_t>& location_ids) {
    require_clean_text(label,    MAX_LABEL,    false, "snippet.label");
    require_clean_text(content,  MAX_CONTENT,  true,  "snippet.content");
    require_clean_text(category, MAX_CATEGORY, false, "snippet.category");
    std::int64_t id = 0;
    exec("BEGIN;");
    try {
        {
            Stmt s(db_,
                "INSERT INTO snippets(label, content, category, created_at) "
                "VALUES (?, ?, ?, strftime('%s','now'));");
            s.bind_text(1, label);
            s.bind_text(2, content);
            s.bind_text(3, category);
            s.step();
            id = sqlite3_last_insert_rowid(db_);
        }
        for (auto lid : location_ids) {
            Stmt s(db_,
                "INSERT OR IGNORE INTO snippet_locations(snippet_id, location_id) "
                "VALUES (?, ?);");
            s.bind_int64(1, id);
            s.bind_int64(2, lid);
            s.step();
        }
        exec("COMMIT;");
    } catch (...) {
        exec("ROLLBACK;");
        throw;
    }
    return id;
}

void Db::update_snippet(std::int64_t id,
                        const std::string& label,
                        const std::string& content,
                        const std::string& category,
                        const std::vector<std::int64_t>& location_ids) {
    require_clean_text(label,    MAX_LABEL,    false, "snippet.label");
    require_clean_text(content,  MAX_CONTENT,  true,  "snippet.content");
    require_clean_text(category, MAX_CATEGORY, false, "snippet.category");
    exec("BEGIN;");
    try {
        {
            Stmt s(db_,
                "UPDATE snippets SET label = ?, content = ?, category = ? WHERE id = ?;");
            s.bind_text(1, label);
            s.bind_text(2, content);
            s.bind_text(3, category);
            s.bind_int64(4, id);
            s.step();
        }
        {
            Stmt s(db_, "DELETE FROM snippet_locations WHERE snippet_id = ?;");
            s.bind_int64(1, id);
            s.step();
        }
        for (auto lid : location_ids) {
            Stmt s(db_,
                "INSERT OR IGNORE INTO snippet_locations(snippet_id, location_id) "
                "VALUES (?, ?);");
            s.bind_int64(1, id);
            s.bind_int64(2, lid);
            s.step();
        }
        exec("COMMIT;");
    } catch (...) {
        exec("ROLLBACK;");
        throw;
    }
}

void Db::remove_snippet(std::int64_t id) {
    Stmt s(db_, "DELETE FROM snippets WHERE id = ?;");
    s.bind_int64(1, id);
    s.step();
}
