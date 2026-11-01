# Changelog

All notable changes to Haul are documented here (format: [Keep a Changelog](https://keepachangelog.com/)).

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
