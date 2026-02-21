#pragma once
#include <string>
#include <vector>

namespace dlr {
namespace tar {

// Create a .tar from a list of file paths (written into base_dir)
bool create(const std::string& tar_path,
            const std::vector<std::string>& files,
            const std::string& base_dir);

// Extract .tar into dest_dir
bool extract(const std::string& tar_path, const std::string& dest_dir);

// List contents
std::vector<std::string> list(const std::string& tar_path);

} // namespace tar
} // namespace dlr
