# Builds the static Qt the single-file Windows program links (.github/workflows/package.yml):
# qtbase and qtimageformats from the pinned sources, release only, MSVC runtime linked in.
# Run from a shell where cl.exe is the pinned compiler (the workflow sets that up).
#
#   build-static-qt.ps1 -Version 6.8.3 -Prefix C:\qt-static -WorkDir $env:RUNNER_TEMP\qt-src
#
# Besides the Qt libraries, <Prefix>\licenses gets the licence texts of Qt and of the third
# party code built into it; the program carries them (src/ui/CMakeLists.txt).
param(
    [Parameter(Mandatory)] [string] $Version,
    [Parameter(Mandatory)] [string] $Prefix,
    [Parameter(Mandatory)] [string] $WorkDir
)
$ErrorActionPreference = "Stop"

# SHA-256 of the source archives, checked against the .sha256 files Qt publishes next to them.
# A new Qt version needs new values here and in cmake/LiteAnnoToolchainPin.cmake.
$sources = [ordered]@{
    "qtbase"         = "56001b905601bb9023d399f3ba780d7fa940f3e4861e496a7c490331f49e0b80"
    "qtimageformats" = "049bfb99845e4801672aca07c3c4fc4c140f932a3a33faa899419579e33ef1c8"
}

function Invoke-Checked([string] $exe, [string[]] $arguments) {
    Write-Host "> $exe $($arguments -join ' ')"
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "$exe failed with $LASTEXITCODE" }
}

New-Item -ItemType Directory -Force $WorkDir | Out-Null
$minor = ($Version -split '\.')[0..1] -join '.'
$base = "https://download.qt.io/archive/qt/$minor/$Version/submodules"
$problems = @()
foreach ($module in $sources.Keys) {
    $archive = "$module-everywhere-src-$Version.tar.xz"
    $file = Join-Path $WorkDir $archive
    Invoke-WebRequest "$base/$archive" -OutFile $file
    $actual = (Get-FileHash -Algorithm SHA256 $file).Hash.ToLowerInvariant()
    $published = ((Invoke-WebRequest "$base/$archive.sha256").Content -split '\s+')[0].ToLowerInvariant()
    Write-Host "$archive sha256 $actual (published $published)"
    if ($actual -ne $published -or $actual -ne $sources[$module]) {
        $problems += "$archive : got $actual, published $published, pinned $($sources[$module])"
    }
}
if ($problems) { $problems | ForEach-Object { Write-Host $_ }; throw "source archives do not match the pinned digests" }

foreach ($module in $sources.Keys) {
    Push-Location $WorkDir
    Invoke-Checked 7z @("x", "-y", "$module-everywhere-src-$Version.tar.xz")
    Invoke-Checked 7z @("x", "-y", "$module-everywhere-src-$Version.tar")
    Pop-Location
}

# Only what the program uses (Core, Gui, Widgets; the Windows platform and style plugins; the
# image readers). Qt's own copies of zlib, libpng, libjpeg, FreeType, HarfBuzz and PCRE2, so
# nothing on the build machine is linked in.
$qtbaseBuild = Join-Path $WorkDir "build-qtbase"
New-Item -ItemType Directory -Force $qtbaseBuild | Out-Null
Push-Location $qtbaseBuild
Invoke-Checked (Join-Path $WorkDir "qtbase-everywhere-src-$Version\configure.bat") @(
    "-prefix", $Prefix, "-static", "-static-runtime", "-release", "-optimize-size",
    "-nomake", "examples", "-nomake", "tests", "-nomake", "benchmarks",
    "-no-opengl", "-no-dbus", "-no-icu",
    "-qt-zlib", "-qt-libpng", "-qt-libjpeg", "-qt-freetype", "-qt-harfbuzz", "-qt-pcre",
    "-no-feature-sql", "-no-feature-network", "-no-feature-printsupport", "-no-feature-concurrent",
    "-no-feature-testlib",
    "--", "-G", "Ninja")
Invoke-Checked cmake @("--build", ".", "--parallel")
Invoke-Checked cmake @("--install", ".")
Pop-Location

$formatsBuild = Join-Path $WorkDir "build-qtimageformats"
New-Item -ItemType Directory -Force $formatsBuild | Out-Null
Push-Location $formatsBuild
Invoke-Checked (Join-Path $Prefix "bin\qt-configure-module.bat") @(
    (Join-Path $WorkDir "qtimageformats-everywhere-src-$Version"), "--", "-G", "Ninja")
Invoke-Checked cmake @("--build", ".", "--parallel")
Invoke-Checked cmake @("--install", ".")
Pop-Location

# Licence texts: Qt's own (LICENSES folders) and every bundled third-party library's
# (qt_attribution.json names its licence file).
$licenses = Join-Path $Prefix "licenses"
New-Item -ItemType Directory -Force $licenses | Out-Null
foreach ($module in $sources.Keys) {
    $src = Join-Path $WorkDir "$module-everywhere-src-$Version"
    Get-ChildItem (Join-Path $src "LICENSES") -File | ForEach-Object {
        Copy-Item $_.FullName (Join-Path $licenses "qt-$($_.Name)") -Force
    }
    Get-ChildItem -Recurse $src -Filter qt_attribution.json | ForEach-Object {
        $dir = $_.DirectoryName
        $entries = Get-Content -Raw $_.FullName | ConvertFrom-Json
        foreach ($entry in @($entries)) {
            if (-not $entry.LicenseFile) { continue }
            foreach ($licenseFile in @($entry.LicenseFile)) {
                $path = Join-Path $dir $licenseFile
                if (-not (Test-Path $path -PathType Leaf)) { continue }
                $name = ($entry.Id + "-" + (Split-Path $licenseFile -Leaf)) -replace '[^\w.\-]', '_'
                Copy-Item $path (Join-Path $licenses $name) -Force
            }
        }
    }
}
Get-ChildItem $licenses | Select-Object -ExpandProperty Name
