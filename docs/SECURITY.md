# Security

## Reporting a vulnerability

Please open a private security advisory on the GitHub repository instead of a public issue.

## What Haul protects against

| Risk | Mitigation | Where |
|---|---|---|
| Downloading from non-HTTP sources, local file disclosure | Only absolute `http://` and `https://` URLs are accepted. libcurl is restricted with `CURLOPT_PROTOCOLS_STR = "http,https"`, **and with `CURLOPT_REDIR_PROTOCOLS_STR`**, so a redirect to `file://` or `ftp://` is refused. | `UrlValidator.cpp`, `CurlEasy::configure`; test `redirectToFileSchemeIsRefused` |
| TLS interception | Peer and host verification are always on (`SSL_VERIFYPEER=1`, `SSL_VERIFYHOST=2`). No setting can turn them off. A certificate failure is a *permanent* error and is never retried. | `CurlEasy::configure`, `errorFromCurl` |
| Credentials leaking into storage or logs | URLs containing a user name or password are rejected. `redactUrl()` removes the query string and fragment for display or logging. No cookies or auth headers are stored. | `UrlValidator.cpp` |
| Path traversal through server-supplied names | Every name, whether from `Content-Disposition`, the URL or the user, passes through `sanitizeFileName`. This keeps only the last path component and removes `..`, separators, control characters and `<>:"\|?*`, prefixes Windows device names (CON, NUL, …), and limits the length. | `FileNameUtils.cpp`; tests `sanitizeFileNames`, `probeSanitizesServerFileName` |
| Overwriting user files | Data is written to `name.haul-part` (or `.haul-segN`) and renamed only when complete. If the final name exists, `name (1).ext` is used. Existing files are never truncated. | `DownloadWorker::finalize`; test `workerNeverOverwritesExistingFile` |
| Reporting partial or corrupted files as complete | A task becomes Completed only after a size check and the rename. Every `206` must match the requested `Content-Range`. A server that sends more data than requested or announced is aborted. Startup recovery never promotes a task to Completed. | `HttpDownloader`, `SegmentDownloader`, `applyStartupRecovery` |
| Mixing two versions of a file | `If-Range` plus a comparison of ETag, Last-Modified and size before every resume. If anything changed, the partial data is discarded. | `DownloadWorker::run`; test `resumeAfterResourceChanged` |
| Resource exhaustion | Free disk space is checked when the size is known. Header storage is capped (200 headers, 4 KB each). The probe reads at most 64 KB. There are connection and stall timeouts. Redirects are capped at 10 and segments at 16. | `CurlEasy`, `HttpProbe`, `DownloadWorker` |
| Compressed bodies breaking size checks | The request header is `Accept-Encoding: identity`. A body with `Content-Encoding` makes the length "unknown" rather than wrong. | `CurlEasy` |
| SQL injection | Every query uses prepared statements with bound parameters. | `Database.cpp`; test `sqlInjectionIsHarmless` |
| Slow cancellation | The libcurl progress callback checks an atomic flag, and rate-limit sleeps are cut into 50 ms steps. Cancellation is tested to finish in under 3 s. | `HttpDownloader`; test `cancellationIsPrompt` |
| Retry storms | Retries are bounded (at most 10, with exponential backoff capped at 5 minutes). 4xx errors other than 408, 425 and 429 are never retried. `Retry-After` is honoured but capped. | `RetryPolicy` |

## Not in scope

* Malware scanning of downloaded files.
* Authenticated downloads (cookies, OAuth, HTTP auth). These are unsupported by design for now.
* DRM, paywall or access-control circumvention. Haul will never implement this.


## Browser hand-off

The extension talks to Haul through the browser's native messaging, not a network port, so web pages have no way to reach Haul.

* The browser only starts `haul-native-host` for the extension listed in the manifest. The host also checks the caller it was given.
* Registration is opt-in (Settings → Browser), per user, and removed when the option is turned off.
* The host accepts one message of at most 64 KiB and forwards only a link that passes the same URL validation as the Add dialog.
* A link from the browser always opens the Add dialog. The browser can suggest a download but cannot start one.
* The extension asks to run on all http and https sites, which Chrome words as "read and change all your data". It needs this to notice media files and show its panel. It looks at response headers, `<video>`/`<audio>` addresses, and the page's title (to name the video). It never reads cookies, passwords or other page text. Nothing is sent anywhere except to Haul on this computer. Manifests are read by the native host, without the browser's cookies.
* Watching responses sends nothing to any server. Haul contacts a server itself only for a manifest it found, or when you open the panel, to read the first bytes of the listed files. If a site uses one-time links, that check could use one up.
* Only the origin of the page a link came from is passed along (`https://site/`), not the full address. It is kept in memory like a login.

## Single-instance channel

A lock file and a local socket in a per-user location (`$XDG_RUNTIME_DIR`, or a per-user named pipe on Windows) keep one copy running. The socket is restricted to the current user, messages are capped at 64 KiB, and URLs are validated again on arrival. Nothing listens on the network.

## Logins

User names, passwords and cookies are kept in memory only: they are never written to the database, settings or logs, and links containing `user:password@` are refused. libcurl does not send them to a different host after a redirect. The dialog warns when a login would travel over unencrypted `http://` or `ftp://`.

## FTP

Plain FTP sends everything, including passwords, unencrypted. `ftps://` verifies the server's certificate like HTTPS does. A web server cannot redirect Haul to FTP.

## Video streams

Haul handles unencrypted HLS and DASH only, and has no site-specific extraction.

* **Encrypted streams are refused, never decrypted.** That covers any `EXT-X-KEY` or `EXT-X-SESSION-KEY` whose method isn't `NONE`, DASH `ContentProtection`, and `encv`, `enca`, `sinf` or `pssh` markers in downloaded init data. Encryption that starts partway through a playlist is caught too.
* **FFmpeg only sees local files** Haul has already assembled, with `-protocol_whitelist file,pipe`. It is never given a manifest or a URL, so it cannot fetch a key or decrypt a stream by itself.
* **Refused access stops the download.** A 401, 403 or 410 on a piece is permanent and not retried, and a web page served in place of video is rejected rather than saved.
* **Manifests are untrusted.** Size is capped at 10 MiB and pieces at 200,000. XML is parsed without fetching external entities, piece addresses must be http or https, and a manifest on the public internet can't point at localhost or private network addresses.
* YouTube hosts are excluded from detection. Live streams are refused because they never end.

