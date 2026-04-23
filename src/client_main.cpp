#include "client.hpp"
#include "config.hpp"
#include "logger.hpp"
#include "server.hpp"
#include "server_tui.hpp"
#include "package_registry.hpp"
#include "version.hpp"
#include <iostream>
#include <string>
#include <vector>
#include <csignal>
#include <memory>
#include <thread>
#include <chrono>

static std::unique_ptr<dlr::Server> g_server;

static void sig_handler(int) {
    if (g_server) g_server->stop();
    std::exit(0);
}

// ── Banner ────────────────────────────────────────────────────────────────────

static void print_banner() {
#ifndef _WIN32
    std::cout << "\033[36m\033[1m";
#endif
    std::cout << R"(
  ____       _ _
 |  _ \  ___| (_)_   _____ _ __
 | | | |/ _ \ | \ \ / / _ \ '__|
 | |_| |  __/ | |\ V /  __/ |
 |____/ \___|_|_| \_/ \___|_|   v)" << dlr::VERSION << "\n";
#ifndef _WIN32
    std::cout << "\033[0m";
#endif
}

// ── Usage ─────────────────────────────────────────────────────────────────────

inline std::string color(const std::string& text, const char* code) {
#ifndef _WIN32
    return std::string(code) + text + "\033[0m";
#else
    return text;
#endif
}

inline std::string red(const std::string& text) { return color(text, "\033[31m"); }

static void usage() {
    print_banner();
    std::cout << R"(
Usage: dlr [command] [options] [args...]

CLIENT COMMANDS
  install  [-y] <pkg>          Install a package from any LAN server or repo
  download [-y] <pkg>          Download + extract a package to current directory
  scan                         Discover LAN servers + fetch repo indexes
  list                         List all known packages
  ping     <server>            Ping a server by name (4 pings)
  search   <query>             Search local package database
  servers  [query]             Show known servers

REPOSITORY COMMANDS
  addrepo    <name> <url>      Add an HTTP/HTTPS package repository
  removerepo <name>            Remove a configured repository
  listrepos                    List all configured repositories

TUI
  enterapp                     Launch the interactive TUI package browser

SERVER COMMANDS  (requires sudo)
  status                       Show server status and package list
  restart                      Restart the server systemd service
  presentfile   [-y] <file> <name>    Register a file as a package
  presentfolder [-y] <dir>  <name>    Register a folder as a package
  attach         <pkg_file>  <name>   Attach a .pkg manifest to a package
  generate       <pkg_file>  <name>   Auto-generate + attach a .pkg
  make           <pkg_file>  <name>   Generate a .pkg file only (no attach)
  unpresentfile  [-y] <name>          Remove a file-based package from server
  unpresentfolder [-y] <name>         Remove a folder-based package from server
  removepackage  [-y] <name>          Remove any package from server
  clear          [-y]                 Remove ALL packages from this server

TESTING / DIAGNOSTICS
  testinstall <name> <secs>    Simulate a full install UI for <secs> seconds
  testspinner <secs>           Animate the spinner widget for <secs> seconds

OPTIONS
  -y, --yes      Auto-confirm all prompts
  --debug        Verbose debug logging
  -h, --help     Show this help
  -v, --version  Show version

)";
}

// ── Helpers ───────────────────────────────────────────────────────────────────

static bool require_args(const std::vector<std::string>& args, size_t n,
                          const std::string& usage_hint) {
    if (args.size() < n) {
        std::cerr << red("Error: ") << "Not enough arguments for '" << args[0] << "'.\n";
        std::cerr << "  Usage: " << usage_hint << "\n\n";
        return false;
    }
    return true;
}

int main(int argc, char** argv) {
    std::signal(SIGINT,  sig_handler);
    std::signal(SIGTERM, sig_handler);

    std::vector<std::string> args(argv + 1, argv + argc);

    if (args.empty() || args[0] == "-h" || args[0] == "--help") {
        usage();
        return 0;
    }
    if (args[0] == "--version" || args[0] == "-v") {
        std::cout << "Deliver LAN Package Manager v" << dlr::VERSION << "\n";
        return 0;
    }

    // ── Strip global flags ─────────────────────────────────────────────────────
    bool auto_yes = false;
    std::vector<std::string> filtered;
    for (auto& a : args) {
        if (a == "-y" || a == "--yes")  auto_yes = true;
        else if (a == "--debug")        dlr::set_log_level(dlr::LogLevel::DEBUG);
        else                            filtered.push_back(a);
    }
    args = filtered;
    if (args.empty()) { usage(); return 0; }

    std::string cmd = args[0];

    // ── Server-side commands (need registry access) ───────────────────────────
    auto srv_cfg = dlr::load_server_config();
    if (srv_cfg.name.empty()) srv_cfg.name = "deliver-server";

    if (cmd == "enterserverapp") {
        if (getuid() != 0) {
            std::cerr << red("Error: ") << "Server TUI must be run as root (use sudo).\n";
            return 1;
        }

        dlr::ServerTuiApp app(srv_cfg);
        return app.run();
    }

    if (cmd == "status") {
        dlr::Server srv(srv_cfg);
        srv.print_status();
        return 0;
    }

    if (cmd == "restart") {
        std::cout << "Restarting Deliver server...\n";
        int r = system("systemctl restart deliver-server 2>/dev/null");
        if (r != 0) r = system("service deliver-server restart 2>/dev/null");
        if (r != 0) std::cout << "  Could not restart via service manager — please restart manually.\n";
        return 0;
    }

    if (cmd == "presentfile") {
        if (!require_args(args, 3, "sudo dlr presentfile [-y] <file> <name>")) return 1;
        dlr::PackageRegistry reg(srv_cfg.registry_file, srv_cfg.data_dir);
        reg.load();
        return reg.present_file(args[1], args[2], auto_yes) ? 0 : 1;
    }

    if (cmd == "presentfolder") {
        if (!require_args(args, 3, "sudo dlr presentfolder [-y] <folder> <name>")) return 1;
        dlr::PackageRegistry reg(srv_cfg.registry_file, srv_cfg.data_dir);
        reg.load();
        return reg.present_folder(args[1], args[2], auto_yes) ? 0 : 1;
    }

    if (cmd == "attach") {
        if (!require_args(args, 3, "sudo dlr attach <pkg_file> <name>")) return 1;
        dlr::PackageRegistry reg(srv_cfg.registry_file, srv_cfg.data_dir);
        reg.load();
        return reg.attach_pkg(args[1], args[2]) ? 0 : 1;
    }

    if (cmd == "generate") {
        if (!require_args(args, 3, "sudo dlr generate <pkg_file> <name>")) return 1;
        dlr::PackageRegistry reg(srv_cfg.registry_file, srv_cfg.data_dir);
        reg.load();
        return reg.generate_pkg(args[1], args[2]) ? 0 : 1;
    }

    if (cmd == "make") {
        if (!require_args(args, 3, "sudo dlr make <pkg_file> <name>")) return 1;
        dlr::PackageRegistry reg(srv_cfg.registry_file, srv_cfg.data_dir);
        reg.load();
        return reg.make_pkg(args[1], args[2]) ? 0 : 1;
    }

    if (cmd == "removepackage") {
        if (!require_args(args, 2, "sudo dlr removepackage [-y] <name>")) return 1;
        dlr::PackageRegistry reg(srv_cfg.registry_file, srv_cfg.data_dir);
        reg.load();
        return reg.remove_package(args[1], auto_yes) ? 0 : 1;
    }

    if (cmd == "unpresentfile") {
        if (!require_args(args, 2, "sudo dlr unpresentfile [-y] <name>")) return 1;
        dlr::PackageRegistry reg(srv_cfg.registry_file, srv_cfg.data_dir);
        reg.load();
        return reg.unpresent_file(args[1], auto_yes) ? 0 : 1;
    }

    if (cmd == "unpresentfolder") {
        if (!require_args(args, 2, "sudo dlr unpresentfolder [-y] <name>")) return 1;
        dlr::PackageRegistry reg(srv_cfg.registry_file, srv_cfg.data_dir);
        reg.load();
        return reg.unpresent_folder(args[1], auto_yes) ? 0 : 1;
    }

    if (cmd == "clear") {
        dlr::PackageRegistry reg(srv_cfg.registry_file, srv_cfg.data_dir);
        reg.load();
        return reg.clear_all(auto_yes) ? 0 : 1;
    }

    if (cmd == "list" && (getuid() == 0 || getenv("DLR_SERVER_LIST"))) {
        dlr::PackageRegistry reg(srv_cfg.registry_file, srv_cfg.data_dir);
        reg.load();
        auto pkgs = reg.list_all();
        std::cout << "\n" << std::string(60, '=') << "\n";
        std::cout << "  " << dlr::bold("Server Packages") << "  (" << pkgs.size() << " total)\n";
        std::cout << std::string(60, '=') << "\n";
        if (pkgs.empty()) {
            std::cout << "  " << dlr::yellow("No packages presented yet.\n");
            std::cout << "  Use: sudo dlr presentfile <file> <name>\n";
        }
        for (auto& p : pkgs) {
            std::cout << "  " << dlr::green("●") << " " << dlr::bold(p.name)
                      << "  v" << p.version
                      << "  [" << dlr::arch_to_string(p.arch)
                      << " / " << dlr::os_to_string(p.operatingsystem) << "]\n";
            if (!p.description.empty())
                std::cout << "      " << p.description << "\n";
            if (!p.file_path.empty())
                std::cout << "      " << dlr::cyan("file: ") << p.file_path << "\n";
            if (!p.dependencies.empty()) {
                std::cout << "      deps: ";
                for (size_t i = 0; i < p.dependencies.size(); i++) {
                    if (i) std::cout << ", ";
                    std::cout << p.dependencies[i];
                }
                std::cout << "\n";
            }
        }
        std::cout << std::string(60, '=') << "\n\n";
        return 0;
    }

    // ── Client commands ───────────────────────────────────────────────────────
    auto cli_cfg = dlr::load_client_config();
    dlr::Client client(cli_cfg);

    if (cmd == "enterserverapp") {
        if (getuid() != 0) {
            std::cerr << red("Error: ") << "Run with sudo.\n";
            return 1;
        }
        dlr::ServerTuiApp app(srv_cfg);
        return app.run();
    }

    if (cmd == "install") {
        if (!require_args(args, 2, "dlr install [-y] <package>")) return 1;
        return client.cmd_install(args[1], auto_yes);
    }
    if (cmd == "download") {
        if (!require_args(args, 2, "dlr download [-y] <package>")) return 1;
        return client.cmd_download(args[1], auto_yes);
    }
    if (cmd == "scan")    return client.cmd_scan();
    if (cmd == "list")    return client.cmd_list();
    if (cmd == "ping") {
        if (!require_args(args, 2, "dlr ping <server>")) return 1;
        return client.cmd_ping(args[1]);
    }
    if (cmd == "search") {
        if (!require_args(args, 2, "dlr search <query>")) return 1;
        return client.cmd_search(args[1]);
    }
    if (cmd == "servers") {
        std::string q = (args.size() >= 2) ? args[1] : "";
        return client.cmd_servers(q);
    }

    // ── Repo commands ──────────────────────────────────────────────────────────
    if (cmd == "addrepo") {
        if (!require_args(args, 3, "dlr addrepo <name> <url>")) return 1;
        return client.cmd_addrepo(args[1], args[2]);
    }
    if (cmd == "removerepo") {
        if (!require_args(args, 2, "dlr removerepo <name>")) return 1;
        return client.cmd_removerepo(args[1]);
    }
    if (cmd == "listrepos") {
        return client.cmd_listrepos();
    }

    // ── TUI ───────────────────────────────────────────────────────────────────
    if (cmd == "enterapp") {
        return client.cmd_enterapp();
    }

    // ── Test / diagnostic commands ────────────────────────────────────────────
    if (cmd == "testspinner") {
        int secs = 5;
        if (args.size() >= 2) {
            try { secs = std::stoi(args[1]); } catch (...) {}
        }
        if (secs <= 0) {
            std::cerr << red("Error: ") << "Duration must be a positive integer.\n";
            return 1;
        }
        return client.cmd_testspinner(secs);
    }

    if (cmd == "testinstall") {
        if (!require_args(args, 3, "dlr testinstall <name> <seconds>")) return 1;
        int secs = 0;
        try { secs = std::stoi(args[2]); } catch (...) {}
        if (secs <= 0) {
            std::cerr << red("Error: ") << "Duration must be a positive integer.\n";
            return 1;
        }
        return client.cmd_testinstall(args[1], secs);
    }

    std::cerr << "\n  " << dlr::red("Unknown command: ") << cmd << "\n";
    std::cerr << "  Run 'dlr --help' for usage.\n\n";
    return 1;
}