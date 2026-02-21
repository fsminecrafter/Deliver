#pragma once
#include <string>

namespace dlr {

enum class LogLevel { DEBUG, INFO, WARN, ERROR };

void set_log_level(LogLevel l);
void log_debug(const std::string& msg);
void log_info(const std::string& msg);
void log_warn(const std::string& msg);
void log_error(const std::string& msg);

// ANSI colour helpers
std::string green(const std::string& s);
std::string red(const std::string& s);
std::string yellow(const std::string& s);
std::string cyan(const std::string& s);
std::string bold(const std::string& s);

} // namespace dlr
