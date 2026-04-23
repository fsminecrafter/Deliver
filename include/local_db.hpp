#pragma once
#include "types.hpp"
#include <string>
#include <vector>
#include <map>
#include <optional>

namespace dlr {

class LocalDB {
public:
    explicit LocalDB(const std::string& db_dir);

    void load();
    void save() const;

    // Package DB
    void upsert_package(const PackageInfo& info);
    std::optional<PackageInfo> find_package(const std::string& name) const;
    void remove_packages_from_server(const std::string& server);
    std::vector<PackageInfo>   search_packages(const std::string& query) const;
    std::vector<PackageInfo>   list_packages() const;

    // Server DB
    void upsert_server(const ServerInfo& info);
    std::vector<ServerInfo>    list_servers() const;
    std::vector<ServerInfo>    search_servers(const std::string& query) const;
    std::optional<ServerInfo>  find_server(const std::string& name) const;

    // Repository DB (HTTP/HTTPS repos)
    void upsert_repo(const RepoInfo& info);
    void remove_repo(const std::string& name);
    std::vector<RepoInfo>      list_repos() const;
    std::optional<RepoInfo>    find_repo(const std::string& name) const;

    // Installed packages tracking
    void mark_installed(const std::string& pkg_name, const std::string& version);
    void unmark_installed(const std::string& name);
    std::vector<PackageInfo> list_installed() const;
    bool is_installed(const std::string& pkg_name) const;
    std::string installed_version(const std::string& pkg_name) const;

private:
    std::string db_dir_;
    std::map<std::string, PackageInfo>  packages_;
    std::map<std::string, ServerInfo>   servers_;
    std::map<std::string, RepoInfo>     repos_;
    std::map<std::string, std::string>  installed_; // name -> version
};

} // namespace dlr