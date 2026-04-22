#!/usr/bin/env bash
# ┌──────────────────────────────────────────────────────────────────────────┐
# │  Deliver — Installer for Windows                                        │
# │                                                                          │
# │  Run AFTER build_windows.sh from an MSYS2 MINGW64 shell AS ADMIN:      │
# │    ./install_windows.sh                                                  │
# │                                                                          │
# │  What this does:                                                         │
# │    1. Copies dlr.exe and dlr_server.exe to C:\Program Files\Deliver\    │
# │    2. Adds that directory to the system PATH                            │
# │    3. Creates config files in C:\ProgramData\Deliver\                   │
# │    4. Registers dlr_server as a Windows Service (via sc.exe)           │
# │    5. Opens firewall ports TCP 4242 and UDP 4243                        │
# └──────────────────────────────────────────────────────────────────────────┘
set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'

info()  { echo -e "  ${GREEN}[OK]${NC} $*"; }
warn()  { echo -e "  ${YELLOW}[!!]${NC} $*"; }
error() { echo -e "  ${RED}[ERR]${NC} $*" >&2; exit 1; }
step()  { echo -e "\n${CYAN}${BOLD}==> $*${NC}"; }

echo -e "${CYAN}${BOLD}"
cat << 'BANNER'
  ____       _ _
 |  _ \  ___| (_)_   _____ _ __
 | | | |/ _ \ | \ \ / / _ \ '__|
 | |_| |  __/ | |\ V /  __/ |
 |____/ \___|_|_| \_/ \___|_|
  Installer — Windows
BANNER
echo -e "${NC}"

# ── Check environment ──────────────────────────────────────────────────────────
[[ -f CMakeLists.txt ]] || error "Run from the deliver/ source directory."

# Detect MSYS2 on Windows
if [[ -z "${MSYSTEM:-}" ]]; then
    warn "MSYSTEM not set — are you in an MSYS2 shell?"
fi

# Convert Windows paths
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_WIN="$SCRIPT_DIR/build_win"

if [[ ! -f "$BUILD_WIN/dlr.exe" || ! -f "$BUILD_WIN/dlr_server.exe" ]]; then
    # Try MSVC build location
    if [[ -f "$SCRIPT_DIR/build_win_msvc/Release/dlr.exe" ]]; then
        BUILD_WIN="$SCRIPT_DIR/build_win_msvc/Release"
    else
        error "Binaries not found. Run ./build_windows.sh first."
    fi
fi

info "Found dlr.exe at: $BUILD_WIN"

# ── Windows paths (via cygpath) ────────────────────────────────────────────────
WIN_INSTALL="C:/Program Files/Deliver"
WIN_DATA="C:/ProgramData/Deliver"
WIN_CACHE="C:/ProgramData/Deliver/cache"
WIN_LOG="C:/ProgramData/Deliver/logs"
WIN_CONFIG="C:/ProgramData/Deliver/etc"
WIN_PKGS="C:/ProgramData/Deliver/packages"
WIN_CLIENT_DB="C:/ProgramData/Deliver/client"
WIN_PKG_INSTALL="C:/ProgramData/Deliver/installed"

step "Creating installation directories..."
for d in "$WIN_INSTALL" "$WIN_DATA" "$WIN_CACHE" "$WIN_LOG" \
          "$WIN_CONFIG" "$WIN_PKGS" "$WIN_CLIENT_DB" "$WIN_PKG_INSTALL"; do
    mkdir -p "$d" && info "$d"
done

step "Copying binaries..."
cp -f "$BUILD_WIN/dlr.exe"        "$WIN_INSTALL/dlr.exe"
cp -f "$BUILD_WIN/dlr_server.exe" "$WIN_INSTALL/dlr_server.exe"

# Copy required MSYS2 runtime DLLs (if built with MinGW)
if [[ -n "${MSYSTEM:-}" ]]; then
    MINGW_BIN="/mingw64/bin"
    NEEDED_DLLS=(
        "libstdc++-6.dll"
        "libgcc_s_seh-1.dll"
        "libwinpthread-1.dll"
        "libssl-3-x64.dll"
        "libcrypto-3-x64.dll"
        "zlib1.dll"
    )
    step "Copying runtime DLLs..."
    for dll in "${NEEDED_DLLS[@]}"; do
        if [[ -f "$MINGW_BIN/$dll" ]]; then
            cp -f "$MINGW_BIN/$dll" "$WIN_INSTALL/"
            info "$dll"
        else
            warn "DLL not found (may not be needed): $dll"
        fi
    done
fi

info "Copied to: $WIN_INSTALL"

step "Writing configuration files..."
HOSTNAME=$(hostname -s 2>/dev/null || echo "deliver-server")

if [[ ! -f "$WIN_CONFIG/server.conf" ]]; then
    # Use Windows-style paths in config
    cat > "$WIN_CONFIG/server.conf" << CONF
[server]
name=$HOSTNAME
port=4242
needs_password=false
password_hash=
data_dir=C:/ProgramData/Deliver/packages
registry_file=C:/ProgramData/Deliver/registry.json
log_file=C:/ProgramData/Deliver/logs/server.log
CONF
    info "server.conf"
fi

if [[ ! -f "$WIN_CONFIG/client.conf" ]]; then
    cat > "$WIN_CONFIG/client.conf" << CONF
[client]
db_dir=C:/ProgramData/Deliver/client
cache_dir=C:/ProgramData/Deliver/cache
log_file=C:/ProgramData/Deliver/logs/client.log
install_dir=C:/ProgramData/Deliver/installed
CONF
    info "client.conf"
fi

# ── Register Windows Service ───────────────────────────────────────────────────
step "Registering Windows Service (deliver-server)..."

EXE_WIN="C:\\Program Files\\Deliver\\dlr_server.exe"

# Check if service already exists
if sc.exe query deliver-server &>/dev/null 2>&1; then
    warn "Service 'deliver-server' already exists — stopping and reconfiguring..."
    sc.exe stop deliver-server 2>/dev/null || true
    sc.exe delete deliver-server 2>/dev/null || true
    sleep 2
fi

sc.exe create deliver-server \
    binPath= "\"$EXE_WIN\"" \
    DisplayName= "Deliver LAN Package Manager" \
    start= auto \
    obj= "LocalSystem" \
    type= own \
    error= normal 2>/dev/null && info "Service registered" || warn "sc.exe create failed (need admin?)"

sc.exe description deliver-server "Deliver LAN Package Manager Server — shares packages over local network" 2>/dev/null || true
sc.exe start deliver-server 2>/dev/null && info "Service started" || warn "Could not auto-start service"

# ── Firewall ───────────────────────────────────────────────────────────────────
step "Adding Windows Firewall rules..."
netsh.exe advfirewall firewall delete rule name="Deliver Server TCP" 2>/dev/null || true
netsh.exe advfirewall firewall delete rule name="Deliver Discovery UDP" 2>/dev/null || true

netsh.exe advfirewall firewall add rule \
    name="Deliver Server TCP" \
    dir=in action=allow protocol=TCP localport=4242 \
    program="$EXE_WIN" enable=yes 2>/dev/null && info "TCP 4242 allowed" || warn "Firewall rule failed"

netsh.exe advfirewall firewall add rule \
    name="Deliver Discovery UDP" \
    dir=in action=allow protocol=UDP localport=4243 \
    enable=yes 2>/dev/null && info "UDP 4243 allowed" || warn "UDP firewall rule failed"

# ── PATH ──────────────────────────────────────────────────────────────────────
step "Adding to system PATH..."
WIN_PATH_WIN="C:\\Program Files\\Deliver"

# Using PowerShell to modify system PATH
powershell.exe -NoProfile -Command "
    \$path = [System.Environment]::GetEnvironmentVariable('Path', 'Machine')
    if (\$path -notlike '*Program Files\Deliver*') {
        [System.Environment]::SetEnvironmentVariable(
            'Path',
            \$path + ';${WIN_PATH_WIN}',
            'Machine'
        )
        Write-Host 'PATH updated'
    } else {
        Write-Host 'Already in PATH'
    }
" 2>/dev/null && info "PATH updated" || warn "Could not update PATH (open new terminal)"

# ── Summary ────────────────────────────────────────────────────────────────────
echo ""
echo -e "${GREEN}${BOLD}"
echo "╔══════════════════════════════════════════════════╗"
echo "║   ✓  Deliver installed on Windows!              ║"
echo "╚══════════════════════════════════════════════════╝"
echo -e "${NC}"
echo "  Install dir: C:\\Program Files\\Deliver\\"
echo "  Config:      C:\\ProgramData\\Deliver\\etc\\"
echo "  Logs:        C:\\ProgramData\\Deliver\\logs\\"
echo ""
echo "  Service: deliver-server"
echo "    sc.exe start deliver-server"
echo "    sc.exe stop  deliver-server"
echo "    sc.exe query deliver-server"
echo ""
echo "  Open a new Command Prompt or PowerShell, then:"
echo "    dlr scan"
echo "    dlr list"
echo "    dlr install mypackage"
echo ""
echo "  NOTE: Packages on Windows run in their own directory."
echo "  Install scripts (.sh) require WSL2 or Git Bash."
echo "  Use installcommand= in the .pkg for Windows-native commands."
echo ""
