#pragma once
#include "config.hpp"
#include "types.hpp"
#include "network.hpp"
#include "package_registry.hpp"
#include <thread>
#include <atomic>
#include <memory>

namespace dlr {

class Server {
public:
    explicit Server(ServerConfig cfg);
    ~Server();

    void start();
    void stop();
    void print_status() const;

private:
    // Process 1: UDP hello broadcast
    void proc_hello();

    // Process 2: Accept TCP connections
    void proc_accept();

    // Process 3: Handle individual client connection
    void proc_handle_client(socket_t client_fd, std::string peer_ip);

    // Handle a specific message type
    void handle_install_request(socket_t fd, const std::vector<uint8_t>& payload,
                                 const std::vector<uint8_t>& session_key,
                                 const std::string& peer_ip);  // add this
    void handle_search_request(socket_t fd, const std::vector<uint8_t>& payload,
                                const std::vector<uint8_t>& session_key,
                                const std::string& peer_ip);   // add this
    void handle_pkg_list(socket_t fd, const std::vector<uint8_t>& session_key);

    bool authenticate(socket_t fd);

    ServerConfig        cfg_;
    PackageRegistry     registry_;
    socket_t            server_fd_{INVALID_SOCK};
    socket_t            udp_fd_{INVALID_SOCK};
    std::atomic<bool>   running_{false};
    std::thread         hello_thread_;
    std::thread         accept_thread_;
};

} // namespace dlr
