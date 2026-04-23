#include "http_repo.hpp"
#include "crypto.hpp"
#include "logger.hpp"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace dlr {
namespace repo {

// ── curl write callbacks ───────────────────────────────────────────────────────

static size_t cb_string(char* p, size_t sz, size_t n, std::string* s) {
    s->append(p, sz * n);
    return sz * n;
}

struct FileCtx {
    std::ofstream* f;
    ProgressCb     progress;
    size_t         written{0};
    size_t         total{0};
};

static size_t cb_file(char* p, size_t sz, size_t n, FileCtx* ctx) {
    size_t bytes = sz * n;
    ctx->f->write(p, bytes);
    ctx->written += bytes;
    if (ctx->progress && ctx->total > 0)
        ctx->progress(ctx->written, ctx->total);
    return bytes;
}

static int cb_progress(void* ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    auto* ctx = static_cast<FileCtx*>(ud);
    if (dltotal > 0) {
        ctx->total = (size_t)dltotal;
        if (ctx->progress)
            ctx->progress((size_t)dlnow, (size_t)dltotal);
    }
    return 0;
}

// ── helpers ───────────────────────────────────────────────────────────────────

bool http_available() {
    CURL* c = curl_easy_init();
    if (!c) return false;
    curl_easy_cleanup(c);
    return true;
}

static std::string http_get(const std::string& url, long timeout = 20) {
    CURL* c = curl_easy_init();
    if (!c) { log_error("curl_easy_init failed"); return {}; }

    std::string body;
    curl_easy_setopt(c, CURLOPT_URL,           url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,  cb_string);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,      &body);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,        timeout);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT,     "Deliver/" DLR_VERSION);

    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) {
        log_error("HTTP GET failed [" + url + "]: " + curl_easy_strerror(rc));
        body.clear();
    }
    curl_easy_cleanup(c);
    return body;
}

// ── public API ────────────────────────────────────────────────────────────────

std::optional<RepoIndex> fetch_index(const std::string& url,
                                     const std::string& repo_name) {
    // Normalise URL → .../index.json
    std::string idx_url = url;
    if (!idx_url.empty() && idx_url.back() == '/') idx_url.pop_back();
    if (idx_url.size() < 5 ||
        idx_url.substr(idx_url.size() - 5) != ".json")
        idx_url += "/index.json";

    log_info("Fetching repo index: " + idx_url);
    std::string body = http_get(idx_url);
    if (body.empty()) return std::nullopt;

    try {
        auto j = json::parse(body);
        RepoIndex idx;
        idx.name        = j.value("name",        repo_name);
        idx.description = j.value("description", "");
        idx.base_url    = url;

        for (auto& p : j.value("packages", json::array())) {
            RepoPackage pkg;
            pkg.name           = p.value("name",           "");
            pkg.version        = p.value("version",        "1.0");
            pkg.description    = p.value("description",    "");
            pkg.download_url   = p.value("url",            "");
            pkg.sha256         = p.value("sha256",         "");
            pkg.arch           = arch_from_string(p.value("arch", "any"));
            pkg.operatingsystem= os_from_string  (p.value("os",   "any"));
            pkg.installscript  = p.value("installscript",  "");
            pkg.installcommand = p.value("installcommand", "");
            pkg.rivalpack      = p.value("rivalpack",      "");
            pkg.repo_name      = repo_name;

            if (p.contains("dependencies"))
                for (auto& d : p["dependencies"])
                    pkg.dependencies.push_back(d.get<std::string>());

            // Make relative download_url absolute
            if (!pkg.download_url.empty() &&
                pkg.download_url.substr(0,4) != "http") {
                std::string base = url;
                if (base.back() != '/') base += '/';
                pkg.download_url = base + pkg.download_url;
            }

            if (!pkg.name.empty() && !pkg.download_url.empty())
                idx.packages.push_back(std::move(pkg));
        }
        log_info("Repo '" + idx.name + "': " +
                 std::to_string(idx.packages.size()) + " package(s)");
        return idx;

    } catch (const std::exception& e) {
        log_error("Failed to parse repo index from " + idx_url +
                  ": " + e.what());
        return std::nullopt;
    }
}

bool download_file(const std::string& url,
                   const std::string& dest_path,
                   const std::string& expected_sha256,
                   ProgressCb progress) {
    CURL* c = curl_easy_init();
    if (!c) { log_error("curl_easy_init failed"); return false; }

    std::ofstream out(dest_path, std::ios::binary);
    if (!out) {
        log_error("Cannot open destination: " + dest_path);
        curl_easy_cleanup(c);
        return false;
    }

    FileCtx ctx{&out, progress, 0, 0};

    curl_easy_setopt(c, CURLOPT_URL,             url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,    cb_file);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,        &ctx);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, cb_progress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA,     &ctx);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS,       0L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION,   1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,          300L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER,   1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT,       "Deliver/" DLR_VERSION);

    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    out.close();

    if (rc != CURLE_OK) {
        log_error("Download failed [" + url + "]: " + curl_easy_strerror(rc));
        fs::remove(dest_path);
        return false;
    }

    if (!expected_sha256.empty()) {
        std::string actual = crypto::sha256_hex_file(dest_path);
        if (actual != expected_sha256) {
            log_error("Checksum mismatch for " + url);
            log_error("  expected: " + expected_sha256);
            log_error("  got:      " + actual);
            fs::remove(dest_path);
            return false;
        }
    }
    return true;
}

} // namespace repo
} // namespace dlr