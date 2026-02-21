#pragma once
#include "types.hpp"
#include <string>
#include <optional>

namespace dlr {

// Parse a .pkg file and return PackageInfo, or nullopt on error
std::optional<PackageInfo> parse_pkg(const std::string& path);

// Write a basic .pkg file
bool write_pkg(const std::string& path, const PackageInfo& info);

// Compare version strings. Returns -1, 0, or 1
int compare_versions(const std::string& a, const std::string& b);

// Check if installed_version satisfies constraint
bool satisfies(const std::string& installed_version, const DependencyConstraint& c);

} // namespace dlr
