# Deliver — LAN Package Manager

A fast, secure, zero-config LAN package manager written in C++17.  
Packages are shared across your local network — no internet required.

---

## Features

- **LAN auto-discovery** via UDP broadcast (no manual server config needed)
- **AES-256-GCM encryption** for all transfers
- **SHA-256 integrity checks** on every download
- **`.pkg` manifest files** for install scripts, dependencies, and conflicts
- **.tar bundles** — always one `.pkg` + your files
- Optional **password authentication** per server
- Systemd service with auto-start
- **Cross-platform**: primary target Debian 13+, Windows via MinGW/WSL2

---

## Quick Install (Debian 13 / Ubuntu)

```bash
cd deliver-package-manager
sudo chmod +x install.sh
git clone https://github.com/fsminecrafter/deliver-package-manager.git
cd deliver
sudo ./install.sh
```

The installer:
1. Installs build deps via `apt`
2. Compiles with CMake + Ninja
3. Installs `dlr` and `dlr_server` to `/usr/local/bin`
4. Creates the `deliver` system user
5. Writes default config to `/etc/deliver/`
6. Installs and starts `deliver-server.service`

---

## Architecture

```
┌───────────────────────────────────────────────────────┐
│                      LAN Network                       │
│                                                        │
│  ┌─────────────────────────────────────────────────┐  │
│  │           Deliver Server (dlr_server)           │  │
│  │                                                 │  │
│  │  Proc 1: UDP broadcast "Hello, <name>"  ─────► broadcast:4243
│  │  Proc 2: TCP accept loop on port 4242          │  │
│  │  Proc 3: Per-client handler                    │  │
│  │          ├─ Key exchange (AES-256-GCM)         │  │
│  │          ├─ Optional auth                      │  │
│  │          ├─ Package streaming                  │  │
│  │          └─ Search / list                      │  │
│  └─────────────────────────────────────────────────┘  │
│                          ▲ TCP:4242                    │
│                          │                            │
│  ┌───────────────────────┴─────────────────────────┐  │
│  │           Deliver Client (dlr)                  │  │
│  │  scan: discover servers via UDP, update DB      │  │
│  │  install: find pkg → download → extract → run   │  │
│  └─────────────────────────────────────────────────┘  │
└───────────────────────────────────────────────────────┘
```

---

## Server Commands

### Register a file as a package
```bash
sudo dlr presentfile /path/to/myapp.bin myapp
```
Copies the file to `/var/lib/deliver/packages/myapp/` and registers it.  
Checks LAN first — **fails if the package name already exists** on another server.

### Register a folder
```bash
sudo dlr presentfolder /path/to/myfolder nicefolder
```

### Attach a .pkg manifest
```bash
sudo dlr attach list.pkg myapp
```
Attach a hand-crafted `.pkg` file to an already-presented package.

### Auto-generate and attach a .pkg
```bash
sudo dlr generate list.pkg myapp
```
Creates a basic `.pkg` (name, version 1.0, empty deps) and attaches it.

### Generate a .pkg without attaching
```bash
sudo dlr make list.pkg myapp
```
Useful for editing the `.pkg` before attaching.

### Server status
```bash
sudo dlr status
```

### Restart the server
```bash
sudo dlr restart
```

---

## Client Commands

### Discover servers and refresh databases
```bash
dlr scan
```
Broadcasts a UDP discovery packet, connects to each responding server, and downloads its package list into the local DB at `/var/lib/deliver/client/`.

### Install a package
```bash
dlr install myapp          # prompts for confirmation
dlr install -y myapp       # auto-confirm
```

### Just download (no install)
```bash
dlr download -y myapp
```
Downloads the `.tar` to `/var/cache/deliver/`.

### Search packages
```bash
dlr search keyword
```

### Browse known servers
```bash
dlr servers            # list all
dlr servers homelab    # filter by name
```

### Ping a server
```bash
dlr ping myserver
# PONG from myserver @ 192.168.1.10 in 3ms
```

---

## .pkg File Format

Bundled as `<package>.tar` — must contain **exactly one** `.pkg` file and at least one other file.

```ini
[Info]
name=mypkg
version=1.0
description=My awesome package
dependencies=otherpkg(=1.0), anotherpkg(>2.0), simpledep
rivalpack=conflicting-pkg

[Install]
installscript=install.sh       ; bash script run after extraction
installcommand=echo Hello!     ; command run after script (optional)
```

### Dependency syntax

| Syntax          | Meaning                        |
|-----------------|--------------------------------|
| `pkgname`       | Any version                    |
| `pkgname(=1.0)` | Exactly version 1.0            |
| `pkgname(<2.0)` | Less than version 2.0          |
| `pkgname(>1.5)` | Greater than version 1.5       |

### rivalpack

If `rivalpack` is set and the rival is installed, the user is warned before proceeding.

---

## Wire Protocol

All TCP frames are **length-prefixed** (4-byte big-endian) then **AES-256-GCM encrypted** after the initial key exchange.

```
Client → Server:  KEY:<base64_32byte_key>
Server → Client:  (uses that key for all subsequent frames)

Frame structure:
  [4 bytes: payload length]
  [N bytes: encrypted payload]
    └─ [1 byte: MsgType]
       [N-1 bytes: body]
```

**Message types:**

| Type             | Hex  | Direction        | Description            |
|------------------|------|------------------|------------------------|
| HELLO            | 0x01 | S→C              | Server greeting        |
| HELLO_ACK        | 0x02 | S→C              | Auth accepted          |
| AUTH_REQUEST     | 0x03 | S→C              | Password prompt        |
| AUTH_RESPONSE    | 0x04 | C→S              | Password response      |
| INSTALL_REQUEST  | 0x10 | C→S              | Request a package      |
| INSTALL_DATA     | 0x11 | S→C              | Package file chunk     |
| INSTALL_END      | 0x12 | S→C              | End + SHA-256 checksum |
| INSTALL_ERROR    | 0x13 | S→C              | Error message          |
| SEARCH_REQUEST   | 0x20 | C→S              | Search query           |
| SEARCH_RESULT    | 0x21 | S→C              | Search results         |
| PKG_LIST         | 0x22 | Bidirectional    | Full package list      |
| PING             | 0x30 | C→S              | Ping                   |
| PONG             | 0x31 | S→C              | Pong                   |

---

## Configuration

### `/etc/deliver/server.conf`
```ini
[server]
name=homelab
port=4242
needs_password=false
password_hash=          ; SHA-256 hex of password if needs_password=true
data_dir=/var/lib/deliver/packages
registry_file=/var/lib/deliver/registry.json
log_file=/var/log/deliver/server.log
```

### `/etc/deliver/client.conf`
```ini
[client]
db_dir=/var/lib/deliver/client
cache_dir=/var/cache/deliver
log_file=/var/log/deliver/client.log
install_dir=/usr/local/deliver
```

---

## Enabling Password Auth

```bash
# Generate hash
echo -n "mysecretpassword" | sha256sum | awk '{print $1}'

# Edit /etc/deliver/server.conf:
needs_password=true
password_hash=<hash_from_above>

sudo dlr restart
```

---

## Windows Support

Deliver can be compiled on Windows with **MinGW-w64** or **MSVC** (C++17).

### Using MinGW (MSYS2)
```powershell
# In MSYS2 shell:
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-openssl mingw-w64-x86_64-nlohmann-json
cd deliver
mkdir build && cd build
cmake .. -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
mingw32-make -j4
```

### Using WSL2 (Recommended for Windows)
WSL2 gives a full Debian/Ubuntu environment. The standard install script works as-is:
```bash
sudo ./install.sh
```

### Windows-specific notes

- The server auto-start service is **not** installed on Windows (use Task Scheduler manually)
- UDP broadcast works on Windows via WinSock2 (`ws2_32.lib` is linked automatically)
- `tar` is bundled with Windows 10 1803+ — the system `tar` calls work fine
- Firewall: allow TCP 4242 and UDP 4243 inbound for the deliver-server process
- Admin rights are required for server commands (equivalent of `sudo`)

---

## File Structure

```
/etc/deliver/
  server.conf
  client.conf

/var/lib/deliver/
  registry.json           ← server package registry
  packages/
    <pkgname>/            ← package files
      <pkgname>.pkg
      <files...>
    <pkgname>.tar         ← built on demand for transfer
  client/
    packages.json         ← client package DB
    servers.json          ← known servers
    installed.json        ← installed package versions

/var/cache/deliver/
  <pkgname>.tar           ← downloaded packages

/var/log/deliver/
  server.log
  client.log

/usr/local/deliver/
  <pkgname>/              ← installed package files
```

---

## Building from Source

```bash
# Dependencies (Debian/Ubuntu)
sudo apt install build-essential cmake ninja-build libssl-dev zlib1g-dev nlohmann-json3-dev

# Build
mkdir build && cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja

# Run server (dev mode, no service)
sudo ./dlr_server

# Use client
./dlr scan
./dlr search mypkg
./dlr install -y mypkg
```

---

## License

MIT
