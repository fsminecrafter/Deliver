#include "local_db.hpp"
#include "logger.hpp"
#include "pkg_parser.hpp"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace dlr {

LocalDB::LocalDB(const std::string& db_dir) : db_dir_(db_dir) {
    std::error_code ec;
    fs::create_directories(db_dir_, ec);
}

static std::string pkg_db_path(const std::string& d) { return d + "/packages.json"; }
static std::string srv_db_path(const std::string& d) { return d + "/servers.json"; }
static std::string ins_db_path(const std::string& d) { return d + "/installed.json"; }
static std::string rep_db_path(const std::string& d) { return d + "/repos.json"; }

void LocalDB::load() {
    // Packages
    {
        std::ifstream f(pkg_db_path(db_dir_));
        if (f) {
            try {
                json j; f >> j;
                for (auto& [name, obj] : j.items()) {
                    PackageInfo info;
                    info.name            = name;
                    info.version         = obj.value("version","1.0");
                    info.description     = obj.value("description","");
                    info.server_origin   = obj.value("server","");
                    info.rivalpack       = obj.value("rivalpack","");
                    info.installscript   = obj.value("installscript","");
                    info.installcommand  = obj.value("installcommand","");
                    info.file_path       = obj.value("file_path","");
                    info.arch            = arch_from_string(obj.value("arch","any"));
                    info.operatingsystem = os_from_string(obj.value("os","any"));
                    if (obj.contains("dependencies"))
                        for (auto& d : obj["dependencies"])
                            info.dependencies.push_back(d.get<std::string>());
                    packages_[name] = info;
                }
            } catch (...) {}
        }
    }
    // Servers
    {
        std::ifstream f(srv_db_path(db_dir_));
        if (f) {
            try {
                json j; f >> j;
                for (auto& [name, obj] : j.items()) {
                    ServerInfo si;
                    si.name           = name;
                    si.host           = obj.value("host","");
                    si.port           = obj.value("port",(uint16_t)4242);
                    si.needs_password = obj.value("needs_password",false);
                    servers_[name]    = si;
                }
            } catch (...) {}
        }
    }
    // Repos
    {
        std::ifstream f(rep_db_path(db_dir_));
        if (f) {
            try {
                json j; f >> j;
                for (auto& [name, obj] : j.items()) {
                    RepoInfo ri;
                    ri.name        = name;
                    ri.url         = obj.value("url","");
                    ri.description = obj.value("description","");
                    ri.enabled     = obj.value("enabled",true);
                    repos_[name]   = ri;
                }
            } catch (...) {}
        }
    }
    // Installed
    {
        std::ifstream f(ins_db_path(db_dir_));
        if (f) {
            try {
                json j; f >> j;
                for (auto& [name, ver] : j.items())
                    installed_[name] = ver.get<std::string>();
            } catch (...) {}
        }
    }
}

void LocalDB::save() const {
    // Packages
    {
        json j;
        for (auto& [name, info] : packages_) {
            j[name] = {
                {"version",        info.version},
                {"description",    info.description},
                {"server",         info.server_origin},
                {"rivalpack",      info.rivalpack},
                {"installscript",  info.installscript},
                {"installcommand", info.installcommand},
                {"file_path",      info.file_path},
                {"arch",           arch_to_string(info.arch)},
                {"os",             os_to_string(info.operatingsystem)},
                {"dependencies",   info.dependencies}
            };
        }
        std::ofstream f(pkg_db_path(db_dir_));
        f << j.dump(2);
    }
    // Servers
    {
        json j;
        for (auto& [name, si] : servers_) {
            j[name] = {
                {"host",           si.host},
                {"port",           si.port},
                {"needs_password", si.needs_password}
            };
        }
        std::ofstream f(srv_db_path(db_dir_));
        f << j.dump(2);
    }
    // Repos
    {
        json j;
        for (auto& [name, ri] : repos_) {
            j[name] = {
                {"url",         ri.url},
                {"description", ri.description},
                {"enabled",     ri.enabled}
            };
        }
        std::ofstream f(rep_db_path(db_dir_));
        f << j.dump(2);
    }
    // Installed
    {
        json j;
        for (auto& [name, ver] : installed_) j[name] = ver;
        std::ofstream f(ins_db_path(db_dir_));
        f << j.dump(2);
    }
}

// ── Package DB ────────────────────────────────────────────────────────────────

void LocalDB::upsert_package(const PackageInfo& info) { packages_[info.name] = info; }

std::optional<PackageInfo> LocalDB::find_package(const std::string& name) const {
    auto it = packages_.find(name);
    if (it == packages_.end()) return std::nullopt;
    return it->second;
}

std::vector<PackageInfo> LocalDB::search_packages(const std::string& query) const {
    std::vector<PackageInfo> r;
    std::string q = query;
    std::transform(q.begin(), q.end(), q.begin(), ::tolower);
    for (auto& [name, info] : packages_) {
        std::string n = name;
        std::transform(n.begin(), n.end(), n.begin(), ::tolower);
        std::string desc = info.description;
        std::transform(desc.begin(), desc.end(), desc.begin(), ::tolower);
        if (n.find(q) != std::string::npos || desc.find(q) != std::string::npos)
            r.push_back(info);
    }
    return r;
}

std::vector<PackageInfo> LocalDB::list_packages() const {
    std::vector<PackageInfo> r;
    for (auto& [n, i] : packages_) r.push_back(i);
    return r;
}

// ── Server DB ─────────────────────────────────────────────────────────────────

void LocalDB::upsert_server(const ServerInfo& info) { servers_[info.name] = info; }

std::vector<ServerInfo> LocalDB::list_servers() const {
    std::vector<ServerInfo> r;
    for (auto& [n, s] : servers_) r.push_back(s);
    return r;
}

std::vector<ServerInfo> LocalDB::search_servers(const std::string& query) const {
    std::vector<ServerInfo> r;
    std::string q = query;
    std::transform(q.begin(), q.end(), q.begin(), ::tolower);
    for (auto& [name, si] : servers_) {
        std::string n = name;
        std::transform(n.begin(), n.end(), n.begin(), ::tolower);
        if (n.find(q) != std::string::npos || si.host.find(query) != std::string::npos)
            r.push_back(si);
    }
    return r;
}

std::optional<ServerInfo> LocalDB::find_server(const std::string& name) const {
    auto it = servers_.find(name);
    if (it == servers_.end()) return std::nullopt;
    return it->second;
}

// ── Repo DB ───────────────────────────────────────────────────────────────────

void LocalDB::upsert_repo(const RepoInfo& info) { repos_[info.name] = info; }

void LocalDB::remove_repo(const std::string& name) { repos_.erase(name); }

std::vector<RepoInfo> LocalDB::list_repos() const {
    std::vector<RepoInfo> r;
    for (auto& [n, ri] : repos_) r.push_back(ri);
    return r;
}

std::optional<RepoInfo> LocalDB::find_repo(const std::string& name) const {
    auto it = repos_.find(name);
    if (it == repos_.end()) return std::nullopt;
    return it->second;
}

// ── Installed tracking ─────────────────────────────────────────────────────────

void LocalDB::mark_installed(const std::string& name, const std::string& version) {
    installed_[name] = version;
}

bool LocalDB::is_installed(const std::string& name) const {
    return installed_.count(name) > 0;
}

std::string LocalDB::installed_version(const std::string& name) const {
    auto it = installed_.find(name);
    return (it == installed_.end()) ? "" : it->second;
}

} // namespace dlr