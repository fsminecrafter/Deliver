#include "server.hpp"
#include "config.hpp"
#include "logger.hpp"
#include "package_registry.hpp"
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
    std::cout << R"(Deliver Server - LAN Package Manager
Usage: dlr_server [command] [args...]

Server management:
  (no command)          Start the server daemon
  status                Show server status and packages
  restart               Restart the server

Package management:
  presentfile <file> <name>     Register a file as a package
  presentfolder <dir> <name>    Register a folder as a package
  attach <pkg_file> <name>      Attach a .pkg file to a package
  generate <pkg_file> <name>    Generate + attach a .pkg file for a package
  make <pkg_file> <name>        Generate a .pkg file without attaching

)";
}

int main(int argc, char** argv) {
    std::signal(SIGINT,  sig_handler);
    std::signal(SIGTERM, sig_handler);

    auto cfg = dlr::load_server_config();
    if (cfg.name.empty()) cfg.name = "deliver-server";

    std::vector<std::string> args(argv+1, argv+argc);

    if (args.empty()) {
        // Start server
        g_server = std::make_unique<dlr::Server>(cfg);
        g_server->start();

        std::cout << "Press Ctrl+C to stop.\n";
        // Block forever
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
        // In a real service, this would signal the daemon; here we just re-exec
        std::cout << "Restarting Deliver server...\n";
        system("systemctl restart deliver-server 2>/dev/null || "
               "service deliver-server restart 2>/dev/null || "
               "echo 'Restart via systemctl failed; please restart manually.'");
        return 0;
    }

    if (cmd == "presentfile") {
        if (args.size() < 3) { usage(); return 1; }
        dlr::PackageRegistry reg(cfg.registry_file, cfg.data_dir);
        reg.load();
        return reg.present_file(args[1], args[2]) ? 0 : 1;
    }

    if (cmd == "presentfolder") {
        if (args.size() < 3) { usage(); return 1; }
        dlr::PackageRegistry reg(cfg.registry_file, cfg.data_dir);
        reg.load();
        return reg.present_folder(args[1], args[2]) ? 0 : 1;
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

    usage();
    return 1;
}
