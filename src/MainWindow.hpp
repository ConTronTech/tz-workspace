#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include <gtkmm.h>
#include <sigc++/connection.h>

#include "Db.hpp"
#include "TzService.hpp"

class MainWindow : public Gtk::ApplicationWindow {
public:
    MainWindow();
    ~MainWindow() override;

private:
    std::unique_ptr<Db>         db_;
    std::unique_ptr<TzService>  svc_;

    // ----- State -----
    std::int64_t                active_location_id_ = 0;  // 0 = none
    std::string                 active_category_   = "";  // "" = all
    bool                        notes_dirty_       = false;
    bool                        suspending_cat_signal_ = false;
    std::vector<std::int64_t>   loc_row_ids_;             // parallel to loc_list_ rows
    // Parallel to all_list_ rows. Each string is the normalized search text
    // (display + zone), tokenized whitespace-separated, used by the fuzzy
    // filter below so alias rows and plain zone rows share one code path.
    std::vector<std::string>    all_row_haystack_;
    // For each all_list_ row, the IANA zone to apply when "Switch" is clicked
    // (equals the row's own zone path for IANA rows, or the alias's target).
    std::vector<std::string>    all_row_zone_;

    // ----- Layout -----
    Gtk::Box     root_;

    // Compact top bar: current zone + clock on left, home zone + clock on the
    // right, then Go Home + ⚙. Two clocks (current system zone + saved home
    // zone) ticking side-by-side so you can't miss the delta when travelling.
    Gtk::Box     topbar_;
    Gtk::Box     current_box_;
    Gtk::Label   current_lbl_;
    Gtk::Label   clock_lbl_;
    Gtk::Box     home_box_;
    Gtk::Label   home_lbl_;
    Gtk::Label   home_clock_lbl_;
    Gtk::Button  revert_btn_;

    // Kept for backward references; populated inside Settings tab now.
    Gtk::Button  set_home_btn_;
    Gtk::Button  clear_home_btn_;
    Gtk::Button  refresh_btn_;

    // Main paned
    Gtk::Paned     paned_;

    // --- Left pane: stack driven by the top-bar switcher ---
    Gtk::Stack          left_stack_;
    Gtk::StackSwitcher  left_switcher_;

    // Tab: Locations
    Gtk::Box            loc_tab_;
    Gtk::Box            loc_header_;
    Gtk::Button         add_loc_btn_;
    Gtk::ScrolledWindow loc_scroll_;
    Gtk::ListBox        loc_list_;

    // Tab: Favorites (compact single-line rows)
    Gtk::Box            fav_tab_;
    Gtk::ScrolledWindow fav_scroll_;
    Gtk::ListBox        fav_list_;

    // Tab: All zones (compact single-line rows + search)
    Gtk::Box            all_tab_;
    Gtk::SearchEntry    search_entry_;
    Gtk::ScrolledWindow all_scroll_;
    Gtk::ListBox        all_list_;

    // Tab: Settings
    Gtk::Box            settings_tab_;
    Gtk::Label          settings_home_lbl_;
    Gtk::CheckButton    settings_24h_chk_;
    Gtk::CheckButton    settings_seconds_chk_;
    Gtk::CheckButton    settings_sw_render_chk_;

    // --- Right pane: Workspace ---
    Gtk::Box       active_row_;
    Gtk::Label     active_lbl_;
    Gtk::Button    switch_active_btn_;
    Gtk::Button    clear_active_btn_;

    Gtk::Label          notes_title_;
    Gtk::ScrolledWindow notes_scroll_;
    Gtk::TextView       notes_view_;

    Gtk::Paned          ws_paned_;

    Gtk::Box            sn_header_;
    Gtk::Label          sn_title_;
    Gtk::DropDown       cat_filter_;
    Gtk::Button         add_snippet_btn_;
    Gtk::Button         manage_cats_btn_;
    Glib::RefPtr<Gtk::StringList> cat_model_;

    Gtk::ScrolledWindow sn_scroll_;
    Gtk::ListBox        sn_list_;

    Gtk::Label          status_;
    sigc::connection    clock_conn_;
    sigc::connection    notes_focus_conn_;
    sigc::connection    scale_conn_;
    Glib::RefPtr<Gtk::CssProvider> scale_css_;
    int                 last_scale_h_ = 0;

    // ----- Build / refresh -----
    void build_ui();
    void build_settings_tab();
    void refresh_all();
    void refresh_current_label();
    void update_home_state();
    void refresh_locations();
    void refresh_favorites();
    void build_all_list();
    void refresh_category_filter();
    void refresh_active_banner();
    void refresh_notes();
    void refresh_snippets();
    void refresh_settings_tab();

    // ----- Actions -----
    bool tick_clock();
    void apply_tz(const std::string& tz);
    void set_active_location(std::int64_t id, bool also_apply);
    void clear_active_location();
    void flush_notes_if_dirty();
    void set_status(const std::string& msg);

    // ----- Persistence helpers -----
    // Parse a DB setting as int with sanity bounds; fall back to `def` on miss
    // or malformed value. Keeps garbage from a hand-edited DB out of size_t
    // territory.
    int  setting_int(const std::string& key, int def) const;
    void save_window_state();

    // ----- Responsive scale -----
    // Recompute the scale CSS (font-size + paddings) from current window
    // height, and push it into scale_css_ so every widget rescales in place.
    void apply_scale();

    // ----- Dialogs -----
    void open_add_location_dialog();
    void open_snippet_dialog(std::int64_t edit_id);  // 0 = new
    void open_manage_categories_dialog();

    // ----- Clipboard -----
    void copy_to_clipboard(const std::string& text, const std::string& label);
};
