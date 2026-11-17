# PuTTY for macOS

A macOS build of [PuTTY](https://www.chiark.greenend.org.uk/~sgtatham/putty/) 0.85,
plus the fixes needed to make its GTK front end behave properly on macOS.

Everything not listed below is stock upstream PuTTY. MIT licensed — see `LICENCE`.

A file-by-file account of every difference from the official PuTTY 0.85
tarball, with the commands to reproduce the diff, is in
[`CHANGES-FROM-UPSTREAM.md`](CHANGES-FROM-UPSTREAM.md).

---

## What this fork changes

### 1. The macOS code paths are now actually enabled

The CMake build never defined `OSX_GTK` (a leftover from the old Makefile-based
build), so macOS builds silently compiled the generic **X11** code paths instead
of the macOS ones. Both `#ifdef OSX_GTK` blocks in `unix/platform.h` now also
accept `__APPLE__`.

Consequences of the fix:

- **Copy and paste works.** The clipboard UI actions were pointing at the X11
  `PRIMARY` selection, which does not exist on macOS. The defaults for the
  mouse paste action, `Ctrl+Shift+Ins` and `Ctrl+Shift+C/V` now target the
  system clipboard.
- **`Cmd+C` / `Cmd+V` work.** Without `OSX_META_KEY_CONFIG` there was no
  Command-key pass-through, so the terminal swallowed `Cmd`-modified keys
  before the menu accelerators could see them.
- **`Ctrl+Ins` / `Shift+Ins` work** in the terminal, as the Window/Selection
  panel documents. A PC keyboard's Insert key reaches macOS as virtual keycode
  0x72, the Mac's "Help" key, so PuTTY was looking for `GDK_KEY_Insert` and
  never saw it; keypad Insert (`GDK_KEY_KP_Insert`) had the same problem.
- **Copy and paste work in dialog boxes** — the configuration box's text
  fields, and selectable message text. GTK only binds these to Control, while
  macOS users press Command; both are accepted now, plus `Ctrl+Ins` /
  `Shift+Ins`.
- The low-level `gdk_selection_*` code — which the source itself notes fails on
  macOS — is replaced by the `GtkClipboard` path.

### 2. Finder drag-and-drop

Dropping a file from the Finder onto the window inserts its path into the
terminal input, ready to use as a command argument.

### 3. Cursor offset fix

`term_set_preedit_text()` allocated the IME pre-edit line unconditionally and
left it non-`NULL` with zero columns when the input method reported an empty
pre-edit string — which GTK on macOS does on ordinary keystrokes. `do_paint()`
then computed the cursor column as `preedit_end - 1`, drawing the cursor one
cell too far left, and read `chars[-1]` out of bounds. Fixed in
`terminal/terminal.c`.

### 4. A configuration folder you can move

PuTTY has always used `~/.putty`. This build offers, once, to keep the
configuration somewhere else instead — `~/Documents/putty-beter-config` by
default. Anything already saved is moved there, and the choice is recorded in
`~/.putty/config-location`, a one-line text file.

Nothing is deleted unless it was copied successfully, and nothing is
overwritten: if the destination already holds one of these files, the old copy
is simply left where it was. Deleting `config-location` returns to `~/.putty`.

Hold **Option (⌥)** while launching PuTTY to reopen the dialog and move the
folder again, or set `PUTTY_CONFIG_RELOCATE=1`.

`plink`, `pscp`, `psftp` and `puttygen` read the same file, so saved host keys
and CA records stay consistent. `PUTTYDIR` (upstream) still overrides
everything.

### 5. Build tooling

`scripts/build-macos.sh`, `scripts/bundle-macos.sh` and `CMakePresets.json`
build the binaries and a self-contained `PuTTY.app` with GTK bundled inside.

### 6. Runtime diagnostics

`PUTTY_DIAG=1` traces key events, clipboard activity, font metrics and cursor
placement to stderr. `PUTTY_CONFIG_DEBUG=1` traces the configuration folder
resolution and relocation, and the remembered configuration-box size. Neither
has any effect when unset.

### 7. Configuration box

The box is resizable and remembers its size, which is kept in
`<config folder>/gui-state` (delete that file to go back to the default).

The Session panel puts "Close window on exit" above the saved-session list, and
the list is the part that grows when the window does — useful once you have a
lot of saved sessions. The Load/Save/Delete buttons keep their natural width,
and the list runs right up to them, so no horizontal space is wasted either.

### 8. Session folders

Sessions can be kept in folders. A session called `Work/beta` lives in the
real folder `Work`, so the structure is visible in the Finder and a folder can
be empty.

In the configuration box the session list is a **collapsible tree**: folders
are in bold with a disclosure triangle, and a session is moved into a folder
by dragging its row onto it. Under the list are **Load**, **Save**, **Delete**
and **New Folder**; New Folder takes the name from the Saved Sessions box, the
same way Save does. Delete removes a folder only when it is empty, so it
cannot throw sessions away by accident.

You can also just type a name with a slash: saving as `Work/beta` creates the
folder as it goes. Names are checked, so a session name can never escape the
sessions directory.

### 9. Changed defaults

These only affect **new** sessions, and settings a saved session already stores
keep their saved values.

| Setting | Upstream | This build |
|---|---|---|
| Session → Logging | None | **Printable output** |
| Log file name | `putty.log` | `<config folder>/logs/&Y-&M-&D_&T-&H.txt` |
| If the log file exists | Ask | **Always append** |
| Terminal → Backspace key | Control-? | **Control-H** |
| Terminal → Function keys | ESC[n~ (Linux) | **VT100+** |
| Window size | 80×24 | **128×36** |
| Keepalives (seconds) | 0 (off) | **120** |
| Internet protocol version | Auto | **IPv4** |
| Connection → Tunnels → IP version (new tunnel form) | Auto | **IPv4** |

---

## Requirements

- macOS (Apple Silicon; see [Intel](#intel-x86_64) below)
- Xcode Command Line Tools — `xcode-select --install`
- Homebrew — <https://brew.sh>

### Build dependencies

```sh
brew install cmake ninja pkgconf gtk+3 adwaita-icon-theme
```

`adwaita-icon-theme` is required because Homebrew's `gtk+3` does not ship the
Adwaita icon set that the bundle needs.

Optional, only for rebuilding the manual:

```sh
brew install halibut
```

### For the `.app` bundle: gtk-mac-bundler

Assembling `PuTTY.app` needs `gtk-mac-bundler`, which copies GTK and its
dependencies into the bundle and rewrites their install names. It is a separate
**GPL-2.0** project, so it is **not** vendored into this MIT tree:

```sh
git clone https://gitlab.gnome.org/GNOME/gtk-mac-bundler.git tools/gtk-mac-bundler
```

It is only needed for the `.app`; the bare binaries build without it.
See [`tools/README.md`](tools/README.md) for details and the lookup order
(`$BUNDLER_DIR`, then `tools/gtk-mac-bundler`, then `~/gtk-mac-bundler`).

---

## How to build

```sh
# 1. Compile the binaries -> build-arm64/
./scripts/build-macos.sh arm64

# 2. Assemble the app bundle
./scripts/bundle-macos.sh arm64

# 3. Run it
open build-arm64/bundle-stage/unix/PuTTY.app
```

**Output locations**

| What | Where |
|---|---|
| Binaries | `build-arm64/` — `putty`, `puttyapp`, `plink`, `pscp`, `psftp`, `psusan`, `puttygen`, `pterm`, `pageant` |
| App bundle | `build-arm64/bundle-stage/unix/PuTTY.app` |

**Test the binaries without bundling:**

```sh
./build-arm64/putty -load "Default Settings"
./build-arm64/plink --version
```

### Build options

```sh
./scripts/build-macos.sh arm64 --clean    # rebuild from scratch
./scripts/build-macos.sh x86_64           # Intel build
DEBUG=1 ./scripts/build-macos.sh arm64    # verbose
DEBUG=1 ./scripts/bundle-macos.sh arm64   # verbose bundling
BUILD_TYPE=Debug ./scripts/build-macos.sh arm64
```

CMake presets are also available:

```sh
cmake --preset arm64 && cmake --build --preset arm64
```

### Manual build, without the scripts

```sh
cmake -B build-arm64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build-arm64 -j"$(sysctl -n hw.ncpu)"
```

---

## Intel (x86_64)

Homebrew builds GTK for a single architecture, so an Intel build needs an Intel
Homebrew in `/usr/local` (installed under Rosetta) — the arm64 Homebrew in
`/opt/homebrew` cannot be used for it.

```sh
arch -x86_64 /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
arch -x86_64 /usr/local/bin/brew install gtk+3 cmake ninja pkgconf adwaita-icon-theme

./scripts/build-macos.sh x86_64
./scripts/bundle-macos.sh x86_64
```

**This path is not yet verified**, and note that Apple is retiring Rosetta:
macOS 27 is the last release that supports it and macOS 28 drops it for general
applications. A native **arm64** build is the one that matters going forward.

---

## Notes

- `PuTTY.app` is ad-hoc signed, not notarized. If macOS quarantines it after
  download, use right-click → **Open**, or
  `xattr -dr com.apple.quarantine PuTTY.app`.
- Build output (`build-*/`) and the `gtk-mac-bundler` checkout are git-ignored.
- The defects fixed here — `OSX_GTK` never being defined, and the IME
  pre-edit cursor offset with its out-of-bounds read — are upstream PuTTY bugs
  and are worth reporting upstream. The full list is at the end of
  [`CHANGES-FROM-UPSTREAM.md`](CHANGES-FROM-UPSTREAM.md).

See [`CHANGES-FROM-UPSTREAM.md`](CHANGES-FROM-UPSTREAM.md) for every difference
from upstream PuTTY 0.85, and [`PLAN.md`](PLAN.md) for the remaining roadmap
(universal `.app`, signing/notarization, and the planned features).