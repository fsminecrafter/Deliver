#pragma once
#include "config.hpp"
#include "package_registry.hpp"
#include "types.hpp"
#include <string>
#include <vector>
#include <atomic>
#include <functional>
#include <chrono>

#ifndef _WIN32
#  include <termios.h>
#endif

namespace dlr {

enum ServerTuiKey : int {
    STK_UP    = 0x2001,
    STK_DOWN  = 0x2002,
    STK_LEFT  = 0x2003,
    STK_RIGHT = 0x2004,
    STK_PGUP  = 0x2005,
    STK_PGDN  = 0x2006,
    STK_HOME  = 0x2007,
    STK_END   = 0x2008,
    STK_ENTER = 13,
    STK_ESC   = 27,
    STK_TAB   = 9,
    STK_BTAB  = 0x2009,
    STK_BACKSP= 127,
    STK_CTRLC = 3,
    STK_CTRLD = 4,
    STK_DEL   = 0x200A,
    STK_F1    = 0x2100,
    STK_F2    = 0x2101,
    STK_F5    = 0x2104,
};

struct SysStats {
    double cpu_percent   = 0.0;
    double ram_used_mb   = 0.0;
    double ram_total_mb  = 0.0;
    double storage_used_gb  = 0.0;
    double storage_total_gb = 0.0;
    double uptime_seconds   = 0.0;
    double cpu_temp_c    = -1.0;   // -1 = unavailable
    uint64_t net_rx_bytes = 0;
    uint64_t net_tx_bytes = 0;
    uint64_t net_rx_delta = 0;   // bytes since last sample
    uint64_t net_tx_delta = 0;
    double   net_rx_rate  = 0.0; // bytes/sec
    double   net_tx_rate  = 0.0;
};

// Simple form field for the package editor
struct FormField {
    std::string label;
    std::string value;
    bool multiline = false;
};

class ServerTuiApp {
public:
    ServerTuiApp(ServerConfig srv_cfg);
    ~ServerTuiApp();

    int run();

    static std::atomic<bool> g_resize_flag;

private:
    // ── Terminal ───────────────────────────────────────────────────────────────
    void term_raw();
    void term_restore();
    void term_size(int& rows, int& cols);
    int  term_read();

    // ── Render ─────────────────────────────────────────────────────────────────
    void render();
    void render_header();
    void render_nav();
    void render_content();
    void render_footer();
    void render_status_bar();

    enum class View {
        Dashboard,   // CPU/RAM/uptime/heat/network
        Packages,    // presented packages list
        CreatePkg,   // wizard to presentfile / presentfolder
        EditPkg,     // view/edit/update a package
        Log,
    };

    void view_dashboard (int top, int left, int h, int w);
    void view_packages  (int top, int left, int h, int w);
    void view_create    (int top, int left, int h, int w);
    void view_edit      (int top, int left, int h, int w);
    void view_log       (int top, int left, int h, int w);

    // ── Draw helpers ───────────────────────────────────────────────────────────
    void buf(const std::string& s);
    void move(int r, int c);
    void clrline();
    void hline(int r, int c, int w, const char* ch = "─");
    std::string trunc(const std::string& s, int w) const;
    std::string pad_right(const std::string& s, int w) const;

    // ANSI
    static std::string A(const std::string& code, const std::string& s);
    static std::string bold(const std::string& s);
    static std::string dim (const std::string& s);
    static std::string col_g(const std::string& s);
    static std::string col_c(const std::string& s);
    static std::string col_y(const std::string& s);
    static std::string col_r(const std::string& s);
    static std::string col_m(const std::string& s);
    static std::string col_b(const std::string& s);
    static std::string rev  (const std::string& s);

    // ── Dashboard helpers ──────────────────────────────────────────────────────
    void   update_stats();
    void   draw_gauge(int r, int c, int w, double pct,
                      const std::string& label, const std::string& detail);
    void   draw_sparkline(int r, int c, int w,
                          const std::vector<double>& hist, double max_val);
    std::string human_size(uint64_t bytes) const;
    std::string human_rate(double bytes_per_sec) const;
    std::string uptime_str(double secs) const;

    // ── Input handling ─────────────────────────────────────────────────────────
    void handle_key(int k);
    void handle_key_form(int k);

    // ── Actions ────────────────────────────────────────────────────────────────
    void action_remove_package();
    void action_save_edit();
    void action_execute_create();
    void select_package_for_edit(int idx);
    void refresh_packages();

    // ── Logging ───────────────────────────────────────────────────────────────
    void tlog(const std::string& msg);
    void set_status(const std::string& msg);

    // ── State ─────────────────────────────────────────────────────────────────
    bool    running_  = true;
    bool    dirty_    = true;
    View    view_     = View::Dashboard;
    int     sel_      = 0;
    int     scroll_   = 0;
    int     rows_     = 24;
    int     cols_     = 80;
    int     nav_w_    = 16;

    std::string status_;
    std::string buf_;

    // Form state (shared between Create and Edit views)
    bool     in_form_    = false;
    int      form_field_ = 0;
    bool     form_confirm_ = false;  // confirmation dialog active
    std::vector<FormField> form_fields_;
    std::string form_title_;
    // For create: "file" or "folder"
    std::string create_mode_;
    // For edit: which package is being edited
    std::string edit_pkg_name_;

    // Data
    std::vector<PackageInfo>  packages_;
    std::vector<std::string>  loglines_;

    // Sysstat
    SysStats stats_;
    std::vector<double> cpu_hist_;
    std::vector<double> rx_hist_;
    std::vector<double> tx_hist_;
    std::chrono::steady_clock::time_point last_stat_time_;
    uint64_t prev_rx_ = 0, prev_tx_ = 0;

    ServerConfig       srv_cfg_;
    PackageRegistry    registry_;

#ifndef _WIN32
    struct termios old_term_;
#endif
    bool term_saved_ = false;
};

} // namespace dlr