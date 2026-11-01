# Haul User Guide

## The window

The list on the left is always visible. It contains:

* **Add download**.
* Four pages: **Downloads**, **Queue**, **Details** and **Settings**.
* The **total speed** of all running downloads.

## Adding a download

1. Click **Add download** or press **Ctrl+N**. If the clipboard holds a link, the dialog fills it in for you.
2. Paste an `http://` or `https://` link. Haul checks it with the server and shows:
   * the **size**, or "Unknown" if the server does not say;
   * whether the download can be **resumed** and use several connections;
   * the **file type**;
   * a suggested **file name**.

   Press **Cancel check** to stop the check. You can still add the download afterwards.
3. Choose the **folder** and, optionally, change the **file name**. If a file with that name already exists, Haul tells you which new name it will use. It never overwrites an existing file.
4. Optionally:
   * choose a **priority**;
   * paste a **SHA-256 checksum** from a source you trust, such as the project's download page. Haul then verifies the finished file.
5. Choose **Start now** to begin immediately, or **Add to queue** to wait until the queue is started.

## Downloads

* **Cards:**
  * active downloads;
  * current total speed, with a one-minute history;
  * completed and failed downloads;
  * total data on disk.
* **Download table:** use the **search box** (**Ctrl+F**) and the **filter** (Downloading, Queued, Paused, Completed, Failed).
* **Context menu** (right-click a row): open the file, open its folder, show details, pause, resume, retry, cancel, copy the URL, or remove it from the list.
* **Keyboard** (with the table focused):
  * **Space** pauses or resumes the selected download.
  * **Enter** opens its details.
  * **Delete** removes it from the list.
* **Pause all / Resume all** act on every download.

"Time left" appears only when Haul knows the size and has measured a speed. Otherwise it shows "—". When the size is unknown, the progress bar shows activity instead of a made-up percentage.

## Download details

The details page shows the URL, destination, sizes, speed, time left, start and finish times, the number of automatic retries, the connections in use, the integrity status and any error message.

For multi-connection downloads, a **Segments** panel shows the progress of each part.

## Queue & scheduler

* **Start queue / Stop queue.** Downloads added with "Add to queue" only start while the queue runs. Stopping the queue puts the downloads it started back into the queue, keeping their partial data. Downloads you started with "Start now" keep running.
* **Tabs:** Queued, Active, Paused, Completed, Failed.
* **Priority** and **Move up / Move down** change the start order of waiting downloads. Higher priority starts first, and equal priorities start in the order they were added.
* **Limits:** simultaneous downloads (1–10) and connections per download (1–16).
* **Schedule:** a daily start time and an optional stop time. The schedule uses your computer's local time.
  * **The schedule only works while Haul is open.**
  * If you open Haul after the start time but before the stop time, the queue starts right away.
  * If the window has already ended, the queue waits until the next day.

## Settings

| Setting | Effect |
|---|---|
| Default download folder | Pre-filled in the Add dialog, and used by `haul --add`. |
| When adding a download | Which button is the default: Start now or Add to queue. |
| Maximum simultaneous downloads | How many downloads run at once. |
| Maximum connections per download | Used when a download starts, if the server supports byte ranges and the file is at least 2 MiB. |
| Bandwidth limit | Total KB/s for all downloads together. 0 means unlimited. Applies immediately. |
| Automatic retry, attempts, delay | Retries temporary failures only. The delay doubles after each attempt. |
| Theme | Dark or Light. Applies immediately. |
| Ask before removing a download | Confirmation dialog, with an option to also delete the file. |
| Start the download queue when Haul opens | Starts queued downloads automatically at launch. |
| Launch Haul when I sign in | Linux and Windows only. |

**Restore defaults** loads the default values. Press **Save** to keep them.

## Pausing, closing and crashes

* **Pausing** keeps the downloaded data.
* **Resuming** checks with the server whether the file changed. If it changed, or if the server cannot resume, the download restarts from the beginning, and Haul tells you.
* **Closing Haul** pauses running downloads. They show as *Interrupted* the next time.
* **After a crash or power cut**, the same happens. The progress is read from the partial files on disk.

## Command line

```
haul --add <url>     # add (and start) a download into the default folder; repeatable
haul --minimized     # start minimized
haul --help
```

## Where Haul keeps data

| | Linux | Windows |
|---|---|---|
| Download history | `~/.local/share/Haul/Haul/haul.db` | `%APPDATA%\Haul\Haul\haul.db` |
| Settings | `~/.config/Haul/Haul.conf` | Registry: `HKCU\Software\Haul\Haul` |
| Partial downloads | Next to the file: `name.haul-part`, `name.haul-segN` | same |

## Downloads that need a login

Open **Login** in the Add dialog and enter a user name and password, or paste a cookie. If a download fails because it needs a login, right-click it and choose **Log in…**. Haul forgets logins when it closes, so it will ask again after a restart.

## FTP

Paste an `ftp://` or `ftps://` link like any other. FTP downloads can resume, but they always use one connection.

## Browser

See "Browser extension" in the README. Opening Haul while it is already running brings the existing window to the front; `haul --add <url>` adds the link to it.

## Video streams

When a page loads audio or video, Haul's list pops up in the corner and a **Download with Haul** button appears on the video. Pick a quality if there is a choice, press **Download**, then confirm in the Add dialog. If you ignore the list it shrinks to a small **Haul: 3** indicator, and if you close it, the extension's popup can bring it back. To stop it popping up, untick **Open the list by itself** in the popup. You can also paste an `.m3u8` or `.mpd` address into the Add dialog yourself.

Sizes and time left start as estimates, shown with "About" or "≈", because a stream's exact size is only known at the end. The Details page shows the format, quality and how many pieces have arrived.

If you see "Protected video", the stream is encrypted and Haul won't download it. "This is a live stream" means it has no fixed length. If a download stops saying the link expired, reopen the page and add it again, because many sites issue links that only last a short time.

## What the list tells you

Each row shows what is known: format, resolution, length, size, bitrate, MIME type and the site it comes from. "≈" marks an estimate. Clicking or hovering over the list also checks each file:

* **Checked: a real MP4 file. Can resume.** Haul read its first bytes.
* **This server didn't let Haul in.** It may need a login or only accept browsers. Press **In browser** to download it with your browser, or use **Download** and enter a login in Haul.
* **This link no longer works.** Reload the page, since many links expire.

A file that claims to be media but turns out to be a web page is removed from the list.
