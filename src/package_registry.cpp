#include "package_registry.hpp"
#include "pkg_parser.hpp"
#include "tar.hpp"
#include "logger.hpp"
#include "network.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
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
        log_warn("Failed to parse registry file: " + registry_file_);
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
    if (!f) {
        log_error("Cannot write registry file: " + registry_file_);
        return;
    }
    f << j.dump(2);
}

// ── present_file ──────────────────────────────────────────────────────────────

bool PackageRegistry::present_file(const std::string& file_path, const std::string& pkg_name,
                                   bool auto_yes) {
    if (pkg_name.empty()) {
        log_error("Package name cannot be empty.");
        return false;
    }
    if (pkg_name.find('/') != std::string::npos || pkg_name.find("..") != std::string::npos) {
        log_error("Invalid package name '" + pkg_name + "': must not contain path separators.");
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (packages_.count(pkg_name)) {
            log_error("Package '" + pkg_name + "' already exists on this server. "
                      "Use 'dlr removepackage " + pkg_name + "' first if you want to replace it.");
            return false;
        }
    }

    if (!fs::exists(file_path)) {
        log_error("File does not exist: " + file_path);
        return false;
    }
    if (!fs::is_regular_file(file_path)) {
        log_error("Path is not a regular file: " + file_path);
        return false;
    }

    uintmax_t sz = fs::file_size(file_path);
    std::cout << "\n  Presenting file as package '" << cyan(pkg_name) << "'\n";
    std::cout << "  Source : " << file_path << "\n";
    std::cout << "  Size   : " << sz << " bytes\n\n";

    if (!auto_yes) {
        std::cout << "  Proceed? [y/N] ";
        std::string ans;
        std::getline(std::cin, ans);
        if (ans != "y" && ans != "Y") {
            std::cout << "  Aborted.\n\n";
            return false;
        }
    }

    // Copy file to data dir
    fs::path dest = fs::path(data_dir_) / pkg_name;
    std::error_code ec;
    fs::create_directories(dest, ec);
    if (ec) {
        log_error("Cannot create package directory '" + dest.string() + "': " + ec.message());
        return false;
    }
    fs::path dst_file = dest / fs::path(file_path).filename();
    fs::copy_file(file_path, dst_file, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        log_error("Failed to copy file: " + ec.message());
        return false;
    }

    PackageInfo info;
    info.name      = pkg_name;
    info.version   = "1.0";
    info.file_path = dst_file.string();

    {
        std::lock_guard<std::mutex> lk(mtx_);
        packages_[pkg_name] = info;
        save();
    }

    log_info("Presented file '" + file_path + "' as package '" + pkg_name + "'");
    std::cout << "  " << green("✓") << " Done. Use 'sudo dlr generate list.pkg " + pkg_name
              + "' to attach a manifest.\n\n";
    return true;
}

// ── present_folder ────────────────────────────────────────────────────────────

bool PackageRegistry::present_folder(const std::string& folder_path, const std::string& pkg_name,
                                     bool auto_yes) {
    if (pkg_name.empty()) {
        log_error("Package name cannot be empty.");
        return false;
    }
    if (pkg_name.find('/') != std::string::npos || pkg_name.find("..") != std::string::npos) {
        log_error("Invalid package name '" + pkg_name + "': must not contain path separators.");
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (packages_.count(pkg_name)) {
            log_error("Package '" + pkg_name + "' already exists on this server. "
                      "Use 'dlr removepackage " + pkg_name + "' first if you want to replace it.");
            return false;
        }
    }

    if (!fs::exists(folder_path)) {
        log_error("Folder does not exist: " + folder_path);
        return false;
    }
    if (!fs::is_directory(folder_path)) {
        log_error("Path is not a directory: " + folder_path);
        return false;
    }

    // Count files for preview
    int file_count = 0;
    for (auto& e : fs::recursive_directory_iterator(folder_path))
        if (fs::is_regular_file(e)) file_count++;

    std::cout << "\n  Presenting folder as package '" << cyan(pkg_name) << "'\n";
    std::cout << "  Source : " << folder_path << "\n";
    std::cout << "  Files  : " << file_count << "\n\n";

    if (!auto_yes) {
        std::cout << "  Proceed? [y/N] ";
        std::string ans;
        std::getline(std::cin, ans);
        if (ans != "y" && ans != "Y") {
            std::cout << "  Aborted.\n\n";
            return false;
        }
    }

    fs::path dest = fs::path(data_dir_) / pkg_name;
    std::error_code ec;
    fs::copy(folder_path, dest,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        log_error("Failed to copy folder: " + ec.message());
        return false;
    }

    PackageInfo info;
    info.name      = pkg_name;
    info.version   = "1.0";
    info.file_path = dest.string();

    {
        std::lock_guard<std::mutex> lk(mtx_);
        packages_[pkg_name] = info;
        save();
    }

    log_info("Presented folder '" + folder_path + "' as package '" + pkg_name + "'");
    std::cout << "  " << green("✓") << " Done. Use 'sudo dlr generate list.pkg " + pkg_name
              + "' to attach a manifest.\n\n";
    return true;
}

// ── attach_pkg ────────────────────────────────────────────────────────────────

bool PackageRegistry::attach_pkg(const std::string& pkg_file, const std::string& pkg_name) {
    std::lock_guard<std::mutex> lk(mtx_);

    if (!packages_.count(pkg_name)) {
        log_error("Package '" + pkg_name + "' not found in registry. "
                  "Use 'sudo dlr presentfile <file> " + pkg_name + "' first.");
        return false;
    }
    if (!fs::exists(pkg_file)) {
        log_error("Manifest file does not exist: " + pkg_file);
        return false;
    }

    auto info_opt = parse_pkg(pkg_file);
    if (!info_opt) {
        log_error("Failed to parse manifest file: " + pkg_file);
        return false;
    }

    // Copy .pkg into package dir
    fs::path dest_dir = fs::path(data_dir_) / pkg_name;
    fs::path dest_pkg = dest_dir / (pkg_name + ".pkg");
    std::error_code ec;
    fs::copy_file(pkg_file, dest_pkg, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        log_error("Failed to copy manifest: " + ec.message());
        return false;
    }

    auto& info = packages_[pkg_name];
    info.installscript  = info_opt->installscript;
    info.installcommand = info_opt->installcommand;
    info.dependencies   = info_opt->dependencies;
    info.rivalpack      = info_opt->rivalpack;
    if (!info_opt->version.empty()) info.version = info_opt->version;
    if (!info_opt->description.empty()) info.description = info_opt->description;
    save();

    log_info("Attached '" + pkg_file + "' to package '" + pkg_name + "'");
    return true;
}

// ── generate_pkg / make_pkg ───────────────────────────────────────────────────

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

    if (!write_pkg(pkg_file_out, info)) {
        log_error("Failed to write manifest to: " + pkg_file_out);
        return false;
    }
    log_info("Generated .pkg file: " + pkg_file_out);
    return true;
}

// ── remove_package_locked (internal, caller holds mtx_) ──────────────────────

bool PackageRegistry::remove_package_locked(const std::string& pkg_name) {
    auto it = packages_.find(pkg_name);
    if (it == packages_.end()) {
        log_error("Package '" + pkg_name + "' not found in registry.");
        return false;
    }

    // Remove package directory
    fs::path pkg_dir = fs::path(data_dir_) / pkg_name;
    std::error_code ec;
    if (fs::exists(pkg_dir)) {
        fs::remove_all(pkg_dir, ec);
        if (ec) {
            log_warn("Could not fully remove package directory '" + pkg_dir.string() + "': " + ec.message());
        }
    }

    // Remove .tar if it exists
    std::string tar_path = (fs::path(data_dir_) / (pkg_name + ".tar")).string();
    if (fs::exists(tar_path)) {
        fs::remove(tar_path, ec);
        if (ec) log_warn("Could not remove tar '" + tar_path + "': " + ec.message());
    }

    packages_.erase(it);
    return true;
}

// ── remove_package ────────────────────────────────────────────────────────────

bool PackageRegistry::remove_package(const std::string& pkg_name, bool auto_yes) {
    // Preview outside lock
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!packages_.count(pkg_name)) {
            log_error("Package '" + pkg_name + "' does not exist on this server.");
            return false;
        }
        auto& info = packages_[pkg_name];
        std::cout << "\n  Remove package: " << red(pkg_name) << "  v" << info.version << "\n";
        if (!info.description.empty())
            std::cout << "  " << info.description << "\n";
        std::cout << "  Files in: " << fs::path(data_dir_) / pkg_name << "\n\n";
        std::cout << "  " << yellow("WARNING: This will delete all package files from this server.") << "\n\n";
    }

    if (!auto_yes) {
        std::cout << "  Remove '" << pkg_name << "'? [y/N] ";
        std::string ans;
        std::getline(std::cin, ans);
        if (ans != "y" && ans != "Y") {
            std::cout << "  Aborted.\n\n";
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!remove_package_locked(pkg_name)) return false;
        save();
    }

    log_info("Removed package '" + pkg_name + "'");
    std::cout << "  " << green("✓") << " Package '" + pkg_name + "' removed.\n\n";
    return true;
}

// ── unpresent_file / unpresent_folder ─────────────────────────────────────────

bool PackageRegistry::unpresent_file(const std::string& pkg_name, bool auto_yes) {
    return remove_package(pkg_name, auto_yes);
}

bool PackageRegistry::unpresent_folder(const std::string& pkg_name, bool auto_yes) {
    return remove_package(pkg_name, auto_yes);
}

// ── clear_all ─────────────────────────────────────────────────────────────────

bool PackageRegistry::clear_all(bool auto_yes) {
    size_t count;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        count = packages_.size();
    }

    if (count == 0) {
        std::cout << "\n  " << yellow("No packages to remove.") << "\n\n";
        return true;
    }

    std::cout << "\n";
    std::cout << "  " << red("WARNING: This will remove ALL " + std::to_string(count) +
                             " package(s) from this server.") << "\n";
    std::cout << "  Data directory: " << data_dir_ << "\n\n";

    // List them
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& [name, info] : packages_)
            std::cout << "    " << red("✗") << " " << name << "  v" << info.version << "\n";
    }
    std::cout << "\n";

    if (!auto_yes) {
        std::cout << "  Remove all packages? [y/N] ";
        std::string ans;
        std::getline(std::cin, ans);
        if (ans != "y" && ans != "Y") {
            std::cout << "  Aborted.\n\n";
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lk(mtx_);
        // Collect names first (can't erase while iterating)
        std::vector<std::string> names;
        names.reserve(packages_.size());
        for (auto& [n, _] : packages_) names.push_back(n);

        int removed = 0, failed = 0;
        for (auto& n : names) {
            if (remove_package_locked(n)) {
                std::cout << "  " << green("✓") << " Removed: " << n << "\n";
                removed++;
            } else {
                failed++;
            }
        }
        save();

        std::cout << "\n";
        if (failed == 0)
            log_info("Cleared all " + std::to_string(removed) + " package(s).");
        else
            log_warn("Removed " + std::to_string(removed) + " package(s); " +
                     std::to_string(failed) + " failed.");
    }
    std::cout << "\n";
    return true;
}

// ── find / search / list_all ──────────────────────────────────────────────────

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
        std::string desc = info.description;
        std::transform(desc.begin(), desc.end(), desc.begin(), ::tolower);
        if (n.find(q) != std::string::npos || desc.find(q) != std::string::npos)
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

// ── build_tar ─────────────────────────────────────────────────────────────────

std::string PackageRegistry::get_tar_path(const std::string& pkg_name) const {
    return (fs::path(data_dir_) / (pkg_name + ".tar")).string();
}

bool PackageRegistry::build_tar(const std::string& pkg_name) {
    std::lock_guard<std::mutex> lk(mtx_);

    auto it = packages_.find(pkg_name);
    if (it == packages_.end()) {
        log_error("Cannot build tar — package not found: " + pkg_name);
        return false;
    }

    fs::path pkg_dir = fs::path(data_dir_) / pkg_name;
    if (!fs::exists(pkg_dir) || !fs::is_directory(pkg_dir)) {
        log_error("Package directory missing: " + pkg_dir.string());
        return false;
    }

    std::string tar_path = get_tar_path(pkg_name);

    std::vector<std::string> files;
    for (auto& entry : fs::recursive_directory_iterator(pkg_dir)) {
        if (fs::is_regular_file(entry)) {
            files.push_back(entry.path().string());
        }
    }

    if (files.empty()) {
        log_error("No files found in package directory: " + pkg_dir.string());
        return false;
    }

    return tar::create(tar_path, files, data_dir_);
}

} // namespace dlr