# Windows packaging

Haul for Windows is built with **MSVC 2022 (x64)**, **Qt 6.8.3** (`win64_msvc2022_64`), and **libcurl** and **SQLite** from **vcpkg**. The installer is made with **NSIS 3**.

## Files

| File | Purpose |
|---|---|
| `haul.ico` | Multi-size icon (16–256 px), rendered from the original `assets/haul.svg` |
| `haul.rc.in` | Icon and version information compiled into `haul.exe` |
| `WindowsPackaging.cmake` | Adds the resources to the build and the install rules |
| `stage.ps1` | `cmake --install`, then `windeployqt`, then copies the vcpkg DLLs (libcurl, zlib, sqlite3, …) |
| `installer.nsi` | NSIS script that creates `Haul-<version>-Setup.exe` |
| `../../.github/workflows/windows.yml` | Builds, tests, packages and smoke-tests on GitHub Actions |

## Building locally

From an "x64 Native Tools Command Prompt for VS 2022" with Qt, CMake, Ninja, vcpkg and NSIS installed:

```powershell
vcpkg install curl:x64-windows sqlite3:x64-windows
cmake --preset windows-msvc -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake -DCMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64
cmake --build --preset windows-msvc
ctest --preset windows-msvc
pwsh packaging\windows\stage.ps1 -BuildDir build\windows-msvc -StageDir %CD%\stage -VcpkgBin %VCPKG_ROOT%\installed\x64-windows\bin
makensis /DVERSION=0.4.0 /DSTAGE=%CD%\stage /DOUTFILE=%CD%\Haul-0.4.0-Setup.exe packaging\windows\installer.nsi
```

## Installing, upgrading, uninstalling

* **Install:** run `Haul-0.4.0-Setup.exe`. It needs administrator rights and installs to `C:\Program Files\Haul`, with a Start-menu entry and an entry in *Apps & features*. For a silent install: `Haul-0.4.0-Setup.exe /S /D=C:\Path`.
* **Upgrade:** run a newer installer. It replaces the program files in place. Your downloads, history (`%APPDATA%\Haul\Haul\haul.db`) and settings (`HKCU\Software\Haul`) are kept.
* **Uninstall:** use *Settings → Apps*, or run `Uninstall.exe` (`/S` for silent). This removes the program files, shortcuts, the uninstall entry and the launch-at-login entry. Your downloaded files, history and settings are kept. To remove the history and settings too, delete `%APPDATA%\Haul` and `HKCU\Software\Haul`.

## How the installer is verified

There is no Windows toolchain on the development machine (Linux), so the Windows build exists **only** in CI. The workflow:

1. builds with MSVC and runs all six test suites with `ctest`;
2. stages the app with `windeployqt` plus the vcpkg DLLs, and builds the installer with NSIS;
3. **installs it silently, launches the installed `haul.exe`** (which renders every page to PNG and exits), checks the exit code and the screenshots, then **uninstalls it silently** and checks the files are gone;
4. uploads the installer and the screenshots as workflow artifacts.

The installer is not code-signed, so Windows SmartScreen will warn about it.

## FFmpeg

The installer doesn't include FFmpeg. To join separate picture and sound, put `ffmpeg.exe` next to `haul.exe` or on `PATH`. Haul finds it automatically.
