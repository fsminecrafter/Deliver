#pragma once
#include "types.hpp"
#include <string>
#include <vector>
#include <cstdint>
#include <functional>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
#define INVALID_SOCK INVALID_SOCKET
#define close_socket closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
using socket_t = int;
#define INVALID_SOCK (-1)
#define close_socket close
#endif

namespace dlr {
namespace net {

// Platform init (needed on Windows for WSAStartup)
void init();
void cleanup();

// Create a listening TCP socket on the given port
socket_t make_server_socket(uint16_t port);

// Create a UDP broadcast socket
socket_t make_udp_broadcast_socket();

// Connect to host:port, returns INVALID_SOCK on failure
socket_t connect_to(const std::string& host, uint16_t port, int timeout_ms = 3000);

// Send/recv helpers (handles partial sends)
bool send_all(socket_t s, const uint8_t* data, size_t len);
bool recv_all(socket_t s, uint8_t* buf, size_t len);

// Frame: 4-byte little-endian length prefix + payload
bool send_frame(socket_t s, const std::vector<uint8_t>& payload);
std::vector<uint8_t> recv_frame(socket_t s); // empty on error

// LAN discovery: broadcast a UDP hello and collect replies for timeout_ms
std::vector<ServerInfo> discover_servers(int timeout_ms = 2000);

// Broadcast a UDP hello (run by server)
void broadcast_hello(socket_t udp_sock, const std::string& server_name,
                     bool needs_password, uint16_t tcp_port);

// Resolve hostname to IP string
std::string resolve_host(const std::string& host);

// Probe a specific host:port and return its ServerInfo (name, auth etc.)
// Returns false if not reachable or not a Deliver server
static bool probe_server(const std::string& host, uint16_t port, ServerInfo& out);

} // namespace net
} // namespace dlr
