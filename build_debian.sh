#!/usr/bin/env bash
# ┌─────────────────────────────────────────────────────────┐
# │  Deliver — Build Script for Debian 13 (Trixie)         │
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
  Build Script — Debian 13 (Trixie)
BANNER
echo -e "${NC}"

# ── Sanity checks ──────────────────────────────────────────────────────────────
[[ $EUID -eq 0 ]] || error "Run as root: sudo ./build_debian.sh"
[[ -f CMakeLists.txt ]] || error "Run this script from the deliver/ source directory."

command -v apt-get &>/dev/null || error "apt-get not found. Are you on Debian/Ubuntu?"

# ── OS check ──────────────────────────────────────────────────────────────────
if [[ -f /etc/os-release ]]; then
    source /etc/os-release
    info "Detected OS: ${PRETTY_NAME}"
    if [[ "$ID" != "debian" ]]; then
        warn "This script targets Debian 13. Current: ${ID}. Proceeding anyway..."
    fi
fi

ARCH=$(dpkg --print-architecture 2>/dev/null || uname -m)
info "Architecture: $ARCH"

# ── Install dependencies ───────────────────────────────────────────────────────
step "Updating package lists..."
apt-get update -qq

step "Installing build dependencies..."
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

MISSING=()
for dep in "${DEPS[@]}"; do
    if ! dpkg -l "$dep" &>/dev/null; then
        MISSING+=("$dep")
    fi
done

if [[ ${#MISSING[@]} -gt 0 ]]; then
    info "Installing: ${MISSING[*]}"
    apt-get install -y --no-install-recommends "${MISSING[@]}"
else
    info "All dependencies already installed"
fi

# Check cmake version (need 3.16+)
CMAKE_VER=$(cmake --version | head -1 | awk '{print $3}')
info "CMake version: $CMAKE_VER"

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

step "Compiling (using $(nproc) cores)..."
ninja -j"$(nproc)"

info "Build successful!"
ls -lh dlr dlr_server 2>/dev/null || ls -lh deliver deliver_server 2>/dev/null || true

echo ""
echo -e "${GREEN}${BOLD}Build complete!${NC}"
echo ""
echo "  Binaries are in: $BUILD_DIR"
echo ""
echo "  Next step: sudo ./install_debian.sh"
echo ""
