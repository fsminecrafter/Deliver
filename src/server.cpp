#include "server.hpp"
#include "logger.hpp"
#include "crypto.hpp"
#include "network.hpp"
#include "pkg_parser.hpp"
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <thread>
#include <cstring>
#include <filesystem>

namespace dlr {

Server::Server(ServerConfig cfg)
    : cfg_(std::move(cfg))
    , registry_(cfg_.registry_file, cfg_.data_dir)
{
    registry_.load();
}

Server::~Server() { stop(); }

void Server::start() {
    net::init();
    running_ = true;

    server_fd_ = net::make_server_socket(cfg_.port);
    udp_fd_    = net::make_udp_broadcast_socket();

    hello_thread_  = std::thread(&Server::proc_hello,  this);
    accept_thread_ = std::thread(&Server::proc_accept, this);

    log_info(bold("Deliver server '" + cfg_.name + "' started on port " + std::to_string(cfg_.port)));
}

void Server::stop() {
    running_ = false;
    if (server_fd_ != INVALID_SOCK) { close_socket(server_fd_); server_fd_ = INVALID_SOCK; }
    if (udp_fd_    != INVALID_SOCK) { close_socket(udp_fd_);    udp_fd_    = INVALID_SOCK; }
    if (hello_thread_.joinable())  hello_thread_.join();
    if (accept_thread_.joinable()) accept_thread_.join();
    net::cleanup();
}

void Server::print_status() const {
    auto pkgs = registry_.list_all();

    // Check if service is running
    bool svc_running = (system("systemctl is-active --quiet deliver-server 2>/dev/null") == 0);

    std::cout << "\n" << std::string(60,'=') << "\n";
    std::cout << "  " << bold("Deliver Server Status") << "\n";
    std::cout << std::string(60,'-') << "\n";
    std::cout << "  Name     : " << cyan(cfg_.name) << "\n";
    std::cout << "  Port     : " << cfg_.port << "  (TCP)\n";
    std::cout << "  Discovery: " << (DISCOVERY_PORT) << "  (UDP broadcast)\n";
    std::cout << "  Auth     : " << (cfg_.needs_password ? yellow("password required") : green("open")) << "\n";
    std::cout << "  Service  : " << (svc_running ? green("running") : yellow("not running as service")) << "\n";
    std::cout << "  Packages : " << pkgs.size() << " presented\n";
    std::cout << "  Data dir : " << cfg_.data_dir << "\n";
    std::cout << std::string(60,'-') << "\n";

    if (pkgs.empty()) {
        std::cout << "  " << yellow("No packages presented yet.") << "\n";
        std::cout << "  Use: sudo dlr presentfile <file> <n>\n";
    } else {
        std::cout << "  " << std::left
                  << std::setw(20) << bold("Package")
                  << std::setw(10) << "Version"
                  << std::setw(10) << "Arch"
                  << "OS\n";
        std::cout << std::string(60,'-') << "\n";
        for (auto& p : pkgs) {
            std::cout << "  " << std::left
                      << std::setw(20) << green(p.name)
                      << std::setw(10) << p.version
                      << std::setw(10) << arch_to_string(p.arch)
                      << os_to_string(p.operatingsystem) << "\n";
            if (!p.description.empty())
                std::cout << "    " << p.description << "\n";
            if (!p.file_path.empty())
                std::cout << "    " << cyan("→ ") << p.file_path << "\n";
        }
    }
    std::cout << std::string(60,'=') << "\n\n";
}

// Process 1: broadcast hello every 5 seconds
void Server::proc_hello() {
    while (running_) {
        if (udp_fd_ != INVALID_SOCK) {
            net::broadcast_hello(udp_fd_, cfg_.name, cfg_.needs_password, cfg_.port);
        }
        for (int i = 0; i < 50 && running_; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// Process 2: accept connections
void Server::proc_accept() {
    while (running_) {
        sockaddr_in client_addr{};
#ifdef _WIN32
        int addr_len = sizeof(client_addr);
#else
        socklen_t addr_len = sizeof(client_addr);
#endif
        socket_t client = accept(server_fd_, (sockaddr*)&client_addr, &addr_len);
        if (client == INVALID_SOCK) {
            if (running_) log_error("accept() failed");
            continue;
        }
        std::string peer = inet_ntoa(client_addr.sin_addr);
        log_info("Connection from " + peer);
        std::thread(&Server::proc_handle_client, this, client, peer).detach();
    }
}

// Process 3: handle a single client
void Server::proc_handle_client(socket_t fd, std::string peer_ip) {
    // --- Handshake ---
    // 1. Send HELLO: "DLR_SERVER|<name>|<needs_pw:0/1>|<proto_ver>"
    std::string hello_str = "DLR_SERVER|" + cfg_.name + "|" +
                             (cfg_.needs_password?"1":"0") + "|" +
                             std::to_string(PROTOCOL_VERSION);
    std::vector<uint8_t> hello_payload(hello_str.begin(), hello_str.end());
    net::send_frame(fd, hello_payload);

    // 2. Key exchange: receive client's ephemeral key (base64 encoded)
    auto key_frame = net::recv_frame(fd);
    if (key_frame.empty()) { close_socket(fd); return; }

    std::vector<uint8_t> session_key;
    std::string key_str(key_frame.begin(), key_frame.end());

    if (key_str.rfind("KEY:", 0) == 0) {
        auto kb64 = key_str.substr(4);
        session_key = crypto::b64_decode(kb64);
    } else {
        // No key provided — plaintext mode (not recommended but fallback)
        session_key.clear();
    }

    // 3. Auth if required
    if (cfg_.needs_password && !authenticate(fd)) {
        close_socket(fd);
        return;
    }

    // 4. Main loop: handle requests
    while (true) {
        auto frame = net::recv_frame(fd);
        if (frame.empty()) break;

        // Decrypt if we have a session key
        std::vector<uint8_t> payload;
        if (!session_key.empty()) {
            try {
                payload = crypto::decrypt(frame, session_key);
            } catch (...) {
                log_warn("Decryption failed from " + peer_ip);
                break;
            }
        } else {
            payload = frame;
        }

        if (payload.empty()) break;
        MsgType type = (MsgType)payload[0];
        std::vector<uint8_t> body(payload.begin()+1, payload.end());

        switch (type) {
            case MsgType::INSTALL_REQUEST:
                handle_install_request(fd, body, session_key, peer_ip);
                break;
            case MsgType::SEARCH_REQUEST:
                handle_search_request(fd, body, session_key, peer_ip);
                break;
            case MsgType::PKG_LIST:
                handle_pkg_list(fd, session_key);
                break;
            case MsgType::PING: {
                std::vector<uint8_t> pong{(uint8_t)MsgType::PONG};
                pong.insert(pong.end(), body.begin(), body.end());
                auto out = session_key.empty() ? pong : crypto::encrypt(pong, session_key);
                net::send_frame(fd, out);
                break;
            }
            default:
                log_warn("Unknown message type from " + peer_ip);
                break;
        }
    }

    log_info("Client disconnected: " + peer_ip);
    close_socket(fd);
}

bool Server::authenticate(socket_t fd) {
    // Send AUTH_REQUEST
    std::vector<uint8_t> req{(uint8_t)MsgType::AUTH_REQUEST};
    net::send_frame(fd, req);

    auto resp = net::recv_frame(fd);
    if (resp.empty()) return false;

    std::string pw(resp.begin(), resp.end());
    std::string pw_hash = crypto::sha256_hex(pw);

    if (pw_hash == cfg_.password_hash) {
        std::vector<uint8_t> ack{(uint8_t)MsgType::HELLO_ACK};
        net::send_frame(fd, ack);
        return true;
    }
    std::vector<uint8_t> err{(uint8_t)MsgType::ERROR};
    std::string msg = "Authentication failed";
    err.insert(err.end(), msg.begin(), msg.end());
    net::send_frame(fd, err);
    return false;
}

void Server::handle_install_request(socket_t fd, const std::vector<uint8_t>& body,
                                     const std::vector<uint8_t>& session_key) {
    std::string pkg_name(body.begin(), body.end());

    // Validate: package name must be non-empty and contain only safe characters
    if (pkg_name.empty() || pkg_name.find("..") != std::string::npos ||
        pkg_name.find('/') != std::string::npos ||
        pkg_name.find('\\') != std::string::npos) {
        std::vector<uint8_t> err{(uint8_t)MsgType::INSTALL_ERROR};
        std::string msg = "Invalid package name";
        err.insert(err.end(), msg.begin(), msg.end());
        auto out = session_key.empty() ? err : crypto::encrypt(err, session_key);
        net::send_frame(fd, out);
        return;
    }

    log_info("Install request for: " + pkg_name);

    auto info = registry_.find(pkg_name);
    if (!info) {
        std::vector<uint8_t> err{(uint8_t)MsgType::INSTALL_ERROR};
        std::string msg = "Package not found: " + pkg_name;
        err.insert(err.end(), msg.begin(), msg.end());
        auto out = session_key.empty() ? err : crypto::encrypt(err, session_key);
        net::send_frame(fd, out);
        return;
    }

    // Build tar on demand
    registry_.build_tar(pkg_name);
    std::string tar_path = registry_.get_tar_path(pkg_name);

    if (!std::filesystem::exists(tar_path)) {
        std::vector<uint8_t> err{(uint8_t)MsgType::INSTALL_ERROR};
        std::string msg = "Failed to build package tar";
        err.insert(err.end(), msg.begin(), msg.end());
        auto out = session_key.empty() ? err : crypto::encrypt(err, session_key);
        net::send_frame(fd, out);
        return;
    }

    // Send INSTALL_DATA in chunks
    std::ifstream f(tar_path, std::ios::binary);
    if (!f) {
        std::vector<uint8_t> err{(uint8_t)MsgType::INSTALL_ERROR};
        std::string msg = "Cannot open tar file";
        err.insert(err.end(), msg.begin(), msg.end());
        auto out = session_key.empty() ? err : crypto::encrypt(err, session_key);
        net::send_frame(fd, out);
        return;
    }

    // First send file size
    uintmax_t file_size = std::filesystem::file_size(tar_path);
    std::vector<uint8_t> size_msg{(uint8_t)MsgType::INSTALL_DATA};
    std::string size_str = "SIZE:" + std::to_string(file_size);
    size_msg.insert(size_msg.end(), size_str.begin(), size_str.end());
    auto out = session_key.empty() ? size_msg : crypto::encrypt(size_msg, session_key);
    net::send_frame(fd, out);

    // Stream the file
    char buf[BUFFER_SIZE];
    while (f.read(buf, sizeof(buf)) || f.gcount() > 0) {
        size_t n = f.gcount();
        std::vector<uint8_t> chunk{(uint8_t)MsgType::INSTALL_DATA};
        chunk.insert(chunk.end(), buf, buf + n);
        auto encrypted = session_key.empty() ? chunk : crypto::encrypt(chunk, session_key);
        if (!net::send_frame(fd, encrypted)) break;
    }

    // Send END
    std::vector<uint8_t> end_msg{(uint8_t)MsgType::INSTALL_END};
    std::string checksum = crypto::sha256_hex_file(tar_path);
    end_msg.insert(end_msg.end(), checksum.begin(), checksum.end());
    auto end_out = session_key.empty() ? end_msg : crypto::encrypt(end_msg, session_key);
    net::send_frame(fd, end_out);

    log_info("Sent package '" + pkg_name + "' to client");
}

void Server::handle_search_request(socket_t fd, const std::vector<uint8_t>& body,
                                    const std::vector<uint8_t>& session_key) {
    std::string query(body.begin(), body.end());
    auto results = registry_.search(query);

    std::ostringstream ss;
    for (auto& p : results) {
        ss << p.name << "|" << p.version << "|" << p.description << "\n";
    }
    std::string r = ss.str();
    if (r.empty()) r = "NONE";

    std::vector<uint8_t> resp{(uint8_t)MsgType::SEARCH_RESULT};
    resp.insert(resp.end(), r.begin(), r.end());
    auto out = session_key.empty() ? resp : crypto::encrypt(resp, session_key);
    net::send_frame(fd, out);
}

void Server::handle_pkg_list(socket_t fd, const std::vector<uint8_t>& session_key) {
    auto pkgs = registry_.list_all();
    std::ostringstream ss;
    for (auto& p : pkgs) {
        ss << p.name << "|" << p.version << "|" << p.description << "\n";
    }
    std::string r = ss.str();

    std::vector<uint8_t> resp{(uint8_t)MsgType::PKG_LIST};
    resp.insert(resp.end(), r.begin(), r.end());
    auto out = session_key.empty() ? resp : crypto::encrypt(resp, session_key);
    net::send_frame(fd, out);
}

} // namespace dlr
