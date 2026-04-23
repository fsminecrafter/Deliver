#include "client.hpp"
#include "logger.hpp"
#include "crypto.hpp"
#include "network.hpp"
#include "pkg_parser.hpp"
#include "tar.hpp"
#include <iostream>
#include <iomanip>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <cstdlib>
#include <sstream>
#include <cmath>

#pragma execution_character_set("utf-8")

namespace fs = std::filesystem;

namespace dlr {


// ── Progress bar renderer ──────────────────────────────────────────────────────


static std::string human_size(uintmax_t bytes) {
    const char* units[] = {"B","KB","MB","GB"};
    double val = (double)bytes;
    int u = 0;
    while (val >= 1024.0 && u < 3) { val /= 1024.0; u++; }
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(u > 0 ? 1 : 0) << val << " " << units[u];
    return ss.str();
}

static void print_progress(const std::string& label,
                          uintmax_t current, uintmax_t total,
                          double speed_bytes_per_sec = 0) {
    const int BAR_WIDTH = 38;
    static size_t last_line_length = 0;

    double progress = (total > 0) ? (double)current / total : 0.0;
    progress = std::min(progress, 1.0);

    int pct = (int)(progress * 100.0);

    const int TOTAL_TICKS = BAR_WIDTH * 8;
    int ticks = (int)(progress * TOTAL_TICKS);

    int full_blocks = ticks / 8;
    int remainder   = ticks % 8;

    static const char* partials[] = {
        "", "▏","▎","▍","▌","▋","▊","▉"
    };

    std::string bar;
    int visible_width = 0;

    for (int i = 0; i < full_blocks; ++i) {
        bar += "▉";
        visible_width++;
    }

    if (full_blocks < BAR_WIDTH && remainder > 0) {
        bar += partials[remainder];
        visible_width++;
    }

    int remaining = BAR_WIDTH - visible_width;
    for (int i = 0; i < remaining; ++i) {
        bar += "░";
    }

    std::ostringstream out;

#ifndef _WIN32
    out << "\033[36m" << std::setw(18) << std::left << label << "\033[0m ";
    out << "\033[90m[\033[0m";
    out << "\033[32m" << bar << "\033[0m";
    out << "\033[90m]\033[0m";
    out << " \033[1m" << std::setw(3) << pct << "%\033[0m";
    out << "  " << human_size(current) << "/" << human_size(total);

    if (speed_bytes_per_sec > 0)
        out << "  \033[33m"
            << human_size((uintmax_t)speed_bytes_per_sec)
            << "/s\033[0m";
#else
    int filled = (int)(BAR_WIDTH * progress);
    out << std::setw(18) << std::left << label << " ";
    out << "[" << std::string(filled,'=')
        << std::string(BAR_WIDTH-filled,' ') << "]";
    out << " " << std::setw(3) << pct << "%";
    out << "  " << human_size(current) << "/" << human_size(total);
#endif

    std::string line = out.str();

    size_t pad = 0;
    if (last_line_length > line.size())
        pad = last_line_length - line.size();

    std::cout << "\r" << line << std::string(pad, ' ') << std::flush;

    last_line_length = line.size();
}

static void print_progress_done(const std::string& label, uintmax_t total,
                                 double elapsed_sec) {
    std::cout << "\r";
#ifndef _WIN32
    std::cout << "\033[36m" << std::setw(18) << std::left << label << "\033[0m ";
    std::cout << "\033[32m[";
    std::cout << std::string(38, '▉') << "] 100%\033[0m";
    std::cout << "  " << human_size(total);
    if (elapsed_sec > 0)
        std::cout << "  \033[33m" << human_size((uintmax_t)(total/elapsed_sec)) << "/s\033[0m";
    std::cout << "  \033[32m✓\033[0m";
#else
    std::cout << std::setw(18) << std::left << label << " ";
    std::cout << "[" << std::string(38,'=') << "] 100%";
    std::cout << "  " << human_size(total) << "  OK";
#endif
    std::cout << "\n";
}

static void spinner_step(const std::string& msg, int step) {
    const char* frames[] = {"⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"};
#ifndef _WIN32
    std::cout << "\r\033[36m" << frames[step % 10] << "\033[0m " << msg << "   " << std::flush;
#else
    const char* wframes[] = {"-","\\","|","/"};
    std::cout << "\r" << wframes[step % 4] << " " << msg << "   " << std::flush;
#endif
}


// ── Installation step tracker ──────────────────────────────────────────────────


static void install_step(int step, int total, const std::string& msg) {
#ifndef _WIN32
    std::cout << "\033[32m[" << step << "/" << total << "]\033[0m " << msg << "\n";
#else
    std::cout << "[" << step << "/" << total << "] " << msg << "\n";
#endif
}

static void install_ok(const std::string& msg) {
#ifndef _WIN32
    std::cout << "  \033[32m✓\033[0m " << msg << "\n";
#else
    std::cout << "  OK " << msg << "\n";
#endif
}

static void install_warn(const std::string& msg) {
#ifndef _WIN32
    std::cout << "  \033[33m⚠\033[0m " << msg << "\n";
#else
    std::cout << "  WARN " << msg << "\n";
#endif
}

static void print_divider(char c = '-', int w = 60) {
    std::cout << std::string(w, c) << "\n";
}


// ── Cache cleanup ──────────────────────────────────────────────────────────────

static void clean_cache(const std::string& cache_dir) {
    std::error_code ec;
    if (!fs::exists(cache_dir)) return;

    int removed = 0;
    for (auto& entry : fs::directory_iterator(cache_dir, ec)) {
        if (ec) break;
        // Remove .tar files and stale extract dirs
        auto& p = entry.path();
        if (p.extension() == ".tar" || p.filename().string().rfind(".extract_", 0) == 0) {
            fs::remove_all(p, ec);
            if (!ec) removed++;
        }
    }
    if (removed > 0)
        log_debug("Cache clean: removed " + std::to_string(removed) + " stale file(s) from " + cache_dir);
}


// ── Client ctor ────────────────────────────────────────────────────────────────


Client::Client(ClientConfig cfg)
    : cfg_(std::move(cfg))
    , db_(cfg_.db_dir)
{
    net::init();
    std::error_code ec;
    fs::create_directories(cfg_.cache_dir, ec);
    if (ec) {
        log_warn("Cannot create cache dir: " + cfg_.cache_dir + " (" + ec.message() + ")");
    }

    // Clean stale cache entries on every startup
    clean_cache(cfg_.cache_dir);

    db_.load();
}


// ── Internal helpers ───────────────────────────────────────────────────────────


static socket_t connect_and_handshake(const ServerInfo& srv,
                                       std::vector<uint8_t>& out_key,
                                       const std::string& password = "") {
    socket_t fd = net::connect_to(srv.host, srv.port, 4000);
    if (fd == INVALID_SOCK) {
        log_error("Cannot connect to '" + srv.name + "' @ " + srv.host + ":" + std::to_string(srv.port));
        return INVALID_SOCK;
    }

    auto hello = net::recv_frame(fd);
    if (hello.empty()) { close_socket(fd); return INVALID_SOCK; }
    log_debug("Hello: " + std::string(hello.begin(), hello.end()));

    auto session_key = crypto::generate_key();
    std::string key_msg = "KEY:" + crypto::b64_encode(session_key);
    net::send_frame(fd, std::vector<uint8_t>(key_msg.begin(), key_msg.end()));

    if (srv.needs_password) {
        auto auth_req = net::recv_frame(fd);
        if (auth_req.empty() || (MsgType)auth_req[0] != MsgType::AUTH_REQUEST) {
            close_socket(fd); return INVALID_SOCK;
        }
        std::string pw = password;
        if (pw.empty()) {
            std::cout << "Password for " << srv.name << ": ";
            std::getline(std::cin, pw);
        }
        net::send_frame(fd, std::vector<uint8_t>(pw.begin(), pw.end()));
        auto ack = net::recv_frame(fd);
        if (ack.empty() || (MsgType)ack[0] != MsgType::HELLO_ACK) {
            log_error("Authentication failed for server: " + srv.name);
            close_socket(fd); return INVALID_SOCK;
        }
    }

    out_key = session_key;
    return fd;
}

static std::vector<uint8_t> make_msg(MsgType t, const std::string& body,
                                      const std::vector<uint8_t>& key) {
    std::vector<uint8_t> msg{(uint8_t)t};
    msg.insert(msg.end(), body.begin(), body.end());
    return key.empty() ? msg : crypto::encrypt(msg, key);
}

static std::vector<uint8_t> recv_dec(socket_t fd, const std::vector<uint8_t>& key) {
    auto frame = net::recv_frame(fd);
    if (frame.empty()) return {};
    if (!key.empty()) {
        try { return crypto::decrypt(frame, key); } catch (...) { return {}; }
    }
    return frame;
}

// ── Server discovery ───────────────────────────────────────────────────────────

std::optional<ServerInfo> Client::find_server_for_package(const std::string& pkg_name) {
    auto cached = db_.find_package(pkg_name);
    if (cached && !cached->server_origin.empty()) {
        auto srv = db_.find_server(cached->server_origin);
        if (srv && !srv->host.empty()) return srv;
    }

    auto servers = net::discover_servers(2500);
    for (auto& srv : servers) {
        std::vector<uint8_t> key;
        socket_t fd = connect_and_handshake(srv, key);
        if (fd == INVALID_SOCK) continue;

        net::send_frame(fd, make_msg(MsgType::SEARCH_REQUEST, pkg_name, key));
        auto resp = recv_dec(fd, key);
        close_socket(fd);

        if (resp.size() < 2) continue;
        std::string body(resp.begin()+1, resp.end());
        if (body != "NONE" && body.find(pkg_name) != std::string::npos) {
            srv.reachable = true;
            db_.upsert_server(srv);
            db_.save();
            return srv;
        }
    }
    return std::nullopt;
}

// ── Download ───────────────────────────────────────────────────────────────────

std::string Client::download_from_server(const ServerInfo& srv, const std::string& pkg_name) {
    std::vector<uint8_t> key;
    socket_t fd = connect_and_handshake(srv, key);
    if (fd == INVALID_SOCK) return "";

    net::send_frame(fd, make_msg(MsgType::INSTALL_REQUEST, pkg_name, key));

    std::string out_path = cfg_.cache_dir + "/" + pkg_name + ".tar";
    std::ofstream out(out_path, std::ios::binary);
    if (!out) { log_error("Cannot create: " + out_path); close_socket(fd); return ""; }

    std::string expected_checksum;
    uintmax_t expected_size = 0, received = 0;
    auto t_start = std::chrono::steady_clock::now();
    auto t_last  = t_start;
    uintmax_t bytes_since_last = 0;
    double speed = 0;

    while (true) {
        auto msg = recv_dec(fd, key);
        if (msg.empty()) { log_error("Connection lost during download"); break; }

        MsgType type = (MsgType)msg[0];
        const char* body_data = (const char*)msg.data() + 1;
        size_t body_len = msg.size() - 1;
        std::string body_str(body_data, body_len);

        if (type == MsgType::INSTALL_ERROR) {
            std::cout << "\n";
            log_error("Server: " + body_str);
            close_socket(fd); out.close(); fs::remove(out_path); return "";
        }
        if (type == MsgType::INSTALL_END) {
            expected_checksum = body_str;
            break;
        }
        if (type == MsgType::INSTALL_DATA) {
            if (body_str.rfind("SIZE:", 0) == 0) {
                expected_size = std::stoull(body_str.substr(5));
                std::cout << "\n";
                print_divider();
                std::cout << bold("  Downloading  ") << cyan(pkg_name)
                          << "  from " << srv.name << "  (" << human_size(expected_size) << ")\n";
                print_divider();
            } else {
                out.write(body_data, body_len);
                received += body_len;
                bytes_since_last += body_len;

                auto now = std::chrono::steady_clock::now();
                double dt = std::chrono::duration<double>(now - t_last).count();
                if (dt >= 0.25) {
                    speed = bytes_since_last / dt;
                    bytes_since_last = 0;
                    t_last = now;
                }

                if (expected_size > 0) {
                    print_progress("  Downloading", received, expected_size, speed);
                }
            }
        }
    }
    out.close();
    close_socket(fd);

    if (expected_size > 0 && received > 0) {
        double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t_start).count();
        print_progress_done("  Downloading", received, elapsed);
    }

    if (!expected_checksum.empty()) {
        std::cout << "  Verifying checksum...";
        std::cout << std::flush;
        std::string actual = crypto::sha256_hex_file(out_path);
        if (actual != expected_checksum) {
            std::cout << "\n";
            log_error("Checksum mismatch! File may be corrupted.");
            log_error("  Expected: " + expected_checksum);
            log_error("  Got:      " + actual);
            fs::remove(out_path);
            return "";
        }
        std::cout << " " << green("✓ OK") << "\n";
    }

    return out_path;
}


// ── Install from tar ───────────────────────────────────────────────────────────


int Client::install_tar(const std::string& tar_path, bool auto_yes) {
    const int TOTAL_STEPS = 5;

    std::string extract_dir = cfg_.cache_dir + "/.extract_" + std::to_string(::getpid());
    fs::remove_all(extract_dir);
    fs::create_directories(extract_dir);

    install_step(1, TOTAL_STEPS, "Extracting package archive...");
    if (!tar::extract(tar_path, extract_dir)) {
        log_error("Failed to extract package archive");
        fs::remove_all(extract_dir);
        return 1;
    }
    install_ok("Extracted successfully");

    std::string pkg_file;
    for (auto& e : fs::recursive_directory_iterator(extract_dir)) {
        if (e.path().extension() == ".pkg") {
            if (!pkg_file.empty()) {
                log_error("Multiple .pkg files found — only 1 allowed per package");
                fs::remove_all(extract_dir);
                return 1;
            }
            pkg_file = e.path().string();
        }
    }
    if (pkg_file.empty()) {
        log_error("No .pkg manifest found inside archive");
        fs::remove_all(extract_dir);
        return 1;
    }

    install_step(2, TOTAL_STEPS, "Reading package manifest...");
    auto info = parse_pkg(pkg_file);
    if (!info) { fs::remove_all(extract_dir); return 1; }

    std::cout << "\n";
    print_divider('=');
#ifndef _WIN32
    std::cout << "  \033[1;36m" << info->name << "\033[0m  v" << info->version << "\n";
#else
    std::cout << "  " << info->name << "  v" << info->version << "\n";
#endif
    if (!info->description.empty())
        std::cout << "  " << info->description << "\n";
    std::cout << "  Arch: " << arch_to_string(info->arch)
              << "   OS: " << os_to_string(info->operatingsystem) << "\n";
    if (!info->dependencies.empty()) {
        std::cout << "  Deps: ";
        for (size_t i=0; i<info->dependencies.size(); i++) {
            if (i) std::cout << ", ";
            std::cout << cyan(info->dependencies[i]);
        }
        std::cout << "\n";
    }
    if (!info->rivalpack.empty())
        std::cout << "  " << yellow("⚠ Conflicts with: " + info->rivalpack) << "\n";
    print_divider('=');
    std::cout << "\n";

    if (!pkg_compatible(info->arch, info->operatingsystem)) {
        log_error("Package is not compatible with this system.");
        log_error("  Package : arch=" + arch_to_string(info->arch)
                  + "  os=" + os_to_string(info->operatingsystem));
        log_error("  Host    : arch=" + arch_to_string(host_arch())
                  + "  os=" + os_to_string(host_os()));
        fs::remove_all(extract_dir);
        return 1;
    }
    install_ok("Compatible with this system  ("
               + arch_to_string(host_arch()) + " / " + os_to_string(host_os()) + ")");

    if (!info->rivalpack.empty() && db_.is_installed(info->rivalpack)) {
        install_warn("Rival package '" + info->rivalpack + "' is already installed — conflicts possible.");
        if (!auto_yes) {
            std::cout << "  Continue anyway? [y/N] ";
            std::string ans; std::getline(std::cin, ans);
            if (ans != "y" && ans != "Y") { fs::remove_all(extract_dir); return 1; }
        }
    }

    for (auto& dep_str : info->dependencies) {
        auto c = parse_dependency(dep_str);
        if (db_.is_installed(c.name)) {
            std::string iv = db_.installed_version(c.name);
            if (!satisfies(iv, c)) {
                install_warn("Dependency conflict: " + c.name + " installed=" + iv
                             + " required=" + dep_str);
            } else {
                install_ok("Dependency satisfied: " + dep_str);
            }
        } else {
            install_warn("Dependency not installed: " + dep_str + "  (may need: dlr install " + c.name + ")");
        }
    }

    if (!auto_yes) {
        std::cout << "\n  Proceed with installation? [y/N] ";
        std::string ans; std::getline(std::cin, ans);
        if (ans != "y" && ans != "Y") { std::cout << "  Aborted.\n"; fs::remove_all(extract_dir); return 1; }
    }

    install_step(3, TOTAL_STEPS, "Installing files...");
    fs::path install_path = fs::path(cfg_.install_dir) / info->name;
    fs::create_directories(install_path);

    std::vector<fs::path> files_to_copy;
    for (auto& e : fs::recursive_directory_iterator(extract_dir)) {
        if (fs::is_regular_file(e) && e.path().extension() != ".pkg")
            files_to_copy.push_back(e.path());
    }

    uintmax_t copied = 0, total_files = files_to_copy.size();
    for (auto& src : files_to_copy) {
        fs::path rel = fs::relative(src, extract_dir);
        fs::path dst = install_path / rel;
        fs::create_directories(dst.parent_path());
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
        copied++;
        print_progress("  Copying files", copied, total_files);
    }
    if (total_files > 0) {
        std::cout << "\r";
        install_ok("Copied " + std::to_string(total_files) + " file(s) to " + install_path.string());
    }

    install_step(4, TOTAL_STEPS, "Running install hooks...");
    bool ran_anything = false;

    if (!info->installscript.empty()) {
        fs::path script = install_path / info->installscript;
        if (fs::exists(script)) {
#ifndef _WIN32
            fs::permissions(script, fs::perms::owner_exec | fs::perms::group_exec |
                                     fs::perms::others_exec, fs::perm_options::add);
            std::string cmd = "bash \"" + script.string() + "\"";
#else
            std::string cmd = "\"" + script.string() + "\"";
#endif
            std::cout << "  Running: " << script.filename().string() << "\n";
            int r = system(cmd.c_str());
            if (r != 0) install_warn("Script exited with code " + std::to_string(r));
            else        install_ok("Script completed");
            ran_anything = true;
        } else {
            install_warn("Install script not found: " + info->installscript);
        }
    }

    if (!info->installcommand.empty()) {
        std::cout << "  Running: " << info->installcommand << "\n";
        int r = system(info->installcommand.c_str());
        if (r != 0) install_warn("Command exited with code " + std::to_string(r));
        else        install_ok("Command completed");
        ran_anything = true;
    }

    if (!ran_anything) install_ok("No install hooks defined");

    install_step(5, TOTAL_STEPS, "Registering package...");
    db_.mark_installed(info->name, info->version);
    db_.upsert_package(*info);
    db_.save();
    install_ok("Registered " + info->name + " v" + info->version);

    fs::remove_all(extract_dir);

    std::cout << "\n";
    print_divider('=');
#ifndef _WIN32
    std::cout << "  \033[1;32m✓ Successfully installed " << info->name
              << " v" << info->version << "\033[0m\n";
#else
    std::cout << "  Successfully installed " << info->name << " v" << info->version << "\n";
#endif
    print_divider('=');
    std::cout << "\n";
    return 0;
}


// ── Public commands ────────────────────────────────────────────────────────────


int Client::cmd_install(const std::string& pkg_name, bool auto_yes) {
    if (db_.is_installed(pkg_name)) {
        std::cout << yellow("Package '" + pkg_name + "' is already installed")
                  << " (v" << db_.installed_version(pkg_name) << ")\n";
        return 0;
    }

    std::cout << "\n" << bold("Installing: ") << cyan(pkg_name) << "\n";

    int spin = 0;
    spinner_step("Searching LAN...", spin++);
    auto srv = find_server_for_package(pkg_name);
    std::cout << "\r" << std::string(50, ' ') << "\r";

    if (!srv) {
        log_error("Package '" + pkg_name + "' not found on any LAN server.");
        std::cout << "  Tip: run " << bold("dlr scan") << " to refresh server/package database.\n\n";
        return 1;
    }

    std::cout << "  Found on server: " << green(srv->name) << " (" << srv->host << ")\n";
    std::string tar_path = download_from_server(*srv, pkg_name);
    if (tar_path.empty()) return 1;

    return install_tar(tar_path, auto_yes);
}

// ── cmd_download: downloads AND extracts into the current working directory ────

int Client::cmd_download(const std::string& pkg_name, bool auto_yes) {
    std::cout << "\n" << bold("Downloading: ") << cyan(pkg_name) << "\n";

    int spin = 0;
    spinner_step("Searching LAN...", spin++);
    auto srv = find_server_for_package(pkg_name);
    std::cout << "\r" << std::string(50, ' ') << "\r";

    if (!srv) {
        log_error("Package '" + pkg_name + "' not found.");
        return 1;
    }

    std::string tar_path = download_from_server(*srv, pkg_name);
    if (tar_path.empty()) return 1;

    // Extract into the directory where the user ran the command
    fs::path cwd = fs::current_path();
    fs::path dest = cwd / pkg_name;

    std::cout << "\n";
    print_divider();
    install_step(1, 2, "Extracting package to current directory...");
    std::cout << "  Destination: " << dest.string() << "\n";

    std::error_code ec;
    fs::create_directories(dest, ec);
    if (ec) {
        log_error("Cannot create directory '" + dest.string() + "': " + ec.message());
        return 1;
    }

    if (!tar::extract(tar_path, dest.string())) {
        log_error("Failed to extract archive");
        return 1;
    }
    install_ok("Extracted successfully");

    install_step(2, 2, "Cleaning up...");
    fs::remove(tar_path, ec);
    install_ok("Done");

    print_divider();
    std::cout << "\n" << green("✓ Package extracted to: ") << bold(dest.string()) << "\n\n";
    return 0;
}

int Client::cmd_scan() {
    std::cout << "\n" << bold("Scanning LAN for Deliver servers") << "...\n";
    print_divider();

    int spin = 0;
    for (int i = 0; i < 6; i++) {
        spinner_step("Broadcasting discovery...", spin++);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    auto servers = net::discover_servers(3000);
    std::cout << "\r" << std::string(50,' ') << "\r";

    if (servers.empty()) {
        std::cout << yellow("No servers found on LAN.\n");
        std::cout << "  Make sure dlr_server is running on at least one machine.\n\n";
        return 0;
    }

    int total_pkgs = 0;
    for (auto& srv : servers) {
        std::cout << "\n  " << green("●") << " " << bold(srv.name)
                  << "  " << srv.host << ":" << srv.port;
        if (srv.needs_password) std::cout << "  " << yellow("[auth]");
        std::cout << "\n";

        std::vector<uint8_t> key;
        socket_t fd = connect_and_handshake(srv, key);
        if (fd == INVALID_SOCK) {
            std::cout << "    " << red("✗ Connection failed") << "\n";
            continue;
        }

        net::send_frame(fd, make_msg(MsgType::PKG_LIST, "", key));
        auto resp = recv_dec(fd, key);
        close_socket(fd);

        if (resp.size() > 1) {
            std::string body(resp.begin()+1, resp.end());
            std::istringstream ss(body);
            std::string line;
            int count = 0;
            while (std::getline(ss, line)) {
                if (line.empty()) continue;
                auto p1 = line.find('|');
                if (p1 == std::string::npos) continue;
                auto p2 = line.find('|', p1+1);
                auto p3 = (p2!=std::string::npos) ? line.find('|', p2+1) : std::string::npos;

                PackageInfo info;
                info.name        = line.substr(0, p1);
                info.version     = line.substr(p1+1, (p2!=std::string::npos?p2-p1-1:std::string::npos));
                info.description = (p2!=std::string::npos && p3==std::string::npos) ?
                                   line.substr(p2+1) : "";
                if (p2!=std::string::npos && p3!=std::string::npos) {
                    info.description = line.substr(p2+1, p3-p2-1);
                }
                info.server_origin = srv.name;
                db_.upsert_package(info);

                std::cout << "    " << cyan(info.name) << "  v" << info.version;
                if (!info.description.empty()) std::cout << "  " << info.description;
                if (db_.is_installed(info.name)) std::cout << "  " << green("(installed)");
                std::cout << "\n";
                count++;
            }
            total_pkgs += count;
        }

        db_.upsert_server(srv);
        srv.reachable = true;
    }

    db_.save();
    std::cout << "\n";
    print_divider();
    std::cout << green("✓") << " Scan complete — "
              << servers.size() << " server(s), "
              << total_pkgs << " package(s) indexed.\n\n";
    return 0;
}

int Client::cmd_ping(const std::string& server_name) {
    ServerInfo srv;
    auto srv_opt = db_.find_server(server_name);
    if (srv_opt) {
        srv = *srv_opt;
    } else {
        auto servers = net::discover_servers(2000);
        bool found = false;
        for (auto& s : servers) {
            if (s.name == server_name) { srv = s; found = true; break; }
        }
        if (!found) {
            log_error("Server '" + server_name + "' not found. Run 'dlr scan' first.");
            return 1;
        }
    }

    for (int i = 0; i < 4; i++) {
        std::vector<uint8_t> key;
        auto t0 = std::chrono::steady_clock::now();
        socket_t fd = connect_and_handshake(srv, key);
        if (fd == INVALID_SOCK) {
            std::cout << "  " << red("Request timeout") << "\n";
            continue;
        }

        net::send_frame(fd, make_msg(MsgType::PING, "PING", key));
        auto resp = recv_dec(fd, key);
        auto t1 = std::chrono::steady_clock::now();
        close_socket(fd);

        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        if (resp.empty() || (MsgType)resp[0] != MsgType::PONG) {
            std::cout << "  " << red("No response") << "\n";
        } else {
            std::cout << green("  PONG") << " from " << bold(server_name)
                      << "  (" << srv.host << ")  " << ms << " ms\n";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return 0;
}

int Client::cmd_search(const std::string& query) {
    auto results = db_.search_packages(query);
    std::cout << "\n";
    if (results.empty()) {
        std::cout << yellow("No packages found matching: ") << bold(query) << "\n";
        std::cout << "  Tip: run " << bold("dlr scan") << " to refresh the package database.\n\n";
        return 0;
    }

    print_divider();
    std::cout << "  " << bold("Results for: ") << cyan(query)
              << "  (" << results.size() << " found)\n";
    print_divider();

    for (auto& p : results) {
        std::cout << "  " << std::left << std::setw(22) << cyan(p.name)
                  << "  v" << std::setw(8) << p.version;
        if (!p.description.empty())
            std::cout << "  " << p.description.substr(0, 35);
        std::cout << "\n";
        std::cout << "    " << arch_to_string(p.arch) << " / " << os_to_string(p.operatingsystem);
        if (!p.server_origin.empty()) std::cout << "  [" << p.server_origin << "]";
        if (db_.is_installed(p.name)) std::cout << "  " << green("✓ installed");
        std::cout << "\n";
    }
    print_divider();
    std::cout << "\n";
    return 0;
}

int Client::cmd_servers(const std::string& query) {
    auto results = query.empty() ? db_.list_servers() : db_.search_servers(query);
    std::cout << "\n";
    if (results.empty()) {
        std::cout << yellow("No servers found");
        if (!query.empty()) std::cout << " matching: " << bold(query);
        std::cout << "\n  Run " << bold("dlr scan") << " to discover servers.\n\n";
        return 0;
    }

    print_divider();
    std::cout << "  " << bold("Known Servers") << "\n";
    print_divider();
    for (auto& s : results) {
        std::cout << "  " << green("●") << " " << std::left << std::setw(20) << bold(s.name)
                  << "  " << s.host << ":" << s.port;
        if (s.needs_password) std::cout << "  " << yellow("[auth required]");
        std::cout << "\n";
    }
    print_divider();
    std::cout << "\n";
    return 0;
}

int Client::cmd_list() {
    auto pkgs = db_.list_packages();
    std::cout << "\n";
    print_divider('=');
    std::cout << "  " << bold("Available Packages") << "\n";
    print_divider('=');

    if (pkgs.empty()) {
        std::cout << "  " << yellow("No packages in database.") << "\n";
        std::cout << "  Run " << bold("dlr scan") << " to discover packages.\n";
    } else {
        std::cout << "  " << std::left
                  << std::setw(22) << bold("Name")
                  << std::setw(10) << "Version"
                  << std::setw(10) << "Arch"
                  << std::setw(20) << "OS"
                  << "Server\n";
        print_divider();

        for (auto& p : pkgs) {
            bool installed = db_.is_installed(p.name);
            std::string name_col = installed ? green(p.name + " ✓") : p.name;
            std::cout << "  " << std::left
                      << std::setw(22) << name_col
                      << std::setw(10) << p.version
                      << std::setw(10) << arch_to_string(p.arch)
                      << std::setw(20) << os_to_string(p.operatingsystem)
                      << p.server_origin << "\n";
            if (!p.description.empty())
                std::cout << "    " << p.description << "\n";
        }
        print_divider('-');
        std::cout << "  " << pkgs.size() << " package(s)";
        long installed_count = 0;
        for (auto& p : pkgs) if (db_.is_installed(p.name)) installed_count++;
        std::cout << "  (" << installed_count << " installed)\n";
    }

    print_divider('=');
    std::cout << "\n";
    return 0;
}


// ── testspinner: animate the spinner for N seconds ────────────────────────────

int Client::cmd_testspinner(int duration_secs) {
    if (duration_secs <= 0) duration_secs = 5;

    std::cout << "\n" << bold("Test Spinner") << "  (" << duration_secs << "s)\n";
    print_divider();

    auto t_end = std::chrono::steady_clock::now() + std::chrono::seconds(duration_secs);
    int frame = 0;
    while (std::chrono::steady_clock::now() < t_end) {
        int remaining_ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
            t_end - std::chrono::steady_clock::now()).count();
        spinner_step("Running spinner test... (" + std::to_string(remaining_ms / 1000 + 1) + "s)", frame++);
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }
    std::cout << "\r" << std::string(60, ' ') << "\r";
    print_divider();
    std::cout << green("✓") << " Spinner test complete.\n\n";
    return 0;
}


// ── testinstall: simulate a full install sequence for N seconds ───────────────

int Client::cmd_testinstall(const std::string& pkg_name, int duration_secs) {
    if (duration_secs <= 0) duration_secs = 5;

    // Divide time budget across phases
    // Phase 1: spinner/discovery  (15%)
    // Phase 2: download progress  (40%)
    // Phase 3: copy files         (25%)
    // Phase 4: install steps      (20%)
    auto total_ms = (long long)(duration_secs) * 1000LL;
    auto phase1_ms = total_ms * 15 / 100;
    auto phase2_ms = total_ms * 40 / 100;
    auto phase3_ms = total_ms * 25 / 100;
    auto phase4_ms = total_ms - phase1_ms - phase2_ms - phase3_ms;

    const int TICK_MS = 50;

    std::cout << "\n" << bold("Test Install: ") << cyan(pkg_name)
              << "  (" << duration_secs << "s simulation)\n";
    print_divider();

    // ── Phase 1: discovery spinner ────────────────────────────────────────────
    {
        long long ticks = phase1_ms / TICK_MS;
        for (long long i = 0; i < ticks; i++) {
            spinner_step("Searching LAN for '" + pkg_name + "'...", (int)i);
            std::this_thread::sleep_for(std::chrono::milliseconds(TICK_MS));
        }
        std::cout << "\r" << std::string(60, ' ') << "\r";
        std::cout << "  Found on server: " << green("test-server") << " (127.0.0.1)\n";
    }

    // ── Phase 2: download progress bar ───────────────────────────────────────
    {
        uintmax_t fake_size = 8ULL * 1024 * 1024; // 8 MB fake file
        uintmax_t received  = 0;
        long long ticks     = phase2_ms / TICK_MS;
        uintmax_t per_tick  = fake_size / (ticks > 0 ? ticks : 1);
        double    fake_speed = (double)fake_size / ((double)phase2_ms / 1000.0);

        std::cout << "\n";
        print_divider();
        std::cout << bold("  Downloading  ") << cyan(pkg_name)
                  << "  from test-server  (" << human_size(fake_size) << ")\n";
        print_divider();

        auto t_dl_start = std::chrono::steady_clock::now();
        for (long long i = 0; i < ticks; i++) {
            received = std::min(received + per_tick, fake_size);
            double elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - t_dl_start).count();
            double speed = elapsed > 0 ? (double)received / elapsed : fake_speed;
            print_progress("  Downloading", received, fake_size, speed);
            std::this_thread::sleep_for(std::chrono::milliseconds(TICK_MS));
        }
        double elapsed_dl = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t_dl_start).count();
        print_progress_done("  Downloading", fake_size, elapsed_dl);

        std::cout << "  Verifying checksum... " << green("✓ OK") << "\n";
    }

    // ── Phase 3: package card + file copy progress ────────────────────────────
    {
        std::cout << "\n";
        print_divider('=');
#ifndef _WIN32
        std::cout << "  \033[1;36m" << pkg_name << "\033[0m  v1.0\n";
#else
        std::cout << "  " << pkg_name << "  v1.0\n";
#endif
        std::cout << "  A simulated test package\n";
        std::cout << "  Arch: any   OS: any\n";
        print_divider('=');
        std::cout << "\n";

        install_step(1, 5, "Extracting package archive...");
        std::this_thread::sleep_for(std::chrono::milliseconds(TICK_MS * 2));
        install_ok("Extracted successfully");

        install_step(2, 5, "Reading package manifest...");
        std::this_thread::sleep_for(std::chrono::milliseconds(TICK_MS));
        install_ok("Compatible with this system  (" +
                   arch_to_string(host_arch()) + " / " + os_to_string(host_os()) + ")");

        install_step(3, 5, "Installing files...");
        uintmax_t total_files = 24;
        long long ticks = phase3_ms / TICK_MS;
        for (uintmax_t i = 1; i <= total_files; i++) {
            print_progress("  Copying files", i, total_files);
            long long delay = (ticks * TICK_MS) / (long long)total_files;
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
        std::cout << "\r";
        install_ok("Copied " + std::to_string(total_files) + " file(s) to /usr/local/deliver/" + pkg_name);
    }

    // ── Phase 4: hooks + registration ────────────────────────────────────────
    {
        long long hook_time = phase4_ms / 3;

        install_step(4, 5, "Running install hooks...");
        std::cout << "  Running: install.sh\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(hook_time));
        install_ok("Script completed");
        std::cout << "  Running: echo \"Hello from " << pkg_name << "!\"\n";
        std::cout << "Hello from " << pkg_name << "!\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(hook_time));
        install_ok("Command completed");

        install_step(5, 5, "Registering package...");
        std::this_thread::sleep_for(std::chrono::milliseconds(hook_time));
        install_ok("Registered " + pkg_name + " v1.0");

        std::cout << "\n";
        print_divider('=');
#ifndef _WIN32
        std::cout << "  \033[1;32m✓ Successfully installed " << pkg_name << " v1.0\033[0m\n";
#else
        std::cout << "  Successfully installed " << pkg_name << " v1.0\n";
#endif
        print_divider('=');
        std::cout << "\n";
        std::cout << yellow("  (This was a simulation — no files were actually installed.)\n\n");
    }

    return 0;
}

} // namespace dlr
