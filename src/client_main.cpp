#include "client.hpp"
#include "config.hpp"
#include "logger.hpp"
#include "server.hpp"
#include "package_registry.hpp"
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

static void usage() {
    std::cout << R"(
  ____       _ _
 |  _ \  ___| (_)_   _____ _ __
 | | | |/ _ \ | \ \ / / _ \ '__|
 | |_| |  __/ | |\ V /  __/ |
 |____/ \___|_|_| \_/ \___|_|   v1.0

Usage: dlr [command] [options] [args...]

CLIENT COMMANDS
  install [-y] <pkg>          Install a package from any LAN server
  download [-y] <pkg>         Download a package .tar without installing
  scan                        Discover LAN servers + refresh package DB
  list                        List all known packages
  ping <server>               Ping a server by name (4 pings)
  search <query>              Search local package database
  servers [query]             Show known servers

SERVER COMMANDS  (requires sudo)
  status                      Show server status and package list
  restart                     Restart the server systemd service
  presentfile <file> <name>   Register a file as a package
  presentfolder <dir> <name>  Register a folder as a package
  attach <pkg_file> <name>    Attach a .pkg manifest to a package
  generate <pkg_file> <name>  Auto-generate + attach a .pkg
  make <pkg_file> <name>      Generate a .pkg file only (no attach)
  list                        List all packages served on this server

OPTIONS
  -y, --yes      Auto-confirm all prompts
  --debug        Verbose debug logging
  -h, --help     Show this help

)";
}

int main(int argc, char** argv) {
    std::signal(SIGINT,  sig_handler);
    std::signal(SIGTERM, sig_handler);

    std::vector<std::string> args(argv+1, argv+argc);
    if (args.empty() || args[0] == "-h" || args[0] == "--help") {
        usage(); return 0;
    }

    if (args[0] == "--version" || args[0] == "-v") {
        std::cout << "Deliver LAN Package Manager v1.0\n";
        return 0;
    }

    // Global flags
    bool auto_yes = false;
    std::vector<std::string> filtered;
    for (auto& a : args) {
        if (a == "-y" || a == "--yes") auto_yes = true;
        else if (a == "--debug")       dlr::set_log_level(dlr::LogLevel::DEBUG);
        else filtered.push_back(a);
    }
    args = filtered;
    if (args.empty()) { usage(); return 0; }
    std::string cmd = args[0];

    // ── Server commands ────────────────────────────────────────────────────
    auto srv_cfg = dlr::load_server_config();
    if (srv_cfg.name.empty()) srv_cfg.name = "deliver-server";

    if (cmd == "status") {
        dlr::Server srv(srv_cfg);
        srv.print_status();
        return 0;
    }

    if (cmd == "restart") {
        std::cout << "Restarting Deliver server...\n";
        int r = system("systemctl restart deliver-server 2>/dev/null");
        if (r != 0) r = system("service deliver-server restart 2>/dev/null");
        if (r != 0) std::cout << "  Could not restart via service manager. Restart manually.\n";
        return 0;
    }

    if (cmd == "presentfile") {
        if (args.size() < 3) { usage(); return 1; }
        dlr::PackageRegistry reg(srv_cfg.registry_file, srv_cfg.data_dir);
        reg.load();
        return reg.present_file(args[1], args[2]) ? 0 : 1;
    }

    if (cmd == "presentfolder") {
        if (args.size() < 3) { usage(); return 1; }
        dlr::PackageRegistry reg(srv_cfg.registry_file, srv_cfg.data_dir);
        reg.load();
        return reg.present_folder(args[1], args[2]) ? 0 : 1;
    }

    if (cmd == "attach") {
        if (args.size() < 3) { usage(); return 1; }
        dlr::PackageRegistry reg(srv_cfg.registry_file, srv_cfg.data_dir);
        reg.load();
        return reg.attach_pkg(args[1], args[2]) ? 0 : 1;
    }

    if (cmd == "generate") {
        if (args.size() < 3) { usage(); return 1; }
        dlr::PackageRegistry reg(srv_cfg.registry_file, srv_cfg.data_dir);
        reg.load();
        return reg.generate_pkg(args[1], args[2]) ? 0 : 1;
    }

    if (cmd == "make") {
        if (args.size() < 3) { usage(); return 1; }
        dlr::PackageRegistry reg(srv_cfg.registry_file, srv_cfg.data_dir);
        reg.load();
        return reg.make_pkg(args[1], args[2]) ? 0 : 1;
    }

    // Server-side list (all presented packages + their files)
    if (cmd == "list" && (getuid() == 0 || getenv("DLR_SERVER_LIST"))) {
        dlr::PackageRegistry reg(srv_cfg.registry_file, srv_cfg.data_dir);
        reg.load();
        auto pkgs = reg.list_all();
        std::cout << "\n";
        std::cout << std::string(60,'═') << "\n";
        std::cout << "  " << dlr::bold("Server Packages") << "  (" << pkgs.size() << " total)\n";
        std::cout << std::string(60,'═') << "\n";
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
        std::cout << std::string(60,'═') << "\n\n";
        return 0;
    }

    // ── Client commands ────────────────────────────────────────────────────
    auto cli_cfg = dlr::load_client_config();
    dlr::Client client(cli_cfg);

    if (cmd == "install") {
        if (args.size() < 2) { usage(); return 1; }
        return client.cmd_install(args[1], auto_yes);
    }
    if (cmd == "download") {
        if (args.size() < 2) { usage(); return 1; }
        return client.cmd_download(args[1], auto_yes);
    }
    if (cmd == "scan")    return client.cmd_scan();
    if (cmd == "list")    return client.cmd_list();
    if (cmd == "ping") {
        if (args.size() < 2) { usage(); return 1; }
        return client.cmd_ping(args[1]);
    }
    if (cmd == "search") {
        if (args.size() < 2) { usage(); return 1; }
        return client.cmd_search(args[1]);
    }
    if (cmd == "servers") {
        std::string q = (args.size() >= 2) ? args[1] : "";
        return client.cmd_servers(q);
    }

    std::cerr << "Unknown command: " << cmd << "\n";
    usage();
    return 1;
}
