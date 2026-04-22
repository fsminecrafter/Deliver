@echo off
:: ============================================================
::  Deliver -- Installer for Windows
::
::  Run AFTER build_windows.sh / MSVC build, AS ADMINISTRATOR:
::    install_windows.bat
::
::  What this does:
::    1. Copies dlr.exe and dlr_server.exe to
::       C:\Program Files\Deliver\
::    2. Adds that directory to the system PATH
::    3. Creates config files in C:\ProgramData\Deliver\
::    4. Registers dlr_server as a Windows Service (sc.exe)
::    5. Opens firewall ports TCP 4242 and UDP 4243
:: ============================================================
setlocal EnableDelayedExpansion

:: ── Colour helpers (via PowerShell write) ─────────────────────────────────────
:: We use simple echo for most output; PS only for the coloured summary box.

echo.
echo   ____       _ _
echo  ^|  _ \  ___^| ^|^(_^)_   _____ _ __
echo  ^| ^| ^| ^|/ _ \ ^| \ \ / / _ \ '__^|
echo  ^| ^|_^| ^|  __/ ^| ^|\ V /  __/ ^|
echo  ^|____/ \___^|_^|_^| \_/ \___^|_^|
echo   Installer -- Windows
echo.

:: ── Admin check ───────────────────────────────────────────────────────────────
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERR] This installer must be run as Administrator.
    echo       Right-click the .bat file and choose "Run as administrator".
    pause
    exit /b 1
)

:: ── Locate source directory (where this .bat lives) ───────────────────────────
set "SCRIPT_DIR=%~dp0"
:: Remove trailing backslash
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"

:: ── Locate built binaries ─────────────────────────────────────────────────────
set "BUILD_WIN=%SCRIPT_DIR%\build_win"
set "DLR_EXE=%BUILD_WIN%\dlr.exe"
set "DLR_SERVER_EXE=%BUILD_WIN%\dlr_server.exe"

if not exist "%DLR_EXE%" (
    :: Try MSVC Release output
    set "BUILD_WIN=%SCRIPT_DIR%\build_win_msvc\Release"
    set "DLR_EXE=%BUILD_WIN%\dlr.exe"
    set "DLR_SERVER_EXE=%BUILD_WIN%\dlr_server.exe"
)

if not exist "%DLR_EXE%" (
    echo [ERR] dlr.exe not found. Run build_windows.sh ^(MSYS2^) or the MSVC build first.
    pause
    exit /b 1
)
if not exist "%DLR_SERVER_EXE%" (
    echo [ERR] dlr_server.exe not found. Build did not complete successfully.
    pause
    exit /b 1
)

echo [OK] Found dlr.exe at: %BUILD_WIN%

:: ── Paths ──────────────────────────────────────────────────────────────────────
set "WIN_INSTALL=C:\Program Files\Deliver"
set "WIN_DATA=C:\ProgramData\Deliver"
set "WIN_CACHE=%WIN_DATA%\cache"
set "WIN_LOG=%WIN_DATA%\logs"
set "WIN_CONFIG=%WIN_DATA%\etc"
set "WIN_PKGS=%WIN_DATA%\packages"
set "WIN_CLIENT_DB=%WIN_DATA%\client"
set "WIN_PKG_INSTALL=%WIN_DATA%\installed"

:: ── Create directories ─────────────────────────────────────────────────────────
echo.
echo ==> Creating installation directories...
for %%D in (
    "%WIN_INSTALL%"
    "%WIN_DATA%"
    "%WIN_CACHE%"
    "%WIN_LOG%"
    "%WIN_CONFIG%"
    "%WIN_PKGS%"
    "%WIN_CLIENT_DB%"
    "%WIN_PKG_INSTALL%"
) do (
    if not exist %%D (
        mkdir %%D
        echo [OK] Created %%D
    ) else (
        echo [OK] Exists  %%D
    )
)

:: ── Copy binaries ──────────────────────────────────────────────────────────────
echo.
echo ==> Copying binaries...
copy /Y "%DLR_EXE%"        "%WIN_INSTALL%\dlr.exe"        >nul && echo [OK] dlr.exe
copy /Y "%DLR_SERVER_EXE%" "%WIN_INSTALL%\dlr_server.exe" >nul && echo [OK] dlr_server.exe

:: ── Copy MinGW runtime DLLs (if present next to the binaries) ─────────────────
echo.
echo ==> Checking for MinGW runtime DLLs...
set "MINGW_BIN=C:\msys64\mingw64\bin"

for %%F in (
    libstdc++-6.dll
    libgcc_s_seh-1.dll
    libwinpthread-1.dll
    libssl-3-x64.dll
    libcrypto-3-x64.dll
    zlib1.dll
) do (
    if exist "%BUILD_WIN%\%%F" (
        copy /Y "%BUILD_WIN%\%%F" "%WIN_INSTALL%\%%F" >nul
        echo [OK] %%F  ^(from build dir^)
    ) else if exist "%MINGW_BIN%\%%F" (
        copy /Y "%MINGW_BIN%\%%F" "%WIN_INSTALL%\%%F" >nul
        echo [OK] %%F  ^(from %MINGW_BIN%^)
    ) else (
        echo [!!] %%F not found -- may not be needed for MSVC builds
    )
)

:: ── Write config files ─────────────────────────────────────────────────────────
echo.
echo ==> Writing configuration files...

:: Get hostname
for /f "tokens=*" %%H in ('hostname') do set "MYHOSTNAME=%%H"

if not exist "%WIN_CONFIG%\server.conf" (
    (
        echo [server]
        echo name=%MYHOSTNAME%
        echo port=4242
        echo needs_password=false
        echo password_hash=
        echo data_dir=C:/ProgramData/Deliver/packages
        echo registry_file=C:/ProgramData/Deliver/registry.json
        echo log_file=C:/ProgramData/Deliver/logs/server.log
    ) > "%WIN_CONFIG%\server.conf"
    echo [OK] server.conf
) else (
    echo [OK] server.conf already exists -- skipped
)

if not exist "%WIN_CONFIG%\client.conf" (
    (
        echo [client]
        echo db_dir=C:/ProgramData/Deliver/client
        echo cache_dir=C:/ProgramData/Deliver/cache
        echo log_file=C:/ProgramData/Deliver/logs/client.log
        echo install_dir=C:/ProgramData/Deliver/installed
    ) > "%WIN_CONFIG%\client.conf"
    echo [OK] client.conf
) else (
    echo [OK] client.conf already exists -- skipped
)

:: ── Register Windows Service ───────────────────────────────────────────────────
echo.
echo ==> Registering Windows Service (deliver-server)...

set "EXE_WIN=%WIN_INSTALL%\dlr_server.exe"

sc query deliver-server >nul 2>&1
if %errorlevel% equ 0 (
    echo [!!] Service 'deliver-server' already exists -- stopping and reconfiguring...
    sc stop deliver-server >nul 2>&1
    timeout /t 2 /nobreak >nul
    sc delete deliver-server >nul 2>&1
    timeout /t 2 /nobreak >nul
)

sc create deliver-server ^
    binPath= "\"%EXE_WIN%\"" ^
    DisplayName= "Deliver LAN Package Manager" ^
    start= auto ^
    obj= LocalSystem ^
    type= own ^
    error= normal >nul 2>&1
if %errorlevel% equ 0 (
    echo [OK] Service registered
) else (
    echo [!!] sc.exe create failed -- are you running as Administrator?
)

sc description deliver-server "Deliver LAN Package Manager Server -- shares packages over the local network" >nul 2>&1

sc start deliver-server >nul 2>&1
if %errorlevel% equ 0 (
    echo [OK] Service started
) else (
    echo [!!] Could not auto-start service -- start it manually with:
    echo      sc start deliver-server
)

:: ── Firewall rules ─────────────────────────────────────────────────────────────
echo.
echo ==> Adding Windows Firewall rules...

netsh advfirewall firewall delete rule name="Deliver Server TCP"    >nul 2>&1
netsh advfirewall firewall delete rule name="Deliver Discovery UDP" >nul 2>&1

netsh advfirewall firewall add rule ^
    name="Deliver Server TCP" ^
    dir=in action=allow protocol=TCP localport=4242 ^
    program="%EXE_WIN%" enable=yes >nul 2>&1
if %errorlevel% equ 0 (echo [OK] TCP 4242 allowed) else (echo [!!] TCP firewall rule failed)

netsh advfirewall firewall add rule ^
    name="Deliver Discovery UDP" ^
    dir=in action=allow protocol=UDP localport=4243 ^
    enable=yes >nul 2>&1
if %errorlevel% equ 0 (echo [OK] UDP 4243 allowed) else (echo [!!] UDP firewall rule failed)

:: ── Add to system PATH ─────────────────────────────────────────────────────────
echo.
echo ==> Adding to system PATH...

powershell -NoProfile -Command ^
    "$p = [System.Environment]::GetEnvironmentVariable('Path','Machine'); if ($p -notlike '*Program Files\Deliver*') { [System.Environment]::SetEnvironmentVariable('Path', $p + ';%WIN_INSTALL%', 'Machine'); Write-Host '[OK] PATH updated -- open a new terminal to use dlr' } else { Write-Host '[OK] Already in PATH' }"

:: ── Summary ────────────────────────────────────────────────────────────────────
echo.
echo ============================================================
echo    Deliver installed successfully on Windows!
echo ============================================================
echo.
echo   Install dir : %WIN_INSTALL%
echo   Config      : %WIN_CONFIG%
echo   Data        : %WIN_DATA%
echo   Logs        : %WIN_LOG%
echo.
echo   Service management:
echo     sc start deliver-server
echo     sc stop  deliver-server
echo     sc query deliver-server
echo.
echo   Open a NEW Command Prompt or PowerShell, then:
echo     dlr scan
echo     dlr list
echo     dlr install mypackage
echo.
echo   NOTE: Install scripts (.sh) require WSL2 or Git Bash.
echo   Use installcommand= in the .pkg for Windows-native commands.
echo.
pause
endlocal
