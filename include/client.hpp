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

    int cmd_install(const std::string& pkg_name, bool auto_yes);
    int cmd_download(const std::string& pkg_name, bool auto_yes);
    int cmd_scan();
    int cmd_ping(const std::string& server_name);
    int cmd_search(const std::string& query);
    int cmd_servers(const std::string& query);
    int cmd_list();

private:
    std::optional<ServerInfo> find_server_for_package(const std::string& pkg_name);
    std::string download_from_server(const ServerInfo& srv, const std::string& pkg_name);
    int install_tar(const std::string& tar_path, bool auto_yes);

    ClientConfig cfg_;
    LocalDB      db_;
};

} // namespace dlr
