$ErrorActionPreference = "Stop"

Write-Host "============================================"
Write-Host "  KSC - Keystroke Counter Build Script"
Write-Host "============================================"
Write-Host ""

# Check for CMake
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake) {
    Write-Host "[ERROR] CMake is not installed or not in PATH."
    Write-Host "Download from: https://cmake.org/download/"
    exit 1
}
Write-Host "[OK] CMake found: $($cmake.Source)"

# Check for GCC (MinGW)
$gcc = Get-Command gcc -ErrorAction SilentlyContinue
if (-not $gcc) {
    Write-Host "[ERROR] GCC (MinGW-w64) is not installed or not in PATH."
    Write-Host "Download from: https://www.mingw-w64.org/"
    exit 1
}
Write-Host "[OK] GCC found: $($gcc.Source)"

# Download SQLite3 amalgamation if not present
$SQLiteYear = "2024"
$SQLiteVer  = "3460100"
$SQLiteUrl  = "https://www.sqlite.org/$SQLiteYear/sqlite-amalgamation-$SQLiteVer.zip"
$SqliteDir  = "sqlite3"
$SqliteZip  = "$SqliteDir\sqlite3.zip"
$SqliteC    = "$SqliteDir\sqlite3.c"

if (Test-Path $SqliteC) {
    Write-Host "[OK] SQLite3 amalgamation found."
} else {
    Write-Host "[INFO] SQLite3 amalgamation not found. Downloading..."
    if (-not (Test-Path $SqliteDir)) {
        New-Item -ItemType Directory -Path $SqliteDir | Out-Null
    }

    Write-Host "[INFO] Downloading from sqlite.org ..."
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -Uri $SQLiteUrl -OutFile $SqliteZip -UseBasicParsing
    } catch {
        Write-Host "[ERROR] Failed to download SQLite3 amalgamation."
        Write-Host "Please download manually from https://www.sqlite.org/download.html"
        Write-Host "Extract sqlite3.c and sqlite3.h into the sqlite3\ directory."
        exit 1
    }

    Write-Host "[INFO] Extracting SQLite3 amalgamation..."
    $TempDir = "$SqliteDir\temp"
    try {
        Expand-Archive -Path $SqliteZip -DestinationPath $TempDir -Force
    } catch {
        Write-Host "[ERROR] Failed to extract SQLite3 amalgamation."
        exit 1
    }

    $ExtractedDir = Get-ChildItem -Path $TempDir -Directory | Select-Object -First 1
    if (-not $ExtractedDir) {
        Write-Host "[ERROR] Unexpected archive structure."
        exit 1
    }

    Move-Item -Path "$($ExtractedDir.FullName)\sqlite3.c" -Destination $SqliteDir -Force
    Move-Item -Path "$($ExtractedDir.FullName)\sqlite3.h" -Destination $SqliteDir -Force
    $sqlite3ext = "$($ExtractedDir.FullName)\sqlite3ext.h"
    if (Test-Path $sqlite3ext) {
        Move-Item -Path $sqlite3ext -Destination $SqliteDir -Force
    }

    Remove-Item -Recurse -Force $TempDir
    Remove-Item -Force $SqliteZip

    if (-not (Test-Path $SqliteC)) {
        Write-Host "[ERROR] Failed to extract sqlite3.c"
        exit 1
    }
    Write-Host "[OK] SQLite3 amalgamation downloaded and extracted."
}

# Download libssh2 if not present
if (-not (Test-Path "libssh2\libssh2.a")) {
    if (Test-Path "libssh2") { Remove-Item -Recurse -Force "libssh2" }
    Write-Host "[INFO] libssh2 not found. Downloading..."

    $LsshDir = "libssh2"
    if (-not (Test-Path $LsshDir)) {
        New-Item -ItemType Directory -Path $LsshDir | Out-Null
    }
    $LsshUrl = "https://www.libssh2.org/download/libssh2-1.11.0.tar.gz"
    $LsshTgz = "$LsshDir\libssh2.tar.gz"
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -Uri $LsshUrl -OutFile $LsshTgz -UseBasicParsing
    } catch {
        Write-Host "[ERROR] Failed to download libssh2."
        Write-Host "Download manually from https://www.libssh2.org/download/"
        Write-Host "Extract to libssh2/ and rebuild."
        exit 1
    }

    Write-Host "[INFO] Extracting libssh2..."
    $LsshTemp = "$LsshDir\temp"
    try {
        New-Item -ItemType Directory -Path $LsshTemp -Force | Out-Null
        & tar -xzf $LsshTgz -C $LsshTemp 2>$null
    } catch {
        Write-Host "[ERROR] Failed to extract libssh2 (tar not found?)."
        exit 1
    }

    $Extracted = Get-ChildItem -Path $LsshTemp -Directory | Select-Object -First 1
    if (-not $Extracted) {
        Write-Host "[ERROR] Unexpected libssh2 archive structure."
        exit 1
    }
    $LsshSrc = $Extracted.FullName

    Write-Host "[INFO] Building libssh2 (WinCNG, static)..."
    $LsshBld = "$LsshDir\build"
    New-Item -ItemType Directory -Path $LsshBld -Force | Out-Null

    Push-Location $LsshBld
    try {
        & cmake $LsshSrc -G "MinGW Makefiles" `
            -DCRYPTO_BACKEND=WinCNG `
            -DBUILD_SHARED_LIBS=OFF `
            -DBUILD_EXAMPLES=OFF `
            -DBUILD_TESTING=OFF `
            -DENABLE_ZLIB_COMPRESSION=OFF `
            -DCMAKE_POLICY_VERSION_MINIMUM="3.5" `
            -DCMAKE_BUILD_TYPE=Release `
            -DCMAKE_C_COMPILER=gcc
        if ($LASTEXITCODE -ne 0) { throw "cmake failed" }
        & cmake --build . --config Release
        if ($LASTEXITCODE -ne 0) { throw "build failed" }
    } finally {
        Pop-Location
    }

    # Copy headers and lib to expected locations
    New-Item -ItemType Directory -Path "$LsshDir\include" -Force | Out-Null
    Copy-Item -Path "$LsshSrc\include\*" -Destination "$LsshDir\include\" -Recurse -Force
    if (Test-Path "$LsshSrc\src\libssh2.a") {
        Copy-Item -Path "$LsshSrc\src\libssh2.a" -Destination "$LsshDir\libssh2.a" -Force
    }
    if (Test-Path "$LsshBld\src\libssh2.a") {
        Copy-Item -Path "$LsshBld\src\libssh2.a" -Destination "$LsshDir\libssh2.a" -Force
    }
    Remove-Item -Recurse -Force $LsshTemp -ErrorAction SilentlyContinue
    Remove-Item -Force $LsshTgz -ErrorAction SilentlyContinue

    Write-Host "[OK] libssh2 built."
} else {
    Write-Host "[OK] libssh2 found."
}

# Build
Write-Host ""
Write-Host "[INFO] Generating application icon..."
try {
    Add-Type -AssemblyName System.Drawing -ErrorAction Stop
    $bmp = New-Object System.Drawing.Bitmap(32, 32)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'HighQuality'
    $g.TextRenderingHint = 'AntiAliasGridFit'

    $bg = New-Object System.Drawing.SolidBrush(
        [System.Drawing.Color]::FromArgb(31, 41, 55))
    $g.FillRectangle($bg, 0, 0, 32, 32)
    $bg.Dispose()

    $font = New-Object System.Drawing.Font('Segoe UI', 20,
        [System.Drawing.FontStyle]::Bold)
    $fg = New-Object System.Drawing.SolidBrush(
        [System.Drawing.Color]::FromArgb(226, 232, 240))
    $fmt = New-Object System.Drawing.StringFormat
    $fmt.Alignment = 'Center'
    $fmt.LineAlignment = 'Center'
    $g.DrawString('K', $font, $fg, [System.Drawing.RectangleF]::new(0, 0, 32, 32), $fmt)

    $g.Dispose()
    $font.Dispose()
    $fg.Dispose()
    $fmt.Dispose()

    $icon = [System.Drawing.Icon]::FromHandle($bmp.GetHicon())
    $icoPath = Join-Path $PSScriptRoot 'src\ksc.ico'
    $fs = [System.IO.FileStream]::new($icoPath, 'Create')
    $icon.Save($fs)
    $fs.Close()
    $icon.Dispose()
    $bmp.Dispose()

    Write-Host "[OK] Icon generated: src\ksc.ico"
} catch {
    Write-Host "[WARN] Could not generate icon: $($_.Exception.Message)"
    Write-Host "[WARN] The exe will use a runtime-generated icon instead."
}

Write-Host ""
Write-Host "[INFO] Configuring project with CMake..."
Write-Host ""

if (Test-Path "build") {
    Remove-Item -Recurse -Force "build"
}
New-Item -ItemType Directory -Path "build" | Out-Null

Push-Location "build"
try {
    $cmakeArgs = @(
        "..",
        "-G", "MinGW Makefiles",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_C_COMPILER=gcc"
    )
    & cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Host ""
        Write-Host "[ERROR] CMake configuration failed."
        exit 1
    }

    Write-Host ""
    Write-Host "[INFO] Building project..."
    Write-Host ""

    & cmake --build . --config Release
    if ($LASTEXITCODE -ne 0) {
        Write-Host ""
        Write-Host "[ERROR] Build failed."
        exit 1
    }
} finally {
    Pop-Location
}

# Copy executable to project root
if (Test-Path "build\ksc.exe") {
    Copy-Item -Path "build\ksc.exe" -Destination "ksc.exe" -Force

    # Self-sign the executable
    Write-Host ""
    Write-Host "[INFO] Signing executable..."
    $certSubject = "CN=bbounce.org ksc 0.9"
    $cert = Get-ChildItem -Path Cert:\CurrentUser\My -CodeSigningCert |
            Where-Object { $_.Subject -eq $certSubject } |
            Select-Object -First 1
    if (-not $cert) {
        try {
            $cert = New-SelfSignedCertificate `
                -Type CodeSigningCert `
                -Subject $certSubject `
                -KeyUsage DigitalSignature `
                -CertStoreLocation Cert:\CurrentUser\My `
                -FriendlyName "ksc Signing Certificate"
            Write-Host "[OK] Created self-signed code-signing certificate."
        } catch {
            Write-Host "[WARN] Could not create signing certificate: $($_.Exception.Message)"
            Write-Host "[WARN] Skipping signature."
        }
    }
    if ($cert) {
        try {
            Set-AuthenticodeSignature -FilePath "ksc.exe" -Certificate $cert -TimestampServer "http://timestamp.digicert.com" | Out-Null
            Write-Host "[OK] Executable signed."

            $certFile = Join-Path $PSScriptRoot "ksc.cer"
            Export-Certificate -Cert $cert -FilePath $certFile -Type CERT -Force | Out-Null
            Write-Host "[INFO] Public certificate exported to ksc.cer"
            Write-Host ""
            Write-Host "  To permanently trust this build, run once (as Admin):"
            Write-Host "    Import-Certificate -FilePath .\ksc.cer -CertStoreLocation Cert:\CurrentUser\TrustedPublisher"
            Write-Host ""
        } catch {
            Write-Host "[WARN] Signing failed: $($_.Exception.Message)"
            Write-Host "[WARN] The exe is unsigned. Right-click Properties > Unblock to run."
        }
    }

    $srcLines = (Get-ChildItem -Path "$PSScriptRoot\src" -Include *.c,*.h -Recurse | Get-Content | Measure-Object -Line).Lines
    $sqlLines = 0
    if (Test-Path "$PSScriptRoot\sqlite3\sqlite3.c") {
        $sqlLines = (Get-Content "$PSScriptRoot\sqlite3\sqlite3.c" | Measure-Object -Line).Lines
    }

    Write-Host ""
    Write-Host "============================================"
    Write-Host "  Build successful!"
    Write-Host "  ksc.exe is ready in the project root."
    Write-Host "  Source lines:  $srcLines (app) + $sqlLines (sqlite3)"
    Write-Host "============================================"
} else {
    Write-Host ""
    Write-Host "[ERROR] Build completed but ksc.exe not found."
    exit 1
}
