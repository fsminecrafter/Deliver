#include "network.hpp"
#include "logger.hpp"
#include <cstring>
#include <stdexcept>
#include <chrono>
#include <sstream>

#ifdef _WIN32
#pragma comment(lib,"Ws2_32.lib")
#else
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#endif

namespace dlr {
namespace net {

static bool g_init = false;

void init() {
#ifdef _WIN32
    if (!g_init) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2,2), &wsa);
        g_init = true;
    }
#else
    g_init = true;
#endif
}

void cleanup() {
#ifdef _WIN32
    WSACleanup();
#endif
}

socket_t make_server_socket(uint16_t port) {
    socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCK) throw std::runtime_error("socket() failed");

    int opt = 1;
#ifdef _WIN32
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close_socket(fd);
        throw std::runtime_error("bind() failed on port " + std::to_string(port));
    }
    if (listen(fd, 16) < 0) {
        close_socket(fd);
        throw std::runtime_error("listen() failed");
    }
    return fd;
}

socket_t make_udp_broadcast_socket() {
    socket_t fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == INVALID_SOCK) throw std::runtime_error("udp socket() failed");
    int bcast = 1;
#ifdef _WIN32
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (const char*)&bcast, sizeof(bcast));
#else
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));
#endif
    return fd;
}

socket_t connect_to(const std::string& host, uint16_t port, int timeout_ms) {
    socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCK) return INVALID_SOCK;

    // Set non-blocking for timeout
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);

    // Resolve hostname
    struct addrinfo hints{}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) == 0) {
        addr.sin_addr = ((sockaddr_in*)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    } else {
        close_socket(fd);
        return INVALID_SOCK;
    }

    int r = connect(fd, (sockaddr*)&addr, sizeof(addr));
#ifdef _WIN32
    if (r == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
        close_socket(fd); return INVALID_SOCK;
    }
    // Wait with select
    fd_set ws; FD_ZERO(&ws); FD_SET(fd, &ws);
    timeval tv{timeout_ms/1000, (timeout_ms%1000)*1000};
    r = select((int)fd+1, nullptr, &ws, nullptr, &tv);
#else
    if (r < 0 && errno != EINPROGRESS) { close_socket(fd); return INVALID_SOCK; }
    pollfd pfd{fd, POLLOUT, 0};
    r = poll(&pfd, 1, timeout_ms);
#endif
    if (r <= 0) { close_socket(fd); return INVALID_SOCK; }

    // Restore blocking
#ifdef _WIN32
    mode = 0; ioctlsocket(fd, FIONBIO, &mode);
#else
    fcntl(fd, F_SETFL, flags);
#endif
    return fd;
}

// Try to connect to a local server and parse its HELLO frame
static bool probe_server(const std::string& host, uint16_t port, ServerInfo& out) {
    socket_t fd = connect_to(host, port, 800);
    if (fd == INVALID_SOCK) return false;

    // Server sends HELLO frame immediately on connect:
    // "DLR_SERVER|<name>|<needs_pw>|<proto_ver>"
    // We need to recv it then close cleanly
    // Reuse recv_frame but we're in net:: scope so it's available
    auto frame = recv_frame(fd);
    close_socket(fd);
    if (frame.empty()) return false;

    std::string msg(frame.begin(), frame.end());
    if (msg.rfind("DLR_SERVER|", 0) != 0) return false;

    // Parse: DLR_SERVER|name|needs_pw|proto_ver
    auto p1 = msg.find('|', 11);
    auto p2 = (p1 != std::string::npos) ? msg.find('|', p1+1) : std::string::npos;
    if (p1 == std::string::npos) return false;

    out.name           = msg.substr(11, p1 - 11);
    out.needs_password = (p2 != std::string::npos)
                         ? msg.substr(p1+1, p2-p1-1) == "1"
                         : msg.substr(p1+1) == "1";
    out.host           = host;
    out.port           = port;
    out.reachable      = true;
    return true;
}

bool send_all(socket_t s, const uint8_t* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int r = send(s, (const char*)(data + sent), (int)(len - sent), 0);
        if (r <= 0) return false;
        sent += r;
    }
    return true;
}

bool recv_all(socket_t s, uint8_t* buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        int r = recv(s, (char*)(buf + got), (int)(len - got), 0);
        if (r <= 0) return false;
        got += r;
    }
    return true;
}

bool send_frame(socket_t s, const std::vector<uint8_t>& payload) {
    uint32_t len = htonl((uint32_t)payload.size());
    if (!send_all(s, (uint8_t*)&len, 4)) return false;
    if (!payload.empty() && !send_all(s, payload.data(), payload.size())) return false;
    return true;
}

std::vector<uint8_t> recv_frame(socket_t s) {
    uint32_t len_net;
    if (!recv_all(s, (uint8_t*)&len_net, 4)) return {};
    uint32_t len = ntohl(len_net);
    if (len == 0) return {};
    if (len > 256*1024*1024) return {}; // sanity: 256 MB max frame
    std::vector<uint8_t> buf(len);
    if (!recv_all(s, buf.data(), len)) return {};
    return buf;
}

// UDP discovery
// Hello packet format: "DLR|<name>|<needs_pw:0/1>|<tcp_port>\n"

void broadcast_hello(socket_t udp_sock, const std::string& server_name,
                     bool needs_password, uint16_t tcp_port) {
    std::string msg = "DLR|" + server_name + "|" + (needs_password?"1":"0") +
                      "|" + std::to_string(tcp_port) + "\n";

    sockaddr_in bcast{};
    bcast.sin_family = AF_INET;
    bcast.sin_port   = htons(DISCOVERY_PORT);
    bcast.sin_addr.s_addr = INADDR_BROADCAST;

    sendto(udp_sock, msg.c_str(), (int)msg.size(), 0,
           (sockaddr*)&bcast, sizeof(bcast));
}

std::vector<ServerInfo> discover_servers(int timeout_ms) {
    socket_t fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == INVALID_SOCK) return {};

    int bcast = 1;
#ifdef _WIN32
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (const char*)&bcast, sizeof(bcast));
#else
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));
#endif

    // Bind to discovery port
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port   = htons(DISCOVERY_PORT);
    local.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, (sockaddr*)&local, sizeof(local)) < 0) {
        close_socket(fd); return {};
    }

    // Send a discovery request
    std::string req = "DLR_DISCOVER\n";
    sockaddr_in bcast_addr{};
    bcast_addr.sin_family = AF_INET;
    bcast_addr.sin_port   = htons(DISCOVERY_PORT);
    bcast_addr.sin_addr.s_addr = INADDR_BROADCAST;
    sendto(fd, req.c_str(), (int)req.size(), 0, (sockaddr*)&bcast_addr, sizeof(bcast_addr));

    // Collect responses
    std::vector<ServerInfo> results;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        int remaining = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) break;

#ifdef _WIN32
        fd_set rs; FD_ZERO(&rs); FD_SET(fd, &rs);
        timeval tv{remaining/1000,(remaining%1000)*1000};
        int r = select((int)fd+1, &rs, nullptr, nullptr, &tv);
#else
        pollfd pfd{fd, POLLIN, 0};
        int r = poll(&pfd, 1, remaining);
#endif
        if (r <= 0) break;

        char buf[512];
        sockaddr_in from{};
#ifdef _WIN32
        int fromlen = sizeof(from);
#else
        socklen_t fromlen = sizeof(from);
#endif
        int n = recvfrom(fd, buf, sizeof(buf)-1, 0, (sockaddr*)&from, &fromlen);
        if (n <= 0) continue;
        buf[n] = 0;

        std::string msg(buf);
        if (msg.rfind("DLR|", 0) != 0) continue;

        // Parse: DLR|name|needs_pw|port
        auto p1 = msg.find('|', 4);
        auto p2 = (p1!=std::string::npos) ? msg.find('|', p1+1) : std::string::npos;
        auto p3 = (p2!=std::string::npos) ? msg.find('|', p2+1) : std::string::npos;
        if (p1 == std::string::npos || p2 == std::string::npos || p3 == std::string::npos) continue;

        ServerInfo si;
        si.name          = msg.substr(4, p1-4);
        si.needs_password= msg.substr(p1+1, p2-p1-1) == "1";
        si.port          = (uint16_t)std::stoi(msg.substr(p2+1, p3-p2-1));
        si.host          = inet_ntoa(from.sin_addr);
        si.reachable     = true;
        results.push_back(si);
    }

    close_socket(fd);
    // Always probe localhost explicitly (broadcast doesn't loop back reliably)
    for (const auto& addr : {"127.0.0.1", "::1"}) {
        bool found = false;
        for (auto& s : results)
            if (s.host == addr) { found = true; break; }
        if (!found) {
            ServerInfo local;
            if (probe_server(addr, DEFAULT_PORT, local)) {
                results.push_back(local);
            }
        }
    }
    return results;
}

std::string resolve_host(const std::string& host) {
    struct addrinfo hints{}, *res;
    hints.ai_family = AF_INET;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0) return host;
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &((sockaddr_in*)res->ai_addr)->sin_addr, ip, sizeof(ip));
    freeaddrinfo(res);
    return ip;
}

} // namespace net
} // namespace dlr
