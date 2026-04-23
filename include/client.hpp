#pragma once
#include "config.hpp"
#include "types.hpp"
#include "local_db.hpp"
#include <thread>
#include <chrono>

namespace dlr {

class Client {
public:
    explicit Client(ClientConfig cfg);

    // ── Client commands ────────────────────────────────────────────────────────
    int cmd_install(const std::string& pkg_name, bool auto_yes);
    int cmd_download(const std::string& pkg_name, bool auto_yes);
    int cmd_scan();
    int cmd_ping(const std::string& server_name);
    int cmd_search(const std::string& query);
    int cmd_servers(const std::string& query);
    int cmd_list();

    // ── Repo management ────────────────────────────────────────────────────────
    int cmd_addrepo(const std::string& name, const std::string& url);
    int cmd_removerepo(const std::string& name);
    int cmd_listrepos();

    // ── TUI ───────────────────────────────────────────────────────────────────
    int cmd_enterapp();

    // ── Diagnostics ───────────────────────────────────────────────────────────
    int cmd_testinstall(const std::string& pkg_name, int duration_secs);
    int cmd_testspinner(int duration_secs);

private:
    std::optional<ServerInfo> find_server_for_package(const std::string& pkg_name);
    std::string download_from_server(const ServerInfo& srv, const std::string& pkg_name);
    std::string download_from_repo(const std::string& pkg_name);
    int install_tar(const std::string& tar_path, bool auto_yes);
    void scan_repos();

    ClientConfig cfg_;
    LocalDB      db_;
};

} // namespace dlr