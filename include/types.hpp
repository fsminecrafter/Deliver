#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <optional>

namespace dlr {

constexpr uint16_t DEFAULT_PORT        = 4242;
constexpr uint16_t DISCOVERY_PORT      = 4243;
constexpr int      PROTOCOL_VERSION    = 1;
constexpr int      BUFFER_SIZE         = 65536;

// Message types exchanged on the wire
enum class MsgType : uint8_t {
    HELLO           = 0x01,
    HELLO_ACK       = 0x02,
    AUTH_REQUEST    = 0x03,
    AUTH_RESPONSE   = 0x04,
    INSTALL_REQUEST = 0x10,
    INSTALL_DATA    = 0x11,
    INSTALL_END     = 0x12,
    INSTALL_ERROR   = 0x13,
    SEARCH_REQUEST  = 0x20,
    SEARCH_RESULT   = 0x21,
    PKG_LIST        = 0x22,
    PING            = 0x30,
    PONG            = 0x31,
    ERROR           = 0xFF,
};

// Supported architectures
enum class Arch : uint8_t {
    ANY = 0,
    X86_64,
    X86,
    ARM64,
    ARM32,
    RISCV64,
    UNKNOWN
};

// OS flags (bitmask-friendly)
enum class OS : uint32_t {
    ANY     = 0,
    LINUX   = 1u << 0,
    WINDOWS = 1u << 1,
    MACOS   = 1u << 2,
    POSIX   = 1u << 3,
    UNIX    = 1u << 4,
};
inline OS operator|(OS a, OS b) { return (OS)((uint32_t)a | (uint32_t)b); }
inline bool os_has(OS set, OS flag) { return ((uint32_t)set & (uint32_t)flag) != 0; }

Arch arch_from_string(const std::string& s);
OS   os_from_string(const std::string& s);   // "POSIX / LINUX / WINDOWS"
std::string arch_to_string(Arch a);
std::string os_to_string(OS o);
Arch host_arch();
OS   host_os();
bool pkg_compatible(Arch a, OS o); // does this pkg run on current host?

struct PackageInfo {
    std::string name;
    std::string version;
    std::string description;
    std::vector<std::string> dependencies;
    std::string rivalpack;
    std::string installscript;
    std::string installcommand;
    Arch        arch{Arch::ANY};
    OS          operatingsystem{OS::ANY};
    std::string server_origin;
    std::string file_path;
};

struct ServerInfo {
    std::string name;
    std::string host;
    uint16_t    port{DEFAULT_PORT};
    bool        needs_password{false};
    bool        reachable{false};
    int64_t     latency_ms{-1};
};

// HTTP/HTTPS repository entry
struct RepoInfo {
    std::string name;
    std::string url;          // base URL, e.g. https://example.com/myrepo
    std::string description;
    bool        enabled{true};
};

struct DependencyConstraint {
    std::string name;
    char        op{0};  // 0 = any, '=' exact, '<' less, '>' greater
    std::string version;
};

DependencyConstraint parse_dependency(const std::string& dep);

} // namespace dlr