# Linux packaging (Debian / Ubuntu)

## Build the package

```bash
sudo apt install build-essential cmake ninja-build pkg-config dpkg-dev \
                 qt6-base-dev qt6-svg-dev libcurl4-openssl-dev libsqlite3-dev python3
packaging/linux/build-deb.sh
# -> build/linux-release/haul_0.1.0_amd64.deb
```

`build-deb.sh` configures the `linux-release` preset (install prefix `/usr`), builds, runs the full test suite, then runs `cpack -G DEB`.

## What the package installs

| Path | Purpose |
|---|---|
| `/usr/bin/haul` | the application |
| `/usr/share/applications/haul.desktop` | menu entry (file name matches the Wayland app id `haul`) |
| `/usr/share/icons/hicolor/scalable/apps/haul.svg` | application icon |
| `/usr/share/metainfo/io.github.tehsaad.Haul.metainfo.xml` | AppStream metadata for software centres |
| `/usr/share/doc/Haul/` | README and LICENSE |

User data is **not** installed by the package and follows the XDG conventions:

* `~/.config/Haul/Haul.conf` holds the settings.
* `~/.local/share/Haul/Haul/haul.db` holds the download history.

## Dependencies

`dpkg-shlibdeps` computes the library dependencies (Qt 6 Core/Gui/Widgets/Svg, libcurl, SQLite, libstdc++). Qt plugins are loaded at run time and cannot be detected that way, so two packages are listed explicitly:

* `qt6-svg-plugins` provides the SVG icon engine.
* `qt6-qpa-plugins` provides the X11 platform integration.

`qt6-wayland` is a Recommends.

The package is built against the Qt of the machine that builds it. A `.deb` built on Ubuntu 26.04 needs Qt ≥ 6.10 and so targets Ubuntu 26.04. On older releases, build from source with the same script.

## Install, upgrade, uninstall

```bash
sudo apt install ./build/linux-release/haul_0.1.0_amd64.deb   # install or upgrade
sudo apt remove haul                                          # uninstall (keeps your data)
rm -rf ~/.config/Haul ~/.local/share/Haul                 # optional: delete your data
```

## Verification done for 0.1.0 (2026-10-24, Ubuntu 26.04)

* `desktop-file-validate packaging/linux/haul.desktop`: no errors.
* `dpkg-deb -c` lists exactly the files above. `dpkg-deb -f` shows the Depends computed by dpkg-shlibdeps plus the two plugin packages.
* The package was extracted with `dpkg-deb -x`, and the extracted `/usr/bin/haul` was run with `tools/capture_screenshots.sh`. It started, downloaded from the local test server and rendered all pages.
* **Not verified:** installing with `apt` on this machine (it needs sudo), and installing on a clean system. Because the required Qt plugins were already installed here, the extraction test cannot prove that the Depends list is complete on a minimal system. The Linux CI workflow builds the package on a clean runner.
