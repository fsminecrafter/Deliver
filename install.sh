#!/usr/bin/env bash
# Deliver Package Manager - Installer
# Supports: Debian 13 (Trixie), Ubuntu 22.04+
# Windows: See README for WSL or MinGW instructions

set -e

INSTALL_PREFIX="/usr/local"
SERVICE_USER="deliver"
CONFIG_DIR="/etc/deliver"
DATA_DIR="/var/lib/deliver"
CACHE_DIR="/var/cache/deliver"
LOG_DIR="/var/log/deliver"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'
BOLD='\033[1m'; NC='\033[0m'
info()  { echo -e "${GREEN}[✓]${NC} $*"; }
warn()  { echo -e "${YELLOW}[!]${NC} $*"; }
error() { echo -e "${RED}[✗]${NC} $*" >&2; }
step()  { echo -e "\n${CYAN}${BOLD}==>${NC} $*"; }

# Check root
if [[ $EUID -ne 0 ]]; then
    error "This installer must be run as root (use sudo)."
    exit 1
fi

# ── Read version from CMakeLists.txt ──────────────────────────────────────────
DLR_VERSION="unknown"
if [[ -f CMakeLists.txt ]]; then
    DLR_VERSION=$(grep -m1 'project(deliver VERSION' CMakeLists.txt \
                  | sed 's/.*VERSION \([0-9][0-9.]*\).*/\1/' || true)
fi

echo -e "${CYAN}${BOLD}"
cat <<'EOF'
  ____       _ _
 |  _ \  ___| (_)_   _____ _ __
 | | | |/ _ \ | \ \ / / _ \ '__|
 | |_| |  __/ | |\ V /  __/ |
 |____/ \___|_|_| \_/ \___|_|
 Package Manager by fsminecrafter
EOF
echo -e "  Version: ${DLR_VERSION}${NC}"
echo ""

# ── Detect OS ──────────────────────────────────────────────────────────────────
step "Detecting system..."
if [[ -f /etc/os-release ]]; then
    source /etc/os-release
    info "OS: $PRETTY_NAME"
else
    warn "Cannot detect OS. Proceeding with generic Linux install."
    ID="unknown"
fi

# ── Install dependencies ───────────────────────────────────────────────────────
step "Installing build dependencies..."

case "$ID" in
    debian|ubuntu|linuxmint)
        apt-get update -qq
        apt-get install -y --no-install-recommends \
            build-essential cmake ninja-build \
            libssl-dev zlib1g-dev \
            nlohmann-json3-dev \
            libcurl4-openssl-dev \
            tar gzip
        ;;
    fedora)
        dnf install -y gcc-c++ cmake ninja-build openssl-devel zlib-devel nlohmann-json-devel tar
        ;;
    arch|manjaro)
        pacman -Sy --noconfirm gcc cmake ninja openssl zlib nlohmann-json tar
        ;;
    *)
        warn "Unknown distro '$ID'. Please install manually: cmake, libssl-dev, zlib-dev, nlohmann-json"
        ;;
esac

# ── Build ──────────────────────────────────────────────────────────────────────
step "Building Deliver v${DLR_VERSION}..."

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake .. -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX"
ninja -j"$(nproc)"

# ── Install binaries ───────────────────────────────────────────────────────────
step "Installing binaries..."
ninja install
info "Installed dlr and dlr_server to $INSTALL_PREFIX/bin"

# ── Create system user ─────────────────────────────────────────────────────────
step "Setting up system user..."
if ! id -u "$SERVICE_USER" &>/dev/null; then
    useradd -r -s /sbin/nologin -d "$DATA_DIR" "$SERVICE_USER"
    info "Created system user: $SERVICE_USER"
else
    info "User '$SERVICE_USER' already exists"
fi

# ── Create directories ─────────────────────────────────────────────────────────
step "Creating directories..."
for d in "$CONFIG_DIR" "$DATA_DIR/packages" "$DATA_DIR/client" "$CACHE_DIR" "$LOG_DIR" "/usr/local/deliver"; do
    mkdir -p "$d"
done
chown -R "$SERVICE_USER:$SERVICE_USER" "$DATA_DIR" "$CACHE_DIR" "$LOG_DIR"
chmod 750 "$DATA_DIR" "$CACHE_DIR"

# ── Default config files ───────────────────────────────────────────────────────
step "Writing default config files..."

if [[ ! -f "$CONFIG_DIR/server.conf" ]]; then
    HOSTNAME=$(hostname -s 2>/dev/null || echo "deliver-server")
    cat > "$CONFIG_DIR/server.conf" <<CONF
[server]
name=$HOSTNAME
port=4242
needs_password=false
password_hash=
data_dir=$DATA_DIR/packages
registry_file=$DATA_DIR/registry.json
log_file=$LOG_DIR/server.log
CONF
    info "Created $CONFIG_DIR/server.conf"
else
    info "Server config already exists, skipping"
fi

if [[ ! -f "$CONFIG_DIR/client.conf" ]]; then
    cat > "$CONFIG_DIR/client.conf" <<CONF
[client]
db_dir=$DATA_DIR/client
cache_dir=$CACHE_DIR
log_file=$LOG_DIR/client.log
install_dir=/usr/local/deliver
CONF
    info "Created $CONFIG_DIR/client.conf"
else
    info "Client config already exists, skipping"
fi

# ── Systemd service ────────────────────────────────────────────────────────────
step "Installing systemd service..."

cat > /etc/systemd/system/deliver-server.service <<SERVICE
[Unit]
Description=Deliver LAN Package Manager Server v${DLR_VERSION}
After=network.target
Wants=network.target

[Service]
Type=simple
User=$SERVICE_USER
Group=$SERVICE_USER
ExecStart=$INSTALL_PREFIX/bin/dlr_server
ExecReload=/bin/kill -HUP \$MAINPID
Restart=on-failure
RestartSec=5
StandardOutput=journal
StandardError=journal
SyslogIdentifier=deliver-server

# Security hardening
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ReadWritePaths=$DATA_DIR $CACHE_DIR $LOG_DIR

[Install]
WantedBy=multi-user.target
SERVICE

systemctl daemon-reload
systemctl enable deliver-server
systemctl start deliver-server

if systemctl is-active --quiet deliver-server; then
    info "deliver-server service started and enabled"
else
    warn "Service failed to start. Check: journalctl -u deliver-server"
fi

# ── Shell completion ────────────────────────────────────────────────────────────
step "Installing bash completion..."
cat > /etc/bash_completion.d/dlr <<'COMP'
_dlr_completion() {
    local cur="${COMP_WORDS[COMP_CWORD]}"
    local commands="install download scan ping search servers list status restart \
                    presentfile presentfolder attach generate make \
                    unpresentfile unpresentfolder removepackage clear"
    if [[ COMP_CWORD -eq 1 ]]; then
        COMPREPLY=($(compgen -W "$commands" -- "$cur"))
    fi
}
complete -F _dlr_completion dlr
COMP
info "Bash completion installed"

# ── Final summary ──────────────────────────────────────────────────────────────
echo -e "\n${GREEN}${BOLD}╔══════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}${BOLD}║   Deliver v${DLR_VERSION} installed successfully!  ✓      ║${NC}"
echo -e "${GREEN}${BOLD}╚══════════════════════════════════════════════════╝${NC}"
echo ""
echo "  Config:  $CONFIG_DIR/"
echo "  Data:    $DATA_DIR/"
echo "  Logs:    $LOG_DIR/server.log"
echo ""
echo "  Quick start:"
echo "    sudo dlr presentfile /path/to/myapp myapp"
echo "    sudo dlr generate list.pkg myapp"
echo "    dlr scan"
echo "    dlr install myapp"
echo ""
echo "  Remove packages:"
echo "    sudo dlr removepackage myapp"
echo "    sudo dlr unpresentfile myapp"
echo "    sudo dlr clear                    # remove all"
echo ""
echo "  Service:  systemctl {start|stop|status} deliver-server"
echo ""
