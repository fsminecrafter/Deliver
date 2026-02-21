#include "pkg_parser.hpp"
#include "logger.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>

namespace dlr {

// ── Arch / OS helpers ──────────────────────────────────────────────────────────

Arch arch_from_string(const std::string& raw) {
    std::string s = raw;
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    if (s == "x86_64" || s == "amd64" || s == "x64") return Arch::X86_64;
    if (s == "x86"    || s == "i386"  || s == "i686") return Arch::X86;
    if (s == "arm64"  || s == "aarch64")               return Arch::ARM64;
    if (s == "arm32"  || s == "armhf" || s == "arm")   return Arch::ARM32;
    if (s == "riscv64")                                 return Arch::RISCV64;
    if (s == "any"    || s.empty())                     return Arch::ANY;
    return Arch::UNKNOWN;
}

std::string arch_to_string(Arch a) {
    switch (a) {
        case Arch::X86_64:  return "x86_64";
        case Arch::X86:     return "x86";
        case Arch::ARM64:   return "arm64";
        case Arch::ARM32:   return "arm32";
        case Arch::RISCV64: return "riscv64";
        case Arch::ANY:     return "any";
        default:            return "unknown";
    }
}

OS os_from_string(const std::string& raw) {
    if (raw.empty()) return OS::ANY;
    OS result = OS::ANY;
    std::istringstream ss(raw);
    std::string tok;
    while (std::getline(ss, tok, '/')) {
        while (!tok.empty() && (tok.front()==' '||tok.front()=='\t')) tok.erase(tok.begin());
        while (!tok.empty() && (tok.back() ==' '||tok.back() =='\t')) tok.pop_back();
        std::string upper = tok;
        std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
        if (upper == "LINUX")               result = result | OS::LINUX;
        else if (upper == "WINDOWS")        result = result | OS::WINDOWS;
        else if (upper == "MACOS" || upper == "DARWIN") result = result | OS::MACOS;
        else if (upper == "POSIX")          result = result | OS::POSIX;
        else if (upper == "UNIX")           result = result | OS::UNIX;
        // "ANY" → leave as ANY (0)
    }
    return result;
}

std::string os_to_string(OS o) {
    if (o == OS::ANY) return "any";
    std::string r;
    auto add = [&](const char* s){ if (!r.empty()) r += " / "; r += s; };
    if (os_has(o, OS::POSIX))   add("POSIX");
    if (os_has(o, OS::LINUX))   add("LINUX");
    if (os_has(o, OS::WINDOWS)) add("WINDOWS");
    if (os_has(o, OS::MACOS))   add("MACOS");
    if (os_has(o, OS::UNIX))    add("UNIX");
    return r.empty() ? "any" : r;
}

Arch host_arch() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return Arch::ARM64;
#elif defined(__arm__) || defined(_M_ARM)
    return Arch::ARM32;
#elif defined(__x86_64__) || defined(_M_X64)
    return Arch::X86_64;
#elif defined(__i386__) || defined(_M_IX86)
    return Arch::X86;
#elif defined(__riscv) && __riscv_xlen == 64
    return Arch::RISCV64;
#else
    return Arch::UNKNOWN;
#endif
}

OS host_os() {
#if defined(_WIN32)
    return OS::WINDOWS;
#elif defined(__APPLE__)
    return OS::MACOS | OS::POSIX | OS::UNIX;
#elif defined(__linux__)
    return OS::LINUX | OS::POSIX | OS::UNIX;
#else
    return OS::POSIX | OS::UNIX;
#endif
}

bool pkg_compatible(Arch a, OS o) {
    if (a != Arch::ANY && a != Arch::UNKNOWN && a != host_arch()) return false;
    if (o != OS::ANY) {
        OS h = host_os();
        if (((uint32_t)o & (uint32_t)h) == 0) return false;
    }
    return true;
}

// ── Dependency parsing ─────────────────────────────────────────────────────────

DependencyConstraint parse_dependency(const std::string& dep) {
    DependencyConstraint c;
    auto paren = dep.find('(');
    if (paren == std::string::npos) {
        c.name = dep;
        while (!c.name.empty() && c.name.back() == ' ') c.name.pop_back();
        return c;
    }
    c.name = dep.substr(0, paren);
    while (!c.name.empty() && c.name.back() == ' ') c.name.pop_back();
    auto close = dep.find(')', paren);
    std::string constraint = dep.substr(paren + 1, close - paren - 1);
    if (!constraint.empty()) {
        c.op = constraint[0];
        c.version = constraint.substr(1);
    }
    return c;
}

// ── Parse / write ──────────────────────────────────────────────────────────────

static std::string trim(const std::string& s) {
    std::string r = s;
    while (!r.empty() && (r.front()==' '||r.front()=='\t')) r.erase(r.begin());
    while (!r.empty() && (r.back() ==' '||r.back() =='\t')) r.pop_back();
    return r;
}

std::optional<PackageInfo> parse_pkg(const std::string& path) {
    std::ifstream f(path);
    if (!f) { log_error("Cannot open pkg file: " + path); return std::nullopt; }

    PackageInfo info;
    std::string line, section;

    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        if (line.front() == '[') {
            auto e = line.find(']');
            section = (e != std::string::npos) ? line.substr(1, e-1) : "";
            continue;
        }

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq+1));
        // Strip inline comments
        for (auto& ch : {std::string(" #"), std::string(" ;")}) {
            auto pos = val.find(ch);
            if (pos != std::string::npos) val = trim(val.substr(0, pos));
        }

        if (section == "Info") {
            if      (key == "name")            info.name        = val;
            else if (key == "version")         info.version     = val;
            else if (key == "description")     info.description = val;
            else if (key == "arch")            info.arch        = arch_from_string(val);
            else if (key == "operatingsystem") info.operatingsystem = os_from_string(val);
            else if (key == "rivalpack")       info.rivalpack   = val;
            else if (key == "dependencies") {
                std::istringstream ss(val);
                std::string tok;
                while (std::getline(ss, tok, ',')) {
                    tok = trim(tok);
                    if (!tok.empty()) info.dependencies.push_back(tok);
                }
            }
        } else if (section == "Install") {
            if      (key == "installscript")  info.installscript  = val;
            else if (key == "installcommand") info.installcommand = val;
        }
    }

    if (info.name.empty()) {
        log_error("pkg file missing 'name' field: " + path);
        return std::nullopt;
    }
    return info;
}

bool write_pkg(const std::string& path, const PackageInfo& info) {
    std::ofstream f(path);
    if (!f) { log_error("Cannot write pkg file: " + path); return false; }

    f << "[Info]\n";
    f << "name="            << info.name << "\n";
    f << "version="         << (info.version.empty() ? "1.0" : info.version) << "\n";
    f << "arch="            << arch_to_string(info.arch) << "\n";
    f << "operatingsystem=" << os_to_string(info.operatingsystem) << "\n";
    if (!info.description.empty()) f << "description=" << info.description << "\n";
    if (!info.dependencies.empty()) {
        f << "dependencies=";
        for (size_t i = 0; i < info.dependencies.size(); i++) {
            if (i) f << ", ";
            f << info.dependencies[i];
        }
        f << "\n";
    }
    if (!info.rivalpack.empty()) f << "rivalpack=" << info.rivalpack << "\n";
    f << "\n[Install]\n";
    if (!info.installscript.empty())  f << "installscript="  << info.installscript  << "\n";
    if (!info.installcommand.empty()) f << "installcommand=" << info.installcommand << "\n";
    return true;
}

int compare_versions(const std::string& a, const std::string& b) {
    auto split = [](const std::string& s) {
        std::vector<int> parts;
        std::istringstream ss(s);
        std::string tok;
        while (std::getline(ss, tok, '.'))
            parts.push_back(tok.empty() ? 0 : std::stoi(tok));
        return parts;
    };
    auto ap = split(a), bp = split(b);
    size_t n = std::max(ap.size(), bp.size());
    ap.resize(n,0); bp.resize(n,0);
    for (size_t i = 0; i < n; i++) {
        if (ap[i] < bp[i]) return -1;
        if (ap[i] > bp[i]) return  1;
    }
    return 0;
}

bool satisfies(const std::string& installed, const DependencyConstraint& c) {
    if (c.op == 0) return true;
    int cmp = compare_versions(installed, c.version);
    switch (c.op) {
        case '=': return cmp == 0;
        case '<': return cmp <  0;
        case '>': return cmp >  0;
    }
    return false;
}

} // namespace dlr
