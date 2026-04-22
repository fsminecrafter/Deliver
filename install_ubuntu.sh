#!/usr/bin/env bash
# ┌─────────────────────────────────────────────────────────┐
# │  Deliver — Installer for Ubuntu 22.04 / 24.04          │
# │  Run AFTER build_ubuntu.sh                             │
# └─────────────────────────────────────────────────────────┘
set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'

info()  { echo -e "  ${GREEN}✓${NC} $*"; }
warn()  { echo -e "  ${YELLOW}⚠${NC} $*"; }
error() { echo -e "  ${RED}✗${NC} $*" >&2; exit 1; }
step()  { echo -e "\n${CYAN}${BOLD}──► $*${NC}"; }

# ── Read version from CMakeLists.txt ──────────────────────────────────────────
DLR_VERSION="unknown"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -f "$SCRIPT_DIR/CMakeLists.txt" ]]; then
    DLR_VERSION=$(grep -m1 'project(deliver VERSION' "$SCRIPT_DIR/CMakeLists.txt" \
                  | sed 's/.*VERSION \([0-9][0-9.]*\).*/\1/' || true)
fi

echo -e "${CYAN}${BOLD}"
cat << 'BANNER'
╔═══════════════════════════════════════════╗
║   Deliver Installer — Ubuntu              ║
╚═══════════════════════════════════════════╝
BANNER
echo -e "   Version: ${DLR_VERSION}${NC}\n"

[[ $EUID -eq 0 ]] || error "Run as root: sudo ./install_ubuntu.sh"

if [[ -f /etc/os-release ]]; then
    source /etc/os-release
    info "OS: $PRETTY_NAME"
fi

BUILD_DIR="$SCRIPT_DIR/build"

[[ -f "$BUILD_DIR/dlr" ]]        || error "Binary not found. Run 'sudo ./build_ubuntu.sh' first."
[[ -f "$BUILD_DIR/dlr_server" ]] || error "dlr_server not found."

BIN_DIR="/usr/local/bin"
CONFIG_DIR="/etc/deliver"
DATA_DIR="/var/lib/deliver"
CACHE_DIR="/var/cache/deliver"
LOG_DIR="/var/log/deliver"
INSTALL_DIR="/usr/local/deliver"
SERVICE_USER="deliver"

step "Creating system user..."
if ! id -u "$SERVICE_USER" &>/dev/null; then
    useradd -r -s /usr/sbin/nologin -d "$DATA_DIR" -c "Deliver daemon" "$SERVICE_USER"
    info "Created: $SERVICE_USER"
else
    info "User exists: $SERVICE_USER"
fi

step "Creating directories..."
for d in "$CONFIG_DIR" "$DATA_DIR/packages" "$DATA_DIR/client" \
          "$CACHE_DIR" "$LOG_DIR" "$INSTALL_DIR"; do
    mkdir -p "$d" && info "$d"
done
chown -R "$SERVICE_USER:$SERVICE_USER" "$DATA_DIR" "$CACHE_DIR" "$LOG_DIR"

step "Installing binaries..."
install -m 755 "$BUILD_DIR/dlr"        "$BIN_DIR/dlr"
install -m 755 "$BUILD_DIR/dlr_server" "$BIN_DIR/dlr_server"
info "dlr        → $BIN_DIR/dlr"
info "dlr_server → $BIN_DIR/dlr_server"

step "Writing config..."
HOSTNAME=$(hostname -s 2>/dev/null || echo "deliver-server")

if [[ ! -f "$CONFIG_DIR/server.conf" ]]; then
    cat > "$CONFIG_DIR/server.conf" << CONF
[server]
name=$HOSTNAME
port=4242
needs_password=false
password_hash=
data_dir=$DATA_DIR/packages
registry_file=$DATA_DIR/registry.json
log_file=$LOG_DIR/server.log
CONF
    chmod 640 "$CONFIG_DIR/server.conf"
    chown root:"$SERVICE_USER" "$CONFIG_DIR/server.conf"
    info "server.conf"
fi

if [[ ! -f "$CONFIG_DIR/client.conf" ]]; then
    cat > "$CONFIG_DIR/client.conf" << CONF
[client]
db_dir=$DATA_DIR/client
cache_dir=$CACHE_DIR
log_file=$LOG_DIR/client.log
install_dir=$INSTALL_DIR
CONF
    info "client.conf"
fi

step "Installing systemd service..."
cat > /etc/systemd/system/deliver-server.service << SERVICE
[Unit]
Description=Deliver LAN Package Manager Server v${DLR_VERSION}
After=network.target network-online.target
Wants=network-online.target

[Service]
Type=simple
User=$SERVICE_USER
Group=$SERVICE_USER
ExecStart=$BIN_DIR/dlr_server
ExecReload=/bin/kill -HUP \$MAINPID
Restart=on-failure
RestartSec=5s
StandardOutput=journal
StandardError=journal
SyslogIdentifier=deliver-server
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=$DATA_DIR $CACHE_DIR $LOG_DIR

[Install]
WantedBy=multi-user.target
SERVICE

systemctl daemon-reload
systemctl enable deliver-server
systemctl start deliver-server
sleep 1
if systemctl is-active --quiet deliver-server; then
    info "deliver-server running"
else
    warn "Service didn't start — check: journalctl -u deliver-server"
fi

# ── Bash completion ────────────────────────────────────────────────────────────
step "Installing bash completion..."
cat > /etc/bash_completion.d/dlr << 'COMP'
_dlr_complete() {
    local cur="${COMP_WORDS[COMP_CWORD]}"
    local cmds="install download scan list ping search servers status restart \
                presentfile presentfolder attach generate make \
                unpresentfile unpresentfolder removepackage clear"
    case "${COMP_WORDS[COMP_CWORD-1]}" in
        install|download|search) ;;
        ping|servers) ;;
        *) COMPREPLY=($(compgen -W "$cmds" -- "$cur")) ;;
    esac
}
complete -F _dlr_complete dlr
COMP
info "Bash completion installed"

# UFW
if command -v ufw &>/dev/null && ufw status | grep -q "Status: active"; then
    step "Configuring UFW..."
    ufw allow 4242/tcp comment "Deliver TCP" 2>/dev/null || true
    ufw allow 4243/udp comment "Deliver UDP" 2>/dev/null || true
    info "Firewall rules added"
fi

# AppArmor profile (Ubuntu-specific)
if command -v apparmor_status &>/dev/null; then
    step "Checking AppArmor..."
    info "AppArmor active — Deliver runs with standard user restrictions (no profile needed)"
fi

echo ""
echo -e "${GREEN}${BOLD}"
echo "╔═══════════════════════════════════════════╗"
echo "║   ✓  Deliver v${DLR_VERSION} installed!         ║"
echo "╚═══════════════════════════════════════════╝"
echo -e "${NC}"
echo "  dlr scan                    → discover servers"
echo "  dlr list                    → list packages"
echo "  dlr install -y <name>       → install a package"
echo "  sudo dlr status             → server status"
echo "  sudo dlr removepackage <n>  → remove a package"
echo "  sudo dlr clear              → remove all packages"
echo ""