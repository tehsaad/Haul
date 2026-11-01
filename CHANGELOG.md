# Changelog

All notable changes to Haul are documented here (format: [Keep a Changelog](https://keepachangelog.com/)).

## [0.4.0] — unreleased

### Added
- Media detection from real responses: status, Content-Type, Content-Disposition, request type and size. Works on extensionless and signed links, redirects, generic MIME types, ranged and chunked responses, and files players fetch in the background.
- All of MP4, MKV, WebM, MOV, AVI, FLV, TS, M4V, MPEG, MPG, 3GP, MP3, WAV, FLAC, AAC, M4A, OGG, Opus and WMA.
- `MediaSniffer` identifies files from their first bytes. The native host's `verify` message confirms format, size and resume support when the panel is opened; fakes are removed.
- A small indicator and toolbar count, with several files per page. The panel shows format, size, duration, resolution, bitrates, MIME type and source, with estimates marked.
- Duplicates are removed across signed links, redirects and identical name and size.
- The detection list survives the browser stopping the extension's background worker.

### Changed
- A browser download is only taken over if the server accepts Haul; otherwise it carries on in the browser. The panel offers **In browser** for files that refuse Haul.
- The panel no longer opens by itself.
- `probeUrl` reads the first 4 KiB instead of 1 byte.

## [0.3.0] — unreleased

### Added
- Unencrypted HLS and MPEG-DASH downloads: master and media playlists, SegmentTemplate, SegmentTimeline, SegmentList and single-file representations, byte ranges and fragmented MP4.
- Quality selection with size estimates in the browser panel and the Add dialog.
- Pieces download several at a time, are retried individually, and survive pausing and restarting.
- Optional FFmpeg support to join separate audio and video and to convert `.ts` to `.mp4`. It is only ever given local files.
- The browser extension detects manifests that players fetch, and shows protected streams as unavailable.
- Database migration to version 3.

### Security
- Encrypted, live and restricted streams are refused, including encryption that only appears in the init data. A public manifest cannot point Haul at the user's local network.

## [0.2.1] — unreleased

### Fixed
- Ticking **Allow the browser extension** and pressing Save unticked the box and registered nothing, so the extension could never reach Haul.

### Changed
- Browser downloads now go to Haul automatically (can be turned off in the extension's popup). If Haul can't be reached, the download continues in the browser.

### Added
- A panel on pages that play an ordinary video or audio file, with a Download button.
- Sends the address of the site a link came from, because some servers refuse downloads without it.
- `tests/browser/e2e.mjs`: drives real Chrome through the extension and the native host.

## [0.2.0] — unreleased

### Changed
- Renamed the project from Haul to Haul. Data now lives under the `Haul` folders; the old `Haul` folders are not imported.
- Reworked the interface: warm neutral colours with one copper accent, a toolbar and dense list instead of statistic cards, a status bar, sentence-case labels and shorter wording.

### Added
- Browser extension for Chromium browsers and Firefox, connected through native messaging (no local web server). Links from the browser always open the Add dialog first.
- Only one copy runs per user; a second copy passes its links to the first and exits.
- Logins for HTTP (user name and password, or a cookie) and FTP, held in memory only. Failed downloads can be retried with **Log in…**.
- `ftp://` and `ftps://` downloads with resume.

## [0.1.0] — 2026-10-24 (unreleased)

### Added
- HTTP/HTTPS download engine on libcurl: probing, resumable single-connection transfers, and segmented multi-connection transfers with verified assembly.
- Safe resume with `Range`/`If-Range`, exact `Content-Range` validation, and restart when the server ignores ranges or the file changed.
- Task state machine (Queued, Downloading, Paused, Completed, Failed, Cancelled) with rejected invalid transitions.
- Automatic retry with bounded exponential backoff for transient errors only.
- Priority + FIFO queue, concurrency limit, start/stop queue, move up/down.
- Daily scheduler with optional stop time and a documented missed-start policy.
- Global bandwidth limit, re-balanced across running downloads.
- SQLite persistence with migrations, and startup recovery of interrupted downloads.
- Optional SHA-256 verification against a trusted checksum.
- Qt 6 Widgets UI: Dashboard, Add Download dialog, Download Details, Queue & Scheduler, Settings; dark and light themes; original icon set.
- Command line: `--add <url>`, `--minimized`.
- Local deterministic HTTP test server and six test suites; segmented-download benchmark tool.
