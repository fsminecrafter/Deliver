#include "server.hpp"
#include "config.hpp"
#include "logger.hpp"
#include "package_registry.hpp"
#include "version.hpp"
#include <iostream>
#include <string>
#include <vector>
#include <csignal>
#include <memory>
#include <thread>
#include <chrono>
#include <filesystem>

namespace fs = std::filesystem;

static std::unique_ptr<dlr::Server> g_server;

static void sig_handler(int) {
    if (g_server) g_server->stop();
    std::exit(0);
}

static void usage() {
#ifndef _WIN32
    std::cout << "\033[36m\033[1m";
#endif
    std::cout << "Deliver Server v" << dlr::VERSION << "\n";
#ifndef _WIN32
    std::cout << "\033[0m";
#endif
    std::cout << R"(
Usage: dlr_server [command] [options] [args...]

Server management:
  (no command)                   Start the server daemon
  status                         Show server status and packages
  restart                        Restart the server

Package management:
  presentfile   [-y] <file> <n>  Register a file as a package
  presentfolder [-y] <dir>  <n>  Register a folder as a package
  attach         <pkg_file> <n>  Attach a .pkg file to a package
  generate       <pkg_file> <n>  Generate + attach a .pkg file
  make           <pkg_file> <n>  Generate a .pkg file (no attach)
  removepackage  [-y] <n>        Remove a single package
  unpresentfile  [-y] <n>        Alias for removepackage
  unpresentfolder [-y] <n>       Alias for removepackage
  clear          [-y]            Remove ALL packages from server

Options:
  -y, --yes      Auto-confirm prompts
  --debug        Verbose logging

)";
}

int main(int argc, char** argv) {
    std::signal(SIGINT,  sig_handler);
    std::signal(SIGTERM, sig_handler);

    auto cfg = dlr::load_server_config();
    if (cfg.name.empty()) cfg.name = "deliver-server";

    std::vector<std::string> args(argv + 1, argv + argc);

    // Strip global flags
    bool auto_yes = false;
    std::vector<std::string> filtered;
    for (auto& a : args) {
        if (a == "-y" || a == "--yes")  auto_yes = true;
        else if (a == "--debug")        dlr::set_log_level(dlr::LogLevel::DEBUG);
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else                            filtered.push_back(a);
    }
    args = filtered;

    // Start daemon if no command
    if (args.empty()) {
        g_server = std::make_unique<dlr::Server>(cfg);
        g_server->start();
        std::cout << "Deliver Server v" << dlr::VERSION << " running. Press Ctrl+C to stop.\n";
        while (true) std::this_thread::sleep_for(std::chrono::seconds(3600));
        return 0;
    }

    std::string cmd = args[0];

    if (cmd == "status") {
        dlr::PackageRegistry reg(cfg.registry_file, cfg.data_dir);
        reg.load();
        dlr::Server srv(cfg);
        srv.print_status();
        return 0;
    }

    if (cmd == "restart") {
        std::cout << "Restarting Deliver server...\n";
        int r = system("systemctl restart deliver-server 2>/dev/null");
        if (r != 0) r = system("service deliver-server restart 2>/dev/null");
        if (r != 0) std::cout << "  Restart via service manager failed — please restart manually.\n";
        return 0;
    }

    if (cmd == "presentfile") {
        if (args.size() < 3) { usage(); return 1; }
        dlr::PackageRegistry reg(cfg.registry_file, cfg.data_dir);
        reg.load();
        return reg.present_file(args[1], args[2], auto_yes) ? 0 : 1;
    }

    if (cmd == "presentfolder") {
        if (args.size() < 3) { usage(); return 1; }
        dlr::PackageRegistry reg(cfg.registry_file, cfg.data_dir);
        reg.load();
        return reg.present_folder(args[1], args[2], auto_yes) ? 0 : 1;
    }

    if (cmd == "attach") {
        if (args.size() < 3) { usage(); return 1; }
        dlr::PackageRegistry reg(cfg.registry_file, cfg.data_dir);
        reg.load();
        return reg.attach_pkg(args[1], args[2]) ? 0 : 1;
    }

    if (cmd == "generate") {
        if (args.size() < 3) { usage(); return 1; }
        dlr::PackageRegistry reg(cfg.registry_file, cfg.data_dir);
        reg.load();
        return reg.generate_pkg(args[1], args[2]) ? 0 : 1;
    }

    if (cmd == "make") {
        if (args.size() < 3) { usage(); return 1; }
        dlr::PackageRegistry reg(cfg.registry_file, cfg.data_dir);
        reg.load();
        return reg.make_pkg(args[1], args[2]) ? 0 : 1;
    }

    if (cmd == "removepackage") {
        if (args.size() < 2) { usage(); return 1; }
        dlr::PackageRegistry reg(cfg.registry_file, cfg.data_dir);
        reg.load();
        return reg.remove_package(args[1], auto_yes) ? 0 : 1;
    }

    if (cmd == "unpresentfile") {
        if (args.size() < 2) { usage(); return 1; }
        dlr::PackageRegistry reg(cfg.registry_file, cfg.data_dir);
        reg.load();
        return reg.unpresent_file(args[1], auto_yes) ? 0 : 1;
    }

    if (cmd == "unpresentfolder") {
        if (args.size() < 2) { usage(); return 1; }
        dlr::PackageRegistry reg(cfg.registry_file, cfg.data_dir);
        reg.load();
        return reg.unpresent_folder(args[1], auto_yes) ? 0 : 1;
    }

    if (cmd == "clear") {
        dlr::PackageRegistry reg(cfg.registry_file, cfg.data_dir);
        reg.load();
        return reg.clear_all(auto_yes) ? 0 : 1;
    }

    std::cerr << "Unknown command: " << cmd << "\n";
    usage();
    return 1;
}