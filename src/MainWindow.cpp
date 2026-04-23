#include "MainWindow.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>

#include <gdkmm/clipboard.h>
#include <gdkmm/display.h>
#include <glibmm/main.h>
#include <glibmm/markup.h>
#include <gtkmm/icontheme.h>

namespace fs = std::filesystem;

static void pad(Gtk::Widget& w, int v) {
    w.set_margin_top(v);
    w.set_margin_bottom(v);
    w.set_margin_start(v);
    w.set_margin_end(v);
}

// Normalize a string for search: lowercase, replace non-alpha runs with a
// single space, trim edges. So "America/New_York" -> "america new york",
// "Memphis, Tennessee" -> "memphis tennessee".
static std::string normalize_search(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool prev_sep = true;
    for (char raw : s) {
        unsigned char u = static_cast<unsigned char>(raw);
        if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9')) {
            out.push_back(static_cast<char>(std::tolower(u)));
            prev_sep = false;
        } else if (!prev_sep) {
            out.push_back(' ');
            prev_sep = true;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// Soundex-4 of an alphabetic token. Used as a phonetic fallback so typos like
// "menfis" match "memphis" and "tenisee" matches "tennessee".
// Returns "" for empty/non-alpha input — callers should treat that as no match.
static std::string soundex4(const std::string& tok) {
    if (tok.empty()) return "";
    unsigned char c0 = static_cast<unsigned char>(tok[0]);
    if (!((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z'))) return "";
    std::string out(1, static_cast<char>(std::toupper(c0)));
    auto digit = [](unsigned char c) -> char {
        c = static_cast<unsigned char>(std::tolower(c));
        switch (c) {
            case 'b': case 'f': case 'p': case 'v': return '1';
            case 'c': case 'g': case 'j': case 'k':
            case 'q': case 's': case 'x': case 'z': return '2';
            case 'd': case 't':                     return '3';
            case 'l':                               return '4';
            case 'm': case 'n':                     return '5';
            case 'r':                               return '6';
            default:                                return '0';  // vowels/h/w/y
        }
    };
    char last = digit(c0);
    for (std::size_t i = 1; i < tok.size() && out.size() < 4; ++i) {
        char d = digit(static_cast<unsigned char>(tok[i]));
        if (d == '0') { last = '0'; continue; }
        if (d == last) continue;
        out.push_back(d);
        last = d;
    }
    while (out.size() < 4) out.push_back('0');
    return out;
}

// Fuzzy token match: substring primary, Soundex fallback for tokens ≥ 4 chars.
// Short tokens use substring-only because Soundex over-matches at length 2-3.
static bool token_matches(const std::string& query_tok, const std::string& target_tok) {
    if (target_tok.find(query_tok) != std::string::npos) return true;
    if (query_tok.size() < 4 || target_tok.size() < 4) return false;
    return soundex4(query_tok) == soundex4(target_tok);
}

// Split a normalized haystack/needle on spaces.
static std::vector<std::string> split_tokens(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ' ') {
            if (!cur.empty()) { out.push_back(std::move(cur)); cur.clear(); }
        } else cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

// True when every query token finds at least one matching target token.
static bool fuzzy_match(const std::string& query_norm, const std::string& target_norm) {
    if (query_norm.empty()) return true;
    // Whole-string substring: catches multi-word queries like "new york".
    if (target_norm.find(query_norm) != std::string::npos) return true;
    auto qt = split_tokens(query_norm);
    auto tt = split_tokens(target_norm);
    if (qt.empty()) return true;
    for (const auto& q : qt) {
        bool hit = false;
        for (const auto& t : tt) {
            if (token_matches(q, t)) { hit = true; break; }
        }
        if (!hit) return false;
    }
    return true;
}

// Turn "America/New_York" into "New York". Keeps "UTC" etc. as-is.
static std::string friendly_zone(const std::string& iana) {
    if (iana.empty()) return iana;
    auto slash = iana.rfind('/');
    std::string city = (slash == std::string::npos) ? iana : iana.substr(slash + 1);
    std::replace(city.begin(), city.end(), '_', ' ');
    return city;
}


// Application icon: a clock face on a green gradient tile. Embedded as an SVG
// string so the binary ships self-contained. On startup we write it to the
// user's data dir and register that dir with the icon theme, so set_icon_name
// picks it up in the taskbar/dock.
static const char* kAppIconName = "tz-workspace";
static const char* kAppIconSvg = R"SVG(<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 128" width="128" height="128">
  <defs>
    <linearGradient id="bg" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="#2ea043"/>
      <stop offset="1" stop-color="#175c2b"/>
    </linearGradient>
  </defs>
  <rect x="6" y="6" width="116" height="116" rx="22" ry="22" fill="url(#bg)"/>
  <circle cx="64" cy="64" r="42" fill="#f5f7f2" stroke="#0d2818" stroke-width="2"/>
  <g stroke="#0d2818" stroke-linecap="round" stroke-width="3">
    <line x1="64" y1="28" x2="64" y2="36"/>
    <line x1="64" y1="92" x2="64" y2="100"/>
    <line x1="28" y1="64" x2="36" y2="64"/>
    <line x1="92" y1="64" x2="100" y2="64"/>
  </g>
  <g stroke="#0d2818" stroke-linecap="round" stroke-width="2" opacity="0.5">
    <line x1="82" y1="33.8" x2="78.7" y2="39.6"/>
    <line x1="94.2" y1="46" x2="88.4" y2="49.3"/>
    <line x1="94.2" y1="82" x2="88.4" y2="78.7"/>
    <line x1="82" y1="94.2" x2="78.7" y2="88.4"/>
    <line x1="46" y1="94.2" x2="49.3" y2="88.4"/>
    <line x1="33.8" y1="82" x2="39.6" y2="78.7"/>
    <line x1="33.8" y1="46" x2="39.6" y2="49.3"/>
    <line x1="46" y1="33.8" x2="49.3" y2="39.6"/>
  </g>
  <line x1="64" y1="64" x2="44" y2="50" stroke="#0d2818" stroke-width="5" stroke-linecap="round"/>
  <line x1="64" y1="64" x2="84" y2="50" stroke="#2ea043" stroke-width="3.5" stroke-linecap="round"/>
  <circle cx="64" cy="64" r="3.5" fill="#0d2818"/>
</svg>
)SVG";

// Install a freedesktop .desktop file on first run so the compositor recognizes
// the window as a first-class application — fixes snap-to-edge / tiling on
// GNOME + KDE + Cinnamon, gives Alt-Tab a proper name+icon, and lets the WM
// remember per-workspace placement across sessions. Safe no-op if the file is
// already current.
static void install_desktop_file() {
    const char* xdg_data = std::getenv("XDG_DATA_HOME");
    const char* home     = std::getenv("HOME");
    fs::path base;
    if (xdg_data && *xdg_data && fs::path(xdg_data).is_absolute()) {
        base = xdg_data;
    } else if (home && *home && fs::path(home).is_absolute()) {
        base = fs::path(home) / ".local" / "share";
    } else {
        return;
    }
    fs::path apps_dir = base / "applications";
    std::error_code ec;
    fs::create_directories(apps_dir, ec);
    if (ec) return;

    // Resolve our own binary path via /proc/self/exe so Exec= points at the
    // right file regardless of the working directory or $PATH.
    fs::path exe_path;
    {
        std::error_code lec;
        exe_path = fs::read_symlink("/proc/self/exe", lec);
        if (lec) return;
    }
    const std::string exe_str = exe_path.string();
    // Reject any exec path containing control chars — the .desktop spec uses
    // bare strings with limited quoting, so a rogue newline could break it.
    for (char c : exe_str) {
        if (static_cast<unsigned char>(c) < 0x20 || c == '"') return;
    }

    std::string content =
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Version=1.0\n"
        "Name=TZ Workspace\n"
        "GenericName=Timezone Workspace\n"
        "Comment=Timezone switching with notes and snippets per location\n"
        // Wrap in double quotes so paths with spaces (e.g. "work tools/") are
        // one Exec argument, not split on whitespace by the launcher.
        "Exec=\"" + exe_str + "\" %U\n"
        "Icon=tz-workspace\n"
        "Terminal=false\n"
        "Categories=Utility;Office;Clock;\n"
        "Keywords=timezone;clock;tz;time;\n"
        "StartupNotify=true\n"
        "StartupWMClass=local.tzworkspace.app\n";

    fs::path desktop_path = apps_dir / "local.tzworkspace.app.desktop";
    bool need_write = true;
    if (fs::exists(desktop_path, ec)) {
        auto sz = fs::file_size(desktop_path, ec);
        if (!ec && sz == content.size()) need_write = false;
    }
    if (need_write) {
        std::ofstream f(desktop_path, std::ios::binary | std::ios::trunc);
        if (f) f.write(content.data(),
                       static_cast<std::streamsize>(content.size()));
    }
}

static void install_app_icon() {
    const char* xdg  = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    fs::path base;
    if (xdg && *xdg && fs::path(xdg).is_absolute()) {
        base = xdg;
    } else if (home && *home && fs::path(home).is_absolute()) {
        base = fs::path(home) / ".local" / "share";
    } else {
        return;
    }
    fs::path icons_root = base / "tz-workspace" / "icons";
    fs::path icon_dir   = icons_root / "hicolor" / "scalable" / "apps";
    std::error_code ec;
    fs::create_directories(icon_dir, ec);
    if (ec) return;
    fs::path icon_path = icon_dir / (std::string(kAppIconName) + ".svg");
    const std::size_t want = std::strlen(kAppIconSvg);
    // Rewrite only if the file is missing or its size differs — lets us ship a
    // new icon revision by bumping the SVG without forcing the user to delete.
    bool need_write = true;
    if (fs::exists(icon_path, ec)) {
        auto sz = fs::file_size(icon_path, ec);
        if (!ec && sz == want) need_write = false;
    }
    if (need_write) {
        std::ofstream f(icon_path, std::ios::binary | std::ios::trunc);
        if (f) f.write(kAppIconSvg, static_cast<std::streamsize>(want));
    }
    auto disp = Gdk::Display::get_default();
    if (!disp) return;
    auto theme = Gtk::IconTheme::get_for_display(disp);
    if (theme) theme->add_search_path(icons_root.string());
}

static fs::path db_path() {
    // Refuse relative or empty env paths — we chmod 0700 on the parent dir, and
    // a crafted relative path could redirect that operation outside $HOME.
    const char* xdg  = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    fs::path base;
    if (xdg && *xdg && fs::path(xdg).is_absolute()) {
        base = xdg;
    } else if (home && *home && fs::path(home).is_absolute()) {
        base = fs::path(home) / ".local" / "share";
    } else {
        throw std::runtime_error(
            "tz-workspace: HOME/XDG_DATA_HOME not set to an absolute path; refusing to start.");
    }
    return base / "tz-workspace" / "data.db";
}

MainWindow::MainWindow()
    : root_(Gtk::Orientation::VERTICAL, 6),
      topbar_(Gtk::Orientation::HORIZONTAL, 10),
      current_box_(Gtk::Orientation::VERTICAL, 0),
      home_box_(Gtk::Orientation::VERTICAL, 0),
      revert_btn_("\xe2\x86\xba Go Home"),
      set_home_btn_("Make current zone my Home"),
      clear_home_btn_("Forget Home"),
      refresh_btn_("Re-check current zone"),
      paned_(Gtk::Orientation::HORIZONTAL),
      loc_tab_(Gtk::Orientation::VERTICAL, 4),
      loc_header_(Gtk::Orientation::HORIZONTAL, 6),
      add_loc_btn_("+ Add place"),
      fav_tab_(Gtk::Orientation::VERTICAL, 4),
      all_tab_(Gtk::Orientation::VERTICAL, 4),
      settings_tab_(Gtk::Orientation::VERTICAL, 10),
      settings_24h_chk_("Use 24-hour clock"),
      settings_seconds_chk_("Show seconds in clock"),
      active_row_(Gtk::Orientation::HORIZONTAL, 6),
      switch_active_btn_("\xe2\x9a\xa1 Switch to this zone"),
      clear_active_btn_("Close"),
      notes_title_("Notes"),
      sn_header_(Gtk::Orientation::HORIZONTAL, 6),
      sn_title_("Snippets"),
      add_snippet_btn_("+ New snippet"),
      manage_cats_btn_("Categories...") {

    set_title("TZ Workspace");
    install_app_icon();
    install_desktop_file();
    set_icon_name(kAppIconName);
    // Low floor so the app fits any reasonable tile — quarter of the Dell
    // (720x450), a 3-column layout (480x...), portrait sidekick displays.
    // Below this, buttons start clipping, but content stays scannable via
    // ellipsize + internal scrollbars. If the user shrinks further, GTK
    // naturally handles it; we don't refuse.
    set_size_request(320, 240);
    // Default_size is applied below, after db_ is constructed, so we can
    // restore the user's last-session size.

    // Custom CSS:
    //   1. "bordered-rows" class — gives every list row a visible card-style
    //      border + inner padding. Applied to every ListBox in the app so the
    //      UI has a consistent, easy-to-scan look for non-technical users.
    //   2. "active-location" class — green highlight for the currently
    //      viewed place; overrides the bordered-row background.
    auto css = Gtk::CssProvider::create();
    css->load_from_data(
        "listbox.bordered-rows {"
        "  background: transparent;"
        "}"
        "listbox.bordered-rows > row {"
        "  border: 1px solid alpha(currentColor, 0.18);"
        "  border-radius: 8px;"
        "  margin: 3px 4px;"
        "  padding: 2px;"
        "  background-color: alpha(currentColor, 0.04);"
        "  transition: border-color 120ms, background-color 120ms;"
        "}"
        "listbox.bordered-rows > row:hover {"
        "  border-color: alpha(currentColor, 0.32);"
        "  background-color: alpha(currentColor, 0.08);"
        "}"
        "row.active-location,"
        "listbox.bordered-rows > row.active-location {"
        "  background-color: alpha(#2ea043, 0.22);"
        "  border: 1px solid alpha(#2ea043, 0.55);"
        "  border-left: 4px solid #2ea043;"
        "}"
        "row.active-location:hover,"
        "listbox.bordered-rows > row.active-location:hover {"
        "  background-color: alpha(#2ea043, 0.32);"
        "}"
        // Management bars — each management row (add-place, snippets header,
        // active-location banner) reads as a bordered card strip anchored to
        // the content below it, not floating buttons in open space.
        "box.mgmt-bar {"
        "  padding: 4px 6px;"
        "  background-color: alpha(currentColor, 0.07);"
        "  border: 1px solid alpha(currentColor, 0.22);"
        "  border-radius: 8px;"
        "}"
        "box.mgmt-bar.active-mgmt {"
        "  background-color: alpha(#2ea043, 0.18);"
        "  border-color: alpha(#2ea043, 0.55);"
        "}"
        "box.mgmt-bar > button.flat {"
        "  min-height: 24px;"
        "  padding: 2px 10px;"
        "}");
    Gtk::StyleContext::add_provider_for_display(
        Gdk::Display::get_default(), css,
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    // Separate CSS provider that scales font-size + button/row padding based
    // on window height. Priority is above the base app CSS so rules here
    // override static paddings. Updated by apply_scale() whenever the window
    // resizes (see the poll timer below).
    scale_css_ = Gtk::CssProvider::create();
    Gtk::StyleContext::add_provider_for_display(
        Gdk::Display::get_default(), scale_css_,
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 10);

    db_  = std::make_unique<Db>(db_path());
    svc_ = std::make_unique<TzService>(*db_);
    svc_->import_legacy_config();

    // First-run: if nothing's been set as Home yet, seed it with the current
    // system zone. The app is usable immediately — Go Home works out of the box.
    if (db_->get_setting("home_zone").empty()) {
        const std::string cur = svc_->current_zone();
        if (svc_->is_valid_zone(cur)) db_->set_setting("home_zone", cur);
    }

    // Window size: restore from DB if saved; else derive from the primary
    // monitor (~60% width, 70% height) so we scale with the user's screen
    // instead of pinning a fixed 1040x720 on every box.
    int win_w = setting_int("window_width", 0);
    int win_h = setting_int("window_height", 0);
    if (win_w <= 0 || win_h <= 0) {
        int mw = 1040, mh = 720;
        auto disp = Gdk::Display::get_default();
        if (disp) {
            auto list = disp->get_monitors();
            if (list && list->get_n_items() > 0) {
                auto obj = list->get_object(0);
                if (auto mon = std::dynamic_pointer_cast<Gdk::Monitor>(obj)) {
                    Gdk::Rectangle r;
                    mon->get_geometry(r);
                    if (r.get_width() > 0 && r.get_height() > 0) {
                        mw = static_cast<int>(r.get_width()  * 0.60);
                        mh = static_cast<int>(r.get_height() * 0.70);
                    }
                }
            }
        }
        if (mw < 520) mw = 520;
        if (mh < 420) mh = 420;
        win_w = mw; win_h = mh;
    }
    set_default_size(win_w, win_h);

    build_ui();
    refresh_all();
    tick_clock();

    clock_conn_ = Glib::signal_timeout().connect_seconds(
        sigc::mem_fun(*this, &MainWindow::tick_clock), 1);

    // Re-apply scale CSS whenever the window height changes. Polling @ 5fps
    // is imperceptible on drag-resize but costs nothing idle since apply_scale
    // early-returns when height is unchanged.
    scale_conn_ = Glib::signal_timeout().connect([this] {
        apply_scale();
        return true;
    }, 200);

    if (svc_->all_zones().empty()) {
        set_status("Couldn't read the system timezone list. "
                   "Make sure 'timedatectl' is installed.");
    } else {
        set_status("Ready. Pick a place on the left, or click Go Home to jump back.");
    }
}

void MainWindow::apply_scale() {
    const int h = get_height();
    if (h <= 0 || !scale_css_) return;
    if (h == last_scale_h_) return;
    last_scale_h_ = h;

    // Reference height = 720px looks "normal". Below scales down; above
    // scales up. Clamp so extremes don't get unreadable or cartoonish.
    double s = static_cast<double>(h) / 720.0;
    if (s < 0.70) s = 0.70;
    if (s > 1.40) s = 1.40;

    const int font_px  = static_cast<int>(std::round(14.0 * s));
    const int btn_v    = std::max(2, static_cast<int>(std::round(4.0  * s)));
    const int btn_h    = std::max(4, static_cast<int>(std::round(10.0 * s)));
    const int row_v    = std::max(1, static_cast<int>(std::round(2.0  * s)));
    const int row_h    = std::max(4, static_cast<int>(std::round(6.0  * s)));

    std::string css;
    css.reserve(256);
    css += "* { font-size: "; css += std::to_string(font_px); css += "px; }\n";
    css += "button { padding: ";
    css += std::to_string(btn_v); css += "px ";
    css += std::to_string(btn_h); css += "px; }\n";
    css += "listbox.bordered-rows > row { padding: ";
    css += std::to_string(row_v); css += "px ";
    css += std::to_string(row_h); css += "px; }\n";

    scale_css_->load_from_data(css);
}

int MainWindow::setting_int(const std::string& key, int def) const {
    if (!db_) return def;
    const std::string v = db_->get_setting(key);
    if (v.empty()) return def;
    char* end = nullptr;
    long n = std::strtol(v.c_str(), &end, 10);
    if (end == v.c_str() || n <= 0 || n > 100000) return def;
    return static_cast<int>(n);
}

void MainWindow::save_window_state() {
    if (!db_) return;
    const int w = get_width();
    const int h = get_height();
    if (w > 0 && h > 0) {
        db_->set_setting("window_width",  std::to_string(w));
        db_->set_setting("window_height", std::to_string(h));
    }
    // Paned positions may be 0 before the window is mapped; skip to avoid
    // persisting a bogus value on a never-shown window.
    const int p_main = paned_.get_position();
    const int p_ws   = ws_paned_.get_position();
    if (p_main > 0) db_->set_setting("paned_main_pos", std::to_string(p_main));
    if (p_ws   > 0) db_->set_setting("paned_ws_pos",   std::to_string(p_ws));
}

MainWindow::~MainWindow() {
    save_window_state();
    flush_notes_if_dirty();
    clock_conn_.disconnect();
    scale_conn_.disconnect();
    notes_focus_conn_.disconnect();
}

// =============================================================
// UI construction
// =============================================================

void MainWindow::build_ui() {
    pad(root_, 8);

    // ---- Compact top bar ----
    // Current zone + live clock (left, flex) | Home zone + live clock (right)
    // | Go Home | ⚙. Two clocks let travellers see both times without hunting
    // for a calculator — avoids the "almost worked an extra hour" trap.
    current_lbl_.set_xalign(0.0f);
    current_lbl_.set_ellipsize(Pango::EllipsizeMode::END);
    current_lbl_.set_max_width_chars(42);

    clock_lbl_.set_xalign(0.0f);
    clock_lbl_.add_css_class("dim-label");
    clock_lbl_.set_ellipsize(Pango::EllipsizeMode::END);

    current_box_.set_hexpand(true);
    current_box_.append(current_lbl_);
    current_box_.append(clock_lbl_);

    home_lbl_.set_xalign(1.0f);
    home_lbl_.add_css_class("dim-label");
    home_lbl_.set_ellipsize(Pango::EllipsizeMode::END);
    home_lbl_.set_max_width_chars(28);

    home_clock_lbl_.set_xalign(1.0f);
    home_clock_lbl_.add_css_class("dim-label");
    home_clock_lbl_.set_ellipsize(Pango::EllipsizeMode::END);
    home_clock_lbl_.set_max_width_chars(32);

    home_box_.set_halign(Gtk::Align::END);
    home_box_.append(home_lbl_);
    home_box_.append(home_clock_lbl_);

    revert_btn_.add_css_class("suggested-action");
    revert_btn_.set_tooltip_text(
        "Switch the computer's clock back to your Home timezone.");
    revert_btn_.set_valign(Gtk::Align::CENTER);
    revert_btn_.signal_clicked().connect([this] {
        const std::string h = db_->get_setting("home_zone");
        if (h.empty()) {
            set_status("No Home set yet. Open Settings to set one.");
            return;
        }
        apply_tz(h);
    });

    // Make/Forget Home buttons now live in Settings; wire them once here.
    set_home_btn_.set_tooltip_text(
        "Remember the current timezone as your Home base.");
    clear_home_btn_.set_tooltip_text("Forget your saved Home.");
    set_home_btn_.signal_clicked().connect([this] {
        const std::string cur = svc_->current_zone();
        if (!svc_->is_valid_zone(cur)) {
            set_status("Can't use this timezone as Home (unrecognized).");
            return;
        }
        db_->set_setting("home_zone", cur);
        update_home_state();
        refresh_settings_tab();
        set_status("Home set to " + friendly_zone(cur) + ".");
    });
    clear_home_btn_.signal_clicked().connect([this] {
        db_->delete_setting("home_zone");
        update_home_state();
        refresh_settings_tab();
        set_status("Forgot your Home.");
    });
    refresh_btn_.signal_clicked().connect([this] {
        refresh_current_label();
        update_home_state();
        set_status("Current timezone refreshed.");
    });

    // Top bar = [current zone (left)] [tab switcher (center, hexpand)]
    // [home + controls (right)]. Current and home clocks sit on opposite
    // sides so the delta is impossible to miss at a glance.
    left_switcher_.set_stack(left_stack_);
    left_switcher_.set_hexpand(true);
    left_switcher_.set_halign(Gtk::Align::CENTER);
    left_switcher_.add_css_class("linked");

    current_box_.set_halign(Gtk::Align::START);
    current_box_.set_hexpand(false);

    auto* right_group = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
    right_group->set_halign(Gtk::Align::END);
    right_group->append(home_box_);
    right_group->append(revert_btn_);

    topbar_.append(current_box_);
    topbar_.append(left_switcher_);
    topbar_.append(*right_group);

    // ---- Left pane ----
    paned_.set_vexpand(true);
    paned_.set_hexpand(true);
    paned_.set_wide_handle(true);
    paned_.set_position(setting_int("paned_main_pos", 380));
    // Allow shrink so the window can go below the combined child mins —
    // essential for small-tile snapping. Ellipsize picks up the slack.
    paned_.set_shrink_start_child(true);
    paned_.set_shrink_end_child(true);
    paned_.set_resize_start_child(true);
    paned_.set_resize_end_child(true);

    // Locations tab — management bar at the top holds the Add button, visually
    // anchored to the list below so the action reads as "act on this list".
    add_loc_btn_.set_tooltip_text(
        "Save a nickname for a city or location (e.g. \"New Rochelle\").");
    add_loc_btn_.add_css_class("flat");
    add_loc_btn_.set_hexpand(true);
    add_loc_btn_.set_halign(Gtk::Align::FILL);
    add_loc_btn_.signal_clicked().connect(
        [this] { open_add_location_dialog(); });
    loc_header_.add_css_class("mgmt-bar");
    loc_header_.append(add_loc_btn_);

    loc_scroll_.set_child(loc_list_);
    loc_scroll_.set_has_frame(true);
    loc_scroll_.set_vexpand(true);
    loc_list_.set_selection_mode(Gtk::SelectionMode::NONE);
    loc_list_.set_activate_on_single_click(true);
    loc_list_.add_css_class("bordered-rows");
    loc_list_.signal_row_activated().connect(
        [this](Gtk::ListBoxRow* row) {
            if (!row) return;
            int idx = row->get_index();
            if (idx < 0 ||
                static_cast<std::size_t>(idx) >= loc_row_ids_.size()) return;
            std::int64_t id = loc_row_ids_[static_cast<std::size_t>(idx)];
            if (id > 0) set_active_location(id, false);
        });

    pad(loc_tab_, 6);
    loc_tab_.append(loc_header_);
    loc_tab_.append(loc_scroll_);

    // Favorites tab — compact single-line rows
    fav_scroll_.set_child(fav_list_);
    fav_scroll_.set_has_frame(true);
    fav_scroll_.set_vexpand(true);
    fav_list_.set_selection_mode(Gtk::SelectionMode::NONE);
    fav_list_.add_css_class("bordered-rows");
    pad(fav_tab_, 6);
    fav_tab_.append(fav_scroll_);

    // All zones tab — search + compact single-line rows
    search_entry_.set_placeholder_text("Search (e.g. new_york, tokyo, utc)");
    search_entry_.set_hexpand(true);
    all_scroll_.set_child(all_list_);
    all_scroll_.set_has_frame(true);
    all_scroll_.set_vexpand(true);
    all_list_.set_selection_mode(Gtk::SelectionMode::NONE);
    all_list_.add_css_class("bordered-rows");
    pad(all_tab_, 6);
    all_tab_.append(search_entry_);
    all_tab_.append(all_scroll_);

    all_list_.set_filter_func([this](Gtk::ListBoxRow* row) -> bool {
        const std::string q = search_entry_.get_text();
        if (q.empty()) return true;
        int idx = row->get_index();
        if (idx < 0 ||
            static_cast<std::size_t>(idx) >= all_row_haystack_.size()) return true;
        return fuzzy_match(normalize_search(q), all_row_haystack_[idx]);
    });
    search_entry_.signal_search_changed().connect(
        [this] { all_list_.invalidate_filter(); });

    build_settings_tab();
    // Wrap settings in a ScrolledWindow — it has a lot of content and a Stack
    // reports its min as the max of all pages' mins. Without this wrap, the
    // settings page forces the whole left pane to ~400px tall and blocks
    // small-tile snapping.
    auto* settings_scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    settings_scroll->set_child(settings_tab_);
    settings_scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    settings_scroll->set_has_frame(false);
    settings_scroll->set_vexpand(true);
    left_stack_.add(loc_tab_, "places",   "My Places");
    left_stack_.add(fav_tab_, "pinned",   "Pinned");
    left_stack_.add(all_tab_, "all",      "All Timezones");
    left_stack_.add(*settings_scroll, "settings", "Settings");
    left_stack_.set_transition_type(Gtk::StackTransitionType::CROSSFADE);
    left_stack_.set_transition_duration(120);
    left_stack_.set_vexpand(true);
    left_stack_.set_hexpand(false);
    left_stack_.set_size_request(140, -1);
    paned_.set_start_child(left_stack_);

    // ---- Right pane: workspace ----
    active_lbl_.set_xalign(0.0f);
    active_lbl_.set_yalign(0.5f);
    active_lbl_.set_hexpand(true);
    active_lbl_.set_halign(Gtk::Align::FILL);
    active_lbl_.set_wrap(false);
    active_lbl_.set_ellipsize(Pango::EllipsizeMode::END);
    active_lbl_.set_width_chars(0);
    clear_active_btn_.set_tooltip_text(
        "Stop viewing this place. Notes and snippets below will hide.");
    clear_active_btn_.set_valign(Gtk::Align::CENTER);
    clear_active_btn_.signal_clicked().connect(
        [this] { clear_active_location(); });

    switch_active_btn_.add_css_class("suggested-action");
    switch_active_btn_.set_valign(Gtk::Align::CENTER);
    switch_active_btn_.set_tooltip_text(
        "Switch the computer's clock to this place's timezone.");
    switch_active_btn_.signal_clicked().connect([this] {
        if (active_location_id_ == 0) return;
        Location l = db_->location_by_id(active_location_id_);
        if (l.id == 0) return;
        apply_tz(l.zone);
    });

    active_row_.add_css_class("mgmt-bar");
    active_row_.add_css_class("active-mgmt");
    clear_active_btn_.add_css_class("flat");
    active_row_.append(active_lbl_);
    active_row_.append(switch_active_btn_);
    active_row_.append(clear_active_btn_);

    notes_title_.set_xalign(0.0f);
    notes_title_.add_css_class("heading");

    notes_view_.set_wrap_mode(Gtk::WrapMode::WORD_CHAR);
    notes_view_.set_monospace(false);
    notes_view_.set_top_margin(6);
    notes_view_.set_bottom_margin(6);
    notes_view_.set_left_margin(6);
    notes_view_.set_right_margin(6);
    notes_scroll_.set_child(notes_view_);
    notes_scroll_.set_has_frame(true);
    notes_scroll_.set_min_content_height(80);
    notes_scroll_.set_vexpand(true);

    // Flush notes when the buffer changes — debounced via "dirty" flag, saved
    // on focus leave / active-location change / shutdown.
    notes_view_.get_buffer()->signal_changed().connect(
        [this] { notes_dirty_ = true; });

    // Save when the TextView loses focus.
    auto focus_ctrl = Gtk::EventControllerFocus::create();
    focus_ctrl->signal_leave().connect([this] { flush_notes_if_dirty(); });
    notes_view_.add_controller(focus_ctrl);

    // Snippets header
    sn_title_.set_xalign(0.0f);
    sn_title_.add_css_class("heading");
    sn_title_.set_hexpand(false);
    sn_title_.set_ellipsize(Pango::EllipsizeMode::END);
    sn_title_.set_margin_end(6);

    cat_model_ = Gtk::StringList::create({});
    cat_filter_.set_model(cat_model_);
    cat_filter_.set_tooltip_text("Filter snippets by category");
    cat_filter_.property_selected().signal_changed().connect([this] {
        if (suspending_cat_signal_) return;
        auto sel = cat_filter_.get_selected();
        if (sel == GTK_INVALID_LIST_POSITION) { active_category_ = ""; }
        else if (sel == 0)                    { active_category_ = ""; }  // "All"
        else {
            if (sel < cat_model_->get_n_items()) {
                active_category_ = cat_model_->get_string(sel);
            }
        }
        refresh_snippets();
    });

    add_snippet_btn_.add_css_class("flat");
    add_snippet_btn_.set_tooltip_text(
        "Save a reusable piece of text. Click \"Copy\" on it later to put it on the clipboard.");
    add_snippet_btn_.signal_clicked().connect(
        [this] { open_snippet_dialog(0); });
    manage_cats_btn_.add_css_class("flat");
    manage_cats_btn_.set_tooltip_text("Create or remove groups for your snippets.");
    manage_cats_btn_.signal_clicked().connect(
        [this] { open_manage_categories_dialog(); });

    sn_header_.add_css_class("mgmt-bar");
    cat_filter_.set_hexpand(true);
    sn_header_.append(sn_title_);
    sn_header_.append(cat_filter_);
    sn_header_.append(manage_cats_btn_);
    sn_header_.append(add_snippet_btn_);

    sn_scroll_.set_child(sn_list_);
    sn_scroll_.set_has_frame(true);
    sn_scroll_.set_vexpand(true);
    sn_list_.set_selection_mode(Gtk::SelectionMode::NONE);
    sn_list_.add_css_class("bordered-rows");

    // Notes group holds the active-location banner + notes editor. Snippets
    // group stacks underneath via ws_paned_, which is the paned's end-child
    // directly — no wrapping box.
    auto* notes_group    = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    auto* snippets_group = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    pad(*notes_group, 6);
    pad(*snippets_group, 6);
    notes_group->append(active_row_);
    notes_group->append(notes_title_);
    notes_group->append(notes_scroll_);
    snippets_group->append(sn_header_);
    snippets_group->append(sn_scroll_);

    ws_paned_.set_orientation(Gtk::Orientation::VERTICAL);
    ws_paned_.set_wide_handle(true);
    ws_paned_.set_vexpand(true);
    ws_paned_.set_start_child(*notes_group);
    ws_paned_.set_end_child(*snippets_group);
    // Both sides host ScrolledWindows, so letting them shrink is safe — the
    // inner scrollbars handle overflow.
    ws_paned_.set_shrink_start_child(true);
    ws_paned_.set_shrink_end_child(true);
    ws_paned_.set_resize_start_child(true);
    ws_paned_.set_resize_end_child(true);
    ws_paned_.set_position(setting_int("paned_ws_pos", 220));
    ws_paned_.set_size_request(180, -1);

    paned_.set_end_child(ws_paned_);

    // ---- Status bar ----
    status_.set_xalign(0.0f);
    status_.add_css_class("dim-label");

    // Assemble root
    root_.append(topbar_);
    root_.append(paned_);
    root_.append(status_);
    set_child(root_);
}

// =============================================================
// Refresh
// =============================================================

void MainWindow::refresh_all() {
    refresh_current_label();
    update_home_state();
    build_all_list();
    refresh_favorites();
    refresh_locations();
    refresh_category_filter();
    refresh_active_banner();
    refresh_notes();
    refresh_snippets();
    refresh_settings_tab();
}

void MainWindow::refresh_current_label() {
    std::string cur = svc_->current_zone();
    if (cur.empty() || cur == "(unknown)") {
        current_lbl_.set_markup("<b>Couldn't detect your timezone</b>");
        return;
    }
    std::string city = friendly_zone(cur);
    current_lbl_.set_markup(
        "\xf0\x9f\x93\x8d <b>" + Glib::Markup::escape_text(city) + "</b>"
        "<span alpha='55%'>  \xc2\xb7 " + Glib::Markup::escape_text(cur) + "</span>");
}

void MainWindow::update_home_state() {
    const std::string h   = db_->get_setting("home_zone");
    const std::string cur = svc_->current_zone();
    if (h.empty()) {
        home_lbl_.set_markup("<span alpha='70%'>\xf0\x9f\x8f\xa0 Home not set</span>");
        home_clock_lbl_.set_visible(false);
        revert_btn_.set_visible(false);
    } else {
        std::string h_city = friendly_zone(h);
        const bool at_home = (h == cur);
        if (at_home) {
            home_lbl_.set_markup(
                "<span foreground='#2ea043'>\xe2\x9c\x93</span> "
                "<span alpha='75%'>Home: </span><b>" +
                Glib::Markup::escape_text(h_city) + "</b>");
            revert_btn_.set_visible(false);
        } else {
            home_lbl_.set_markup(
                "<span alpha='75%'>\xf0\x9f\x8f\xa0 Home: </span><b>" +
                Glib::Markup::escape_text(h_city) + "</b>");
            revert_btn_.set_visible(true);
            revert_btn_.set_sensitive(true);
            revert_btn_.set_tooltip_text(
                "Switch the computer's clock back to " + h_city + ".");
        }
        home_clock_lbl_.set_visible(true);
    }
}

// Fills `out` with local time at the given IANA zone for epoch `now`. Uses
// the standard TZ / tzset / localtime_r round-trip because glibc has no
// reentrant "localtime in named zone" call. This runs only from tick_clock
// on the GTK main thread, so the setenv isn't racing anything.
static void localtime_in_zone(const std::string& zone, std::time_t now, std::tm& out) {
    const char* prev_tz = std::getenv("TZ");
    std::string saved = prev_tz ? prev_tz : "";
    bool had_prev = (prev_tz != nullptr);
    ::setenv("TZ", zone.c_str(), 1);
    ::tzset();
    ::localtime_r(&now, &out);
    if (had_prev) ::setenv("TZ", saved.c_str(), 1);
    else          ::unsetenv("TZ");
    ::tzset();
}

// "+1h", "-2h30", "(same)". For fractional-hour zones (IST, NPT) shows h:mm.
static std::string fmt_offset_delta(long seconds) {
    if (seconds == 0) return "(same)";
    char sign = (seconds > 0) ? '+' : '-';
    long abs_s = (seconds > 0) ? seconds : -seconds;
    int h = static_cast<int>(abs_s / 3600);
    int m = static_cast<int>((abs_s % 3600) / 60);
    std::string out = "(";
    out.push_back(sign);
    out += std::to_string(h);
    if (m != 0) {
        out.push_back(':');
        if (m < 10) out.push_back('0');
        out += std::to_string(m);
    } else {
        out.push_back('h');
    }
    out.push_back(')');
    return out;
}

bool MainWindow::tick_clock() {
    ::tzset();
    const std::time_t now = std::time(nullptr);
    std::tm t_cur;
    ::localtime_r(&now, &t_cur);

    const bool use_24h  = (db_->get_setting("clock_24h")     == "1");
    const bool with_sec = (db_->get_setting("clock_seconds") == "1");
    const char* fmt;
    if (use_24h)  fmt = with_sec ? "%a %b %e \xc2\xb7 %H:%M:%S %Z"
                                 : "%a %b %e \xc2\xb7 %H:%M %Z";
    else          fmt = with_sec ? "%a %b %e \xc2\xb7 %I:%M:%S %p %Z"
                                 : "%a %b %e \xc2\xb7 %I:%M %p %Z";
    char buf[96];
    std::strftime(buf, sizeof(buf), fmt, &t_cur);
    clock_lbl_.set_text(buf);

    // Home clock — separate tz-override round-trip so it reflects home time
    // regardless of what the system clock is currently set to.
    const std::string home = db_->get_setting("home_zone");
    if (home.empty()) {
        home_clock_lbl_.set_visible(false);
        return true;
    }
    std::tm t_home;
    localtime_in_zone(home, now, t_home);

    char hbuf[96];
    // No zone abbrev here — localtime_in_zone leaves tm_zone pointing at
    // transient memory after we restore TZ, so %Z is unsafe. We show the
    // friendly city name in home_lbl_ above instead.
    const char* hfmt;
    if (use_24h)  hfmt = with_sec ? "%a %b %e \xc2\xb7 %H:%M:%S" : "%a %b %e \xc2\xb7 %H:%M";
    else          hfmt = with_sec ? "%a %b %e \xc2\xb7 %I:%M:%S %p" : "%a %b %e \xc2\xb7 %I:%M %p";
    std::strftime(hbuf, sizeof(hbuf), hfmt, &t_home);

    const long delta = static_cast<long>(t_home.tm_gmtoff) -
                       static_cast<long>(t_cur.tm_gmtoff);
    std::string markup;
    if (delta == 0) {
        markup = std::string("<span alpha='65%'>") +
                 Glib::Markup::escape_text(hbuf) + "</span>";
    } else {
        // Non-zero delta = physically in a different zone from home. Make
        // the delta badge prominent (orange) so you see it at a glance.
        markup = std::string("<span alpha='80%'>") +
                 Glib::Markup::escape_text(hbuf) +
                 "</span>  <span foreground='#f0883e' weight='bold'>" +
                 Glib::Markup::escape_text(fmt_offset_delta(delta)) + "</span>";
    }
    home_clock_lbl_.set_markup(markup);
    home_clock_lbl_.set_visible(true);
    return true;
}

void MainWindow::refresh_locations() {
    while (auto* c = loc_list_.get_first_child()) loc_list_.remove(*c);
    loc_row_ids_.clear();
    const auto locs = db_->locations();

    if (locs.empty()) {
        auto* row = Gtk::make_managed<Gtk::ListBoxRow>();
        auto* lbl = Gtk::make_managed<Gtk::Label>(
            "No places yet.\n\n"
            "Click \"+ Add place\" above to save a nickname "
            "like \"Office\" or \"New Rochelle\" so you can jump to it in one click.");
        lbl->set_xalign(0.0f);
        lbl->set_wrap(true);
        lbl->add_css_class("dim-label");
        pad(*lbl, 10);
        row->set_child(*lbl);
        row->set_selectable(false);
        row->set_activatable(false);
        loc_list_.append(*row);
        loc_row_ids_.push_back(0);
        return;
    }

    for (const auto& loc : locs) {
        auto* row  = Gtk::make_managed<Gtk::ListBoxRow>();
        auto* hbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        hbox->set_margin_top(3);
        hbox->set_margin_bottom(3);
        hbox->set_margin_start(6);
        hbox->set_margin_end(4);

        auto* textbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 1);
        textbox->set_hexpand(true);
        textbox->set_halign(Gtk::Align::FILL);

        bool is_active = (loc.id == active_location_id_);

        auto* name_lbl = Gtk::make_managed<Gtk::Label>();
        name_lbl->set_markup("<b>" + Glib::Markup::escape_text(loc.name) + "</b>");
        name_lbl->set_xalign(0.0f);
        name_lbl->set_ellipsize(Pango::EllipsizeMode::END);
        name_lbl->set_width_chars(0);
        name_lbl->set_hexpand(true);
        name_lbl->set_halign(Gtk::Align::FILL);

        std::string city = friendly_zone(loc.zone);
        auto* sub_lbl = Gtk::make_managed<Gtk::Label>(
            city + (city == loc.zone ? "" : "  \xc2\xb7  " + loc.zone));
        sub_lbl->set_xalign(0.0f);
        sub_lbl->add_css_class("dim-label");
        sub_lbl->set_ellipsize(Pango::EllipsizeMode::END);
        sub_lbl->set_width_chars(0);
        sub_lbl->set_hexpand(true);
        sub_lbl->set_halign(Gtk::Align::FILL);

        textbox->append(*name_lbl);
        textbox->append(*sub_lbl);

        auto* rm_btn = Gtk::make_managed<Gtk::Button>("\xc3\x97");
        rm_btn->set_tooltip_text(
            "Delete this saved place. Snippets linked to it become unassigned.");
        const std::int64_t lid = loc.id;
        const std::string nm = loc.name;
        rm_btn->signal_clicked().connect([this, lid, nm] {
            db_->remove_location(lid);
            if (active_location_id_ == lid) active_location_id_ = 0;
            refresh_all();
            set_status("Removed \"" + nm + "\".");
        });

        hbox->append(*textbox);
        hbox->append(*rm_btn);
        row->set_child(*hbox);
        row->set_activatable(true);
        if (is_active) row->add_css_class("active-location");
        loc_list_.append(*row);
        loc_row_ids_.push_back(loc.id);
    }
}

// Build one compact single-line row: "<b>City</b> · Zone/Path   [switch] [star]"
// Shared between the Pinned and All Timezones tabs.
static Gtk::ListBoxRow* make_zone_row(
    const std::string& tz,
    Gtk::Button*& out_action,   // right-most action button ("×" or "☆/★")
    Gtk::Button*& out_switch) {
    auto* row  = Gtk::make_managed<Gtk::ListBoxRow>();
    auto* hbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
    hbox->set_margin_top(1);
    hbox->set_margin_bottom(1);
    hbox->set_margin_start(6);
    hbox->set_margin_end(4);

    auto* lbl = Gtk::make_managed<Gtk::Label>();
    std::string city = friendly_zone(tz);
    std::string markup = "<b>" + Glib::Markup::escape_text(city) + "</b>";
    if (city != tz) {
        markup += "  <span alpha='55%'>\xc2\xb7 " +
                  Glib::Markup::escape_text(tz) + "</span>";
    }
    lbl->set_markup(markup);
    lbl->set_xalign(0.0f);
    lbl->set_hexpand(true);
    lbl->set_halign(Gtk::Align::FILL);
    lbl->set_ellipsize(Pango::EllipsizeMode::END);
    lbl->set_width_chars(0);

    auto* sw = Gtk::make_managed<Gtk::Button>("Switch");
    sw->add_css_class("flat");
    sw->add_css_class("suggested-action");
    sw->set_tooltip_text("Change the computer's clock to " + city + ".");

    auto* act = Gtk::make_managed<Gtk::Button>();
    act->add_css_class("flat");

    hbox->append(*lbl);
    hbox->append(*sw);
    hbox->append(*act);
    row->set_child(*hbox);

    out_action = act;
    out_switch = sw;
    return row;
}

void MainWindow::refresh_favorites() {
    while (auto* c = fav_list_.get_first_child()) fav_list_.remove(*c);
    const auto favs = db_->favorites();

    if (favs.empty()) {
        auto* row = Gtk::make_managed<Gtk::ListBoxRow>();
        auto* lbl = Gtk::make_managed<Gtk::Label>(
            "No pinned timezones yet. Star one in All Timezones.");
        lbl->set_xalign(0.0f);
        lbl->set_wrap(true);
        lbl->add_css_class("dim-label");
        pad(*lbl, 8);
        row->set_child(*lbl);
        row->set_selectable(false);
        row->set_activatable(false);
        fav_list_.append(*row);
        return;
    }

    for (const auto& tz : favs) {
        Gtk::Button* act = nullptr;
        Gtk::Button* sw  = nullptr;
        auto* row = make_zone_row(tz, act, sw);
        sw->signal_clicked().connect([this, tz] { apply_tz(tz); });
        act->set_label("\xc3\x97");
        act->set_tooltip_text("Unpin this timezone.");
        act->signal_clicked().connect([this, tz] {
            db_->remove_favorite(tz);
            refresh_favorites();
            build_all_list();
            set_status("Unpinned " + friendly_zone(tz) + ".");
        });
        fav_list_.append(*row);
    }
}

void MainWindow::build_all_list() {
    while (auto* c = all_list_.get_first_child()) all_list_.remove(*c);
    all_row_haystack_.clear();
    all_row_zone_.clear();

    // Alias rows first — they put city/state names (Memphis, Tennessee, etc.)
    // at the top of the list, above the IANA zones. These share a target IANA
    // zone, so Switch applies and the Pin star mirrors the underlying zone.
    for (const auto& al : svc_->aliases()) {
        auto* row  = Gtk::make_managed<Gtk::ListBoxRow>();
        auto* hbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
        hbox->set_margin_top(1);
        hbox->set_margin_bottom(1);
        hbox->set_margin_start(6);
        hbox->set_margin_end(4);

        auto* lbl = Gtk::make_managed<Gtk::Label>();
        lbl->set_markup(
            "<b>" + Glib::Markup::escape_text(al.display) + "</b>"
            "  <span alpha='55%'>\xc2\xb7 " +
            Glib::Markup::escape_text(al.zone) + "</span>");
        lbl->set_xalign(0.0f);
        lbl->set_hexpand(true);
        lbl->set_ellipsize(Pango::EllipsizeMode::END);

        auto* sw = Gtk::make_managed<Gtk::Button>("Switch");
        sw->add_css_class("flat");
        sw->add_css_class("suggested-action");
        sw->set_tooltip_text("Change the computer's clock to " + al.zone + ".");
        const std::string z = al.zone;
        sw->signal_clicked().connect([this, z] { apply_tz(z); });

        const bool fav = db_->is_favorite(al.zone);
        auto* act = Gtk::make_managed<Gtk::Button>(
            fav ? "\xe2\x98\x85" : "\xe2\x98\x86");
        act->add_css_class("flat");
        act->set_tooltip_text(
            fav ? "Unpin the underlying zone (" + al.zone + ")."
                : "Pin the underlying zone (" + al.zone + ").");
        act->signal_clicked().connect([this, z, fav] {
            if (fav) db_->remove_favorite(z);
            else     db_->add_favorite(z);
            refresh_favorites();
            build_all_list();
            set_status(std::string(fav ? "Unpinned " : "Pinned ") +
                       friendly_zone(z) + ".");
        });

        hbox->append(*lbl);
        hbox->append(*sw);
        hbox->append(*act);
        row->set_child(*hbox);
        all_list_.append(*row);

        all_row_haystack_.push_back(
            normalize_search(al.display + " " + al.zone));
        all_row_zone_.push_back(al.zone);
    }

    // IANA zones — the canonical list.
    for (const auto& tz : svc_->all_zones()) {
        Gtk::Button* act = nullptr;
        Gtk::Button* sw  = nullptr;
        auto* row = make_zone_row(tz, act, sw);
        sw->signal_clicked().connect([this, tz] { apply_tz(tz); });
        const bool fav = db_->is_favorite(tz);
        act->set_label(fav ? "\xe2\x98\x85" : "\xe2\x98\x86");
        act->set_tooltip_text(fav ? "Unpin this timezone." : "Pin this timezone.");
        act->signal_clicked().connect([this, tz, fav] {
            if (fav) db_->remove_favorite(tz);
            else     db_->add_favorite(tz);
            refresh_favorites();
            build_all_list();
            set_status(std::string(fav ? "Unpinned " : "Pinned ") +
                       friendly_zone(tz) + ".");
        });
        all_list_.append(*row);
        all_row_haystack_.push_back(normalize_search(tz));
        all_row_zone_.push_back(tz);
    }
    all_list_.invalidate_filter();
}

void MainWindow::refresh_category_filter() {
    // Rebuild the dropdown model: first item is "All", then the DB categories.
    const std::string keep = active_category_;
    suspending_cat_signal_ = true;
    cat_model_->splice(0, cat_model_->get_n_items(), {});
    cat_model_->append("All categories");
    guint restore = 0;
    const auto cats = db_->categories();
    for (std::size_t i = 0; i < cats.size(); ++i) {
        cat_model_->append(cats[i]);
        if (!keep.empty() && cats[i] == keep) restore = static_cast<guint>(i + 1);
    }
    cat_filter_.set_selected(restore);
    suspending_cat_signal_ = false;
}

void MainWindow::refresh_active_banner() {
    if (active_location_id_ == 0) {
        active_lbl_.set_markup(
            "<b>Pick a place on the left</b>  "
            "<span alpha='65%'>"
            "to see its notes and snippets. "
            "Or click \"+ New snippet\" to save a general one."
            "</span>");
        clear_active_btn_.set_visible(false);
        switch_active_btn_.set_visible(false);
        notes_title_.set_text("Notes");
    } else {
        Location l = db_->location_by_id(active_location_id_);
        std::string city = friendly_zone(l.zone);
        active_lbl_.set_markup(
            "Viewing: <b>" + Glib::Markup::escape_text(l.name) + "</b> "
            "<span alpha='60%'>\xc2\xb7 " + Glib::Markup::escape_text(city) + "</span>");
        switch_active_btn_.set_visible(true);
        clear_active_btn_.set_visible(true);
        clear_active_btn_.set_sensitive(true);
        notes_title_.set_text("Notes");
    }
}

void MainWindow::refresh_notes() {
    auto buf = notes_view_.get_buffer();
    if (active_location_id_ == 0) {
        buf->set_text("");
        notes_view_.set_editable(false);
        notes_view_.set_sensitive(false);
    } else {
        Location l = db_->location_by_id(active_location_id_);
        buf->set_text(l.notes);
        notes_view_.set_editable(true);
        notes_view_.set_sensitive(true);
    }
    notes_dirty_ = false;
}

void MainWindow::refresh_snippets() {
    while (auto* c = sn_list_.get_first_child()) sn_list_.remove(*c);

    std::vector<Snippet> rows;
    if (active_location_id_ > 0) {
        rows = db_->snippets(active_location_id_, active_category_);
    } else {
        rows = db_->snippets_untagged(active_category_);
    }

    if (rows.empty()) {
        auto* row = Gtk::make_managed<Gtk::ListBoxRow>();
        auto* lbl = Gtk::make_managed<Gtk::Label>(
            active_location_id_ > 0
                ? "No snippets for this place yet.\n\n"
                  "Click \"+ New snippet\" above to save something you copy often "
                  "(a meeting link, a short signature, a command, anything)."
                : "No general snippets yet.\n\n"
                  "Click \"+ New snippet\" above to save something you copy often. "
                  "Snippets you create while viewing a place get linked to that place.");
        lbl->set_xalign(0.0f);
        lbl->set_wrap(true);
        lbl->add_css_class("dim-label");
        pad(*lbl, 10);
        row->set_child(*lbl);
        row->set_selectable(false);
        sn_list_.append(*row);
        return;
    }

    for (const auto& sn : rows) {
        auto* row  = Gtk::make_managed<Gtk::ListBoxRow>();
        auto* hbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        pad(*hbox, 4);

        auto* textbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
        textbox->set_hexpand(true);

        auto* lbl = Gtk::make_managed<Gtk::Label>();
        lbl->set_markup("<b>" + Glib::Markup::escape_text(sn.label) + "</b>  "
                        "<span alpha='60%'>[" +
                        Glib::Markup::escape_text(sn.category) + "]</span>");
        lbl->set_xalign(0.0f);

        auto* preview = Gtk::make_managed<Gtk::Label>();
        Glib::ustring up(sn.content);
        std::string p;
        if (up.length() > 80) p = Glib::ustring(up.substr(0, 77) + "...").raw();
        else                  p = sn.content;
        for (auto& ch : p) if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
        preview->set_text(p);
        preview->set_xalign(0.0f);
        preview->add_css_class("dim-label");

        textbox->append(*lbl);
        textbox->append(*preview);

        auto* copy_btn = Gtk::make_managed<Gtk::Button>("Copy");
        copy_btn->add_css_class("suggested-action");
        const std::string content = sn.content;
        const std::string name    = sn.label;
        copy_btn->signal_clicked().connect(
            [this, content, name] { copy_to_clipboard(content, name); });

        auto* edit_btn = Gtk::make_managed<Gtk::Button>("Edit");
        const std::int64_t sid = sn.id;
        edit_btn->signal_clicked().connect(
            [this, sid] { open_snippet_dialog(sid); });

        auto* rm_btn = Gtk::make_managed<Gtk::Button>("\xc3\x97");
        rm_btn->set_tooltip_text("Delete this snippet");
        rm_btn->signal_clicked().connect([this, sid, name] {
            db_->remove_snippet(sid);
            refresh_snippets();
            set_status("Deleted snippet \"" + name + "\".");
        });

        hbox->append(*textbox);
        hbox->append(*copy_btn);
        hbox->append(*edit_btn);
        hbox->append(*rm_btn);
        row->set_child(*hbox);
        sn_list_.append(*row);
    }
}

// =============================================================
// Actions
// =============================================================

void MainWindow::apply_tz(const std::string& tz) {
    std::string city = friendly_zone(tz);
    set_status("Switching to " + city +
               "... You may be asked for your admin password.");
    while (Glib::MainContext::get_default()->iteration(false)) {}
    std::string err;
    const bool ok = svc_->apply(tz, err);
    ::tzset();
    if (ok) {
        refresh_current_label();
        update_home_state();
        tick_clock();
        set_status("Done. Your computer's clock is now on " + city + ".");
    } else {
        set_status("Couldn't change the timezone: " + err);
    }
}

void MainWindow::set_active_location(std::int64_t id, bool also_apply) {
    flush_notes_if_dirty();
    Location l = db_->location_by_id(id);
    if (l.id == 0) return;
    active_location_id_ = id;
    refresh_active_banner();
    refresh_notes();
    refresh_snippets();
    refresh_locations();  // bullet moves
    if (also_apply) apply_tz(l.zone);
    else            set_status("Viewing notes and snippets for " + l.name + ".");
}

void MainWindow::clear_active_location() {
    flush_notes_if_dirty();
    active_location_id_ = 0;
    refresh_active_banner();
    refresh_notes();
    refresh_snippets();
    refresh_locations();
    set_status("Closed. Pick another place anytime.");
}

void MainWindow::flush_notes_if_dirty() {
    if (!notes_dirty_ || active_location_id_ == 0) return;
    auto buf  = notes_view_.get_buffer();
    std::string t = buf->get_text();
    if (t.size() > Db::MAX_NOTES) t = t.substr(0, Db::MAX_NOTES);
    try {
        db_->update_location_notes(active_location_id_, t);
        notes_dirty_ = false;
    } catch (const std::exception& e) {
        set_status(std::string("Notes save failed: ") + e.what());
    }
}

void MainWindow::set_status(const std::string& msg) {
    status_.set_text(msg);
}

void MainWindow::copy_to_clipboard(const std::string& text, const std::string& label) {
    auto display = get_display();
    if (!display) { set_status("Couldn't reach the clipboard."); return; }
    auto clip = display->get_clipboard();
    clip->set_text(text);
    set_status("Copied \"" + label + "\". Paste it anywhere with Ctrl+V.");
}

// =============================================================
// Settings tab
// =============================================================

void MainWindow::build_settings_tab() {
    pad(settings_tab_, 12);

    auto make_heading = [](const std::string& s) {
        auto* lbl = Gtk::make_managed<Gtk::Label>();
        lbl->set_markup("<b>" + Glib::Markup::escape_text(s) + "</b>");
        lbl->set_xalign(0.0f);
        lbl->add_css_class("heading");
        return lbl;
    };
    auto make_hint = [](const std::string& s) {
        auto* lbl = Gtk::make_managed<Gtk::Label>(s);
        lbl->set_xalign(0.0f);
        lbl->set_wrap(true);
        lbl->add_css_class("dim-label");
        return lbl;
    };

    // -- Home zone group --
    settings_tab_.append(*make_heading("Home timezone"));
    settings_tab_.append(*make_hint(
        "\"Home\" is the zone you want to return to. Go Home in the top bar "
        "snaps your computer's clock back here."));

    settings_home_lbl_.set_xalign(0.0f);
    settings_home_lbl_.set_wrap(true);
    settings_home_lbl_.add_css_class("title-4");
    settings_tab_.append(settings_home_lbl_);

    auto* home_btns = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    set_home_btn_.set_label("Use current zone as Home");
    set_home_btn_.add_css_class("suggested-action");
    clear_home_btn_.set_label("Clear Home");
    home_btns->append(set_home_btn_);
    home_btns->append(clear_home_btn_);
    settings_tab_.append(*home_btns);

    auto* sep1 = Gtk::make_managed<Gtk::Separator>();
    sep1->set_margin_top(6);
    sep1->set_margin_bottom(6);
    settings_tab_.append(*sep1);

    // -- Clock group --
    settings_tab_.append(*make_heading("Clock"));
    settings_24h_chk_.signal_toggled().connect([this] {
        db_->set_setting("clock_24h", settings_24h_chk_.get_active() ? "1" : "0");
        tick_clock();
    });
    settings_seconds_chk_.signal_toggled().connect([this] {
        db_->set_setting("clock_seconds", settings_seconds_chk_.get_active() ? "1" : "0");
        tick_clock();
    });
    settings_tab_.append(settings_24h_chk_);
    settings_tab_.append(settings_seconds_chk_);

    auto* sep2 = Gtk::make_managed<Gtk::Separator>();
    sep2->set_margin_top(6);
    sep2->set_margin_bottom(6);
    settings_tab_.append(*sep2);

    // -- System tools --
    settings_tab_.append(*make_heading("System"));
    auto* sys_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    sys_row->append(refresh_btn_);
    auto* reimport = Gtk::make_managed<Gtk::Button>("Re-import legacy data");
    reimport->set_tooltip_text(
        "Pull any saved zones/favorites from the old time-zone-changer config.");
    reimport->signal_clicked().connect([this] {
        db_->delete_setting("legacy_imported");
        svc_->import_legacy_config();
        refresh_all();
        set_status("Legacy data re-imported.");
    });
    sys_row->append(*reimport);
    settings_tab_.append(*sys_row);

    auto* sep3 = Gtk::make_managed<Gtk::Separator>();
    sep3->set_margin_top(6);
    sep3->set_margin_bottom(6);
    settings_tab_.append(*sep3);

    // -- About --
    auto* about = Gtk::make_managed<Gtk::Label>();
    about->set_markup(
        "<b>TZ Workspace</b>  <span alpha='65%'>\xc2\xb7 local build</span>\n"
        "<span alpha='70%'>Data: ~/.local/share/tz-workspace/data.db</span>");
    about->set_xalign(0.0f);
    about->set_wrap(true);
    settings_tab_.append(*about);
}

void MainWindow::refresh_settings_tab() {
    const std::string h = db_->get_setting("home_zone");
    if (h.empty()) {
        settings_home_lbl_.set_markup("<span alpha='65%'>(none set)</span>");
        clear_home_btn_.set_sensitive(false);
    } else {
        settings_home_lbl_.set_markup(
            Glib::Markup::escape_text(friendly_zone(h)) +
            "  <span alpha='60%'>" + Glib::Markup::escape_text(h) + "</span>");
        clear_home_btn_.set_sensitive(true);
    }
    // Avoid firing toggled signal back into ourselves while syncing.
    settings_24h_chk_.set_active(db_->get_setting("clock_24h")     == "1");
    settings_seconds_chk_.set_active(db_->get_setting("clock_seconds") == "1");
}

// =============================================================
// Dialogs
// =============================================================

void MainWindow::open_add_location_dialog() {
    const auto& zones = svc_->all_zones();
    if (zones.empty()) {
        set_status("Cannot add a location: timezone list is empty.");
        return;
    }

    auto* win = new Gtk::Window();
    win->set_title("Add Location");
    win->set_transient_for(*this);
    win->set_modal(true);
    win->set_default_size(440, -1);

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    pad(*box, 12);

    auto* name_lbl = Gtk::make_managed<Gtk::Label>(
        "Name (e.g. \"New Rochelle\", \"Office\"):");
    name_lbl->set_xalign(0.0f);
    auto* name_entry = Gtk::make_managed<Gtk::Entry>();
    name_entry->set_max_length(static_cast<int>(Db::MAX_LOCATION_NAME));

    auto* zone_lbl = Gtk::make_managed<Gtk::Label>("Timezone:");
    zone_lbl->set_xalign(0.0f);

    auto model = Gtk::StringList::create({});
    for (const auto& z : zones) model->append(z);
    auto* dropdown = Gtk::make_managed<Gtk::DropDown>();
    dropdown->set_model(model);
    dropdown->set_enable_search(true);
    dropdown->set_hexpand(true);

    const std::string cur = svc_->current_zone();
    auto cur_it = std::find(zones.begin(), zones.end(), cur);
    if (cur_it != zones.end()) {
        dropdown->set_selected(
            static_cast<guint>(std::distance(zones.begin(), cur_it)));
    }

    auto* err_lbl = Gtk::make_managed<Gtk::Label>("");
    err_lbl->set_xalign(0.0f);
    err_lbl->add_css_class("error");

    auto* btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    btn_row->set_halign(Gtk::Align::END);
    auto* cancel_btn = Gtk::make_managed<Gtk::Button>("Cancel");
    auto* save_btn   = Gtk::make_managed<Gtk::Button>("Save");
    save_btn->add_css_class("suggested-action");
    btn_row->append(*cancel_btn);
    btn_row->append(*save_btn);

    box->append(*name_lbl);
    box->append(*name_entry);
    box->append(*zone_lbl);
    box->append(*dropdown);
    box->append(*err_lbl);
    box->append(*btn_row);
    win->set_child(*box);

    cancel_btn->signal_clicked().connect([win] { win->close(); });

    save_btn->signal_clicked().connect(
        [this, win, name_entry, dropdown, err_lbl] {
            const std::string name = name_entry->get_text();
            if (!TzService::is_display_name_safe(name, Db::MAX_LOCATION_NAME)) {
                err_lbl->set_text("Name is empty or contains invalid characters.");
                return;
            }
            const auto& zs = svc_->all_zones();
            guint sel = dropdown->get_selected();
            if (sel == GTK_INVALID_LIST_POSITION || sel >= zs.size()) {
                err_lbl->set_text("Please pick a timezone.");
                return;
            }
            const std::string zone = zs[sel];
            try {
                db_->add_location(name, zone);
            } catch (const std::exception& e) {
                err_lbl->set_text(std::string("Error: ") + e.what());
                return;
            }
            refresh_all();
            set_status("Added location \"" + name + "\" -> " + zone + ".");
            win->close();
        });

    win->signal_close_request().connect([win] {
        win->set_visible(false);
        delete win;
        return true;
    }, false);

    name_entry->grab_focus();
    win->present();
}

void MainWindow::open_snippet_dialog(std::int64_t edit_id) {
    Snippet existing;
    if (edit_id > 0) existing = db_->snippet_by_id(edit_id);

    auto* win = new Gtk::Window();
    win->set_title(edit_id ? "Edit Snippet" : "New Snippet");
    win->set_transient_for(*this);
    win->set_modal(true);
    win->set_default_size(560, 480);

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    pad(*box, 12);

    auto* lbl_label = Gtk::make_managed<Gtk::Label>("Label:");
    lbl_label->set_xalign(0.0f);
    auto* label_entry = Gtk::make_managed<Gtk::Entry>();
    label_entry->set_max_length(static_cast<int>(Db::MAX_LABEL));
    if (edit_id) label_entry->set_text(existing.label);

    auto* content_label = Gtk::make_managed<Gtk::Label>("Content:");
    content_label->set_xalign(0.0f);
    auto* content_scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    content_scroll->set_has_frame(true);
    content_scroll->set_vexpand(true);
    auto* content_view = Gtk::make_managed<Gtk::TextView>();
    content_view->set_wrap_mode(Gtk::WrapMode::WORD_CHAR);
    content_view->set_top_margin(6);
    content_view->set_bottom_margin(6);
    content_view->set_left_margin(6);
    content_view->set_right_margin(6);
    if (edit_id) content_view->get_buffer()->set_text(existing.content);
    content_scroll->set_child(*content_view);

    // Category row (dropdown + add-new button)
    auto* cat_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto* cat_label = Gtk::make_managed<Gtk::Label>("Category:");
    auto cats = db_->categories();
    auto cat_sl = Gtk::StringList::create({});
    for (const auto& c : cats) cat_sl->append(c);
    auto* cat_dd = Gtk::make_managed<Gtk::DropDown>();
    cat_dd->set_model(cat_sl);
    cat_dd->set_enable_search(true);
    cat_dd->set_hexpand(true);
    guint cat_sel = 0;
    const std::string want_cat = edit_id ? existing.category : std::string(Db::DEFAULT_CATEGORY);
    for (std::size_t i = 0; i < cats.size(); ++i) {
        if (cats[i] == want_cat) { cat_sel = static_cast<guint>(i); break; }
    }
    cat_dd->set_selected(cat_sel);
    auto* new_cat_btn = Gtk::make_managed<Gtk::Button>("+ New");
    new_cat_btn->set_tooltip_text("Create a new category (saved when you save the snippet).");
    cat_row->append(*cat_label);
    cat_row->append(*cat_dd);
    cat_row->append(*new_cat_btn);

    new_cat_btn->signal_clicked().connect([this, win, cat_dd, cat_sl] {
        auto* dlg = new Gtk::Window();
        dlg->set_title("New Category");
        dlg->set_transient_for(*win);
        dlg->set_modal(true);
        dlg->set_default_size(300, -1);
        auto* b = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
        pad(*b, 12);
        auto* e = Gtk::make_managed<Gtk::Entry>();
        e->set_max_length(static_cast<int>(Db::MAX_CATEGORY));
        e->set_placeholder_text("Category name");
        auto* err = Gtk::make_managed<Gtk::Label>();
        err->add_css_class("error");
        err->set_xalign(0.0f);
        auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        row->set_halign(Gtk::Align::END);
        auto* cancel = Gtk::make_managed<Gtk::Button>("Cancel");
        auto* ok = Gtk::make_managed<Gtk::Button>("Add");
        ok->add_css_class("suggested-action");
        row->append(*cancel);
        row->append(*ok);
        b->append(*e);
        b->append(*err);
        b->append(*row);
        dlg->set_child(*b);
        cancel->signal_clicked().connect([dlg] { dlg->close(); });
        ok->signal_clicked().connect([this, dlg, e, err, cat_dd, cat_sl] {
            const std::string nm = e->get_text();
            if (!TzService::is_display_name_safe(nm, Db::MAX_CATEGORY)) {
                err->set_text("Invalid name."); return;
            }
            try {
                db_->add_category(nm);
            } catch (const std::exception& ex) {
                err->set_text(std::string("Error: ") + ex.what()); return;
            }
            cat_sl->append(nm);
            cat_dd->set_selected(cat_sl->get_n_items() - 1);
            dlg->close();
        });
        dlg->signal_close_request().connect([dlg] {
            dlg->set_visible(false); delete dlg; return true;
        }, false);
        e->grab_focus();
        dlg->present();
    });

    // Locations multi-select (checkboxes)
    auto* tag_label = Gtk::make_managed<Gtk::Label>(
        "Tag locations (leave empty to make this snippet untagged / \"global\"):");
    tag_label->set_xalign(0.0f);
    tag_label->set_wrap(true);

    auto* tag_scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    tag_scroll->set_has_frame(true);
    tag_scroll->set_min_content_height(110);
    auto* tag_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    pad(*tag_box, 4);
    tag_scroll->set_child(*tag_box);

    auto locations_all = db_->locations();
    // Map location_id -> checkbox* so we can collect on save.
    auto check_map = std::make_shared<std::vector<std::pair<std::int64_t, Gtk::CheckButton*>>>();
    for (const auto& l : locations_all) {
        auto* cb = Gtk::make_managed<Gtk::CheckButton>(l.name + "  (" + l.zone + ")");
        bool on = std::find(existing.location_ids.begin(),
                            existing.location_ids.end(),
                            l.id) != existing.location_ids.end();
        if (!edit_id && active_location_id_ == l.id) on = true;  // pre-tag active loc
        cb->set_active(on);
        tag_box->append(*cb);
        check_map->push_back({l.id, cb});
    }
    if (locations_all.empty()) {
        auto* none = Gtk::make_managed<Gtk::Label>(
            "(No locations yet. Add one in the Locations tab to tag it.)");
        none->add_css_class("dim-label");
        none->set_xalign(0.0f);
        tag_box->append(*none);
    }

    auto* err_lbl = Gtk::make_managed<Gtk::Label>("");
    err_lbl->set_xalign(0.0f);
    err_lbl->add_css_class("error");

    auto* btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    btn_row->set_halign(Gtk::Align::END);
    auto* cancel_btn = Gtk::make_managed<Gtk::Button>("Cancel");
    auto* save_btn   = Gtk::make_managed<Gtk::Button>(edit_id ? "Save" : "Add");
    save_btn->add_css_class("suggested-action");
    btn_row->append(*cancel_btn);
    btn_row->append(*save_btn);

    box->append(*lbl_label);
    box->append(*label_entry);
    box->append(*content_label);
    box->append(*content_scroll);
    box->append(*cat_row);
    box->append(*tag_label);
    box->append(*tag_scroll);
    box->append(*err_lbl);
    box->append(*btn_row);
    win->set_child(*box);

    cancel_btn->signal_clicked().connect([win] { win->close(); });

    save_btn->signal_clicked().connect(
        [this, win, edit_id, label_entry, content_view, cat_dd, cat_sl,
         check_map, err_lbl] {
            const std::string label   = label_entry->get_text();
            const std::string content = content_view->get_buffer()->get_text();

            if (label.empty() || label.size() > Db::MAX_LABEL) {
                err_lbl->set_text("Label must be 1-" +
                                  std::to_string(Db::MAX_LABEL) + " characters.");
                return;
            }
            if (content.size() > Db::MAX_CONTENT) {
                err_lbl->set_text("Content is too long (" +
                                  std::to_string(content.size()) + " > " +
                                  std::to_string(Db::MAX_CONTENT) + ").");
                return;
            }
            guint cs = cat_dd->get_selected();
            std::string category = Db::DEFAULT_CATEGORY;
            if (cs != GTK_INVALID_LIST_POSITION && cs < cat_sl->get_n_items()) {
                category = cat_sl->get_string(cs);
            }
            std::vector<std::int64_t> tags;
            for (auto& [lid, cb] : *check_map) if (cb->get_active()) tags.push_back(lid);
            try {
                if (edit_id) {
                    db_->update_snippet(edit_id, label, content, category, tags);
                } else {
                    db_->add_snippet(label, content, category, tags);
                }
            } catch (const std::exception& e) {
                err_lbl->set_text(std::string("Error: ") + e.what());
                return;
            }
            refresh_category_filter();
            refresh_snippets();
            set_status(edit_id ? ("Updated snippet \"" + label + "\".")
                               : ("Added snippet \"" + label + "\"."));
            win->close();
        });

    win->signal_close_request().connect([win] {
        win->set_visible(false);
        delete win;
        return true;
    }, false);

    label_entry->grab_focus();
    win->present();
}

void MainWindow::open_manage_categories_dialog() {
    auto* win = new Gtk::Window();
    win->set_title("Manage Categories");
    win->set_transient_for(*this);
    win->set_modal(true);
    win->set_default_size(380, 400);

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    pad(*box, 12);

    auto* scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_has_frame(true);
    scroll->set_vexpand(true);
    auto* list = Gtk::make_managed<Gtk::ListBox>();
    list->set_selection_mode(Gtk::SelectionMode::NONE);
    list->add_css_class("bordered-rows");
    scroll->set_child(*list);

    auto rebuild_list = std::make_shared<std::function<void()>>();
    *rebuild_list = [this, list, rebuild_list] {
        while (auto* c = list->get_first_child()) list->remove(*c);
        for (const auto& c : db_->categories()) {
            auto* row  = Gtk::make_managed<Gtk::ListBoxRow>();
            auto* hbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
            pad(*hbox, 4);
            auto* lbl = Gtk::make_managed<Gtk::Label>(c);
            lbl->set_xalign(0.0f);
            lbl->set_hexpand(true);
            hbox->append(*lbl);
            const bool is_def = (c == Db::DEFAULT_CATEGORY);
            if (!is_def) {
                auto* rm = Gtk::make_managed<Gtk::Button>("\xc3\x97");
                rm->set_tooltip_text("Delete (snippets fall back to General)");
                const std::string nm = c;
                rm->signal_clicked().connect([this, nm, rebuild_list] {
                    db_->remove_category(nm);
                    refresh_category_filter();
                    refresh_snippets();
                    (*rebuild_list)();
                });
                hbox->append(*rm);
            } else {
                auto* tag = Gtk::make_managed<Gtk::Label>("(default)");
                tag->add_css_class("dim-label");
                hbox->append(*tag);
            }
            row->set_child(*hbox);
            list->append(*row);
        }
    };
    (*rebuild_list)();

    auto* add_row   = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto* new_entry = Gtk::make_managed<Gtk::Entry>();
    new_entry->set_max_length(static_cast<int>(Db::MAX_CATEGORY));
    new_entry->set_placeholder_text("New category name");
    new_entry->set_hexpand(true);
    auto* add_btn = Gtk::make_managed<Gtk::Button>("Add");
    auto* err     = Gtk::make_managed<Gtk::Label>();
    err->add_css_class("error");
    err->set_xalign(0.0f);
    add_row->append(*new_entry);
    add_row->append(*add_btn);

    add_btn->signal_clicked().connect([this, new_entry, err, rebuild_list] {
        const std::string nm = new_entry->get_text();
        if (!TzService::is_display_name_safe(nm, Db::MAX_CATEGORY)) {
            err->set_text("Invalid name."); return;
        }
        try { db_->add_category(nm); }
        catch (const std::exception& e) {
            err->set_text(std::string("Error: ") + e.what()); return;
        }
        new_entry->set_text("");
        err->set_text("");
        refresh_category_filter();
        (*rebuild_list)();
    });

    auto* close_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    close_row->set_halign(Gtk::Align::END);
    auto* close_btn = Gtk::make_managed<Gtk::Button>("Close");
    close_btn->signal_clicked().connect([win] { win->close(); });
    close_row->append(*close_btn);

    box->append(*scroll);
    box->append(*add_row);
    box->append(*err);
    box->append(*close_row);
    win->set_child(*box);

    win->signal_close_request().connect([win] {
        win->set_visible(false);
        delete win;
        return true;
    }, false);

    win->present();
}
