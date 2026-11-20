# SF100LinuxGUI

[![build](https://github.com/jfeulner95/SF100LinuxGui/actions/workflows/build.yml/badge.svg)](https://github.com/jfeulner95/SF100LinuxGui/actions/workflows/build.yml)

Qt 6 GUI and command-line tool for DediProg SF100 and SF600 SPI NOR flash programmers (SF600PG2 and SF700 as far as the engine supports them), running on Linux and macOS (Apple Silicon and Intel). It is built on DediProg's open-source [SF100Linux](https://github.com/DediProgSW/SF100Linux) engine. This project is not affiliated with DediProg.

## Features

- GUI toolbar operations: Detect, Select (manual chip choice from the chip database), File (load .bin/.hex/.s19), Save (chip buffer), Blank, Erase, Prog, Verify, Batch, Read, Config, Status Reg, Cancel.
- Hex view of the file buffer and the chip buffer; on a verify mismatch the view jumps to the first difference (Next difference: F3, Go to address: Ctrl+G).
- Log pane and progress bar; operations run on a worker thread and can be cancelled.
- Config dialog: Vcc (Auto, 3.3, 2.5, 1.8 V), SPI clock (24 MHz down to 375 kHz), target (Socket/ISP 1, Socket/ISP 2, Card, SKT), optional partial range (start/length in hex), and the Batch sequence (Erase, Blank check, Program, Verify, always in that order).
- `dpcmd` CLI, kept compatible with the original engine (it is the engine's own `dpcmd.c`).
- `libsf100core`: a small C API (`src/core/sf100core.h`) around the engine, with log and progress hooks. The GUI uses only this API.

Things to know:

- Prog does NOT erase first. Use Erase, or Batch (Erase, Blank, Prog, Verify).
- Partial erase works in units of the chip's erase block (often 64 KiB); start and length must be multiples of it.
- Dual/Quad IO is not offered; the engine uses single IO.
- SPI NAND is not supported by the GUI/API.
- Status register bit names are generic (Winbond-style). Check your chip's datasheet before writing. Unprotect clears SRP0, SEC, TB, BP2..BP0 in SR1 and CMP in SR2, then writes SR1+SR2. SR3 is read-only.

## Repository layout

| Path | Content |
|------|---------|
| `external/SF100Linux` | Git submodule: fork [jfeulner95/SF100Linux](https://github.com/jfeulner95/SF100Linux), branch `macos-gui-fixes` (upstream plus small fixes, see below) |
| `src/core` | `libsf100core` API facade (`sf100core.c/.h`) and the logging hook (`sflog.c`, `sf_hook.h`) |
| `src/cli` | `dpcmd` entry point (calls the engine's `main`, renamed `dpcmd_main`) |
| `gui` | Qt GUI (`sf100gui`); design notes in `gui/ARCHITECTURE.md` |
| `tests` | Hardware-free tests of the core API |

Fixes in the fork on top of upstream (`git log 1c912cc..HEAD`), summarized:

- libusb: include path via pkg-config, device enumeration/release, index normalisation and error codes in control requests, `BulkPipeRead(Ex)` returns the actual length.
- Correctness: NUL-terminated board type, cancel flag values, statically initialised mutex, freed verify buffer, 2.5 V chips no longer powered at 3.5 V, Intel HEX gap padding, `g_iNandBadBlockManagement` extern type.
- CLI: sleep instead of busy-wait in `Wait()`.
- Portability: executable-relative chip DB lookup (macOS), `OpBegin`/`OpEnd` split of `threadRun`, optional bulk-transfer progress hook, loading the whole chip database.
- udev: `uaccess` tag added to the rules file.

## Getting the source

```bash
git clone --recursive https://github.com/jfeulner95/SF100LinuxGui.git
```

For an existing clone:

```bash
git submodule update --init
```

## Build on Ubuntu / Debian

```bash
sudo apt-get install -y build-essential cmake pkg-config libusb-1.0-0-dev libxml2-dev qt6-base-dev libgl1-mesa-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Qt 6.4 or newer is required for the GUI (Ubuntu 24.04's `qt6-base-dev` is enough). If Qt6 is not found, the GUI is skipped with a message. For the CLI only, add `-DSF100_BUILD_GUI=OFF` to the configure step.

Outputs: `build/dpcmd` and `build/gui/sf100gui`. `ChipInfoDb.dedicfg` is copied next to each binary.

## Build on macOS

```bash
brew install libusb pkg-config libxml2 qt
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  "-DCMAKE_PREFIX_PATH=$(brew --prefix libxml2);$(brew --prefix qt)"
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Outputs: `build/dpcmd` and `build/gui/sf100gui.app` (the chip database is placed in the bundle's `Resources`). This build-tree .app still links Homebrew Qt; to make it self-contained, run the same steps as the CI job "Deploy macOS app" (`macdeployqt`, ad-hoc `codesign`). No driver or permission setup is needed on macOS.

## Linux USB permissions (udev)

Without a udev rule, opening the programmer fails with "Permission denied" (`SF_ERR_ACCESS`; the GUI shows a "USB access denied" dialog).

Easiest way: in the GUI (also the AppImage) use **Help > Install udev rule…**, or the **Install udev rule…** button in the "USB access denied" dialog. The rule is embedded in the program; it is installed with `pkexec` (you are asked for your password) to `/etc/udev/rules.d/60-dediprog.rules`, and udev is reloaded. This needs polkit, which every common desktop has.

Manually:

```bash
sudo cp external/SF100Linux/60-dediprog.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

Then unplug and replug the programmer. The rule matches USB 0483:dada. It tags the device with `uaccess`, so the user of the active desktop session gets access; it also sets mode 666 and group `plugdev`, which covers non-desktop logins (for example over SSH, where the user should be in the `plugdev` group).

`sudo cmake --install build` installs this rules file too (see below).

## Install

```bash
sudo cmake --install build
```

Installs (prefix defaults to `/usr/local`):

- `dpcmd` to `<prefix>/bin`
- `sf100gui` to `<prefix>/bin` (on macOS, the .app bundle to `<prefix>`)
- `ChipInfoDb.dedicfg` to `<prefix>/share/DediProg/`
- on Linux only: `60-dediprog.rules` to `/etc/udev/rules.d` (honours `DESTDIR`)

The chip database is searched in this order: `DPCMD_CHIPDB` environment variable (full path to the file); next to the executable; `<exe dir>/../share/DediProg/`; `<exe dir>/../Resources/` (inside a macOS .app); the install data directory compiled in at build time.

## Usage

GUI quick workflow: connect the programmer and press Detect; open the image with File; press Batch (Erase, Blank, Prog, Verify as chosen in Config). Use Read, then Save, to dump the chip.

CLI (see `dpcmd --help` for all switches; with no arguments it prints usage and exits):

```bash
dpcmd --detect                              # detect the chip
dpcmd -r dump.bin                           # read the whole chip into a file
dpcmd -rSTDOUT -a0x100 -l0x23               # read 0x23 bytes from 0x100 and print them
dpcmd -u image.bin                          # update the whole chip (erase only differing sectors, program)
dpcmd -u image.bin -a0x100                  # replace only the area covered by the file; rest unchanged
dpcmd -p image.bin -a0x100                  # program without erase (area assumed blank)
```

## CI artifacts

The `build` workflow runs on ubuntu-24.04, macos-latest (Apple Silicon) and macos-15-intel, and also builds the engine standalone with its own Makefile on Linux. Each run uploads `dpcmd-<os>.tar.gz`, containing a `dpcmd/` directory with:

- `dpcmd`
- `ChipInfoDb.dedicfg`
- `60-dediprog.rules`
- `LICENSE`
- `sf100gui` (Linux) or `sf100gui.app` (macOS)

On Linux each run also uploads `sf100gui-x86_64-appimage` with `sf100gui-x86_64.AppImage`: a single file with Qt, libusb and libxml2 bundled, so no packages need to be installed. Make it executable and start it:

```bash
chmod +x sf100gui-x86_64.AppImage
./sf100gui-x86_64.AppImage
```

- Requires a distribution at least as new as Ubuntu 24.04 (glibc 2.39), for example Ubuntu 24.04+, Debian 13+, Fedora 40+, because it is built on the ubuntu-24.04 runner.
- Starting an AppImage needs FUSE 2 (`sudo apt install libfuse2t64` on Ubuntu 24.04). Without FUSE: `./sf100gui-x86_64.AppImage --appimage-extract-and-run`.
- The udev rule cannot be installed by the AppImage itself; use Help > Install udev rule… once (see "Linux USB permissions").

On macOS each run also uploads `sf100gui-<os>-dmg` with a disk image. The app inside is self-contained: Qt, libusb and libxml2 are bundled, and `dpcmd` is included at `sf100gui.app/Contents/MacOS/dpcmd`. Homebrew is not needed. Use the image that matches your Mac (`macos-latest` = Apple Silicon, `macos-15-intel` = Intel).

Notes:

- Linux runtime dependencies: libusb-1.0, libxml2, Qt 6 (`qt6-base-dev` or the Qt 6 runtime packages).
- The macOS app requires at least the macOS version of the CI runner that built it, because the bundled Homebrew libraries are built for that version.
- The app is signed ad hoc, not with an Apple Developer ID, so Gatekeeper blocks it after download. Either right-click the app and choose Open, or run `xattr -dr com.apple.quarantine /Applications/sf100gui.app` (adjust the path to where you copied it).
- After extracting the tar.gz, the executable bit is preserved; if needed, `chmod +x dpcmd sf100gui`.
- On Linux, install the udev rule from the archive as described above.

## Working with the engine submodule

Changing the engine:

```bash
cd external/SF100Linux
git checkout macos-gui-fixes
# edit, then:
git commit -am "fix: ..."
git push origin macos-gui-fixes
cd ../..
git add external/SF100Linux
git commit -m "Update engine"     # records the new submodule pointer
```

Updating from upstream DediProg:

```bash
cd external/SF100Linux
git remote add upstream https://github.com/DediProgSW/SF100Linux.git   # once
git fetch upstream
git checkout macos-gui-fixes
git rebase upstream/master
git push --force-with-lease origin macos-gui-fixes
cd ../..
git add external/SF100Linux && git commit -m "Update engine to upstream"
```

## Status

The code builds and passes the hardware-free tests and GUI/CLI smoke tests in CI. It has not yet been tested with a real programmer; hardware testing is pending. Treat it as unverified: do not rely on it for irreplaceable chip contents, and keep backups.

## License

GPL-2.0-only, see [LICENSE](LICENSE). The engine (`external/SF100Linux`) is DediProg's SF100Linux, licensed under GPL version 2 without an "or later" clause; because `dpcmd`, `libsf100core` and the GUI link it, this repository uses the same license.

Third-party components used by the binaries:

| Component | License | Notes |
|-----------|---------|-------|
| SF100Linux engine | GPL-2.0 | `external/SF100Linux` |
| Qt 6 (Core, Gui, Widgets) | used under GPL-2.0 (Qt is also offered under LGPL-3.0 / GPL-3.0) | bundled in the macOS app |
| libusb-1.0 | LGPL-2.1-or-later | bundled in the macOS app |
| libxml2 | MIT | bundled in the macOS app |

Source code for every released binary is this repository at the corresponding commit (plus the submodule commit it pins).
