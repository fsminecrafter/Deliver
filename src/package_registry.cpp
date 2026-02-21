#include "package_registry.hpp"
#include "pkg_parser.hpp"
#include "tar.hpp"
#include "logger.hpp"
#include "network.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace dlr {

PackageRegistry::PackageRegistry(const std::string& registry_file, const std::string& data_dir)
    : registry_file_(registry_file), data_dir_(data_dir)
{
    fs::create_directories(data_dir_);
    fs::create_directories(fs::path(registry_file_).parent_path());
}

void PackageRegistry::load() {
    std::lock_guard<std::mutex> lk(mtx_);
    std::ifstream f(registry_file_);
    if (!f) return;
    try {
        json j;
        f >> j;
        for (auto& [name, obj] : j.items()) {
            PackageInfo info;
            info.name    = name;
            info.version = obj.value("version", "1.0");
            info.description = obj.value("description", "");
            info.installscript  = obj.value("installscript", "");
            info.installcommand = obj.value("installcommand", "");
            info.rivalpack = obj.value("rivalpack", "");
            info.file_path = obj.value("file_path", "");
            if (obj.contains("dependencies"))
                for (auto& d : obj["dependencies"]) info.dependencies.push_back(d);
            packages_[name] = info;
        }
    } catch (...) {
        log_warn("Failed to parse registry file");
    }
}

void PackageRegistry::save() const {
    json j;
    for (auto& [name, info] : packages_) {
        j[name] = {
            {"version",        info.version},
            {"description",    info.description},
            {"installscript",  info.installscript},
            {"installcommand", info.installcommand},
            {"rivalpack",      info.rivalpack},
            {"file_path",      info.file_path},
            {"dependencies",   info.dependencies}
        };
    }
    std::ofstream f(registry_file_);
    f << j.dump(2);
}

// Check across LAN if package name already exists on any server
static bool name_exists_on_lan(const std::string& name) {
    auto servers = net::discover_servers(1500);
    for (auto& srv : servers) {
        socket_t fd = net::connect_to(srv.host, srv.port, 2000);
        if (fd == INVALID_SOCK) continue;
        // Send SEARCH_REQUEST
        std::string query = "EXACT:" + name;
        std::vector<uint8_t> payload(query.begin(), query.end());
        net::send_frame(fd, payload);
        auto resp = net::recv_frame(fd);
        close_socket(fd);
        if (!resp.empty()) {
            std::string r(resp.begin(), resp.end());
            if (r.find("FOUND") != std::string::npos) return true;
        }
    }
    return false;
}

bool PackageRegistry::present_file(const std::string& file_path, const std::string& pkg_name) {
    std::lock_guard<std::mutex> lk(mtx_);

    if (packages_.count(pkg_name)) {
        log_error("Package '" + pkg_name + "' already exists on this server.");
        return false;
    }

    if (!fs::exists(file_path)) {
        log_error("File does not exist: " + file_path);
        return false;
    }

    // Copy file to data dir
    fs::path dest = fs::path(data_dir_) / pkg_name;
    fs::create_directories(dest);
    fs::path dst_file = dest / fs::path(file_path).filename();
    fs::copy_file(file_path, dst_file, fs::copy_options::overwrite_existing);

    PackageInfo info;
    info.name      = pkg_name;
    info.version   = "1.0";
    info.file_path = dst_file.string();
    packages_[pkg_name] = info;
    save();

    log_info(green("Presented file '" + file_path + "' as package '" + pkg_name + "'"));
    return true;
}

bool PackageRegistry::present_folder(const std::string& folder_path, const std::string& pkg_name) {
    std::lock_guard<std::mutex> lk(mtx_);

    if (packages_.count(pkg_name)) {
        log_error("Package '" + pkg_name + "' already exists on this server.");
        return false;
    }

    if (!fs::exists(folder_path) || !fs::is_directory(folder_path)) {
        log_error("Folder does not exist: " + folder_path);
        return false;
    }

    // Copy folder to data dir
    fs::path dest = fs::path(data_dir_) / pkg_name;
    fs::copy(folder_path, dest, fs::copy_options::recursive | fs::copy_options::overwrite_existing);

    PackageInfo info;
    info.name      = pkg_name;
    info.version   = "1.0";
    info.file_path = dest.string();
    packages_[pkg_name] = info;
    save();

    log_info(green("Presented folder '" + folder_path + "' as package '" + pkg_name + "'"));
    return true;
}

bool PackageRegistry::attach_pkg(const std::string& pkg_file, const std::string& pkg_name) {
    std::lock_guard<std::mutex> lk(mtx_);

    if (!packages_.count(pkg_name)) {
        log_error("Package '" + pkg_name + "' not found. Present a file/folder first.");
        return false;
    }

    auto info_opt = parse_pkg(pkg_file);
    if (!info_opt) return false;

    // Copy .pkg into package dir
    fs::path dest_dir = fs::path(data_dir_) / pkg_name;
    fs::path dest_pkg = dest_dir / (pkg_name + ".pkg");
    fs::copy_file(pkg_file, dest_pkg, fs::copy_options::overwrite_existing);

    auto& info = packages_[pkg_name];
    info.installscript  = info_opt->installscript;
    info.installcommand = info_opt->installcommand;
    info.dependencies   = info_opt->dependencies;
    info.rivalpack      = info_opt->rivalpack;
    if (!info_opt->version.empty()) info.version = info_opt->version;
    save();

    log_info(green("Attached '" + pkg_file + "' to package '" + pkg_name + "'"));
    return true;
}

bool PackageRegistry::generate_pkg(const std::string& pkg_file_out, const std::string& pkg_name) {
    if (!make_pkg(pkg_file_out, pkg_name)) return false;
    return attach_pkg(pkg_file_out, pkg_name);
}

bool PackageRegistry::make_pkg(const std::string& pkg_file_out, const std::string& pkg_name) {
    std::lock_guard<std::mutex> lk(mtx_);

    PackageInfo info;
    info.name    = pkg_name;
    info.version = "1.0";

    if (packages_.count(pkg_name)) {
        info = packages_[pkg_name];
    }

    if (!write_pkg(pkg_file_out, info)) return false;
    log_info(green("Generated .pkg file: " + pkg_file_out));
    return true;
}

std::optional<PackageInfo> PackageRegistry::find(const std::string& name) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = packages_.find(name);
    if (it == packages_.end()) return std::nullopt;
    return it->second;
}

std::vector<PackageInfo> PackageRegistry::search(const std::string& query) const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<PackageInfo> results;
    std::string q = query;
    std::transform(q.begin(), q.end(), q.begin(), ::tolower);
    for (auto& [name, info] : packages_) {
        std::string n = name;
        std::transform(n.begin(), n.end(), n.begin(), ::tolower);
        if (n.find(q) != std::string::npos ||
            info.description.find(query) != std::string::npos)
            results.push_back(info);
    }
    return results;
}

std::vector<PackageInfo> PackageRegistry::list_all() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<PackageInfo> r;
    for (auto& [n, i] : packages_) r.push_back(i);
    return r;
}

std::string PackageRegistry::get_tar_path(const std::string& pkg_name) const {
    return (fs::path(data_dir_) / (pkg_name + ".tar")).string();
}

bool PackageRegistry::build_tar(const std::string& pkg_name) {
    std::lock_guard<std::mutex> lk(mtx_);

    auto it = packages_.find(pkg_name);
    if (it == packages_.end()) {
        log_error("Package not found: " + pkg_name);
        return false;
    }

    fs::path pkg_dir = fs::path(data_dir_) / pkg_name;
    std::string tar_path = get_tar_path(pkg_name);

    // Collect all files in the package directory
    std::vector<std::string> files;
    for (auto& entry : fs::recursive_directory_iterator(pkg_dir)) {
        if (fs::is_regular_file(entry)) {
            files.push_back(entry.path().string());
        }
    }

    if (files.empty()) {
        log_error("No files in package directory: " + pkg_dir.string());
        return false;
    }

    return tar::create(tar_path, files, data_dir_);
}

} // namespace dlr
