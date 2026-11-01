# Testing

## How to run

```bash
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug            # all suites; GUI tests use QT_QPA_PLATFORM=offscreen
```

To run one suite with full output: `./build/linux-debug/tests/tst_network -v2`.

The network, manager and UI suites start `tests/server/test_server.py` themselves, using `python3 -I`. The server is standard-library Python only and listens on `127.0.0.1` on a random free port. No Internet access is needed, and results are deterministic.

## Controlled test server

`tests/server/test_server.py` serves generated content: byte *i* = `((i mod 65521) × 31 + seed) & 0xFF`. The C++ helper `testsupport::expectedContent()` generates the same bytes, so every download can be compared byte-for-byte or by SHA-256.

| Scenario | How it is produced |
|---|---|
| Successful download | `/file/<size>` |
| Slow download | `?rate=<bytes per second>` |
| Interrupted download | `?drop=<n>` closes the connection after *n* body bytes |
| Server supports byte ranges | default (`Accept-Ranges: bytes`, correct `206`) |
| Server ignores byte ranges | `?ranges=0` (always `200`, full body) |
| Wrong `Content-Range` | `?badrange=1` (range shifted by one byte) |
| Server errors | `?status=404`, `503`, `403`, … with optional `retryafter=` |
| Fails, then works | `/flaky/<size>?key=K&fail=N` |
| Content changes between attempts | `/mutable/<size>` plus `/bump` (new content and new ETag) |
| Unknown content length | `?nolength=1` |
| Server-suggested name (including path traversal) | `?name=..%2F..%2Fevil.sh` |
| Redirect to `file://` | `/redirect-file` |

## Results

These were recorded on **2026-10-24** on Ubuntu 26.04 (x86_64, g++ 15.2, Qt 6.10.2, libcurl 8.18.0, SQLite 3.46.1) in a Debug build. Every number below comes from an actual run.

`ctest`: **6/6 test executables passed**.

| Suite | Result | What it covers |
|---|---|---|
| `tst_core` | 41 passed, 0 failed | URL validation (12 cases), state transitions, progress and ETA with unknown size or speed, byte formatting, file-name sanitisation (10 cases), Content-Disposition, name-conflict handling, categories, retry policy, HTTP status classification, speed smoothing, rate limiter, segment planning, SHA-256 |
| `tst_queue` | 15 passed, 0 failed | FIFO and priority ordering, concurrency limit, stopped queue, retry delay, move up and down (including across priorities), scheduler: once per day, missed start inside and outside the window, stop, windows across midnight |
| `tst_storage` | 15 passed, 0 failed | migrations, refusing a newer schema, upgrading a real version-1 database, round trip of every field and segment, SQL injection attempt stored literally, persistence across reopen, crash recovery (interrupted → Paused, bytes from disk, never Completed), settings validation, persistence, rejection and fallback for corrupt values |
| `tst_network` | 33 passed, 0 failed | probe (ranges, no ranges, unknown length, 404, name sanitising, redirects, `file://` redirect refused, cancellation), full download, unknown length, HTTP errors and Retry-After, connection refused, dropped connection, resume with ranges, server ignoring ranges, content changing between attempts, wrong Content-Range rejected, unwritable destination, prompt cancellation (< 3 s), progress callbacks, bandwidth limit, segmented download and assembly, segmented resume, segmented fallback, segmented wrong ranges, incomplete segments refused, worker checksum verify and mismatch, existing file never overwritten, worker restart when the remote file changed |
| `tst_manager` | 21 passed, 0 failed | add → complete → persisted, invalid and duplicate rejection, unique names, pause/resume (single and 4-segment), cancel deletes partial data, invalid operations rejected, transient retries, bounded retries, permanent errors not retried, concurrency limit never exceeded (max active observed = 1 with limit 1), queue waits until started, stop-queue re-queues, priority and ordering, graceful restart restores and resumes, crash recovery and resume, remove with file, remove while downloading, bandwidth limit from settings |
| `tst_ui` | 11 passed, 0 failed | empty state on first run, all screens reachable, add dialog rejects `ftp://`, add dialog inspects a real server (size, resume support, Content-Disposition name) and starts the download, dashboard rows equal real tasks, segment panel shown only for segmented downloads, **GUI responsiveness**, no overlapping statistics cards at the minimum window size, light/dark theme switch |

(QtTest counts include each suite's `initTestCase` and `cleanupTestCase`, and every data row.)

### GUI responsiveness

`tst_ui::uiStaysResponsiveDuringTransfers` runs a download throttled to about 4 MB/s and measures the largest gap between 10 ms timer ticks on the GUI thread over 2 seconds.

**Measured worst gap: 14 ms** (the limit is 250 ms).

## Segmented download benchmark

The tool is `tools/bench_segments.cpp`. It was run in a Release build against the local test server on 2026-10-24:

```bash
cmake --preset linux-release && cmake --build --preset linux-release --target bench_segments
./build/linux-release/src/bench_segments "http://127.0.0.1:PORT/file/20971520?rate=2000000" 20971520 1 2 4 8
./build/linux-release/src/bench_segments "http://127.0.0.1:PORT/file/209715200" 209715200 1 4
```

| Scenario | Connections | Time | Throughput |
|---|---|---|---|
| 20 MiB, server limits **each connection** to 2 MB/s | 1 | 10.58 s | 1.89 MiB/s |
| | 2 | 5.31 s | 3.77 MiB/s |
| | 4 | 2.68 s | 7.47 MiB/s |
| | 8 | 2.36 s | 8.47 MiB/s |
| 200 MiB, unthrottled loopback | 1 | 0.21 s | 930 MiB/s |
| | 4 | 0.36 s | 556 MiB/s |

**Interpretation.** Segmentation helps only when something limits each connection, such as per-connection server throttling or long-distance TCP connections. With 8 connections the gain flattens, because the test server and the 1 MiB-per-segment rule become the bottleneck. When the link is not the bottleneck, as on loopback, segmentation is **slower**: Haul assembles segment files with one extra copy at the end. Haul therefore does not claim that more connections are always faster.

## Manual verification checklist

Run before a release. Results are recorded in the release notes.

1. Launch `haul` and confirm the Dashboard empty state appears.
2. Add a real HTTPS URL. Confirm the size and resume support are detected, then start it.
3. Pause and resume. Confirm the progress continues and does not restart.
4. Close Haul during a download, reopen it, and confirm the task shows *Interrupted*. Resume it.
5. Add two downloads with "Add to queue", then use Start queue and Stop queue.
6. Download a file with a known published SHA-256 and confirm the status is *Verified*.
7. Switch the theme in Settings and confirm the change applies immediately.

## Continuous integration

| Workflow | Runner | What it does | Verified run (2026-10-24) |
|---|---|---|---|
| `linux.yml` on `feature/shared-core` | ubuntu-24.04 (Qt 6.4.2) | build + `ctest` | [success](https://github.com/tehsaad/Haul/actions/runs/40079460088): 6/6 suites |
| `linux.yml` on `feature/linux-desktop` | ubuntu-24.04 | build + `ctest` + `.deb` + artifact | [success](https://github.com/tehsaad/Haul/actions/runs/40079884556) |
| `windows.yml` on `feature/windows-desktop` | windows-2022, MSVC 2022, Qt 6.8.3, vcpkg curl/sqlite3 | build + `ctest` + NSIS installer + silent install → launch installed app → silent uninstall | [success](https://github.com/tehsaad/Haul/actions/runs/40080104328): 6/6 suites, `Haul-0.1.0-Setup.exe` (38.1 MB) |

These CI runs used the code before the minimum-size layout fix. Later runs are listed on the repository's Actions page.
