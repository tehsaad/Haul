# Haul Architecture

Haul is one C++17 code base for Linux and Windows. It is split into two static libraries and a small executable, so the download engine can be tested without a GUI.

```
                        ┌──────────────────────────── haul (executable) ───┐
                        │ src/app/main.cpp — wires everything together          │
                        └───────────────┬──────────────────────────────────────┘
                                        │
┌───────────── haul_ui (Qt Widgets) ▼──────────────────────────────────────────┐
│ MainWindow ─ DashboardPage ─ QueuePage ─ DownloadDetailsPage ─ SettingsPage      │
│ AddDownloadDialog   DownloadTableModel/FilterModel/ItemDelegate   TaskActions    │
│ Theme (design tokens + stylesheet)   Widgets (StatCard, EmptyState, Toast, ...)  │
└───────────────┬──────────────────────────────────────────────────────────────────┘
                │ signals / slots (GUI thread only)
┌───────────────▼──────────── haul_engine (no widgets) ────────────────────────┐
│ app/      DownloadManager  — owns all tasks, life cycle, retries, persistence      │
│ scheduler/ DownloadQueue (pure ordering logic)   Schedule + Scheduler (daily)      │
│ network/  DownloadWorker (one per running download, own QThread)                   │
│           HttpProbe · HttpDownloader (single) · SegmentDownloader (multi) · CurlEasy│
│ storage/  Database (SQLite + migrations) · DownloadRepository · SettingsRepository │
│ core/     DownloadTask · TaskState · Formatters · UrlValidator · FileNameUtils ·   │
│           RetryPolicy · SpeedMeter · RateLimiter · SegmentPlan · Checksum          │
│ platform/ database path, launch-at-login (Linux autostart / Windows Run key)       │
└───────────────────────────────────────────────────────────────────────────────────┘
          libcurl (HTTP/HTTPS)            SQLite 3              Qt 6 Core
```

## Responsibilities

| Class | Responsibility |
|---|---|
| `DownloadTask` | Plain value type holding everything about one download. It is copied, never shared, between threads. |
| `TaskState`, `canTransition()` | The six states and the table of allowed transitions. Invalid operations are rejected. |
| `DownloadManager` | Owns every task (`std::map<id, DownloadTask>`). Starts and stops workers, applies retry policy and concurrency limits, saves to SQLite and emits signals for the UI. Lives on the GUI thread. |
| `DownloadQueue` | Pure functions that pick which queued tasks start (priority, then FIFO, limited by free slots) and reorder the queue. |
| `Schedule` / `Scheduler` | Pure decision function (`evaluateSchedule`) plus a 15-second `QTimer` that starts or stops the queue. |
| `DownloadWorker` | Runs one attempt for one task on its own `QThread`: probe → revalidate → single or segmented transfer → assemble → rename → checksum. |
| `HttpProbe` | `GET` with `Range: bytes=0-0`. It learns the size, range support, validators and the suggested name. |
| `HttpDownloader` | Single-connection transfer with safe resume (`Range` + `If-Range` + `Content-Range` checks). |
| `SegmentDownloader` | Several ranges in parallel on one libcurl *multi* handle, each written to its own file, then verified and assembled. |
| `CurlEasy` | RAII wrapper that applies Haul's security defaults to every libcurl handle. |
| `Database` / `DownloadRepository` | SQLite with prepared statements, transactions and numbered migrations. |
| `SettingsRepository` | `QSettings` + validation. It emits `changed` so the engine reacts immediately. |
| `Theme` | Colour tokens for dark and light themes and the application stylesheet built from them. |

## Threading model

* **GUI thread:** all widgets, `DownloadManager`, `Scheduler` and every database write.
* **One worker thread per running download:** `DownloadWorker` lives on its own `QThread`. A segmented download still uses a single thread, because libcurl's multi interface drives all its connections.
* **Communication:** workers only *emit* signals (`metadataReady`, `progress`, `finished`). These cross threads as queued connections, so the manager handles them on the GUI thread. No mutex is needed because no data is shared.
* **The only shared object** is `TransferControl`, which holds an `std::atomic_bool stop` and an atomic speed limit. The manager writes them, and the worker reads them from libcurl's progress and write callbacks.
* **The Add dialog's probe** runs through `QtConcurrent::run` and a `QFutureWatcher`, so the dialog never blocks.

The UI test `uiStaysResponsiveDuringTransfers` measures the largest gap between 10 ms timer ticks on the GUI thread while a 4 MB/s download runs. The measured worst case was 14 ms.

## Download life cycle

```
           add (Start now / queue)      queue slot free            all bytes verified + renamed
 Queued ─────────────────────────────▶ Downloading ───────────────────────────────▶ Completed
   ▲  ▲                                  │   │   │
   │  │ resume / retry      pause ◀──────┘   │   └──── permanent error / retries used up ──▶ Failed
   │  └──────────── Paused ◀─────────────────┘ (queue stopped → Queued)                       │
   │                                                                                           │
   └──────────── retry ◀── Cancelled ◀── cancel (from Queued, Downloading, Paused, Failed) ◀───┘
```

A transient failure with retries remaining goes back to **Queued**, with a `retryAt` time (exponential backoff). It does not go to Failed. Completed is terminal.

## How a download attempt works (`DownloadWorker::run`)

1. **Probe** the URL again. Do not trust what was learned in an earlier session.
2. **Revalidate.** If the ETag, Last-Modified or size changed, or the server no longer supports ranges, delete the partial data and start from zero. The worker records a note explaining why.
3. **Check free disk space** when the size is known.
4. **Choose a mode.** Segmented mode needs a known size, a confirmed `206` response and at least 2 segments of 1 MiB or more. An existing single-connection partial file is continued as-is. Existing segment files are continued with their saved plan.
5. **Transfer.** Every `206` must carry exactly the requested `Content-Range`. A `200` reply to a range request means the server ignored the range. Segmented mode then falls back to one connection, and single-connection mode truncates the file and restarts. A partial file is never appended to blindly.
6. **Assemble** (segmented mode only). Verify each segment's size, concatenate the segments into `name.haul-part`, then verify the total size.
7. **Finalize.** `rename()` the `.haul-part` file to the final name. If a file with that name appeared in the meantime, choose `name (1).ext`. Nothing is ever overwritten.
8. **Verify** the SHA-256 if the user supplied one. The result is *Verified*, *Mismatch* or *Not checked*.

## Persistence

* Database: `QStandardPaths::AppDataLocation/haul.db`
  * Linux: `~/.local/share/Haul/Haul/`
  * Windows: `%APPDATA%\Haul\Haul\`
* Tables: `downloads`, `segments` and `schema_version`.
* Migrations live in `kMigrations[]` in `src/storage/Database.cpp`. Each one runs in its own transaction. Released migrations are never edited; new ones are appended. Version 2 (`auto_name`) is an example, and `tst_storage::upgradesVersionOneDatabase` checks it.
* Progress is written about once a second (the `tick()` timer). Full rows are written on every state change.
* **Startup recovery** (`applyStartupRecovery`):
  * A row still marked *Downloading* means the previous session ended abruptly. It becomes *Paused* and is flagged *Interrupted*.
  * Byte counts are corrected from the files actually on disk.
  * Nothing is ever promoted to *Completed*.
  * Nothing starts automatically unless the user enabled "Start the queue when Haul opens".
* Settings are stored with `QSettings`: `~/.config/Haul/Haul.conf` on Linux and the registry (`HKCU\Software\Haul`) on Windows.

## Platform-specific code

Shared code uses `QStandardPaths` for every path. The only `#ifdef`s are:

* `platform/Platform.cpp`: launch-at-login (Linux XDG autostart file, Windows `Run` registry key).
* `ui/TaskActions.cpp`: "Open containing folder" selects the file in Explorer on Windows.

Packaging lives only on the platform branches: Debian packaging on `feature/linux-desktop`, and the NSIS installer and Windows CI on `feature/windows-desktop`.

## Architectural decisions

| Decision | Reason |
|---|---|
| libcurl, not `QNetworkAccessManager` | The project requires it. It also gives precise control over protocols (`CURLOPT_PROTOCOLS_STR`), redirects, `Range` handling and stall timeouts. |
| Separate segment files, then assembly | No two connections ever write to the same file, so no locking is needed. Each segment can be verified on its own and resumed from its own size. The cost is one extra copy at the end. |
| One libcurl multi handle per segmented download | N connections on one thread. This keeps the threading model simple: one thread per download. |
| Raw `sqlite3` API with a small RAII `Statement` | It is easy to explain in a viva, avoids the QtSql plugin deployment on Windows, and enforces prepared statements. |
| `QSettings` for preferences | It uses each platform's native location and needs no schema. |
| A value-type `DownloadTask` copied into workers | No shared mutable state between threads. |
| No background service or daemon | Simpler and safer. The scheduler therefore only runs while the app is open, and the UI says so. |

## Video streams

`src/media/` handles HLS and DASH, and plugs into the existing worker and manager instead of adding a second download system.

| Part | Job |
|---|---|
| `HlsParser`, `DashParser` | Turn manifest text into variants and pieces. Pure functions with no network, so they are easy to test. |
| `StreamResolver` | Fetches manifests, lists qualities (`inspectStream`), builds the piece list for one quality (`resolveStreamPlan`), and enforces the refusal rules. |
| `StreamDownloader` | Fetches pieces with one libcurl multi handle. Finished pieces are `.seg` files in `<file>.haul-stream/`, so the disk is the record of progress. |
| `MediaMerger` | Joins pieces and optionally runs FFmpeg. |
| `DownloadWorker::downloadStream` | Ties these together, then uses the normal `finalize()`. |

Detection lives in the extension's `background.js`, but the only parser is the C++ one: the extension asks `haul-native-host` to describe a manifest. Pieces aren't stored in `task.segments` or the database, because a long video has thousands of them.

FFmpeg runs as a separate program instead of being linked. That keeps the download small, leaves Haul's licence unaffected, and users can update it themselves.

## Media detection

Three layers, from cheapest to most certain:

1. **`browser/detector.js`** classifies each response from `webRequest` headers. It is a pure function, tested with `node --test` and no browser. It decides on status, MIME type, `Content-Disposition`, request type and size, and rejects non-media and stream pieces.
2. **`browser/content.js`** adds `<video>`, `<audio>` and links on the page, reads duration and resolution from playing elements, removes duplicates, and shows the pop-up list, the indicator and the button on the video.
3. **`media/MediaSniffer`** identifies a file from its first bytes. It runs through the native host's `verify` message after a click or hover on the list, so a list that merely pops up causes no extra requests.

`background.js` keeps a per-tab list in `storage.session`, because Manifest V3 can stop the worker at any time. Downloads leave through the same `add` message as before, so all manager features apply.

