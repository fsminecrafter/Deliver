#pragma once
#include "config.hpp"
#include "local_db.hpp"
#include "types.hpp"
#include <string>
#include <vector>
#include <atomic>

#ifndef _WIN32
#  include <termios.h>
#endif

namespace dlr {

// Extended key codes (above ASCII range)
enum TuiKey : int {
    TK_UP     = 0x1001,
    TK_DOWN   = 0x1002,
    TK_LEFT   = 0x1003,
    TK_RIGHT  = 0x1004,
    TK_PGUP   = 0x1005,
    TK_PGDN   = 0x1006,
    TK_HOME   = 0x1007,
    TK_END    = 0x1008,
    TK_F1     = 0x1100,
    TK_ENTER  = 13,
    TK_ESC    = 27,
    TK_TAB    = 9,
    TK_BTAB   = 0x1009,   // Shift-Tab
    TK_BACKSP = 127,
    TK_CTRLC  = 3,
    TK_CTRLR  = 18,
    TK_CTRLD  = 4,
};

class TuiApp {
public:
    TuiApp(ClientConfig cli_cfg, ServerConfig srv_cfg);
    ~TuiApp();

    // Enter the interactive app.  Returns process exit code.
    int run();

    // Signal the app to stop (used from signal handler).
    static std::atomic<bool> g_resize_flag;

private:
    // ── Terminal management ────────────────────────────────────────────────────
    void   term_raw();
    void   term_restore();
    void   term_size(int& rows, int& cols);
    int    term_read();   // blocking read; returns TuiKey or char

    // ── Rendering ─────────────────────────────────────────────────────────────
    void   render();
    void   render_header();
    void   render_nav();
    void   render_content();
    void   render_footer();

    // ── Per-view renderers ─────────────────────────────────────────────────────
    enum class View { Packages, Servers, Repos, Installed, Log };

    void   view_packages  (int r, int c, int h, int w);
    void   view_servers   (int r, int c, int h, int w);
    void   view_repos     (int r, int c, int h, int w);
    void   view_installed (int r, int c, int h, int w);
    void   view_log       (int r, int c, int h, int w);

    // ── Low-level draw helpers ─────────────────────────────────────────────────
    // All emit into buf_ which is flushed once per frame.
    void   buf(const std::string& s);
    void   move(int r, int c);
    void   clrline();
    void   hline(int r, int c, int w, const char* ch = "─");
    void   box_top   (int r, int c, int w, const std::string& title = "");
    void   box_bottom(int r, int c, int w);
    void   draw_row  (int r, int c, int w, const std::string& s,
                      bool selected, bool installed);
    std::string trunc(const std::string& s, int w) const;

    // ANSI colour helpers
    static std::string A(const std::string& code, const std::string& s);
    static std::string bold  (const std::string& s);
    static std::string dim   (const std::string& s);
    static std::string rev   (const std::string& s);   // reverse video (selected)
    static std::string col_g (const std::string& s);   // green
    static std::string col_c (const std::string& s);   // cyan
    static std::string col_y (const std::string& s);   // yellow
    static std::string col_r (const std::string& s);   // red
    static std::string col_m (const std::string& s);   // magenta
    static std::string reset ();

    // ── Input handling ─────────────────────────────────────────────────────────
    void   handle_key(int k);
    void   handle_key_search(int k);
    void   handle_key_prompt(int k);

    // ── Actions ───────────────────────────────────────────────────────────────
    void   do_scan();
    void   do_install_selected();
    void   do_download_selected();
    void   do_remove_selected();
    void   do_ping_selected();
    void   do_add_repo();
    void   do_remove_repo();

    // ── Data management ───────────────────────────────────────────────────────
    void   load_data();
    void   rebuild_filtered();
    int    list_size() const;
    void   clamp_sel();

    // ── Logging / status ──────────────────────────────────────────────────────
    void   tlog(const std::string& msg);
    void   set_status(const std::string& msg);

    // ── State ─────────────────────────────────────────────────────────────────
    bool        running_    = true;
    bool        dirty_      = true;
    View        view_       = View::Packages;
    int         sel_        = 0;
    int         scroll_     = 0;
    int         rows_       = 24;
    int         cols_       = 80;
    int         nav_w_      = 14;   // sidebar width
    std::string status_;
    std::string buf_;               // render buffer

    // Search
    bool        searching_  = false;
    std::string search_str_;

    // Prompt (for add-repo)
    bool        prompting_  = false;
    std::string prompt_label_;
    std::string prompt_buf_;
    std::string prompt_buf2_;
    int         prompt_step_ = 0;
    std::function<void(const std::string&, const std::string&)> prompt_done_;

    // Data
    std::vector<PackageInfo>  packages_;     // all pkgs (LAN + repos)
    std::vector<ServerInfo>   servers_;
    std::vector<RepoInfo>     repos_;
    std::vector<PackageInfo>  installed_;
    std::vector<std::string>  loglines_;
    std::vector<PackageInfo>  filtered_;     // search-filtered subset of packages_

    LocalDB       db_;
    ClientConfig  cli_cfg_;
    ServerConfig  srv_cfg_;

#ifndef _WIN32
    struct termios old_term_;
#endif
    bool term_saved_ = false;
};

} // namespace dlr