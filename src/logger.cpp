#include "logger.hpp"
#include <iostream>
#include <ctime>
#include <mutex>

namespace dlr {

static LogLevel g_level = LogLevel::INFO;
static std::mutex g_log_mtx;

void set_log_level(LogLevel l) { g_level = l; }

static std::string timestamp() {
    time_t t = time(nullptr);
    char buf[20];
    strftime(buf, sizeof(buf), "%H:%M:%S", localtime(&t));
    return buf;
}

// FIX: param order was (prefix, msg, colour_code) but callers passed (prefix, colour_code, msg)
// Corrected to match caller convention: print(prefix, colour_code, msg)
static void print(const std::string& prefix, const std::string& colour_code, const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_log_mtx);
#ifndef _WIN32
    std::cerr << "\033[" << colour_code << "m[" << timestamp() << "] [" << prefix << "]\033[0m " << msg << "\n";
#else
    std::cerr << "[" << timestamp() << "] [" << prefix << "] " << msg << "\n";
#endif
}

void log_debug(const std::string& msg) { if (g_level <= LogLevel::DEBUG) print("DEBUG", "37", msg); }
void log_info (const std::string& msg) { if (g_level <= LogLevel::INFO)  print("INFO",  "32", msg); }
void log_warn (const std::string& msg) { if (g_level <= LogLevel::WARN)  print("WARN",  "33", msg); }
void log_error(const std::string& msg) { if (g_level <= LogLevel::ERROR) print("ERROR", "31", msg); }

std::string green (const std::string& s) {
#ifndef _WIN32
    return "\033[32m" + s + "\033[0m";
#else
    return s;
#endif
}
std::string red   (const std::string& s) {
#ifndef _WIN32
    return "\033[31m" + s + "\033[0m";
#else
    return s;
#endif
}
std::string yellow(const std::string& s) {
#ifndef _WIN32
    return "\033[33m" + s + "\033[0m";
#else
    return s;
#endif
}
std::string cyan  (const std::string& s) {
#ifndef _WIN32
    return "\033[36m" + s + "\033[0m";
#else
    return s;
#endif
}
std::string bold  (const std::string& s) {
#ifndef _WIN32
    return "\033[1m" + s + "\033[0m";
#else
    return s;
#endif
}

} // namespace dlr