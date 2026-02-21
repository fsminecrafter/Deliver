#!/usr/bin/env bash
# ┌─────────────────────────────────────────────────────────┐
# │  Deliver — Build Script for Ubuntu 22.04 / 24.04       │
# │  Installs dependencies and compiles the project         │
# └─────────────────────────────────────────────────────────┘
set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'

info()  { echo -e "${GREEN}[✓]${NC} $*"; }
warn()  { echo -e "${YELLOW}[!]${NC} $*"; }
error() { echo -e "${RED}[✗]${NC} $*" >&2; exit 1; }
step()  { echo -e "\n${CYAN}${BOLD}==> $*${NC}"; }

echo -e "${CYAN}${BOLD}"
cat << 'BANNER'
  ____       _ _
 |  _ \  ___| (_)_   _____ _ __
 | | | |/ _ \ | \ \ / / _ \ '__|
 | |_| |  __/ | |\ V /  __/ |
 |____/ \___|_|_| \_/ \___|_|
  Build Script — Ubuntu 22.04 / 24.04
BANNER
echo -e "${NC}"

[[ $EUID -eq 0 ]] || error "Run as root: sudo ./build_ubuntu.sh"
[[ -f CMakeLists.txt ]] || error "Run from the deliver/ source directory."
command -v apt-get &>/dev/null || error "apt-get not found."

if [[ -f /etc/os-release ]]; then
    source /etc/os-release
    info "Detected: $PRETTY_NAME"
    if [[ "$ID" != "ubuntu" ]]; then
        warn "This script targets Ubuntu. Detected: $ID. Proceeding..."
    fi
    # Check version
    VER_NUM="${VERSION_ID//./}"
    if [[ "$VER_NUM" -lt 2204 ]]; then
        warn "Ubuntu ${VERSION_ID} detected. Recommend 22.04+."
    fi
fi

ARCH=$(dpkg --print-architecture 2>/dev/null || uname -m)
info "Architecture: $ARCH"

step "Updating package lists..."
apt-get update -qq

step "Installing build dependencies..."

# nlohmann-json is in universe on Ubuntu
apt-get install -y software-properties-common 2>/dev/null || true
add-apt-repository -y universe 2>/dev/null || true

DEPS=(
    build-essential
    cmake
    ninja-build
    pkg-config
    libssl-dev
    zlib1g-dev
    nlohmann-json3-dev
    tar
    gzip
    git
)

# Ubuntu 22.04 might have older cmake, check and upgrade if needed
if apt-cache show cmake | grep -q "Version: 3.2"; then
    warn "Old cmake detected. Installing newer version via Kitware APT..."
    apt-get install -y gpg wget
    wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc 2>/dev/null \
        | gpg --dearmor - | tee /usr/share/keyrings/kitware-archive-keyring.gpg >/dev/null
    echo "deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] \
        https://apt.kitware.com/ubuntu/ $(lsb_release -cs) main" \
        | tee /etc/apt/sources.list.d/kitware.list
    apt-get update -qq
fi

apt-get install -y --no-install-recommends "${DEPS[@]}"
info "Dependencies installed"

# ── Build ──────────────────────────────────────────────────────────────────────
step "Configuring build..."
BUILD_DIR="$(pwd)/build"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake .. \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS="-O2 -DNDEBUG" \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    2>&1 | grep -v "^--" || true

step "Compiling ($(nproc) cores)..."
ninja -j"$(nproc)"

info "Build successful!"
ls -lh dlr dlr_server 2>/dev/null || true

echo ""
echo -e "${GREEN}${BOLD}Build complete!${NC}"
echo "  Binaries: $BUILD_DIR"
echo "  Next:     sudo ./install_ubuntu.sh"
echo ""
