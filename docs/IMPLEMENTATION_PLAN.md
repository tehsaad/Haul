# Haul — Implementation Plan (Phase 0)

## Repository audit (2026-10-24)

| Item | Finding |
|---|---|
| Existing project | None. `~/Haul` was created fresh; there was no earlier Haul code. |
| Git | New repository. `main` holds only the license/readme baseline. Development happens on `feature/shared-core`. |
| OS | Ubuntu 26.04.1 LTS, x86_64, 8 cores |
| Compiler | g++ 15.2.0 (C++17 supported) |
| CMake | 4.2.3 |
| Qt | Qt 6.10.2 runtime libraries installed; **dev packages (`qt6-base-dev`) missing** |
| libcurl | Runtime only; **`libcurl4-openssl-dev` missing** |
| SQLite | **`libsqlite3-dev` missing** |
| Build helpers | `ninja` and `pkg-config` missing; `make` present |
| Packaging | `dpkg-deb` present → CPack DEB generator usable |
| Windows toolchain | No MSVC, MinGW or NSIS on this machine → the Windows build can only be verified on GitHub Actions |
| Display | X11/Wayland session available → the GUI can be launched for real |
| GitHub | `gh` is authenticated, but nothing will be pushed without explicit permission |

Required packages (install once, needs sudo):

```
sudo apt install qt6-base-dev qt6-svg-dev libcurl4-openssl-dev libsqlite3-dev pkg-config ninja-build
```

## Technical decisions

- **Settings**: `QSettings` (INI on Linux under `~/.config/Haul`, registry/INI on Windows), wrapped by `SettingsRepository`, which validates every value.
- **Tasks and segments**: raw `libsqlite3` with prepared statements, a `schema_version` table and numbered migrations. The database lives in `QStandardPaths::AppDataLocation`.
- **Networking**: libcurl, one easy handle per transfer, run inside a worker `QThread`. Protocols are restricted to `http,https`, including on redirects. TLS verification always stays on. Cancellation works by returning nonzero from the progress callback. Bandwidth limits use `CURLOPT_MAX_RECV_SPEED_LARGE`, with the global cap split across active transfers.
- **Resume**: send `Range` plus `If-Range` (the stored ETag or Last-Modified). A `206` response is accepted only when `Content-Range` matches the request exactly. A `200` means the range was ignored, so the partial file is truncated and the transfer restarts.
- **Segments**: each segment writes its own `name.haul-segN` temp file. Each file's size is verified, then the files are concatenated into `name.haul-part` and finally `rename()`d to the real name. A task is never marked complete before that rename succeeds.
- **Threads**: one worker `QThread` per active transfer, and one curl handle per segment. Workers communicate with the GUI only through queued signals.
- **Tests**: Qt Test + CTest. A deterministic local HTTP server (`tests/server/test_server.py`, Python stdlib only) supports switchable modes: ranges on/off, slow, drop mid-stream, changing ETag, no Content-Length and error codes.

## Phase checklist

- [x] Phase 0 — audit and plan
- [x] Phase 1 — CMake, app shell, design system, five screens
- [x] Phase 2 — single-connection HTTP downloader
- [x] Phase 3 — lifecycle, cancel, pause/resume, retries
- [x] Phase 4 — SQLite persistence and recovery
- [x] Phase 5 — queue, concurrency, scheduler
- [x] Phase 6 — segmented downloading
- [x] Phase 7 — integrity, hardening, tests
- [x] Phase 8 — polish, docs
- [ ] Phase 9 — Linux packaging (`feature/linux-desktop`)
- [ ] Phase 10 — Windows packaging + CI (`feature/windows-desktop`)
- [ ] Phase 11 — final verification
