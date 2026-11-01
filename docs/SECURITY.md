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

## Browser integration (future): security requirements

There is no local service or listener in Haul today. If browser hand-off is added, it must:

1. Be **off by default** and enabled explicitly in Settings.
2. Use **native messaging** (the browser starts Haul over stdio) in preference to a network port. If a port is unavoidable, bind it to `127.0.0.1` only, never `0.0.0.0`.
3. Require a per-installation random token on every request, and check the `Origin` and `Host` headers. This defeats drive-by requests from web pages and DNS rebinding, because a rebinding page sends its own `Host`.
4. Accept only `{url, suggestedName, referrer}`, and feed them through the same `validateDownloadUrl` and `sanitizeFileName` checks. The destination folder is never chosen by the browser.
5. Never expose file reads, directory listings or command execution.
6. Request only the `downloads` and `nativeMessaging` extension permissions.
