#pragma once
#include "types.hpp"
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <optional>

namespace dlr {

class PackageRegistry {
public:
    explicit PackageRegistry(const std::string& registry_file, const std::string& data_dir);

    void load();
    void save() const;

    // Register a file as a package with the given name
    // Returns false if name already exists on any server (checked via LAN)
    bool present_file(const std::string& file_path, const std::string& pkg_name);
    bool present_folder(const std::string& folder_path, const std::string& pkg_name);

    // Attach a .pkg file to an existing package
    bool attach_pkg(const std::string& pkg_file, const std::string& pkg_name);

    // Generate a basic .pkg and attach it
    bool generate_pkg(const std::string& pkg_file_out, const std::string& pkg_name);

    // Generate a basic .pkg without attaching
    bool make_pkg(const std::string& pkg_file_out, const std::string& pkg_name);

    std::optional<PackageInfo> find(const std::string& name) const;
    std::vector<PackageInfo>   search(const std::string& query) const;
    std::vector<PackageInfo>   list_all() const;

    // Get path to the .tar bundle for a package
    std::string get_tar_path(const std::string& pkg_name) const;

    // Build or rebuild the .tar bundle for a package
    bool build_tar(const std::string& pkg_name);

private:
    std::string              registry_file_;
    std::string              data_dir_;
    std::map<std::string, PackageInfo> packages_;
    mutable std::mutex       mtx_;
};

} // namespace dlr
