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

Outputs: `build/dpcmd` and `build/gui/sf100gui.app` (the chip database is placed in the bundle's `Resources`). The .app links Homebrew Qt and is not self-contained yet (`macdeployqt` is a later task). No driver or permission setup is needed on macOS.

## Linux USB permissions (udev)

Without a udev rule, opening the programmer fails with "Permission denied" (`SF_ERR_ACCESS`; the GUI shows a "USB access denied" dialog).

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

The `build` workflow runs on ubuntu-latest, macos-latest (Apple Silicon) and macos-15-intel, and also builds the engine standalone with its own Makefile on Linux. Each run uploads `dpcmd-<os>.tar.gz`, containing a `dpcmd/` directory with:

- `dpcmd`
- `ChipInfoDb.dedicfg`
- `60-dediprog.rules`
- `sf100gui` (Linux) or `sf100gui.app` (macOS)

Notes:

- Runtime dependencies: libusb-1.0, libxml2, Qt 6 (on macOS: Homebrew `libusb`, `libxml2`, `qt`).
- After extracting, the executable bit is preserved by tar; if needed, `chmod +x dpcmd sf100gui`.
- macOS may block downloaded, unsigned binaries. Remove the quarantine flag: `xattr -d com.apple.quarantine dpcmd` (and `xattr -dr com.apple.quarantine sf100gui.app`).
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

The engine (`external/SF100Linux`) is licensed under the GNU General Public License, version 2 (see `external/SF100Linux/LICENSE`).

License of this repository: to be decided (must be GPL-compatible because it links the GPL engine). There is no LICENSE file yet.
