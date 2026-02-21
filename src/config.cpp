#include "config.hpp"
#include "logger.hpp"
#include <fstream>
#include <sstream>
#include <filesystem>

namespace fs = std::filesystem;

namespace dlr {

static std::map<std::string,std::string> parse_ini(const std::string& path) {
    std::map<std::string,std::string> m;
    std::ifstream f(path);
    if (!f) return m;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0]=='#' || line[0]==';' || line[0]=='[') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq+1);
        // trim
        while (!k.empty() && k.back()==' ') k.pop_back();
        while (!v.empty() && v.front()==' ') v.erase(v.begin());
        m[k] = v;
    }
    return m;
}

ServerConfig load_server_config(const std::string& path) {
    ServerConfig cfg;
    auto m = parse_ini(path);
    if (m.count("name"))          cfg.name          = m["name"];
    if (m.count("port"))          cfg.port          = (uint16_t)std::stoi(m["port"]);
    if (m.count("needs_password"))cfg.needs_password= (m["needs_password"]=="true"||m["needs_password"]=="1");
    if (m.count("password_hash")) cfg.password_hash = m["password_hash"];
    if (m.count("data_dir"))      cfg.data_dir      = m["data_dir"];
    if (m.count("registry_file")) cfg.registry_file = m["registry_file"];
    if (m.count("log_file"))      cfg.log_file      = m["log_file"];
    return cfg;
}

ClientConfig load_client_config(const std::string& path) {
    ClientConfig cfg;
    auto m = parse_ini(path);
    if (m.count("db_dir"))      cfg.db_dir      = m["db_dir"];
    if (m.count("cache_dir"))   cfg.cache_dir   = m["cache_dir"];
    if (m.count("log_file"))    cfg.log_file    = m["log_file"];
    if (m.count("install_dir")) cfg.install_dir = m["install_dir"];
    return cfg;
}

void save_server_config(const ServerConfig& cfg, const std::string& path) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream f(path);
    f << "[server]\n";
    f << "name=" << cfg.name << "\n";
    f << "port=" << cfg.port << "\n";
    f << "needs_password=" << (cfg.needs_password?"true":"false") << "\n";
    f << "password_hash=" << cfg.password_hash << "\n";
    f << "data_dir=" << cfg.data_dir << "\n";
    f << "registry_file=" << cfg.registry_file << "\n";
    f << "log_file=" << cfg.log_file << "\n";
}

void save_client_config(const ClientConfig& cfg, const std::string& path) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream f(path);
    f << "[client]\n";
    f << "db_dir=" << cfg.db_dir << "\n";
    f << "cache_dir=" << cfg.cache_dir << "\n";
    f << "log_file=" << cfg.log_file << "\n";
    f << "install_dir=" << cfg.install_dir << "\n";
}

} // namespace dlr
