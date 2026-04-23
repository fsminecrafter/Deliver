#include "tui_app.hpp"
#include "client.hpp"
#include "network.hpp"
#include "http_repo.hpp"
#include "logger.hpp"
#include "version.hpp"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstring>
#include <csignal>
#include <chrono>
#include <thread>
#include <filesystem>

#ifndef _WIN32
#  include <sys/ioctl.h>
#  include <unistd.h>
#  include <poll.h>
#else
#  include <conio.h>
#  include <windows.h>
#endif

namespace fs = std::filesystem;

namespace dlr {

std::atomic<bool> TuiApp::g_resize_flag{false};

#ifndef _WIN32
static void sigwinch_handler(int) { TuiApp::g_resize_flag.store(true); }
#endif

// ── Constructor / Destructor ───────────────────────────────────────────────────

TuiApp::TuiApp(ClientConfig cli_cfg, ServerConfig srv_cfg)
    : db_(cli_cfg.db_dir)
    , cli_cfg_(std::move(cli_cfg))
    , srv_cfg_(std::move(srv_cfg))
{
    net::init();
    db_.load();
}

TuiApp::~TuiApp() {
    if (term_saved_) term_restore();
    net::cleanup();
}

// ── Terminal management ────────────────────────────────────────────────────────

void TuiApp::term_raw() {
#ifndef _WIN32
    if (tcgetattr(STDIN_FILENO, &old_term_) == 0) {
        term_saved_ = true;
        struct termios raw = old_term_;
        raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
        raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
        raw.c_cflag |=  CS8;
        raw.c_oflag &= ~OPOST;
        raw.c_cc[VMIN]  = 0;
        raw.c_cc[VTIME] = 1;  // 100 ms timeout
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    }
    signal(SIGWINCH, sigwinch_handler);
#else
    term_saved_ = true;
#endif
    // Enter alternate screen, hide cursor
    std::cout << "\033[?1049h\033[?25l" << std::flush;
}

void TuiApp::term_restore() {
#ifndef _WIN32
    if (term_saved_)
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_term_);
    signal(SIGWINCH, SIG_DFL);
#endif
    // Restore screen, show cursor
    std::cout << "\033[?25h\033[?1049l" << std::flush;
    term_saved_ = false;
}

void TuiApp::term_size(int& rows, int& cols) {
#ifndef _WIN32
    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
        rows = ws.ws_row;
        cols = ws.ws_col;
        return;
    }
#else
    CONSOLE_SCREEN_BUFFER_INFO ci{};
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &ci)) {
        cols = ci.srWindow.Right  - ci.srWindow.Left + 1;
        rows = ci.srWindow.Bottom - ci.srWindow.Top  + 1;
        return;
    }
#endif
    rows = 24; cols = 80;
}

int TuiApp::term_read() {
#ifndef _WIN32
    unsigned char c = 0;
    // Poll up to 100 ms so we can re-render on resize
    pollfd pfd{STDIN_FILENO, POLLIN, 0};
    if (poll(&pfd, 1, 100) <= 0) return -1;
    if (read(STDIN_FILENO, &c, 1) <= 0) return -1;

    if (c == '\033') {
        unsigned char seq[4]{};
        if (poll(&pfd, 1, 30) <= 0) return TK_ESC;
        if (read(STDIN_FILENO, &seq[0], 1) <= 0) return TK_ESC;
        if (seq[0] == '[') {
            if (poll(&pfd, 1, 30) <= 0) return TK_ESC;
            read(STDIN_FILENO, &seq[1], 1);
            switch (seq[1]) {
                case 'A': return TK_UP;
                case 'B': return TK_DOWN;
                case 'C': return TK_RIGHT;
                case 'D': return TK_LEFT;
                case 'H': return TK_HOME;
                case 'F': return TK_END;
                case 'Z': return TK_BTAB;
                case '5': read(STDIN_FILENO, &seq[2], 1); return TK_PGUP;
                case '6': read(STDIN_FILENO, &seq[2], 1); return TK_PGDN;
                default:  break;
            }
        } else if (seq[0] == 'O') {
            if (poll(&pfd, 1, 30) <= 0) return TK_ESC;
            read(STDIN_FILENO, &seq[1], 1);
            if (seq[1] == 'P') return TK_F1;
        }
        return TK_ESC;
    }
    return (int)c;
#else
    // Windows: use _kbhit / _getch with 100ms sleep
    for (int i = 0; i < 10; ++i) {
        if (_kbhit()) {
            int c = _getch();
            if (c == 0 || c == 0xE0) {
                int e = _getch();
                switch (e) {
                    case 72: return TK_UP;
                    case 80: return TK_DOWN;
                    case 75: return TK_LEFT;
                    case 77: return TK_RIGHT;
                    case 73: return TK_PGUP;
                    case 81: return TK_PGDN;
                    default: return -1;
                }
            }
            return c;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return -1;
#endif
}

// ── ANSI helpers ──────────────────────────────────────────────────────────────

std::string TuiApp::A(const std::string& code, const std::string& s) {
    return "\033[" + code + "m" + s + "\033[0m";
}
std::string TuiApp::bold (const std::string& s) { return A("1",    s); }
std::string TuiApp::dim  (const std::string& s) { return A("2",    s); }
std::string TuiApp::rev  (const std::string& s) { return A("7",    s); }
std::string TuiApp::col_g(const std::string& s) { return A("32",   s); }
std::string TuiApp::col_c(const std::string& s) { return A("36",   s); }
std::string TuiApp::col_y(const std::string& s) { return A("33",   s); }
std::string TuiApp::col_r(const std::string& s) { return A("31",   s); }
std::string TuiApp::col_m(const std::string& s) { return A("35",   s); }
std::string TuiApp::reset()                      { return "\033[0m"; }

// ── Render buffer helpers ─────────────────────────────────────────────────────

void TuiApp::buf(const std::string& s)        { buf_ += s; }
void TuiApp::move(int r, int c)               { buf_ += "\033[" + std::to_string(r) + ";" + std::to_string(c) + "H"; }
void TuiApp::clrline()                         { buf_ += "\033[K"; }

void TuiApp::hline(int r, int c, int w, const char* ch) {
    move(r, c);
    for (int i = 0; i < w; i++) buf_ += ch;
}

std::string TuiApp::trunc(const std::string& s, int w) const {
    if (w <= 0) return "";
    // Count visible (non-ANSI) chars — simple heuristic: just count bytes for
    // plain text.  For our data, content is always plain ASCII.
    if ((int)s.size() <= w) return s;
    return s.substr(0, (size_t)w - 1) + "…";
}

void TuiApp::draw_row(int r, int c, int w, const std::string& s,
                      bool selected, bool installed) {
    move(r, c);
    std::string line = trunc(s, w);
    // Pad to width
    int vis = (int)line.size();
    std::string padded = line + std::string(std::max(0, w - vis), ' ');

    if (selected)
        buf_ += "\033[1;7m" + padded + "\033[0m";
    else if (installed)
        buf_ += "\033[32m"  + padded + "\033[0m";
    else
        buf_ += padded;
}

// ── Top-level render ──────────────────────────────────────────────────────────

void TuiApp::render() {
    buf_.clear();

    // Clear full screen and move cursor to top-left
    buf_ += "\033[2J\033[H";

    buf_ += "\033[?25l";  // hide cursor

    term_size(rows_, cols_);
    nav_w_ = 14;

    render_header();
    render_nav();
    render_content();
    render_footer();

    // Flush
    std::cout << buf_ << std::flush;
    dirty_ = false;
}

// ── Header ────────────────────────────────────────────────────────────────────

void TuiApp::render_header() {
    move(1, 1);
    // Full-width header bar
    std::string left  = " \u25c8 " + bold("DELIVER") + "  LAN Package Manager  " +
                        dim("v" + std::string(dlr::VERSION));
    std::string right = dim("Tab") + ":" + col_c("switch")  + "  " +
                        dim("q")   + ":" + col_r("quit")    + "  ";
    // Simple background: use reverse on the whole line
    buf_ += "\033[1;44m";  // bold + blue bg
    buf_ += " \u25c8 DELIVER  LAN Package Manager  v";
    buf_ += dlr::VERSION;
    int used = 5 + 28 + (int)strlen(dlr::VERSION);
    int spaces = cols_ - used - 22;
    if (spaces > 0) buf_ += std::string(spaces, ' ');
    buf_ += "Tab:switch  q:quit  ";
    buf_ += "\033[0m";
    (void)left; (void)right;
}

// ── Navigation sidebar ─────────────────────────────────────────────────────────

void TuiApp::render_nav() {
    static const struct { View v; const char* label; } items[] = {
        { View::Packages,  "Packages"  },
        { View::Servers,   "Servers"   },
        { View::Repos,     "Repos"     },
        { View::Installed, "Installed" },
        { View::Log,       "Log"       },
    };
    constexpr int N = 5;

    int content_h = rows_ - 2;  // header + footer

    // Draw sidebar column
    for (int row = 0; row < content_h; row++) {
        move(2 + row, 1);
        buf_ += "\033[48;5;236m";  // dark background
        buf_ += std::string(nav_w_, ' ');
        buf_ += "\033[0m";
    }

    // Title
    move(2, 1);
    buf_ += "\033[48;5;236m\033[36;1m";
    buf_ += " NAVIGATE     ";
    buf_ += "\033[0m";

    // Separator
    move(3, 1);
    buf_ += "\033[48;5;236m\033[90m";
    buf_ += " " + std::string(nav_w_ - 2, '-') + " ";
    buf_ += "\033[0m";

    // Items
    for (int i = 0; i < N; i++) {
        move(4 + i, 1);
        bool active = (view_ == items[i].v);
        if (active) {
            buf_ += "\033[1;32;48;5;236m";
            buf_ += " \u25b6 ";
        } else {
            buf_ += "\033[0;37;48;5;236m";
            buf_ += "   ";
        }
        std::string lbl = std::string(items[i].label);
        // Pad to nav_w_ - 3
        if ((int)lbl.size() < nav_w_ - 4) lbl += std::string(nav_w_ - 4 - lbl.size(), ' ');
        buf_ += lbl.substr(0, nav_w_ - 4);
        buf_ += "\033[0m";
    }

    // Vertical divider
    for (int row = 0; row < content_h; row++) {
        move(2 + row, nav_w_ + 1);
        buf_ += "\033[90m\u2502\033[0m";
    }
}

// ── Content area dispatch ─────────────────────────────────────────────────────

void TuiApp::render_content() {
    int top   = 2;                           // row 2 (1-indexed)
    int left  = nav_w_ + 2;
    int h     = rows_ - 2;
    int w     = cols_ - nav_w_ - 1;

    switch (view_) {
        case View::Packages:  view_packages (top, left, h, w); break;
        case View::Servers:   view_servers  (top, left, h, w); break;
        case View::Repos:     view_repos    (top, left, h, w); break;
        case View::Installed: view_installed(top, left, h, w); break;
        case View::Log:       view_log      (top, left, h, w); break;
    }
}

// ── Footer ────────────────────────────────────────────────────────────────────

void TuiApp::render_footer() {
    move(rows_, 1);
    buf_ += "\033[1;44m";

    std::string hints;
    if (searching_) {
        hints = " ESC:cancel  Enter:confirm  Type to filter  ";
    } else if (prompting_) {
        hints = " ESC:cancel  Enter:confirm  ";
    } else {
        switch (view_) {
            case View::Packages:
                hints = " i/Enter:Install  d:Download  /:Search  R:Refresh  Tab:Switch ";
                break;
            case View::Servers:
                hints = " p:Ping  R:Rescan  Tab:Switch  ";
                break;
            case View::Repos:
                hints = " a:Add Repo  Del:Remove  R:Refresh  Tab:Switch  ";
                break;
            case View::Installed:
                hints = " r:Remove tracking  Tab:Switch  ";
                break;
            case View::Log:
                hints = " Tab:Switch  ";
                break;
        }
    }
    // Pad to full width
    int n = (int)hints.size();
    if (n < cols_) hints += std::string(cols_ - n, ' ');
    buf_ += hints.substr(0, cols_);
    buf_ += "\033[0m";
}

// ── Packages view ─────────────────────────────────────────────────────────────

void TuiApp::view_packages(int top, int left, int h, int w) {
    // Decide which list to show: filtered_ when searching, else packages_
    const auto& list = searching_ ? filtered_ : packages_;
    int n = (int)list.size();

    // Title bar
    move(top, left);
    buf_ += "\033[1;36m";
    if (searching_) {
        buf_ += "  Search: " + search_str_ + "_";
        buf_ += trunc("", w - 12 - (int)search_str_.size());
    } else {
        std::string title = "  PACKAGES  ";
        std::string info  = dim("(" + std::to_string(n) + " available");
        int inst = 0;
        for (auto& p : packages_) if (db_.is_installed(p.name)) inst++;
        info += dim(" \u00b7 " + std::to_string(inst) + " installed)");
        buf_ += title + "\033[0m" + info;
    }
    buf_ += "\033[0m";

    // Column headers
    move(top + 1, left);
    buf_ += dim("  Name                  Version    Arch      Status        Source");
    hline(top + 2, left, w - 1);

    // Rows
    int vis_h = h - 4;  // space for title + header + hline + footer
    if (vis_h < 1) return;

    clamp_sel();

    // Page scroll so sel_ is always visible
    if (sel_ < scroll_)        scroll_ = sel_;
    if (sel_ >= scroll_ + vis_h) scroll_ = sel_ - vis_h + 1;

    for (int i = 0; i < vis_h; i++) {
        int idx = scroll_ + i;
        int row = top + 3 + i;
        move(row, left);
        buf_ += "\033[K";  // clear line

        if (idx >= n) continue;
        auto& p = list[idx];
        bool  sel  = (idx == sel_);
        bool  inst = db_.is_installed(p.name);

        // Format columns
        char line[256];
        std::string status_str = inst
            ? "\033[32m\u2714 installed\033[0m"
            : "\033[2mavailable\033[0m";
        std::string src = p.server_origin.empty() ? "?" : p.server_origin;

        if (sel) buf_ += "\033[1;7m";
        // Name
        std::string nm = "  " + trunc(p.name, 22);
        nm += std::string(std::max(0, 24 - (int)nm.size()), ' ');
        // Version
        std::string ver = trunc(p.version, 10);
        ver += std::string(std::max(0, 11 - (int)ver.size()), ' ');
        // Arch
        std::string arch_s = trunc(arch_to_string(p.arch), 9);
        arch_s += std::string(std::max(0, 10 - (int)arch_s.size()), ' ');

        if (sel) {
            // Plain selected row (reverse video strips colours)
            snprintf(line, sizeof(line), "%s%s%s%-12s  %s",
                     nm.c_str(), ver.c_str(), arch_s.c_str(),
                     (inst ? "* installed" : "available"),
                     src.c_str());
            buf_ += trunc(std::string(line), w - 1);
            buf_ += "\033[0m";
        } else {
            buf_ += nm + ver + arch_s;
            buf_ += status_str;
            buf_ += "  " + dim(src);
        }
    }
}

// ── Servers view ──────────────────────────────────────────────────────────────

void TuiApp::view_servers(int top, int left, int h, int w) {
    int n = (int)servers_.size();
    move(top, left);
    buf_ += bold("  SERVERS") + "  " + dim("(" + std::to_string(n) + " known)");

    move(top + 1, left);
    buf_ += dim("  Name                  Host               Port   Auth");
    hline(top + 2, left, w - 1);

    int vis_h = h - 4;
    clamp_sel();
    if (sel_ < scroll_)          scroll_ = sel_;
    if (sel_ >= scroll_ + vis_h) scroll_ = sel_ - vis_h + 1;

    for (int i = 0; i < vis_h; i++) {
        int idx = scroll_ + i;
        int row = top + 3 + i;
        move(row, left);
        buf_ += "\033[K";
        if (idx >= n) continue;
        auto& s = servers_[idx];
        bool  sel = (idx == sel_);

        std::string nm   = "  " + trunc(s.name, 22);
        nm += std::string(std::max(0, 24 - (int)nm.size()), ' ');
        std::string host = trunc(s.host, 18);
        host += std::string(std::max(0, 19 - (int)host.size()), ' ');
        std::string port = std::to_string(s.port);
        std::string auth = s.needs_password ? col_y("  [auth]") : "";

        std::string line = nm + host + port + auth;
        if (sel) buf_ += "\033[1;7m";
        buf_ += trunc(line, w - 1);
        if (sel) buf_ += "\033[0m";
    }
}

// ── Repos view ────────────────────────────────────────────────────────────────

void TuiApp::view_repos(int top, int left, int h, int w) {
    int n = (int)repos_.size();
    move(top, left);
    buf_ += bold("  REPOS") + "  " + dim("(" + std::to_string(n) + " configured)");

    move(top + 1, left);
    buf_ += dim("  Name                  URL");
    hline(top + 2, left, w - 1);

    int vis_h = h - 4;
    clamp_sel();
    if (sel_ < scroll_)          scroll_ = sel_;
    if (sel_ >= scroll_ + vis_h) scroll_ = sel_ - vis_h + 1;

    for (int i = 0; i < vis_h; i++) {
        int idx = scroll_ + i;
        int row = top + 3 + i;
        move(row, left);
        buf_ += "\033[K";
        if (idx >= n) continue;
        auto& r = repos_[idx];
        bool  sel = (idx == sel_);

        std::string nm  = "  " + trunc(r.name, 22);
        nm += std::string(std::max(0, 24 - (int)nm.size()), ' ');
        std::string url = trunc(r.url, w - 27);

        std::string line = nm + dim(url);
        if (sel) { buf_ += "\033[1;7m"; buf_ += nm + url; buf_ += "\033[0m"; }
        else     { buf_ += nm + dim(url); }
    }

    // Add-repo prompt (shown at bottom of content area)
    if (prompting_) {
        int pr = top + h - 3;
        hline(pr, left, w - 1);
        move(pr + 1, left);
        buf_ += "  " + col_c(prompt_label_) + ": " + prompt_buf_ + "_";
        buf_ += "\033[K";
    }
}

// ── Installed view ────────────────────────────────────────────────────────────

void TuiApp::view_installed(int top, int left, int h, int w) {
    int n = (int)installed_.size();
    move(top, left);
    buf_ += bold("  INSTALLED") + "  " + dim("(" + std::to_string(n) + " packages)");

    move(top + 1, left);
    buf_ += dim("  Name                  Version    Installed from");
    hline(top + 2, left, w - 1);

    int vis_h = h - 4;
    clamp_sel();
    if (sel_ < scroll_)          scroll_ = sel_;
    if (sel_ >= scroll_ + vis_h) scroll_ = sel_ - vis_h + 1;

    for (int i = 0; i < vis_h; i++) {
        int idx = scroll_ + i;
        int row = top + 3 + i;
        move(row, left);
        buf_ += "\033[K";
        if (idx >= n) continue;
        auto& p = installed_[idx];
        bool  sel = (idx == sel_);

        std::string nm  = "  " + trunc(p.name, 22);
        nm += std::string(std::max(0, 24 - (int)nm.size()), ' ');
        std::string ver = trunc(p.version, 10);
        ver += std::string(std::max(0, 11 - (int)ver.size()), ' ');
        std::string src = p.server_origin.empty() ? "" : p.server_origin;

        std::string line = nm + ver + dim(src);
        if (sel) { buf_ += "\033[1;7m"; buf_ += nm + ver + src; buf_ += "\033[0m"; }
        else     { buf_ += col_g(nm) + ver + dim(src); }
    }
}

// ── Log view ──────────────────────────────────────────────────────────────────

void TuiApp::view_log(int top, int left, int h, int w) {
    move(top, left);
    buf_ += bold("  LOG");

    hline(top + 1, left, w - 1);

    int vis_h = h - 3;
    int n     = (int)loglines_.size();

    // Show most recent lines, scrollable
    int start = std::max(0, n - vis_h - scroll_);
    int end   = std::min(n, start + vis_h);

    for (int i = 0; i < vis_h; i++) {
        move(top + 2 + i, left);
        buf_ += "\033[K";
        int idx = start + i;
        if (idx < end) {
            buf_ += "  " + dim(trunc(loglines_[idx], w - 3));
        }
    }
}

// ── Input handling ─────────────────────────────────────────────────────────────

void TuiApp::handle_key(int k) {
    if (prompting_) { handle_key_prompt(k); return; }
    if (searching_) { handle_key_search(k); return; }

    // Global keys
    switch (k) {
        case 'q': case 'Q': case TK_CTRLC: case TK_CTRLD:
            running_ = false; return;
        case TK_TAB: case TK_RIGHT:
            view_ = (View)(((int)view_ + 1) % 5);
            sel_ = scroll_ = 0; dirty_ = true; return;
        case TK_BTAB: case TK_LEFT:
            view_ = (View)(((int)view_ + 4) % 5);
            sel_ = scroll_ = 0; dirty_ = true; return;
        case '1': view_ = View::Packages;  sel_=scroll_=0; dirty_=true; return;
        case '2': view_ = View::Servers;   sel_=scroll_=0; dirty_=true; return;
        case '3': view_ = View::Repos;     sel_=scroll_=0; dirty_=true; return;
        case '4': view_ = View::Installed; sel_=scroll_=0; dirty_=true; return;
        case '5': view_ = View::Log;       sel_=scroll_=0; dirty_=true; return;
        case 'R': do_scan(); return;
        case '/': if (view_==View::Packages) { searching_=true; search_str_.clear(); rebuild_filtered(); dirty_=true; } return;
    }

    // Navigation
    switch (k) {
        case TK_UP:   if (sel_ > 0) { sel_--; dirty_=true; } return;
        case TK_DOWN: if (sel_ < list_size()-1) { sel_++; dirty_=true; } return;
        case TK_PGUP: sel_ = std::max(0, sel_ - (rows_-5)); dirty_=true; return;
        case TK_PGDN: sel_ = std::min(list_size()-1, sel_+(rows_-5)); dirty_=true; return;
        case TK_HOME: sel_=0; dirty_=true; return;
        case TK_END:  sel_=std::max(0,list_size()-1); dirty_=true; return;
    }

    // View-specific actions
    switch (view_) {
        case View::Packages:
            if (k==TK_ENTER||k=='i'||k=='I') { do_install_selected(); }
            else if (k=='d'||k=='D')          { do_download_selected(); }
            break;
        case View::Servers:
            if (k==TK_ENTER||k=='p'||k=='P') { do_ping_selected(); }
            break;
        case View::Repos:
            if (k=='a'||k=='A')              { do_add_repo(); }
            else if (k==TK_ESC||k==127||k=='d'||k=='D'||k==0x7f) { do_remove_repo(); }
            break;
        case View::Installed:
            if (k=='r'||k=='R') {
                if (!installed_.empty() && sel_ < (int)installed_.size()) {
                    set_status("Removed tracking for " + installed_[sel_].name);
                    tlog("Untracked: " + installed_[sel_].name);
                }
            }
            break;
        default: break;
    }
}

void TuiApp::handle_key_search(int k) {
    if (k == TK_ESC) {
        searching_    = false;
        search_str_.clear();
        rebuild_filtered();
        dirty_ = true;
    } else if (k == TK_ENTER) {
        searching_ = false;
        dirty_     = true;
    } else if (k == TK_BACKSP || k == 127) {
        if (!search_str_.empty()) { search_str_.pop_back(); rebuild_filtered(); dirty_=true; }
    } else if (k >= 32 && k < 127) {
        search_str_ += (char)k;
        rebuild_filtered();
        dirty_ = true;
        sel_   = 0;
        scroll_= 0;
    }
}

void TuiApp::handle_key_prompt(int k) {
    if (k == TK_ESC) {
        prompting_ = false; dirty_ = true; return;
    }
    if (k == TK_ENTER) {
        if (prompt_step_ == 0) {
            // First field done → move to second if needed
            prompt_step_  = 1;
            prompt_buf2_  = "";
            prompt_label_ = "URL";
            dirty_ = true;
        } else {
            // Done
            prompting_ = false;
            if (prompt_done_) prompt_done_(prompt_buf_, prompt_buf2_);
            dirty_ = true;
        }
        return;
    }
    if (k == TK_BACKSP || k == 127) {
        if (prompt_step_ == 0 && !prompt_buf_.empty()) { prompt_buf_.pop_back(); dirty_=true; }
        if (prompt_step_ == 1 && !prompt_buf2_.empty()){ prompt_buf2_.pop_back(); dirty_=true; }
        return;
    }
    if (k >= 32 && k < 127) {
        if (prompt_step_ == 0) prompt_buf_  += (char)k;
        else                   prompt_buf2_ += (char)k;
        dirty_ = true;
    }
}

// ── Actions ────────────────────────────────────────────────────────────────────

void TuiApp::do_scan() {
    term_restore();
    std::cout << "\033[2J\033[H";
    std::cout << "\033[36m[Scanning LAN...]\033[0m\n";

    // LAN scan
    Client cli(cli_cfg_);
    cli.cmd_scan();

    // Repo scan
    auto repo_list = db_.list_repos();
    if (!repo_list.empty()) {
        std::cout << "\n\033[36m[Fetching repo indexes...]\033[0m\n";
        for (auto& r : repo_list) {
            auto idx = repo::fetch_index(r.url, r.name);
            if (idx) {
                for (auto& rp : idx->packages) {
                    PackageInfo pi;
                    pi.name            = rp.name;
                    pi.version         = rp.version;
                    pi.description     = rp.description;
                    pi.arch            = rp.arch;
                    pi.operatingsystem = rp.operatingsystem;
                    pi.dependencies    = rp.dependencies;
                    pi.installscript   = rp.installscript;
                    pi.installcommand  = rp.installcommand;
                    pi.rivalpack       = rp.rivalpack;
                    pi.server_origin   = "[repo] " + r.name;
                    // Store download URL in file_path for later use
                    pi.file_path       = rp.download_url;
                    db_.upsert_package(pi);
                }
                std::cout << "  " << idx->name << ": "
                          << idx->packages.size() << " package(s)\n";
            }
        }
        db_.save();
    }

    std::cout << "\nPress any key to return...";
    std::cout.flush();
    term_raw();
    term_read();
    load_data();
    dirty_ = true;
    tlog("Scan complete");
}

void TuiApp::do_install_selected() {
    const auto& list = searching_ ? filtered_ : packages_;
    if (list.empty() || sel_ >= (int)list.size()) return;
    auto& p = list[sel_];

    term_restore();
    std::cout << "\033[2J\033[H";
    std::cout << "\033[1;36mInstalling: " << p.name << "\033[0m\n\n";

    Client cli(cli_cfg_);
    cli.cmd_install(p.name, false);

    std::cout << "\nPress any key to continue...";
    std::cout.flush();
    term_raw();
    term_read();
    load_data();
    dirty_ = true;
    tlog("Installed: " + p.name);
}

void TuiApp::do_download_selected() {
    const auto& list = searching_ ? filtered_ : packages_;
    if (list.empty() || sel_ >= (int)list.size()) return;
    auto& p = list[sel_];

    term_restore();
    std::cout << "\033[2J\033[H";
    std::cout << "\033[1;36mDownloading: " << p.name << "\033[0m\n\n";

    Client cli(cli_cfg_);
    cli.cmd_download(p.name, false);

    std::cout << "\nPress any key to continue...";
    std::cout.flush();
    term_raw();
    term_read();
    dirty_ = true;
    tlog("Downloaded: " + p.name);
}

void TuiApp::do_ping_selected() {
    if (servers_.empty() || sel_ >= (int)servers_.size()) return;
    auto& s = servers_[sel_];

    term_restore();
    std::cout << "\033[2J\033[H";
    std::cout << "\033[1;36mPinging: " << s.name << "\033[0m\n\n";

    Client cli(cli_cfg_);
    cli.cmd_ping(s.name);

    std::cout << "\nPress any key to continue...";
    std::cout.flush();
    term_raw();
    term_read();
    dirty_ = true;
}

void TuiApp::do_add_repo() {
    prompting_    = true;
    prompt_step_  = 0;
    prompt_buf_.clear();
    prompt_buf2_.clear();
    prompt_label_ = "Repo name";

    prompt_done_ = [this](const std::string& name, const std::string& url) {
        if (name.empty() || url.empty()) {
            set_status("Add repo cancelled (empty input)");
            return;
        }
        RepoInfo ri;
        ri.name = name;
        ri.url  = url;
        db_.upsert_repo(ri);
        db_.save();
        repos_ = db_.list_repos();
        set_status("Added repo: " + name + " → " + url);
        tlog("Added repo: " + name);
    };

    dirty_ = true;
}

void TuiApp::do_remove_repo() {
    if (repos_.empty() || sel_ >= (int)repos_.size()) return;
    std::string name = repos_[sel_].name;
    db_.remove_repo(name);
    db_.save();
    repos_ = db_.list_repos();
    if (sel_ >= (int)repos_.size()) sel_ = std::max(0, (int)repos_.size()-1);
    set_status("Removed repo: " + name);
    tlog("Removed repo: " + name);
    dirty_ = true;
}

// ── Data ──────────────────────────────────────────────────────────────────────

void TuiApp::load_data() {
    db_.load();
    packages_  = db_.list_packages();
    servers_   = db_.list_servers();
    repos_     = db_.list_repos();
    installed_.clear();
    for (auto& p : packages_)
        if (db_.is_installed(p.name)) installed_.push_back(p);
    rebuild_filtered();
}

void TuiApp::rebuild_filtered() {
    if (search_str_.empty()) {
        filtered_ = packages_;
        return;
    }
    std::string q = search_str_;
    std::transform(q.begin(), q.end(), q.begin(), ::tolower);
    filtered_.clear();
    for (auto& p : packages_) {
        std::string n = p.name; std::transform(n.begin(),n.end(),n.begin(),::tolower);
        std::string d = p.description; std::transform(d.begin(),d.end(),d.begin(),::tolower);
        if (n.find(q)!=std::string::npos || d.find(q)!=std::string::npos)
            filtered_.push_back(p);
    }
}

int TuiApp::list_size() const {
    switch (view_) {
        case View::Packages:  return (int)(searching_ ? filtered_.size() : packages_.size());
        case View::Servers:   return (int)servers_.size();
        case View::Repos:     return (int)repos_.size();
        case View::Installed: return (int)installed_.size();
        case View::Log:       return (int)loglines_.size();
    }
    return 0;
}

void TuiApp::clamp_sel() {
    int n = list_size();
    if (n == 0) { sel_ = 0; return; }
    if (sel_ < 0)   sel_ = 0;
    if (sel_ >= n)  sel_ = n - 1;
}

void TuiApp::tlog(const std::string& msg) {
    auto now  = std::chrono::system_clock::now();
    auto t    = std::chrono::system_clock::to_time_t(now);
    char ts[16]; strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&t));
    loglines_.push_back(std::string(ts) + "  " + msg);
    if (loglines_.size() > 200) loglines_.erase(loglines_.begin());
}

void TuiApp::set_status(const std::string& msg) {
    status_ = msg;
    tlog(msg);
    dirty_  = true;
}

// ── Main loop ─────────────────────────────────────────────────────────────────

int TuiApp::run() {
    term_raw();
    term_size(rows_, cols_);
    load_data();

    tlog("Deliver TUI started  v" + std::string(dlr::VERSION));
    tlog("Use Tab to switch views, q to quit");
    set_status("Welcome to Deliver!  Press R to scan, Tab to navigate.");

    while (running_) {
        // Handle terminal resize
        if (g_resize_flag.exchange(false)) {
            term_size(rows_, cols_);
            dirty_ = true;
        }

        if (dirty_) render();

        int k = term_read();
        if (k == -1) continue;  // timeout, loop again (handles resize)
        handle_key(k);
        dirty_ = true;
    }

    term_restore();
    std::cout << "\n\033[1;36mGoodbye from Deliver!\033[0m\n";
    return 0;
}

} // namespace dlr