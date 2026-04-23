cmake_minimum_required(VERSION 3.16)
project(deliver VERSION 1.0.3 LANGUAGES CXX)

# Single source of truth: version always comes from project() above.
add_compile_definitions(DLR_VERSION="${PROJECT_VERSION}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(CURL REQUIRED)

include_directories(include)

# Common sources shared by both binaries
set(COMMON_SOURCES
    src/pkg_parser.cpp
    src/crypto.cpp
    src/network.cpp
    src/tar.cpp
    src/config.cpp
    src/logger.cpp
)

# ── Server binary ──────────────────────────────────────────────────────────────
add_executable(dlr_server
    ${COMMON_SOURCES}
    src/server_main.cpp
    src/server.cpp
    src/package_registry.cpp
)

target_link_libraries(dlr_server
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
)

# ── Client binary (main dlr CLI) ───────────────────────────────────────────────
add_executable(dlr
    ${COMMON_SOURCES}
    src/client_main.cpp
    src/client.cpp
    src/local_db.cpp
    src/server.cpp
    src/package_registry.cpp
    src/http_repo.cpp
    src/tui_app.cpp
    src/server_tui.cpp
)

target_link_libraries(dlr
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
    CURL::libcurl
)

install(TARGETS dlr dlr_server DESTINATION /usr/local/bin)