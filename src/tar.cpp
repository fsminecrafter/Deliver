#include "tar.hpp"
#include "logger.hpp"
#include <cstdlib>
#include <sstream>
#include <filesystem>

namespace fs = std::filesystem;

namespace dlr {
namespace tar {

// We use the system `tar` command for simplicity and broad format support.
// On Windows we check for GNU tar (comes with Git for Windows or WSL).

static std::string sh(const std::string& cmd) {
    // Returns stdout of command
    std::string result;
#ifdef _WIN32
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) return "";
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe)) result += buf;
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return result;
}

bool create(const std::string& tar_path,
            const std::vector<std::string>& files,
            const std::string& base_dir) {
    if (files.empty()) {
        log_error("tar::create: no files provided");
        return false;
    }

    std::ostringstream cmd;
    cmd << "tar -cf " << std::quoted(tar_path) << " -C " << std::quoted(base_dir);
    for (auto& f : files) {
        // Make relative to base_dir
        fs::path rel = fs::relative(f, base_dir);
        cmd << " " << std::quoted(rel.string());
    }
    cmd << " 2>&1";
    std::string out = sh(cmd.str());
    if (!out.empty()) {
        log_warn("tar create: " + out);
    }
    return fs::exists(tar_path);
}

bool extract(const std::string& tar_path, const std::string& dest_dir) {
    fs::create_directories(dest_dir);
    std::ostringstream cmd;
    cmd << "tar -xf " << std::quoted(tar_path)
        << " -C " << std::quoted(dest_dir) << " 2>&1";
    std::string out = sh(cmd.str());
    if (!out.empty()) {
        log_warn("tar extract: " + out);
    }
    return true;
}

std::vector<std::string> list(const std::string& tar_path) {
    std::ostringstream cmd;
    cmd << "tar -tf " << std::quoted(tar_path) << " 2>/dev/null";
    std::string out = sh(cmd.str());
    std::vector<std::string> result;
    std::istringstream ss(out);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty()) result.push_back(line);
    }
    return result;
}

} // namespace tar
} // namespace dlr
