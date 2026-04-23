#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtkmm/application.h>
#include "MainWindow.hpp"

static int validate_env() {
    const char* xdg  = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    const bool xdg_ok  = (xdg  && *xdg  && std::filesystem::path(xdg).is_absolute());
    const bool home_ok = (home && *home && std::filesystem::path(home).is_absolute());
    if (!xdg_ok && !home_ok) {
        std::fprintf(stderr,
            "tz-workspace: HOME/XDG_DATA_HOME not set to an absolute path; refusing to start.\n");
        return 1;
    }
    return 0;
}

// Read <data_dir>/renderer.conf and push GSK_RENDERER into the environment
// BEFORE Gtk::Application::create — GTK inspects that env var at first init,
// so we can't change it later. File is a single line: "sw" forces Cairo
// (software, lighter memory, no Vulkan/GL libs loaded), any other value or
// a missing file leaves GTK to pick its default. Allow-listed values only;
// we never pass arbitrary strings to GTK.
static void apply_renderer_pref() {
    namespace fs = std::filesystem;
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
    const fs::path cfg = base / "tz-workspace" / "renderer.conf";
    std::ifstream f(cfg);
    if (!f) return;
    std::string pref;
    std::getline(f, pref);
    while (!pref.empty() &&
           (pref.back() == '\n' || pref.back() == '\r' ||
            pref.back() == ' '  || pref.back() == '\t')) {
        pref.pop_back();
    }
    if (pref == "sw" || pref == "cairo") {
        ::setenv("GSK_RENDERER", "cairo", 1);
    } else if (pref == "gl") {
        ::setenv("GSK_RENDERER", "gl", 1);
    } else if (pref == "vulkan") {
        ::setenv("GSK_RENDERER", "vulkan", 1);
    }
    // Anything else (including empty): leave env untouched, GTK auto-picks.
}

int main(int argc, char* argv[]) {
    if (int rc = validate_env(); rc != 0) return rc;
    apply_renderer_pref();
    try {
        auto app = Gtk::Application::create("local.tzworkspace.app");
        return app->make_window_and_run<MainWindow>(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "tz-workspace failed to start: %s\n", e.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "tz-workspace failed to start: unknown error\n");
        return 1;
    }
}
