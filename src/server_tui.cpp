#include "server_tui.hpp"
#include "pkg_parser.hpp"
#include "logger.hpp"
#include "version.hpp"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <algorithm>
#include <cstring>
#include <csignal>
#include <chrono>
#include <thread>
#include <filesystem>

#ifndef _WIN32
#  include <sys/ioctl.h>
#  include <sys/sysinfo.h>
#  include <sys/statvfs.h>
#  include <unistd.h>
#  include <poll.h>
#endif

namespace fs = std::filesystem;

namespace dlr {

std::atomic<bool> ServerTuiApp::g_resize_flag{false};

#ifndef _WIN32
static void srv_sigwinch(int) { ServerTuiApp::g_resize_flag.store(true); }
#endif

// ── Constructor / Destructor ──────────────────────────────────────────────────

ServerTuiApp::ServerTuiApp(ServerConfig srv_cfg)
    : srv_cfg_(std::move(srv_cfg))
    , registry_(srv_cfg_.registry_file, srv_cfg_.data_dir)
{
    registry_.load();
    refresh_packages();
    last_stat_time_ = std::chrono::steady_clock::now();
    cpu_hist_.resize(40, 0.0);
    rx_hist_.resize(40, 0.0);
    tx_hist_.resize(40, 0.0);
}

ServerTuiApp::~ServerTuiApp() {
    if (term_saved_) term_restore();
}

// ── Terminal ──────────────────────────────────────────────────────────────────

void ServerTuiApp::term_raw() {
#ifndef _WIN32
    if (tcgetattr(STDIN_FILENO, &old_term_) == 0) {
        term_saved_ = true;
        struct termios raw = old_term_;
        raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
        raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
        raw.c_cflag |= CS8;
        raw.c_oflag &= ~OPOST;
        raw.c_cc[VMIN]  = 0;
        raw.c_cc[VTIME] = 2; // 200ms timeout → ~5 fps
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    }
    signal(SIGWINCH, srv_sigwinch);
#else
    term_saved_ = true;
#endif
    std::cout << "\033[?1049h\033[?25l" << std::flush;
}

void ServerTuiApp::term_restore() {
#ifndef _WIN32
    if (term_saved_)
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_term_);
    signal(SIGWINCH, SIG_DFL);
#endif
    std::cout << "\033[?25h\033[?1049l" << std::flush;
    term_saved_ = false;
}

void ServerTuiApp::term_size(int& rows, int& cols) {
#ifndef _WIN32
    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
        rows = ws.ws_row; cols = ws.ws_col; return;
    }
#endif
    rows = 24; cols = 80;
}

int ServerTuiApp::term_read() {
#ifndef _WIN32
    unsigned char c = 0;
    pollfd pfd{STDIN_FILENO, POLLIN, 0};
    if (poll(&pfd, 1, 200) <= 0) return -1;
    if (read(STDIN_FILENO, &c, 1) <= 0) return -1;

    if (c == '\033') {
        unsigned char seq[4]{};
        if (poll(&pfd, 1, 30) <= 0) return STK_ESC;
        if (read(STDIN_FILENO, &seq[0], 1) <= 0) return STK_ESC;
        if (seq[0] == '[') {
            if (poll(&pfd, 1, 30) <= 0) return STK_ESC;
            read(STDIN_FILENO, &seq[1], 1);
            switch (seq[1]) {
                case 'A': return STK_UP;
                case 'B': return STK_DOWN;
                case 'C': return STK_RIGHT;
                case 'D': return STK_LEFT;
                case 'H': return STK_HOME;
                case 'F': return STK_END;
                case 'Z': return STK_BTAB;
                case '5': read(STDIN_FILENO, &seq[2], 1); return STK_PGUP;
                case '6': read(STDIN_FILENO, &seq[2], 1); return STK_PGDN;
                case '3': read(STDIN_FILENO, &seq[2], 1); return STK_DEL;
                default:  break;
            }
        } else if (seq[0] == 'O') {
            if (poll(&pfd, 1, 30) <= 0) return STK_ESC;
            read(STDIN_FILENO, &seq[1], 1);
            if (seq[1] == 'P') return STK_F1;
            if (seq[1] == 'Q') return STK_F2;
        }
        return STK_ESC;
    }
    if (c == 3)  return STK_CTRLC;
    if (c == 4)  return STK_CTRLD;
    if (c == 9)  return STK_TAB;
    if (c == 13) return STK_ENTER;
    return (int)c;
#else
    return -1;
#endif
}

// ── ANSI helpers ──────────────────────────────────────────────────────────────

std::string ServerTuiApp::A(const std::string& code, const std::string& s) {
    return "\033[" + code + "m" + s + "\033[0m";
}
std::string ServerTuiApp::bold (const std::string& s) { return A("1",    s); }
std::string ServerTuiApp::dim  (const std::string& s) { return A("2",    s); }
std::string ServerTuiApp::rev  (const std::string& s) { return A("7",    s); }
std::string ServerTuiApp::col_g(const std::string& s) { return A("32",   s); }
std::string ServerTuiApp::col_c(const std::string& s) { return A("36",   s); }
std::string ServerTuiApp::col_y(const std::string& s) { return A("33",   s); }
std::string ServerTuiApp::col_r(const std::string& s) { return A("31",   s); }
std::string ServerTuiApp::col_m(const std::string& s) { return A("35",   s); }
std::string ServerTuiApp::col_b(const std::string& s) { return A("34",   s); }

// ── Buffer helpers ────────────────────────────────────────────────────────────

void ServerTuiApp::buf(const std::string& s)  { buf_ += s; }
void ServerTuiApp::move(int r, int c)          { buf_ += "\033[" + std::to_string(r) + ";" + std::to_string(c) + "H"; }
void ServerTuiApp::clrline()                    { buf_ += "\033[K"; }

void ServerTuiApp::hline(int r, int c, int w, const char* ch) {
    move(r, c);
    for (int i = 0; i < w; i++) buf_ += ch;
}

std::string ServerTuiApp::trunc(const std::string& s, int w) const {
    if (w <= 0) return "";
    if ((int)s.size() <= w) return s;
    return s.substr(0, (size_t)w - 1) + "…";
}

std::string ServerTuiApp::pad_right(const std::string& s, int w) const {
    std::string r = trunc(s, w);
    if ((int)r.size() < w) r += std::string(w - r.size(), ' ');
    return r;
}

// ── Sysstat ───────────────────────────────────────────────────────────────────

std::string ServerTuiApp::human_size(uint64_t bytes) const {
    const char* u[] = {"B","KB","MB","GB","TB"};
    double v = (double)bytes; int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(i>0?1:0) << v << " " << u[i];
    return ss.str();
}

std::string ServerTuiApp::human_rate(double bps) const {
    return human_size((uint64_t)bps) + "/s";
}

std::string ServerTuiApp::uptime_str(double secs) const {
    int s = (int)secs;
    int d = s / 86400; s %= 86400;
    int h = s / 3600;  s %= 3600;
    int m = s / 60;    s %= 60;
    std::ostringstream ss;
    if (d>0) ss << d << "d ";
    ss << std::setw(2) << std::setfill('0') << h << ":"
       << std::setw(2) << std::setfill('0') << m << ":"
       << std::setw(2) << std::setfill('0') << s;
    return ss.str();
}

void ServerTuiApp::update_stats() {
    auto now = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(now - last_stat_time_).count();
    if (dt < 0.5) return;
    last_stat_time_ = now;

#ifndef _WIN32
    // ── CPU (via /proc/stat) ──────────────────────────────────────────────────
    {
        static uint64_t prev_idle=0, prev_total=0;
        std::ifstream f("/proc/stat");
        std::string line;
        std::getline(f, line);
        std::istringstream ss(line);
        std::string cpu;
        uint64_t user,nice,sys,idle,iowait,irq,softirq,steal,g,g_nice;
        ss >> cpu >> user >> nice >> sys >> idle >> iowait >> irq >> softirq >> steal >> g >> g_nice;
        uint64_t total = user+nice+sys+idle+iowait+irq+softirq+steal;
        uint64_t delta_total = total - prev_total;
        uint64_t delta_idle  = idle + iowait - prev_idle;
        if (delta_total > 0)
            stats_.cpu_percent = 100.0 * (1.0 - (double)delta_idle / delta_total);
        else
            stats_.cpu_percent = 0.0;
        prev_total = total;
        prev_idle  = idle + iowait;
    }

    // ── RAM (via /proc/meminfo) ───────────────────────────────────────────────
    {
        std::ifstream f("/proc/meminfo");
        std::string key; uint64_t val; std::string unit;
        uint64_t total=0, avail=0;
        for (std::string line; std::getline(f, line);) {
            std::istringstream ss(line);
            ss >> key >> val >> unit;
            if (key == "MemTotal:") total = val;
            if (key == "MemAvailable:") avail = val;
            if (total && avail) break;
        }
        stats_.ram_total_mb = total / 1024.0;
        stats_.ram_used_mb  = (total - avail) / 1024.0;
    }

    // ── Uptime ────────────────────────────────────────────────────────────────
    {
        std::ifstream f("/proc/uptime");
        f >> stats_.uptime_seconds;
    }

    // ── CPU temp (via /sys/class/thermal) ─────────────────────────────────────
    {
        stats_.cpu_temp_c = -1.0;
        for (int i = 0; i < 10; i++) {
            std::string path = "/sys/class/thermal/thermal_zone" + std::to_string(i) + "/temp";
            std::ifstream f(path);
            if (!f) break;
            int t; f >> t;
            if (t > 0) { stats_.cpu_temp_c = t / 1000.0; break; }
        }
    }

    // ── Storage (/var/lib/deliver data dir) ───────────────────────────────────
    {
        struct statvfs sv{};
        if (statvfs(srv_cfg_.data_dir.c_str(), &sv) == 0) {
            uint64_t bs = sv.f_frsize;
            stats_.storage_total_gb = (double)(sv.f_blocks * bs) / (1024*1024*1024);
            stats_.storage_used_gb  = (double)((sv.f_blocks - sv.f_bfree) * bs) / (1024*1024*1024);
        }
    }

    // ── Network (/proc/net/dev) ────────────────────────────────────────────────
    {
        std::ifstream f("/proc/net/dev");
        std::string line;
        std::getline(f, line); std::getline(f, line); // skip 2 header lines
        uint64_t total_rx=0, total_tx=0;
        while (std::getline(f, line)) {
            std::istringstream ss(line);
            std::string iface;
            ss >> iface;
            if (iface == "lo:") continue;
            uint64_t rx,x1,x2,x3,x4,x5,x6,x7,tx;
            ss >> rx >> x1 >> x2 >> x3 >> x4 >> x5 >> x6 >> x7 >> tx;
            total_rx += rx; total_tx += tx;
        }
        stats_.net_rx_delta = (total_rx >= prev_rx_) ? total_rx - prev_rx_ : 0;
        stats_.net_tx_delta = (total_tx >= prev_tx_) ? total_tx - prev_tx_ : 0;
        stats_.net_rx_rate  = stats_.net_rx_delta / dt;
        stats_.net_tx_rate  = stats_.net_tx_delta / dt;
        stats_.net_rx_bytes = total_rx;
        stats_.net_tx_bytes = total_tx;
        prev_rx_ = total_rx;
        prev_tx_ = total_tx;
    }
#endif

    // Update history
    cpu_hist_.erase(cpu_hist_.begin());
    cpu_hist_.push_back(stats_.cpu_percent);
    rx_hist_.erase(rx_hist_.begin());
    rx_hist_.push_back(stats_.net_rx_rate);
    tx_hist_.erase(tx_hist_.begin());
    tx_hist_.push_back(stats_.net_tx_rate);

    dirty_ = true;
}

// ── Dashboard gauge ───────────────────────────────────────────────────────────

void ServerTuiApp::draw_gauge(int r, int c, int w,
                               double pct, const std::string& label,
                               const std::string& detail) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    int bar_w = std::max(4, w - 22);
    double filled_f = (pct / 100.0) * bar_w;
    int filled = (int)filled_f;

    static const char* blocks[] = {
        " ", "▏","▎","▍","▌","▋","▊","▉","█"
    };

    std::string bar;
    bar.reserve(bar_w * 3);

    int i = 0;
    for (; i < filled; i++) {
        bar += "█";
    }

    double partial = filled_f - filled;
    if (i < bar_w) {
        int idx = (int)(partial * 8.0);
        if (idx > 0 && idx < 8) {
            bar += blocks[idx];
            i++;
        }
    }

    for (; i < bar_w; i++) {
        bar += "·";
    }

    std::string color;
    if (pct >= 90) color = "\033[31m";
    else if (pct >= 70) color = "\033[33m";
    else color = "\033[32m";

    move(r, c);
    buf_ += "\033[36m" + pad_right(label, 12) + "\033[0m ";
    buf_ += "\033[90m[\033[0m";
    buf_ += color + bar + "\033[0m";
    buf_ += "\033[90m]\033[0m";

    char pct_buf[16];
    snprintf(pct_buf, sizeof(pct_buf), " %5.1f%%", pct);

    buf_ += "\033[1m" + std::string(pct_buf) + "\033[0m";
    buf_ += "  " + dim(trunc(detail, w - bar_w - 22));
}

void ServerTuiApp::draw_sparkline(int r, int c, int w,
                                   const std::vector<double>& hist,
                                   double max_val) {
    if (max_val <= 0) max_val = 1.0;
    static const char* bars[] = {" ","▁","▂","▃","▄","▅","▆","▇","█"};
    move(r, c);
    int n = (int)hist.size();
    int start = std::max(0, n - w);
    for (int i = start; i < n; i++) {
        int idx = (int)(hist[i] / max_val * 8.0);
        if (idx > 8) idx = 8;
        if (idx < 0) idx = 0;
        buf_ += "\033[32m" + std::string(bars[idx]) + "\033[0m";
    }
    clrline();
}

// ── Render top-level ──────────────────────────────────────────────────────────

void ServerTuiApp::render() {
    buf_.clear();
    buf_ += "\033[2J\033[H\033[?25l";
    term_size(rows_, cols_);

    render_header();
    render_nav();
    render_content();
    render_footer();

    std::cout << buf_ << std::flush;
    dirty_ = false;
}

void ServerTuiApp::render_header() {
    move(1, 1);
    buf_ += "\033[1;45m"; // bold + magenta bg
    std::string title = " ◈ DELIVER SERVER  v" + std::string(dlr::VERSION)
                      + "  ·  " + srv_cfg_.name
                      + "  ·  port " + std::to_string(srv_cfg_.port);
    buf_ += title;
    int pad = cols_ - (int)title.size() - 22;
    if (pad > 0) buf_ += std::string(pad, ' ');
    buf_ += "Tab:switch  q:quit  ";
    buf_ += "\033[0m";
}

void ServerTuiApp::render_nav() {
    static const struct { View v; const char* label; } items[] = {
        { View::Dashboard, "Dashboard" },
        { View::Packages,  "Packages"  },
        { View::CreatePkg, "Create Pkg"},
        { View::EditPkg,   "Edit Pkg"  },
        { View::Log,       "Log"       },
    };
    constexpr int N = 5;
    int h = rows_ - 2;

    for (int i = 0; i < h; i++) {
        move(2+i, 1);
        buf_ += "\033[48;5;235m" + std::string(nav_w_, ' ') + "\033[0m";
    }
    move(2, 1);
    buf_ += "\033[48;5;235m\033[35;1m SERVER MENU  \033[0m\033[48;5;235m  \033[0m";
    move(3, 1);
    buf_ += "\033[48;5;235m\033[90m " + std::string(nav_w_-2, '-') + " \033[0m";

    for (int i = 0; i < N; i++) {
        move(4+i*2, 1);
        bool active = (view_ == items[i].v);
        if (active) buf_ += "\033[1;35;48;5;235m ▶ ";
        else        buf_ += "\033[0;37;48;5;235m   ";
        std::string lbl = items[i].label;
        lbl = pad_right(lbl, nav_w_-4);
        buf_ += lbl.substr(0, nav_w_-4) + "\033[0m";
    }

    for (int i = 0; i < h; i++) {
        move(2+i, nav_w_+1);
        buf_ += "\033[90m│\033[0m";
    }
}

void ServerTuiApp::render_content() {
    int top  = 2;
    int left = nav_w_+2;
    int h    = rows_-2;
    int w    = cols_-nav_w_-1;
    switch (view_) {
        case View::Dashboard: view_dashboard(top,left,h,w); break;
        case View::Packages:  view_packages (top,left,h,w); break;
        case View::CreatePkg: view_create   (top,left,h,w); break;
        case View::EditPkg:   view_edit     (top,left,h,w); break;
        case View::Log:       view_log      (top,left,h,w); break;
    }
}

void ServerTuiApp::render_footer() {
    move(rows_, 1);
    buf_ += "\033[1;45m";
    std::string hints;
    if (!status_.empty()) {
        hints = "  " + status_;
    } else {
        switch (view_) {
            case View::Dashboard: hints = "  R:Refresh  Tab:Switch"; break;
            case View::Packages:  hints = "  e:Edit  Del/d:Remove  r:Refresh  Tab:Switch"; break;
            case View::CreatePkg: hints = "  Tab:Next Field  Enter:Confirm  ESC:Cancel"; break;
            case View::EditPkg:   hints = "  Tab:Next Field  S:Save  ESC:Cancel  Del:Remove"; break;
            case View::Log:       hints = "  Tab:Switch"; break;
        }
    }
    int n = (int)hints.size();
    if (n < cols_) hints += std::string(cols_-n, ' ');
    buf_ += hints.substr(0, cols_) + "\033[0m";
}

// ── Dashboard view ────────────────────────────────────────────────────────────

void ServerTuiApp::view_dashboard(int top, int left, int h, int w) {
    int r = top;

    move(r++, left);
    buf_ += bold("  SYSTEM DASHBOARD") + "  " + dim(uptime_str(stats_.uptime_seconds));
    hline(r++, left, w-1);

    int gauge_w = std::min(w-2, 70);

    // ── CPU ─────────────────────────────────────────────
    draw_gauge(r++, left+1, gauge_w,
               stats_.cpu_percent,
               "CPU",
               std::to_string((int)stats_.cpu_percent) + "%");

    move(r++, left); clrline();

    // ── RAM (NOW PERCENT ONLY) ──────────────────────────
    double ram_pct = (stats_.ram_total_mb > 0)
        ? (stats_.ram_used_mb / stats_.ram_total_mb) * 100.0
        : 0.0;

    draw_gauge(r++, left+1, gauge_w,
               ram_pct,
               "RAM",
               std::to_string((int)ram_pct) + "%");

    move(r++, left); clrline();

    // ── Storage ─────────────────────────────────────────
    double stor_pct = (stats_.storage_total_gb > 0)
        ? (stats_.storage_used_gb / stats_.storage_total_gb) * 100.0
        : 0.0;

    std::ostringstream stor_detail;
    stor_detail << std::fixed << std::setprecision(1)
                << stats_.storage_used_gb << " / "
                << stats_.storage_total_gb << " GB";

    draw_gauge(r++, left+1, gauge_w,
               stor_pct,
               "Storage",
               stor_detail.str());

    move(r++, left); clrline();

    // ── CPU TEMP ────────────────────────────────────────
    move(r, left+1);
    buf_ += "\033[36m" + pad_right("CPU Temp", 12) + "\033[0m ";

    if (stats_.cpu_temp_c >= 0) {
        std::string tc_color = (stats_.cpu_temp_c >= 80) ? "\033[31m"
                             : (stats_.cpu_temp_c >= 65) ? "\033[33m"
                             : "\033[32m";

        std::ostringstream ts;
        ts << std::fixed << std::setprecision(1)
           << stats_.cpu_temp_c << " °C";

        buf_ += tc_color + bold(ts.str()) + "\033[0m";
    } else {
        buf_ += dim("N/A");
    }

    clrline();
    r++;

    move(r++, left); clrline();
    hline(r++, left, w-1);

    // ── NETWORK ────────────────────────────────────────
    move(r++, left);
    buf_ += bold("  NETWORK");

    // RX
    move(r, left+1);
    buf_ += "\033[36m" + pad_right("↓ Download", 12) + "\033[0m ";
    buf_ += "\033[32m" + bold(human_rate(stats_.net_rx_rate)) + "\033[0m";
    buf_ += "  " + dim("total: " + human_size(stats_.net_rx_bytes));
    clrline();
    r++;

    move(r++, left+14);
    draw_sparkline(r-1, left+14, gauge_w-14, rx_hist_, 1024.0);

    move(r++, left); clrline();

    // TX
    move(r, left+1);
    buf_ += "\033[36m" + pad_right("↑ Upload", 12) + "\033[0m ";
    buf_ += "\033[33m" + bold(human_rate(stats_.net_tx_rate)) + "\033[0m";
    buf_ += "  " + dim("total: " + human_size(stats_.net_tx_bytes));
    clrline();
    r++;

    move(r++, left+14);
    draw_sparkline(r-1, left+14, gauge_w-14, tx_hist_, 1024.0);

    move(r++, left); clrline();
    hline(r++, left, w-1);

    // ── SERVER INFO ─────────────────────────────────────
    move(r++, left);
    buf_ += bold("  SERVER INFO");

    move(r++, left+2);
    buf_ += "\033[36mName     \033[0m  " + srv_cfg_.name; clrline();

    move(r++, left+2);
    buf_ += "\033[36mPort     \033[0m  " + std::to_string(srv_cfg_.port) + " (TCP)";
    clrline();

    move(r++, left+2);
    buf_ += "\033[36mDiscovery\033[0m  " + std::to_string(DISCOVERY_PORT) + " (UDP)";
    clrline();

    move(r++, left+2);
    buf_ += "\033[36mAuth     \033[0m  ";
    buf_ += srv_cfg_.needs_password ? col_y("Password required") : col_g("Open");
    clrline();

    move(r++, left+2);
    buf_ += "\033[36mPackages \033[0m  " + std::to_string(packages_.size());
    clrline();

    move(r++, left+2);
    buf_ += "\033[36mData dir \033[0m  " + srv_cfg_.data_dir;
    clrline();

    for (; r <= top + h - 1; r++) {
        move(r, left);
        clrline();
    }
}

// ── Packages view ─────────────────────────────────────────────────────────────

void ServerTuiApp::view_packages(int top, int left, int h, int w) {
    int n = (int)packages_.size();

    move(top, left);
    buf_ += bold("  PRESENTED PACKAGES") + "  " + dim("(" + std::to_string(n) + " total)");
    clrline();
    hline(top+1, left, w-1);

    move(top+2, left);
    buf_ += dim("  " + pad_right("Name", 22) + pad_right("Version", 10)
               + pad_right("Arch", 10) + pad_right("OS", 14) + "File path");
    clrline();
    hline(top+3, left, w-1);

    int vis_h = h - 5;
    if (vis_h < 1) return;

    if (sel_ < scroll_)           scroll_ = sel_;
    if (sel_ >= scroll_ + vis_h)  scroll_ = sel_ - vis_h + 1;
    if (scroll_ < 0) scroll_ = 0;

    for (int i = 0; i < vis_h; i++) {
        int idx = scroll_ + i;
        int row = top + 4 + i;
        move(row, left);
        buf_ += "\033[K";
        if (idx >= n) continue;
        auto& p = packages_[idx];
        bool sel = (idx == sel_);

        std::string nm  = "  " + pad_right(p.name,    20);
        std::string ver = pad_right(p.version,  9);
        std::string arc = pad_right(arch_to_string(p.arch), 9);
        std::string os  = pad_right(os_to_string(p.operatingsystem), 13);
        std::string fp  = trunc(p.file_path, w - 67);

        if (sel) {
            buf_ += "\033[1;7m"
                 + nm + ver + arc + os + fp
                 + "\033[0m";
        } else {
            buf_ += col_c(nm) + ver + arc + dim(os) + dim(fp);
        }
    }
    // Clear remainder
    for (int i = vis_h; i < h - 4; i++) {
        move(top + 4 + i, left); clrline();
    }

    // Instructions at bottom
    if (n == 0) {
        move(top+5, left+2);
        buf_ += col_y("No packages presented.  Use 'Create Pkg' to add one.");
    }
}

// ── Create Package view ───────────────────────────────────────────────────────

void ServerTuiApp::view_create(int top, int left, int h, int w) {
    move(top, left);
    buf_ += bold("  CREATE / PRESENT PACKAGE"); clrline();
    hline(top+1, left, w-1);

    if (!in_form_ && !form_confirm_) {
        // Mode selection
        move(top+3, left+2);
        buf_ += "What do you want to present?"; clrline();
        move(top+5, left+4);
        buf_ += col_c("f") + "  →  Present a " + bold("File"); clrline();
        move(top+6, left+4);
        buf_ += col_c("F") + "  →  Present a " + bold("Folder / Directory"); clrline();
        move(top+8, left+2);
        buf_ += dim("Press f or F to start the wizard."); clrline();
        return;
    }

    // Form rendering (also shown during confirm so fields are visible)
    move(top+2, left+2);
    buf_ += bold(form_title_); clrline();
    hline(top+3, left+2, w-4);

    for (int i = 0; i < (int)form_fields_.size(); i++) {
        auto& ff = form_fields_[i];
        bool active = (i == form_field_) && in_form_ && !form_confirm_;
        int row = top + 5 + i * 3;
        move(row, left+2);
        buf_ += (active ? col_c(bold(ff.label)) : col_c(ff.label)) + ":"; clrline();
        move(row+1, left+4);
        if (active)
            buf_ += "\033[1;7m " + pad_right(ff.value + "_", w-8) + " \033[0m";
        else
            buf_ += " " + ff.value; clrline();
    }

    if (form_confirm_) {
        int cr = top + 5 + (int)form_fields_.size() * 3 + 2;
        hline(cr, left+2, w-4);
        move(cr+1, left+2);
        buf_ += col_y("Confirm?  ") + bold("[y]") + " Yes  /  " + bold("[ESC/n]") + " Cancel";
        clrline();
    } else {
        int hr = top + 5 + (int)form_fields_.size() * 3 + 1;
        move(hr, left+2);
        buf_ += dim("Tab/Enter: next  |  ESC: cancel  |  After last field: press Enter to confirm");
        clrline();
    }
}

// ── Edit Package view ─────────────────────────────────────────────────────────

void ServerTuiApp::view_edit(int top, int left, int h, int w) {
    move(top, left);
    if (edit_pkg_name_.empty()) {
        buf_ += bold("  EDIT PACKAGE") + "  " + dim("(select a package from the Packages view with 'e')");
        clrline();
        hline(top+1, left, w-1);
        move(top+4, left+2);
        buf_ += col_y("No package selected."); clrline();
        move(top+5, left+2);
        buf_ += dim("Switch to 'Packages' (Tab), highlight a package, press ") + bold("e") + dim(" to edit it.");
        clrline();
        return;
    }

    buf_ += bold("  EDIT PACKAGE: ") + col_c(edit_pkg_name_); clrline();
    hline(top+1, left, w-1);

    if (form_fields_.empty()) {
        move(top+3, left+2);
        buf_ += col_r("Package not found."); clrline();
        return;
    }

    if (form_confirm_) {
        // Show fields greyed out + confirm prompt
        for (int i = 0; i < (int)form_fields_.size(); i++) {
            auto& ff = form_fields_[i];
            int row = top + 3 + i * 3;
            if (row >= top + h - 4) break;
            move(row, left+2);
            buf_ += col_c(ff.label) + ":"; clrline();
            move(row+1, left+4);
            buf_ += dim(" " + ff.value); clrline();
        }
        int cr = top + 3 + (int)form_fields_.size() * 3 + 1;
        if (cr < top + h - 1) {
            hline(cr, left+2, w-4);
            move(cr+1, left+2);
            buf_ += col_y("Save changes?  ") + bold("[y]") + " Yes  /  " + bold("[n/ESC]") + " Cancel";
            clrline();
        }
        return;
    }

    for (int i = 0; i < (int)form_fields_.size(); i++) {
        auto& ff = form_fields_[i];
        bool active = in_form_ && (i == form_field_);
        int row = top + 3 + i * 3;
        if (row >= top + h - 4) break;
        move(row, left+2);
        buf_ += (active ? col_c(bold(ff.label)) : col_c(ff.label)) + ":"; clrline();
        move(row+1, left+4);
        if (active)
            buf_ += "\033[1;7m " + pad_right(ff.value + "_", w - 8) + " \033[0m";
        else
            buf_ += " " + dim(ff.value);
        clrline();
    }

    // Footer hint inside content area
    int hr = top + h - 2;
    hline(hr, left+2, w-4);
    move(hr+1, left+2);
    if (in_form_) {
        buf_ += dim("Tab:next field  Ctrl+S:save  ESC:cancel  Del:remove package");
    } else {
        buf_ += dim("Start typing to edit  |  Ctrl+S:save  |  ESC:deselect  |  Del:remove");
    }
    clrline();
}

// ── Log view ──────────────────────────────────────────────────────────────────

void ServerTuiApp::view_log(int top, int left, int h, int w) {
    move(top, left);
    buf_ += bold("  SERVER LOG"); clrline();
    hline(top+1, left, w-1);
    int vis_h = h - 3;
    int n = (int)loglines_.size();
    int start = std::max(0, n - vis_h);
    for (int i = 0; i < vis_h; i++) {
        move(top+2+i, left);
        int idx = start+i;
        if (idx < n) buf_ += "  " + dim(trunc(loglines_[idx], w-4));
        clrline();
    }
}

// ── Input handling ────────────────────────────────────────────────────────────

void ServerTuiApp::handle_key(int k) {
    // Form keys for create/edit
    if (in_form_) { handle_key_form(k); return; }

    // Confirmation dialog
    if (form_confirm_) {
        if (k == 'y' || k == 'Y') {
            form_confirm_ = false;  // clear FIRST before action modifies state
            if (view_ == View::CreatePkg) action_execute_create();
            else if (view_ == View::EditPkg) action_save_edit();
        } else if (k == 27 || k == 'n' || k == 'N') {
            form_confirm_ = false;
            tlog("Save cancelled by user.");
            set_status("Cancelled.");
        }
        dirty_ = true; return;
    }

    // Global
    switch (k) {
        case 'q': case 'Q': case STK_CTRLC: case STK_CTRLD:
            running_ = false; return;
        case STK_TAB: case STK_RIGHT:
            view_ = (View)(((int)view_+1) % 5);
            sel_=scroll_=form_field_=0; in_form_=false; dirty_=true; return;
        case STK_BTAB: case STK_LEFT:
            view_ = (View)(((int)view_+4) % 5);
            sel_=scroll_=form_field_=0; in_form_=false; dirty_=true; return;
        case '1': view_=View::Dashboard;  sel_=scroll_=0; in_form_=false; dirty_=true; return;
        case '2': view_=View::Packages;   sel_=scroll_=0; dirty_=true; return;
        case '3': view_=View::CreatePkg;  sel_=scroll_=0; in_form_=false; dirty_=true; return;
        case '4': view_=View::EditPkg;    sel_=scroll_=0; dirty_=true; return;
        case '5': view_=View::Log;        sel_=scroll_=0; dirty_=true; return;
        case 'R': case 'r':
            refresh_packages(); set_status("Packages refreshed."); return;
    }

    // Navigation
    int list_n = (view_==View::Packages) ? (int)packages_.size() : 0;
    switch (k) {
        case STK_UP:   if (sel_>0) { sel_--; dirty_=true; } return;
        case STK_DOWN: if (sel_<list_n-1) { sel_++; dirty_=true; } return;
        case STK_PGUP: sel_=std::max(0, sel_-(rows_-5)); dirty_=true; return;
        case STK_PGDN: sel_=std::min(std::max(0,list_n-1), sel_+(rows_-5)); dirty_=true; return;
        case STK_HOME: sel_=0; dirty_=true; return;
        case STK_END:  sel_=std::max(0,list_n-1); dirty_=true; return;
    }

    // View-specific
    if (view_ == View::Packages) {
        if (k=='e'||k=='E'||k==STK_ENTER) {
            if (!packages_.empty() && sel_<(int)packages_.size()) {
                select_package_for_edit(sel_);
                view_ = View::EditPkg;
                dirty_ = true;
            }
            return;
        }
        if (k=='d'||k=='D'||k==STK_DEL||k==0x7f) {
            action_remove_package(); return;
        }
    }

    if (view_ == View::CreatePkg) {
        if (k=='f') {
            create_mode_ = "file";
            form_title_  = "Present File as Package";
            form_fields_ = {
                {"File path",    "", false},
                {"Package name", "", false},
            };
            form_field_ = 0; in_form_ = true; form_confirm_ = false;
            dirty_ = true;
        } else if (k=='F') {
            create_mode_ = "folder";
            form_title_  = "Present Folder as Package";
            form_fields_ = {
                {"Folder path",  "", false},
                {"Package name", "", false},
            };
            form_field_ = 0; in_form_ = true; form_confirm_ = false;
            dirty_ = true;
        }
        return;
    }

    if (view_ == View::EditPkg) {
        if (!edit_pkg_name_.empty()) {
            if (k == 19) {  // Ctrl+S — save
                if (!in_form_) {
                    // not editing yet, nothing to save
                } else {
                    form_confirm_ = true;
                    in_form_      = false;
                }
                dirty_ = true;
                return;
            }
            if (k == 27) {  // ESC — cancel editing or deselect
                if (in_form_) {
                    in_form_     = false;
                    form_field_  = 0;
                    set_status("Edit cancelled.");
                } else {
                    edit_pkg_name_.clear();
                    form_fields_.clear();
                    set_status("Deselected.");
                }
                dirty_ = true;
                return;
            }
            if ((k == STK_DEL || k == 0x7f || k == 'd' || k == 'D') && !in_form_) {
                action_remove_package();
                return;
            }
            // Any printable key or Tab/Enter while a package is selected
            // → start editing immediately and pass the key to the form handler
            if (!in_form_ && ((k >= 32 && k < 127) || k == 9 || k == 13)) {
                in_form_     = true;
                form_field_  = 0;
                form_confirm_= false;
            }
            if (in_form_) {
                handle_key_form(k);
                return;
            }
        }
    }
}

void ServerTuiApp::handle_key_form(int k) {
    int n_fields = (int)form_fields_.size();

    if (n_fields == 0) { in_form_ = false; dirty_ = true; return; }
    if (form_field_ < 0) form_field_ = 0;
    if (form_field_ >= n_fields) form_field_ = n_fields - 1;

    if (k == 27) {  // ESC — cancel form
        in_form_      = false;
        form_confirm_ = false;
        set_status("Edit cancelled.");
        dirty_ = true;
        return;
    }

    if (k == 19) {  // Ctrl+S — trigger save confirm from anywhere in the form
        in_form_      = false;
        form_confirm_ = true;
        dirty_ = true;
        return;
    }

    if (k == 13 || k == 9) {  // Enter or Tab — advance field
        if (form_field_ < n_fields - 1) {
            form_field_++;
        } else {
            // Last field: Tab wraps to first, Enter triggers save
            if (k == 9) {
                form_field_ = 0;
            } else {
                in_form_      = false;
                form_confirm_ = true;
            }
        }
        dirty_ = true;
        return;
    }

    if (k == 127 || k == 8) {  // Backspace
        if (!form_fields_[form_field_].value.empty()) {
            form_fields_[form_field_].value.pop_back();
            dirty_ = true;
        }
        return;
    }

    if (k >= 32 && k < 127) {
        form_fields_[form_field_].value += (char)k;
        dirty_ = true;
    }
}

// ── Actions ───────────────────────────────────────────────────────────────────

void ServerTuiApp::refresh_packages() {
    registry_.load();
    packages_ = registry_.list_all();
    // clamp sel
    if (sel_ >= (int)packages_.size()) sel_ = std::max(0, (int)packages_.size()-1);
    dirty_ = true;
}

void ServerTuiApp::select_package_for_edit(int idx) {
    if (idx < 0 || idx >= (int)packages_.size()) return;
    auto& p = packages_[idx];
    edit_pkg_name_ = p.name;
    in_form_       = true;   // start in edit mode immediately
    form_confirm_  = false;
    form_field_    = 0;

    form_fields_ = {
        {"Name",           p.name},
        {"Version",        p.version},
        {"Description",    p.description},
        {"Install script", p.installscript},
        {"Install cmd",    p.installcommand},
        {"Dependencies",   [&](){
            std::string s;
            for (size_t i = 0; i < p.dependencies.size(); i++) {
                if (i) s += ", ";
                s += p.dependencies[i];
            }
            return s;
        }()},
        {"Rival pack",     p.rivalpack},
        {"File path",      p.file_path},
    };
    tlog("Editing: " + p.name);
    set_status("Editing " + p.name + " — Tab:next  Ctrl+S:save  ESC:cancel");
}

void ServerTuiApp::action_remove_package() {
    if (packages_.empty()) return;
    // Determine which package
    std::string pkg_name;
    if (view_ == View::EditPkg && !edit_pkg_name_.empty()) {
        pkg_name = edit_pkg_name_;
    } else if (view_ == View::Packages && sel_ < (int)packages_.size()) {
        pkg_name = packages_[sel_].name;
    } else {
        return;
    }

    // Restore terminal for the interactive remove prompt
    term_restore();
    std::cout << "\033[2J\033[H";
    std::cout << "\n\033[1;31mRemove package: " << pkg_name << "\033[0m\n";
    std::cout << "Confirm? [y/N] ";
    std::string ans;
    std::getline(std::cin, ans);
    if (ans == "y" || ans == "Y") {
        bool ok = registry_.remove_package(pkg_name, /*auto_yes=*/true);
        if (ok) {
            tlog("Removed package: " + pkg_name);
            set_status("Removed: " + pkg_name);
            if (view_ == View::EditPkg) { edit_pkg_name_.clear(); form_fields_.clear(); }
        } else {
            set_status("Failed to remove: " + pkg_name);
        }
        refresh_packages();
    } else {
        set_status("Remove cancelled.");
    }
    term_raw();
    dirty_ = true;
}

void ServerTuiApp::action_save_edit() {
    if (edit_pkg_name_.empty()) {
        tlog("action_save_edit: no package selected");
        set_status("Error: no package selected.");
        return;
    }
    if ((int)form_fields_.size() < 8) {
        tlog("action_save_edit: only " + std::to_string(form_fields_.size()) + " fields, need 8");
        set_status("Error: form incomplete.");
        return;
    }

    // Build the target paths first
    fs::path pkg_dir  = fs::path(srv_cfg_.data_dir) / edit_pkg_name_;
    fs::path pkg_file = pkg_dir / (edit_pkg_name_ + ".pkg");

    tlog("Saving: " + pkg_file.string());

    // Ensure directory exists
    std::error_code ec;
    fs::create_directories(pkg_dir, ec);
    if (ec) {
        tlog("action_save_edit: mkdir failed: " + ec.message());
        set_status("Error: cannot create directory.");
        return;
    }

    // Build PackageInfo from form — don't call registry_.find() or registry_.load()
    // as that can reset state. Build directly from form fields.
    PackageInfo info;
    info.name           = edit_pkg_name_;
    info.version        = form_fields_[1].value;
    info.description    = form_fields_[2].value;
    info.installscript  = form_fields_[3].value;
    info.installcommand = form_fields_[4].value;
    info.rivalpack      = form_fields_[6].value;
    info.file_path      = form_fields_[7].value;
    info.arch           = Arch::ANY;
    info.operatingsystem = OS::ANY;

    // Parse comma-separated dependencies
    {
        std::istringstream dep_ss(form_fields_[5].value);
        std::string dep_tok;
        while (std::getline(dep_ss, dep_tok, ',')) {
            while (!dep_tok.empty() && dep_tok.front() == ' ') dep_tok.erase(dep_tok.begin());
            while (!dep_tok.empty() && dep_tok.back()  == ' ') dep_tok.pop_back();
            if (!dep_tok.empty()) info.dependencies.push_back(dep_tok);
        }
    }

    // Write .pkg file
    if (!write_pkg(pkg_file.string(), info)) {
        tlog("action_save_edit: write_pkg failed for: " + pkg_file.string());
        set_status("Error: failed to write .pkg file.");
        return;
    }
    tlog("write_pkg OK: " + pkg_file.string());

    // Attach updates the in-memory registry and saves the JSON
    if (!registry_.attach_pkg(pkg_file.string(), edit_pkg_name_)) {
        tlog("action_save_edit: attach_pkg failed for: " + edit_pkg_name_);
        set_status("Error: failed to update registry.");
        return;
    }

    tlog("Saved changes to: " + edit_pkg_name_);
    set_status("Saved: " + edit_pkg_name_);
    in_form_      = false;
    form_confirm_ = false;
    refresh_packages();
    dirty_ = true;
}

void ServerTuiApp::action_execute_create() {
    if (form_fields_.size() < 2) return;
    std::string path     = form_fields_[0].value;
    std::string pkg_name = form_fields_[1].value;

    if (path.empty() || pkg_name.empty()) {
        set_status("Error: path and package name are required.");
        in_form_ = true; form_field_ = 0;
        form_confirm_ = false;
        dirty_ = true;
        return;
    }

    // Switch to normal terminal for registry output
    term_restore();
    std::cout << "\033[2J\033[H";

    bool ok = false;
    if (create_mode_ == "file") {
        ok = registry_.present_file(path, pkg_name, /*auto_yes=*/true);
    } else {
        ok = registry_.present_folder(path, pkg_name, /*auto_yes=*/true);
    }

    if (ok) {
        std::string pkg_file = (fs::path(srv_cfg_.data_dir) / pkg_name / (pkg_name + ".pkg")).string();
        registry_.generate_pkg(pkg_file, pkg_name);
        tlog("Presented " + create_mode_ + ": " + path + " → " + pkg_name);
    } else {
        tlog("Failed to present " + create_mode_ + ": " + path);
    }

    std::cout << "\nPress any key to continue..." << std::flush;

    // Back to raw mode BEFORE reading the keypress
    term_raw();

    // Drain one key in raw mode (non-blocking with timeout)
    for (int i = 0; i < 50; i++) {
        int c = term_read();  // 200ms timeout each
        if (c != -1) break;
    }

    refresh_packages();
    in_form_      = false;
    form_confirm_ = false;
    form_fields_.clear();
    create_mode_.clear();

    if (ok) set_status("Created package: " + pkg_name);
    else    set_status("Failed to create package: " + path);

    dirty_ = true;
}

// ── Logging / status ──────────────────────────────────────────────────────────

void ServerTuiApp::tlog(const std::string& msg) {
    auto t  = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char ts[16]; strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&t));
    loglines_.push_back(std::string(ts) + "  " + msg);
    if (loglines_.size() > 500) loglines_.erase(loglines_.begin());
}

void ServerTuiApp::set_status(const std::string& msg) {
    status_ = msg;
    tlog(msg);
    dirty_  = true;
}

// ── Main loop ─────────────────────────────────────────────────────────────────

int ServerTuiApp::run() {
    term_raw();
    term_size(rows_, cols_);
    update_stats();

    tlog("Server TUI started  v" + std::string(dlr::VERSION));
    set_status("Welcome!  Tab=switch views  q=quit  R=refresh");

    auto last_stat = std::chrono::steady_clock::now();

    while (running_) {
        if (g_resize_flag.exchange(false)) {
            term_size(rows_, cols_); dirty_ = true;
        }
        // Refresh stats every 2 s
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - last_stat).count() >= 2.0) {
            update_stats();
            last_stat = now;
        }

        if (dirty_) render();

        int k = term_read();
        if (k == -1) { dirty_ = true; continue; }  // timeout → re-render (stats update)
        handle_key(k);
        dirty_ = true;

        // Clear status after first keypress
        if (!status_.empty()) status_.clear();
    }

    term_restore();
    std::cout << "\n\033[1;35mGoodbye from Deliver Server Manager!\033[0m\n";
    return 0;
}

} // namespace dlr
