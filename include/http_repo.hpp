#pragma once
#include "types.hpp"
#include <string>
#include <vector>
#include <optional>
#include <functional>

namespace dlr {
namespace repo {

struct RepoPackage {
    std::string name;
    std::string version;
    std::string description;
    std::string download_url;     // full URL to the .tar file
    std::string sha256;           // expected checksum (may be empty)
    Arch        arch{Arch::ANY};
    OS          operatingsystem{OS::ANY};
    std::vector<std::string> dependencies;
    std::string installscript;
    std::string installcommand;
    std::string rivalpack;
    std::string repo_name;        // which repo this came from
};

struct RepoIndex {
    std::string             name;
    std::string             description;
    std::string             base_url;
    std::vector<RepoPackage> packages;
};

using ProgressCb = std::function<void(size_t current, size_t total)>;

// Fetch and parse a repository index from a URL.
// Appends /index.json if the URL does not end with .json.
std::optional<RepoIndex> fetch_index(const std::string& url,
                                     const std::string& repo_name);

// Download a file from URL to dest_path, optionally verifying checksum.
bool download_file(const std::string& url,
                   const std::string& dest_path,
                   const std::string& expected_sha256 = "",
                   ProgressCb progress = nullptr);

// Returns true if libcurl is available.
bool http_available();

} // namespace repo
} // namespace dlr