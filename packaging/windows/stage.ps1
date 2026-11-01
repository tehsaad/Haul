# Assembles a self-contained Haul folder ready for the installer.
#   pwsh packaging/windows/stage.ps1 -BuildDir build/windows-msvc -StageDir stage
param(
    [Parameter(Mandatory)] [string] $BuildDir,
    [Parameter(Mandatory)] [string] $StageDir,
    [string] $VcpkgBin = "$env:VCPKG_INSTALLATION_ROOT\installed\x64-windows\bin"
)
$ErrorActionPreference = "Stop"

cmake --install $BuildDir --prefix $StageDir
$bin = Join-Path $StageDir "bin"

# Qt libraries and plugins (platforms, styles, imageformats incl. SVG, iconengines).
windeployqt --release --no-translations --no-system-d3d-compiler --no-opengl-sw `
            --compiler-runtime (Join-Path $bin "haul.exe")
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed" }

# windeployqt does not know about libcurl, SQLite or their dependencies (zlib, ...).
Get-ChildItem -Path $VcpkgBin -Filter *.dll | Copy-Item -Destination $bin -Force

Get-ChildItem $bin | Select-Object Name, Length | Format-Table -AutoSize
