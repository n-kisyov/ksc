@echo off
setlocal enabledelayedexpansion

echo ============================================
echo   KSC - Keystroke Counter Build Script
echo ============================================
echo.

:: Check for CMake
where cmake >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo [ERROR] CMake is not installed or not in PATH.
    echo Download from: https://cmake.org/download/
    exit /b 1
)
echo [OK] CMake found.

:: Check for GCC (MinGW)
where gcc >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo [ERROR] GCC (MinGW-w64) is not installed or not in PATH.
    echo Download from: https://www.mingw-w64.org/
    exit /b 1
)
echo [OK] GCC found.

:: Download SQLite3 amalgamation if not present
set SQLITE_YEAR=2024
set SQLITE_VER=3460100

if not exist "sqlite3\sqlite3.c" (
    echo [INFO] SQLite3 amalgamation not found. Downloading...
    if not exist "sqlite3" mkdir sqlite3

    echo [INFO] Downloading sqlite3.c ...
    powershell -Command "& {[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12; Invoke-WebRequest -Uri 'https://www.sqlite.org/2025/sqlite-amalgamation-%SQLITE_VER%.zip' -OutFile 'sqlite3\sqlite3.zip'}"

    if %ERRORLEVEL% neq 0 (
        echo [ERROR] Failed to download SQLite3 amalgamation.
        echo Please download manually from https://www.sqlite.org/download.html
        echo and extract sqlite3.c and sqlite3.h to the sqlite3\ directory.
        exit /b 1
    )

    echo [INFO] Extracting SQLite3 amalgamation...
    powershell -Command "& {Expand-Archive -Path 'sqlite3\sqlite3.zip' -DestinationPath 'sqlite3\temp'}"

    if %ERRORLEVEL% neq 0 (
        echo [ERROR] Failed to extract SQLite3 amalgamation.
        exit /b 1
    )

    move "sqlite3\temp\sqlite-amalgamation-%SQLITE_VER%\sqlite3.c" "sqlite3\" >nul
    move "sqlite3\temp\sqlite-amalgamation-%SQLITE_VER%\sqlite3.h" "sqlite3\" >nul
    if exist "sqlite3\temp\sqlite-amalgamation-%SQLITE_VER%\sqlite3ext.h" (
        move "sqlite3\temp\sqlite-amalgamation-%SQLITE_VER%\sqlite3ext.h" "sqlite3\" >nul
    )
    rmdir /s /q "sqlite3\temp" >nul 2>&1
    del /q "sqlite3\sqlite3.zip" >nul 2>&1

    if not exist "sqlite3\sqlite3.c" (
        echo [ERROR] Failed to extract sqlite3.c
        exit /b 1
    )
    echo [OK] SQLite3 amalgamation downloaded and extracted.
) else (
    echo [OK] SQLite3 amalgamation found.
)

:: Build
echo.
echo [INFO] Configuring project with CMake...
echo.

if exist "build" rmdir /s /q "build"
mkdir build

cmake -S . -B build -G "MinGW Makefiles" ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_C_COMPILER=gcc

if %ERRORLEVEL% neq 0 (
    echo.
    echo [ERROR] CMake configuration failed.
    exit /b 1
)

echo.
echo [INFO] Building project...
echo.

cmake --build build --config Release

if %ERRORLEVEL% neq 0 (
    echo.
    echo [ERROR] Build failed.
    exit /b 1
)

:: Copy executable to project root
if exist "build\ksc.exe" (
    copy /y "build\ksc.exe" "ksc.exe" >nul
    echo.
    echo ============================================
    echo   Build successful!
    echo   ksc.exe is ready in the project root.
    echo ============================================
) else (
    echo.
    echo [ERROR] Build completed but ksc.exe not found.
    exit /b 1
)

endlocal
