#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>

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

int main(int argc, char* argv[]) {
    if (int rc = validate_env(); rc != 0) return rc;
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
