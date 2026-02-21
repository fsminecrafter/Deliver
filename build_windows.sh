#!/usr/bin/env bash
# ┌─────────────────────────────────────────────────────────────────────────┐
# │  Deliver — Build Script for Windows (MSYS2 / MinGW-w64)               │
# │                                                                         │
# │  Run this inside an MSYS2 MINGW64 shell:                               │
# │    Right-click Start → search "MSYS2 MinGW 64-bit"                     │
# │    cd /path/to/deliver                                                  │
# │    ./build_windows.sh                                                   │
# │                                                                         │
# │  If you prefer WSL2, use build_debian.sh or build_ubuntu.sh instead.  │
# └─────────────────────────────────────────────────────────────────────────┘
set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'

info()  { echo -e "${GREEN}[OK]${NC} $*"; }
warn()  { echo -e "${YELLOW}[!!]${NC} $*"; }
error() { echo -e "${RED}[ERR]${NC} $*" >&2; exit 1; }
step()  { echo -e "\n${CYAN}${BOLD}==> $*${NC}"; }

echo -e "${CYAN}${BOLD}"
cat << 'BANNER'
  ____       _ _
 |  _ \  ___| (_)_   _____ _ __
 | | | |/ _ \ | \ \ / / _ \ '__|
 | |_| |  __/ | |\ V /  __/ |
 |____/ \___|_|_| \_/ \___|_|
  Build Script — Windows (MSYS2 / MinGW-w64)
BANNER
echo -e "${NC}"

[[ -f CMakeLists.txt ]] || error "Run from the deliver/ source directory."

# ── Detect environment ─────────────────────────────────────────────────────────
if [[ -n "${MSYSTEM:-}" ]]; then
    info "MSYS2 environment detected: $MSYSTEM"
    if [[ "$MSYSTEM" != "MINGW64" ]]; then
        warn "Recommend using MINGW64 shell (64-bit). Current: $MSYSTEM"
    fi
elif command -v cl.exe &>/dev/null; then
    info "MSVC detected — switching to MSVC build path"
    BUILD_WITH_MSVC=1
else
    warn "Neither MSYS2 nor MSVC detected."
    warn "Options:"
    warn "  1. Install MSYS2 from https://www.msys2.org/ (recommended)"
    warn "  2. Use WSL2 with build_debian.sh"
    warn "  3. Install Visual Studio 2022 with C++ workload"
    error "Cannot continue without a suitable build environment."
fi

ARCH=$(uname -m 2>/dev/null || echo "x86_64")
info "Target arch: $ARCH"

# ── MSYS2 / MinGW path ────────────────────────────────────────────────────────
if [[ -z "${BUILD_WITH_MSVC:-}" ]]; then

    step "Checking MSYS2 package manager (pacman)..."
    command -v pacman &>/dev/null || error "pacman not found. Are you in an MSYS2 shell?"

    step "Syncing packages..."
    pacman -Sy --noconfirm 2>/dev/null || warn "Sync failed (offline?)"

    step "Installing MinGW-w64 build tools..."
    PKGS=(
        mingw-w64-x86_64-gcc
        mingw-w64-x86_64-g++
        mingw-w64-x86_64-cmake
        mingw-w64-x86_64-ninja
        mingw-w64-x86_64-openssl
        mingw-w64-x86_64-zlib
        mingw-w64-x86_64-nlohmann-json
        mingw-w64-x86_64-pkg-config
        mingw-w64-x86_64-make
    )

    MISSING=()
    for pkg in "${PKGS[@]}"; do
        if ! pacman -Q "$pkg" &>/dev/null; then
            MISSING+=("$pkg")
        fi
    done

    if [[ ${#MISSING[@]} -gt 0 ]]; then
        info "Installing: ${MISSING[*]}"
        pacman -S --noconfirm "${MISSING[@]}"
    else
        info "All MSYS2 packages present"
    fi

    step "Configuring CMake (MinGW Makefiles)..."
    BUILD_DIR="$(pwd)/build_win"
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"

    cmake .. \
        -G "Ninja" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="-O2 -DNDEBUG -D_WIN32_WINNT=0x0601" \
        -DCMAKE_INSTALL_PREFIX="$(pwd)/../dist_win" \
        -DCMAKE_SYSTEM_NAME=Windows \
        2>&1 | grep -v "^--" || true

    step "Compiling..."
    ninja -j${NUMBER_OF_PROCESSORS:-4}

    info "Build complete!"
    ls -lh dlr.exe dlr_server.exe 2>/dev/null || ls -lh *.exe 2>/dev/null || true

    echo ""
    echo -e "${GREEN}${BOLD}Windows build complete!${NC}"
    echo ""
    echo "  Binaries: $BUILD_DIR"
    echo "  dlr.exe"
    echo "  dlr_server.exe"
    echo ""
    echo "  Run ./install_windows.sh to install system-wide."
    echo ""
    echo "  NOTE: dlr_server.exe must be allowed through Windows Firewall:"
    echo "    TCP 4242 (inbound) and UDP 4243 (inbound)"
    echo ""

else
    # ── MSVC path ──────────────────────────────────────────────────────────────
    step "MSVC build path"
    warn "nlohmann-json: you may need to manually install via vcpkg:"
    warn "  vcpkg install nlohmann-json openssl zlib"
    warn "  Then pass -DCMAKE_TOOLCHAIN_FILE=[vcpkg]/scripts/buildsystems/vcpkg.cmake"

    BUILD_DIR="$(pwd)/build_win_msvc"
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"

    VCPKG_ROOT="${VCPKG_ROOT:-C:/vcpkg}"
    CMAKE_EXTRA=""
    if [[ -f "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" ]]; then
        CMAKE_EXTRA="-DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        info "Using vcpkg toolchain: $VCPKG_ROOT"
    else
        warn "vcpkg not found at $VCPKG_ROOT. Set VCPKG_ROOT env variable."
    fi

    cmake .. \
        -G "Visual Studio 17 2022" \
        -A x64 \
        -DCMAKE_BUILD_TYPE=Release \
        $CMAKE_EXTRA \
        2>&1 | grep -v "^--" || true

    cmake --build . --config Release --parallel

    echo -e "${GREEN}${BOLD}MSVC build complete!${NC}"
    echo "  Binaries in: $BUILD_DIR/Release/"
fi
