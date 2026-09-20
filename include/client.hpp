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

    // What to do when a server answers with a Minimal-OS .mpkg instead of
    // a tar (see "Archive format" in the README). This platform has no
    // way to extract one, so `install` refuses it up front - before
    // spending the bandwidth - while `download` keeps it as a file to
    // carry to a Minimal-OS machine.
    enum class OnMpkg { Refuse, Keep };

    // Returns the path of the downloaded archive, or "" on failure. The
    // extension says what it is: ".tar" (extract it) or ".mpkg" (only
    // possible with OnMpkg::Keep; cannot be extracted here).
    std::string download_from_server(const ServerInfo& srv, const std::string& pkg_name,
                                     OnMpkg on_mpkg = OnMpkg::Refuse);
    std::string download_from_repo(const std::string& pkg_name);
    int install_tar(const std::string& tar_path, bool auto_yes);
    void scan_repos();

    ClientConfig cfg_;
    LocalDB      db_;
};

} // namespace dlr