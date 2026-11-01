# Demonstration Script (about 10 minutes)

Preparation (one terminal):

```bash
cmake --preset linux-release && cmake --build --preset linux-release
python3 -I tests/server/test_server.py      # prints "PORT <n>"; keep it running
export P=<n>
```

| # | Step | What to show |
|---|---|---|
| 1 | Launch `./build/linux-release/src/haul` | Dashboard empty state, sidebar, dark theme |
| 2 | **Add download** → paste `http://127.0.0.1:$P/file/300000000?rate=3000000&name=demo-video.mp4` | Live check: size 286 MB, "Resume / multi-connection: Supported", name taken from the server |
| 3 | **Start now** | Progress bar, speed card and graph, time left |
| 4 | Pause, then Resume (or select the row and press Space) | Progress continues from the same byte, not from 0 |
| 5 | Double-click the row → **Download details** | 4 segments filling independently (the test server limits each connection to 3 MB/s) |
| 6 | Close Haul during the download and reopen it | The task is restored as **Interrupted**; Resume revalidates and continues |
| 7 | Add two links with **Add to queue**, set "Simultaneous downloads" to 1, then **Start queue** | Only one runs at a time; Move up/down changes the order |
| 8 | Add `http://127.0.0.1:$P/file/40000000?ranges=0` | Details shows one connection: the server does not support ranges |
| 9 | Download a file whose SHA-256 you know, pasting it in the Add dialog | Status **Verified**; a wrong checksum shows **Bad checksum** |
| 10 | Add `http://127.0.0.1:$P/file/10?status=404` | A clear "HTTP 404 Not Found" error, not retried |
| 11 | Terminal: `ctest --preset linux-debug` | All suites pass |
| 12 | Show `packaging/linux` (`.deb`) and the Windows Actions run + installer artifact | Release workflows |

Segmentation benchmark (step 5 explanation): `docs/TESTING.md` → "Segmented download benchmark".
