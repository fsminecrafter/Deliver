#!/usr/bin/env bash
# ┌─────────────────────────────────────────────────────────┐
# │  Deliver — Installer for Debian 13 (Trixie)            │
# │  Run AFTER build_debian.sh                             │
# └─────────────────────────────────────────────────────────┘
set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'

info()  { echo -e "  ${GREEN}✓${NC} $*"; }
warn()  { echo -e "  ${YELLOW}⚠${NC} $*"; }
error() { echo -e "  ${RED}✗${NC} $*" >&2; exit 1; }
step()  { echo -e "\n${CYAN}${BOLD}──► $*${NC}"; }
banner(){ echo -e "${CYAN}${BOLD}$*${NC}"; }

# ── Read version from CMakeLists.txt ──────────────────────────────────────────
DLR_VERSION="unknown"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -f "$SCRIPT_DIR/CMakeLists.txt" ]]; then
    DLR_VERSION=$(grep -m1 'project(deliver VERSION' "$SCRIPT_DIR/CMakeLists.txt" \
                  | sed 's/.*VERSION \([0-9][0-9.]*\).*/\1/' || true)
fi

banner "╔═══════════════════════════════════════════╗"
banner "║   Deliver Installer — Debian 13           ║"
banner "╚═══════════════════════════════════════════╝"
echo -e "   Version: ${DLR_VERSION}\n"

[[ $EUID -eq 0 ]] || error "Must be run as root: sudo ./install_debian.sh"

BUILD_DIR="$SCRIPT_DIR/build"

# ── Check binaries ─────────────────────────────────────────────────────────────
step "Checking build output..."
if [[ ! -f "$BUILD_DIR/dlr" || ! -f "$BUILD_DIR/dlr_server" ]]; then
    error "Binaries not found in $BUILD_DIR. Run 'sudo ./build_debian.sh' first."
fi
info "Found: dlr        $(du -sh "$BUILD_DIR/dlr"        | cut -f1)"
info "Found: dlr_server $(du -sh "$BUILD_DIR/dlr_server" | cut -f1)"

# ── Paths ──────────────────────────────────────────────────────────────────────
BIN_DIR="/usr/local/bin"
CONFIG_DIR="/etc/deliver"
DATA_DIR="/var/lib/deliver"
CACHE_DIR="/var/cache/deliver"
LOG_DIR="/var/log/deliver"
INSTALL_DIR="/usr/local/deliver"
SERVICE_USER="deliver"

# ── System user ────────────────────────────────────────────────────────────────
step "Setting up system user..."
if ! id -u "$SERVICE_USER" &>/dev/null; then
    useradd -r -s /usr/sbin/nologin -d "$DATA_DIR" -c "Deliver service account" "$SERVICE_USER"
    info "Created user: $SERVICE_USER"
else
    info "User '$SERVICE_USER' already exists"
fi

# ── Directories ────────────────────────────────────────────────────────────────
step "Creating directories..."
for d in "$CONFIG_DIR" "$DATA_DIR/packages" "$DATA_DIR/client" \
          "$CACHE_DIR" "$LOG_DIR" "$INSTALL_DIR"; do
    mkdir -p "$d"
    info "Created: $d"
done
chown -R "$SERVICE_USER:$SERVICE_USER" "$DATA_DIR" "$CACHE_DIR" "$LOG_DIR"
chmod 750 "$DATA_DIR" "$CACHE_DIR"

# ── Install binaries ───────────────────────────────────────────────────────────
step "Installing binaries to $BIN_DIR..."
install -m 755 "$BUILD_DIR/dlr"        "$BIN_DIR/dlr"
install -m 755 "$BUILD_DIR/dlr_server" "$BIN_DIR/dlr_server"
info "Installed: dlr"
info "Installed: dlr_server"

# ── Config files ───────────────────────────────────────────────────────────────
step "Writing configuration files..."
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
    info "Created: $CONFIG_DIR/server.conf"
else
    info "Kept existing: $CONFIG_DIR/server.conf"
fi

if [[ ! -f "$CONFIG_DIR/client.conf" ]]; then
    cat > "$CONFIG_DIR/client.conf" << CONF
[client]
db_dir=$DATA_DIR/client
cache_dir=$CACHE_DIR
log_file=$LOG_DIR/client.log
install_dir=$INSTALL_DIR
CONF
    chmod 644 "$CONFIG_DIR/client.conf"
    info "Created: $CONFIG_DIR/client.conf"
else
    info "Kept existing: $CONFIG_DIR/client.conf"
fi

# ── systemd service ────────────────────────────────────────────────────────────
step "Installing systemd service..."
cat > /etc/systemd/system/deliver-server.service << SERVICE
[Unit]
Description=Deliver LAN Package Manager Server v${DLR_VERSION}
Documentation=https://github.com/fsminecrafter/deliver-package-manager
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
TimeoutStopSec=30s
StandardOutput=journal
StandardError=journal
SyslogIdentifier=deliver-server

# Security hardening
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=$DATA_DIR $CACHE_DIR $LOG_DIR
CapabilityBoundingSet=CAP_NET_BIND_SERVICE

[Install]
WantedBy=multi-user.target
SERVICE

systemctl daemon-reload
systemctl enable deliver-server
systemctl start deliver-server

sleep 1
if systemctl is-active --quiet deliver-server; then
    info "deliver-server started and enabled"
else
    warn "Service failed to start — check: journalctl -u deliver-server"
fi

# ── Bash completion ────────────────────────────────────────────────────────────
step "Installing shell completion..."
cat > /etc/bash_completion.d/dlr << 'COMP'
_dlr_complete() {
    local cur="${COMP_WORDS[COMP_CWORD]}"
    local prev="${COMP_WORDS[COMP_CWORD-1]}"
    local cmds="install download scan list ping search servers status restart \
                presentfile presentfolder attach generate make \
                unpresentfile unpresentfolder removepackage clear"
    case "$prev" in
        install|download|search) ;;
        ping|servers) ;;
        *) COMPREPLY=($(compgen -W "$cmds" -- "$cur")) ;;
    esac
}
complete -F _dlr_complete dlr
COMP
info "Bash completion installed"

# ── Firewall hint ──────────────────────────────────────────────────────────────
if command -v ufw &>/dev/null && ufw status | grep -q "Status: active"; then
    step "Configuring UFW firewall..."
    ufw allow 4242/tcp comment "Deliver server TCP" 2>/dev/null || true
    ufw allow 4243/udp comment "Deliver discovery UDP" 2>/dev/null || true
    info "UFW rules added (TCP 4242, UDP 4243)"
fi

# ── Summary ────────────────────────────────────────────────────────────────────
echo ""
echo -e "${GREEN}${BOLD}"
cat << 'SUCCESS'
╔═══════════════════════════════════════════════════╗
║   ✓  Deliver installed successfully!              ║
╚═══════════════════════════════════════════════════╝
SUCCESS
echo -e "${NC}"
echo "  Version:    $DLR_VERSION"
echo "  Binaries:   /usr/local/bin/dlr"
echo "              /usr/local/bin/dlr_server"
echo "  Config:     /etc/deliver/"
echo "  Data:       /var/lib/deliver/"
echo "  Logs:       /var/log/deliver/"
echo ""
echo "  Quick start:"
echo ""
echo "    ${BOLD}# Server — share a file:${NC}"
echo "    sudo dlr presentfile /path/to/app myapp"
echo "    sudo dlr generate list.pkg myapp"
echo ""
echo "    ${BOLD}# Server — remove packages:${NC}"
echo "    sudo dlr removepackage myapp       # remove one"
echo "    sudo dlr unpresentfile myapp       # alias"
echo "    sudo dlr clear                     # remove all"
echo ""
echo "    ${BOLD}# Client — install it:${NC}"
echo "    dlr scan"
echo "    dlr install -y myapp"
echo ""
echo "    ${BOLD}# Service management:${NC}"
echo "    sudo systemctl {start|stop|status|restart} deliver-server"
echo "    journalctl -u deliver-server -f"
echo ""