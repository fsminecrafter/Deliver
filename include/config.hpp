#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace dlr {

struct ServerConfig {
    std::string name;
    uint16_t    port{4242};
    bool        needs_password{false};
    std::string password_hash; // SHA-256 hex of password
    std::string data_dir{"/var/lib/deliver/packages"};
    std::string registry_file{"/var/lib/deliver/registry.json"};
    std::string log_file{"/var/log/deliver/server.log"};
};

struct ClientConfig {
    std::string db_dir{"/var/lib/deliver/client"};
    std::string cache_dir{"/var/cache/deliver"};
    std::string log_file{"/var/log/deliver/client.log"};
    std::string install_dir{"/usr/local/deliver"};
    std::vector<std::string> pinned_servers; // host:port pairs
};

ServerConfig load_server_config(const std::string& path = "/etc/deliver/server.conf");
ClientConfig load_client_config(const std::string& path = "/etc/deliver/client.conf");
void save_server_config(const ServerConfig& cfg, const std::string& path = "/etc/deliver/server.conf");
void save_client_config(const ClientConfig& cfg, const std::string& path = "/etc/deliver/client.conf");

} // namespace dlr
