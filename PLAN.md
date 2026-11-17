# putty-beter — Development Plan

Status: **in development.** Phases 0 and 1a are done and most of Phase 2 is
done: a native arm64 `.app` builds and bundles, with session folders,
per-session comments, a relocatable configuration folder and a reworked
configuration box. The universal (Intel + ARM) `.app`, ZMODEM and MCP are still
to come — see the milestones at the end.

Base: vanilla **PuTTY 0.85** source, MIT-licensed.
Repo: <https://github.com/beterhans/putty-beter>

---

## 1. Goals

**Targets (must-have)**

| # | Goal | Difficulty | Notes |
|---|---|---|---|
| T1 | Support macOS **Intel + ARM** | **2** basic / **5** shippable signed universal `.app` | Pure C, arch-agnostic. Real cost is GTK deps + bundling + signing |
| T2 | Support **ZMODEM** (`sz`/`rz`) file transfer | **7** | No support today; needs protocol engine + terminal stream interception |

**Options (nice-to-have)**

| # | Goal | Difficulty | Notes |
|---|---|---|---|
| O1 | Session records: **folders**, **saved password**, **comments** | **4** | Password storage is the only risky part |
| O2 | **MCP for AI** — AI drives the same session window; pause at password for human | **7** in-process / **3** on tmux | `prompts_t` machinery already exists |
| O3 | **Rust rewrite** with native macOS UI | **9** full parity / **5** MVP | Separate product, not a fork |

**Decision:** keep the PuTTY C core and layer features on top. **O3 is deferred** — a rewrite
throws away 25 years of SSH/crypto/terminal correctness and invalidates T2/O1/O2 work.

**Ordering rationale:** order by "how much of the base is kept" and "cheap wins first".
Each phase adds to the previous; none invalidates another. Expensive/risky items (ZMODEM,
MCP IPC) come after cheap wins (universal build, sessions) so there is always a working build.

---

## 2. Environment

**Toolchain (verified on this machine, arm64 macOS)**

| Tool | Version | Status |
|---|---|---|
| cmake | 4.5.0 | OK |
| ninja | 1.13.2 | OK |
| pkg-config | 3.0.10 (pkgconf) | OK |
| gtk+-3.0 | 3.24.52 | OK |
| halibut | 1.3 | OK (`brew install halibut`) |
| gtk-mac-bundler | GNOME gitlab `fbc6ffd` | `tools/gtk-mac-bundler` (in-project; git-ignored, GPL-2.0, see `tools/README.md`). Override with `$BUNDLER_DIR` |
| perl | 5.34.1 | OK (needed by `licence.pl` for docs) |
| git | 2.54.0 | OK |

**Runtime for later phases**

| Runtime | Version | Planned use |
|---|---|---|
| system python3 | 3.9.6 | gtk-mac-bundler (if it works) |
| uv python | 3.12 (`~/.local/bin/python3.12`) | MCP server option B (Python) |
| node | v26.11.1 | MCP server option A (TypeScript) |
| npm | 11.20.0 | MCP SDK install |

**Platform model in the source**

- `cmake/platforms/unix.cmake` and `cmake/platforms/windows.cmake` — only two targets.
- macOS uses the **unix/GTK** build. No native Cocoa UI exists.
- macOS `.app` = `unix/putty.bundle` + `unix/putty.plist` + `unix/osxlaunch.c`, built with `gtk-mac-bundler`.
- ARM CPU feature detection already present: `unix/utils/arm_arch_queries.c`.

---

## 3. Phase 0 — Build baseline  *(do first)*

**Goal:** reproducible native arm64 build, then a scripted build entry point.

**Tasks**
1. `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build`
2. Fix any macOS compile warnings/errors that surface.
3. Add `scripts/build-macos.sh` (debug switch, `-j`, native vs universal flag).
4. Add a `CMakePresets.json` with `native` and `universal` presets.
5. Confirm binaries run: `putty`, `plink`, `pscp`, `psftp`, `puttygen`, `pterm`, `psusan`.

**Files:** new `CMakePresets.json`, new `scripts/build-macos.sh` (no core edits expected).

**Verification:** all binaries present in `build/`; `./build/putty --version`; `./build/plink --help`.

**Risk:** low. Only surprise is GTK/X11 feature detection on macOS.

---

## 4. Phase 1 — T1: macOS Intel + ARM

**Goal:** a `.app` that runs natively on both Apple Silicon and Intel Macs.

**Status (2026-11-17)**

- **1a — done.** `scripts/build-macos.sh arm64` then
  `scripts/bundle-macos.sh arm64` produce a self-contained `PuTTY.app` that
  launches and runs. All standalone tests and cryptsuite pass.
- **1b — not started.** The x86_64 build. Needs Homebrew under `/usr/local`
  (through Rosetta), so that the GTK dependencies are x86_64 as well.
- **1c — not started.** `scripts/make-universal.sh`: `lipo -create` every
  Mach-O in the two bundles into one, then re-sign.
- **Signing / notarisation — not started.** Needs an Apple Developer account, a
  hardened runtime and `notarytool`.

**Reality check (researched 2026-11-16)**
- **Rosetta 2 is going away.** macOS 27 (this machine) is the *last* release with Rosetta;
  from **macOS 28** it only works for a few legacy games. macOS 26 (Tahoe) was the last
  release supporting Intel Macs. → **We must ship a native arm64 build.** Shipping x86_64
  only and relying on Rosetta is a dead end.
- Intel Macs on macOS ≤27 still need an x86_64 slice, so a universal `.app` remains worthwhile.
- **A universal *binary* needs universal *dependencies*.** Homebrew GTK is single-arch, so
  `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"` alone fails at link.

**Toolkit question: should we drop GTK? No.**
- Qt / wxWidgets / SDL from Homebrew are **also single-arch** → same problem, plus heavier
  (and Qt is LGPL/commercial). Switching toolkit buys nothing.
- The only toolkit that is universal *for free* is **Apple AppKit/Cocoa** (system frameworks
  ship universal). PuTTY has no Cocoa frontend → that is Phase 5 (O3), a rewrite.
- **Decision: stay on GTK3**; solve universality at the build/bundle layer.

**Options for universal GTK**

| # | Approach | Effort | Verdict |
|---|---|---|---|
| A | Two per-arch builds → two `.app`s (arm64 + x86_64) | Medium | Fallback |
| **B** | Two per-arch builds → **`lipo` every Mach-O in the bundle** → **one universal `.app`** | High, fragile | ✅ **CHOSEN** |
| C | Build GTK + ~20 deps from source, twice, then lipo (gtk-osx/jhbuild) | Very high | Only if B fails |
| D | MacPorts `gtk3 +universal` | — | ❌ Broken (defect #69686) |
| E | Drop GTK for a native frontend | Rewrite | ❌ That is Phase 5 |

**DECISION: option B (1c) — build both arches, bundle each, `lipo`-merge into one universal `.app`.**

**Why B is workable here (verified in the source)**
- `unix/osxlaunch.c` is **fully bundle-relative at runtime**: it derives
  `Contents`/`Contents/Resources` from its own executable path and sets `GTK_PATH`,
  `GTK_DATA_PREFIX`, `GTK_EXE_PREFIX`, `PANGO_LIBDIR`, `XDG_*` relative to the bundle.
- `DYLD_LIBRARY_PATH` is **commented out** in favour of `install_name_tool` rewriting
  (`<run-install-name-tool/>` in `unix/putty.bundle`), so all dylib references are
  bundle-relative, not absolute.
- ⇒ the arm64 and x86_64 bundles have **byte-for-byte identical directory structure** with
  matching relative paths → `lipo -create` pairs up cleanly, no path surgery needed.
- Note: PuTTY 0.85 has **no Makefile** (CMake replaced it), so the bundling driver that used
  to set `PUTTY_GTK_PREFIX_FROM_MAKEFILE` is gone → **we must write our own bundling script**.

**Bundle layout to merge** (per arch, identical relative paths)
- `Contents/MacOS/PuTTY` — the `osxlaunch` launcher (Mach-O)
- `Contents/MacOS/PuTTY-bin` — the real GTK app (`puttyapp`) (Mach-O)
- `Contents/Resources/lib/*.dylib` — GTK + glib/pango/cairo/gdk-pixbuf/harfbuzz/libepoxy…
- `Contents/Resources/lib/gtk-3.0/**/immodules/*.so`, `printbackends/*.so`
- `Contents/Resources/lib/gdk-pixbuf-2.0/**/loaders/*.so`
- `Contents/Resources/{etc,share}/**` — themes, locale, mime data

**Tasks**
1. **1a — native arm64 build + bundle** using `/opt/homebrew` GTK. *The future-proof deliverable.*
   ```bash
   cmake -B build-arm64 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64
   cmake --build build-arm64 -j"$(sysctl -n hw.ncpu)"
   ```
2. **1b — x86_64 build + bundle** via a second Homebrew under Rosetta at `/usr/local`
   (does not touch the arm64 one):
   ```bash
   arch -x86_64 /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
   arch -x86_64 /usr/local/bin/brew install gtk+3 cmake ninja pkgconf
   arch -x86_64 env PATH=/usr/local/bin:$PATH \
     PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:/usr/local/opt/gtk+3/lib/pkgconfig \
     cmake -B build-x86_64 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=x86_64
   arch -x86_64 env PATH=/usr/local/bin:$PATH cmake --build build-x86_64 -j"$(sysctl -n hw.ncpu)"
   ```
3. **Bundling driver** — write `scripts/bundle-macos.sh` (new file): builds `puttyapp`, builds
   `osxlaunch`, runs `gtk-mac-bundler` with `unix/putty.bundle`, setting
   `PUTTY_GTK_PREFIX_FROM_MAKEFILE` from `pkg-config --variable=prefix gtk+-3.0`. Run once per
   arch against the matching prefix. Needs Python (system 3.9.6 may be too old → use `uv` 3.12).
4. **`scripts/make-universal.sh`** (new file) — walk both bundles, `lipo -create` every pair of
   Mach-O files at matching relative paths into a third `universal/PuTTY.app`; copy non-Mach-O
   files through; fail loudly on any file present in only one bundle.
   ```bash
   # sketch
   for f in $(cd arm64/PuTTY.app && find . -type f); do
     if file "arm64/PuTTY.app/$f" | grep -q 'Mach-O'; then
       lipo -create "arm64/PuTTY.app/$f" "x86_64/PuTTY.app/$f" -output "universal/PuTTY.app/$f"
     else
       cp "arm64/PuTTY.app/$f" "universal/PuTTY.app/$f"
     fi
   done
   ```
5. **Re-sign inside-out** after lipo (inner dylibs/plugins first, then the bundle):
   `codesign --force --sign <id> …`; verify with `codesign --verify --deep --strict`.
6. Set real version strings in `unix/putty.plist` (currently "Unidentified build").
7. macOS nits: Retina/HiDPI, `NSHighResolutionCapable`, menu bar, app icon (`icons/PuTTY.icns`).
8. Notarization for distribution (`notarytool`).
9. **Verify:** `lipo -archs` on *every* Mach-O in the bundle reports `x86_64 arm64`; launch on
   arm64 natively; verify x86_64 on Intel hardware or an x86_64 VM (**not** Rosetta — it is
   on borrowed time).

**Risks specific to 1c**
- Some bundled files may exist in only one arch's bundle (e.g. arch-specific loader paths) →
  the script must detect and handle this, not silently drop.
- Signing after lipo is mandatory; an unsigned or partially-signed universal bundle is rejected
  by Gatekeeper.
- `gtk-mac-bundler` is unmaintained and its HEAD targets Python 3.14 → likely need `uv` python
  or a small patch.
- Debugging a merged bundle is harder than two separate ones → keep the per-arch bundles as
  build artefacts for bisecting.

**Files:** `unix/putty.bundle`, `unix/putty.plist`, `unix/osxlaunch.c`, `icons/`, new build scripts.

**Verification:** `lipo -info` shows both arches; app launches on both; Gatekeeper accepts it.

**Risks / gotchas**
- `gtk-mac-bundler` is effectively unmaintained; HEAD targets Python 3.14 (system python is 3.9.6) → may need `uv` python or patching.
- Notarization requires an Apple Developer account + hardened runtime.
- GTK3 on macOS looks dated; a "native" look is **not** achievable without O3.

---

## 4b. macOS clipboard & drag-and-drop  *(done 2026-11-16)*

**Goal:** make copy/paste work with other macOS apps, default the clipboard UI
actions to the system clipboard, and support dragging a file from the Finder.

**Root cause of "can't copy/paste" (corrected):** the CMake build **never defines
`OSX_GTK`** — that was a leftover from the old Makefile-based build. `unix/platform.h`
contains *two* separate `#ifdef OSX_GTK` blocks, so on macOS all the macOS-specific
tweaks were silently disabled and the generic X11 code paths were compiled instead:

| Tweak (all off) | Consequence |
|---|---|
| `CLIPUI_DEFAULT_*` from the `#else` branch | Ctrl+Ins/Shift+Ins and mouse paste aimed at the **X11 PRIMARY selection**, which does not exist on macOS |
| `OSX_META_KEY_CONFIG` | No Command-key pass-through, so the terminal ate Cmd+C/Cmd+V and the menu accelerators never fired |
| `JUST_USE_GTK_CLIPBOARD_UTF8` | Used the low-level `gdk_selection_*` code, which the source itself says fails on macOS |

**Fix:** both blocks now test `#if defined(OSX_GTK) || defined(__APPLE__)`.
Verified: `conf: mousepaste=2 ctrlshiftins=2 ctrlshiftcv=2`. Cmd+C/Cmd+V confirmed working.

This is an **upstream PuTTY 0.85 defect** (affects every macOS CMake build) and is worth
reporting upstream. It also revealed a latent bug in `clipboard_text_received` that had
never been compiled: `length` was undeclared, fixed to
`dup_mb_to_wc_c(CS_UTF8, text, strlen(text), &paste_len)`.

**Cursor "one cell left" — resolved.** The frontend was proven correct: a window capture
showed `draw_cursor: cell x=24 -> pixel x=169` rendering the block at exactly x=338
(device px). After the `OSX_GTK` fix the same prompt also renders the cursor at x=338,
with a 3px gap after the last glyph (previously x=324, exactly one cell left).
The precise mechanism linking the two is not fully explained by the diff; the
`PUTTY_DIAG` tracing added during the investigation remains available if it recurs.

**Changes**

| File | Change |
|---|---|
| `unix/platform.h` | OSX_GTK: `CLIPUI_DEFAULT_MOUSE` → `CLIPUI_EXPLICIT`; added `CLIPUI_DEFAULT_CV = CLIPUI_EXPLICIT`. Other unix / Windows keep `CLIPUI_DEFAULT_CV = CLIPUI_NONE` |
| `settings.c` | `CtrlShiftCV` default uses `CLIPUI_DEFAULT_CV` instead of a hardcoded `CLIPUI_NONE` |
| `unix/window.c` | New `drag_data_received()` handler + `gtk_drag_dest_set()` on the drawing area + signal connection: `text/uri-list` → `g_filename_from_uri` → space-joined paths → `term_do_paste()` |
| `scripts/bundle-macos.sh` | Regenerate `gdk-pixbuf` `loaders.cache` after the reference fix (gtk-mac-bundler wrote it too early, leaving it empty) |

**Notes**
- `JUST_USE_GTK_CLIPBOARD_UTF8` is already defined for unix, so the GTK clipboard
  path (which maps to the macOS pasteboard) is in use.
- Cmd+C / Cmd+V work in `puttyapp` (the bundle) via `main-gtk-application.c`
  accelerators. The plain `putty` binary (`main-gtk-simple.c`) has **no menu**.
- GTK3 on macOS reports Command as `GDK_MOD2_MASK` (GTK4 uses `GDK_META_MASK`),
  matching PuTTY's `system_mod_mask` check.
- Backups: `unix/platform.h-01.old`, `settings.c-01.old`,
  `windows/platform.h-01.old`, `unix/window.c-01.old`.

**Not done (not requested):** copy-on-select (`CLIPUI_DEFAULT_AUTOCOPY true`) and
`MOUSE_SELECT_CLIPBOARD = CLIP_CLIPBOARD` on macOS.

---

## 4c. Relocatable configuration folder  *(done 2026-11-17)*

**Goal:** let the user choose where PuTTY-beter keeps its configuration instead
of always using `~/.putty`.

**Base location** (`unix/storage.c`, `make_filename(INDEX_DIR)`), unchanged from
upstream, in priority order:

1. `$PUTTYDIR`
2. `$XDG_CONFIG_HOME/putty` or `~/.config/putty` — **only if already present**
3. `~/.putty`  ← what this machine uses
4. home-from-`getpwuid()`/.putty, then `/.putty`

**How it works**

| Piece | Where |
|---|---|
| Redirect file | `<base>/config-location` — comments allowed, first other line is the absolute path |
| Reads it | `unix/storage.c`, so `putty`, `plink`, `pscp`, `psftp`, `puttygen` all follow it; a missing or invalid target is ignored with a debug trace |
| Writes it | `char *config_location_set(const char *dir)` — normalises `~`/relative paths, creates the target, **moves** `sessions/`, `sshhostkeys`, `sshhostkeys.tmp`, `sshhostcas/`, `randomseed`, then writes the file at the base |
| Dialog | `unix/config-location.c`, GTK only, called from `main-gtk-simple.c` and `main-gtk-application.c` |

**When the dialog appears:** the configuration is missing (no `sessions`
subdirectory), **or** Option/Alt is held at launch (`CGEventSourceFlagsState`,
via `-framework ApplicationServices`), **or** `PUTTY_CONFIG_RELOCATE=1`.

Default proposal: `$HOME/Documents/putty-beter-config`.

**Safety rules baked in**
- Nothing is deleted unless it was copied successfully first (cross-filesystem
  `EXDEV` fallback: recursive copy, then remove).
- An item that already exists in the destination is **left alone**; the old copy
  stays where it was. No merging, no overwriting.
- Cancelling the dialog records the current folder as configured, so the dialog
  really is one-shot.
- Choosing the base location again removes the redirect file.

**Debug:** `PUTTY_CONFIG_DEBUG=1` traces base/effective/configured/forced/moves.

**Verified:** redirect read (valid / broken / configured), move within a volume,
move across volumes, never-overwrite, reset-to-base, dialog rendering (normal and
forced), and that a real `~/.putty` installation is left alone.

**Not verified:** the physical Option-key test (needs a human key press).

---

## 4d. Changed default settings  *(done 2026-11-17)*

Defaults live in `conf.h` (`CONF_OPTION` / `DEFAULT_*`), so these are one-line
changes; the tunnel form default is in `config.c`, the log file name in
`unix/window.c`.

| Key | Upstream | Now | Where |
|---|---|---|---|
| `LogType` | `LGTYP_NONE` | `LGTYP_ASCII` (Printable output) | `conf.h` |
| `LogFileClash` | `LGXF_ASK` | `LGXF_APN` (always append) | `conf.h` |
| `LogFileName` | `putty.log` | `<config>/logs/&Y-&M-&D_&T-&H.txt` | `unix/window.c:platform_default_filename` |
| `BackspaceIsDelete` | `true` (^?) | `false` (^H) | `conf.h` |
| `LinuxFunctionKeys` | `FUNKY_TILDE` | `FUNKY_VT100P` | `conf.h` |
| `TermWidth` / `TermHeight` | 80 / 24 | 128 / 36 | `conf.h` |
| `PingInterval` | 0 | 120 s | `conf.h` |
| `AddressFamily` | `ADDRTYPE_UNSPEC` | `ADDRTYPE_IPV4` | `conf.h` |
| tunnel form IP version | radio index 0 (Auto) | index 1 (IPv4) | `config.c:portfwd_handler` |

**Log file name ordering matters.** `conf_new()` captures the `LogFileName`
default, so `config_location_init()` must run **before** it — hence the call was
moved to just after `gtk_init()`, before `conf = conf_new()`. The `logs`
directory is created by `config_location_init()` (only when `saved_sessions`),
because PuTTY does not create missing directories for log files.

**pterm** keeps `putty.log`: `platform_default_filename` branches on
`saved_sessions`.

**Tests:** `test/test_conf.c` encodes the upstream defaults, so its expected
values were updated for the seven changed keys (`test_conf` passes).

**Verified:** `2026-11-17_010935-127.0.0.1.txt` written into
`<config>/logs/` by a `-raw` session, whole content logged; `default log file:`
trace under `PUTTY_CONFIG_DEBUG=1`.

**Caveat:** a saved session that already stores these keys wins over the
defaults — `MacMini2018.local` stores all of them (`AddressFamily=0`,
`LogType=0`, `LogFileClash=-1`, `PingInterval=0`, `TermHeight=40`, ...).
Remove those keys from the session file, or Load → adjust → Save in the GUI.

---

## 4f. Configuration box layout and size  *(done 2026-11-17)*

**Goal:** the box was unusable with a long session list — a fixed 7-row listbox
with a gap, and a window that could not usefully be made bigger.

**Changes**

| Where | What |
|---|---|
| `config.c` | "Close window on exit" moved above the saved-session list; the saved-sessions controlset is marked `vexpand`; the listbox is its `vexpand` child (7 rows = minimum); the column split is 80/20; the Load/Save/Delete buttons are marked `force_left` |
| `dialog.h`, `dialog.c` | `vexpand` on `struct controlset`; `vexpand` + `force_left` on `struct dlgcontrol`; initialised in `ctrl_getset()` / `ctrl_new()` |
| `unix/dialog.c` | pack a vexpand controlset with `expand`; `columns_vexpand` for its control; `columns_force_left_align` + `columns_content_last_col` for `force_left` controls; window resizable, size restored with `gtk_window_set_default_size()`, saved on `destroy` |
| `unix/columns.c/.h` | `columns_content_last_col()`: last column of each block is exactly as wide as its widest left-aligned child, and its children fill it; guarded so other dialogs are untouched |
| `unix/storage.c` | `config_state_get_int()` / `config_state_set_int()` → `<config folder>/gui-state` (`width=`, `height=`) |

**Why the Columns change was needed:** marking the buttons `force_left` alone
made them natural width but the column kept its 20% share, leaving a gap
between the list and the buttons. `content_last_col` moves the column boundary
in to the buttons, so the list takes the rest.

**Verified:** headless `Columns` allocation test (a standalone GTK program
linking `libutils.a`), at widths 600/1000/2000 px — list width 525/925/1925,
buttons always 71 px and flush with the right edge; entry row aligned with the
list. Window size restore verified with a seeded `gui-state` (`1100x900` →
`[config-dialog] size 1100x900` in the trace). The size *save* runs on `destroy`
(Open/Cancel/⌘W); killing the process with SIGTERM does not run it.

**Notes**
- `pterm` is excluded from the size memory: it has no configuration folder, so
  both the restore and the save check `saved_sessions`.
- Mid-session "Change Settings" uses the same code, with only the Save button.

---

## 4e. Clipboard: Insert keys and dialog copy/paste  *(done 2026-11-17)*

**Symptom 1 — terminal:** `Ctrl+Ins` / `Shift+Ins` did nothing, while `Cmd+C` /
`Cmd+V` worked, even though the session had `CtrlShiftIns=explicit`.

**Cause:** `key_event()` tested `event->keyval == GDK_KEY_Insert`. macOS has no
PC Insert key; a PC keyboard's Insert arrives as virtual keycode 0x72
(`kVK_Help`), which GDK reports as **`GDK_KEY_Help`**. Apple keyboards with a
numeric keypad report the keypad key as `GDK_KEY_KP_Insert`. Neither matched.

**Fix:** new `is_insert_key()` in `unix/window.c` accepts any keyval whose GDK
name contains `Insert`, plus `GDK_KEY_Help` on macOS. Used by both the
Shift+Ins and Ctrl+Ins branches, so the configured clipboard action
(`CLIPUI_IMPLICIT/EXPLICIT/CUSTOM/NONE`) is honoured as before.

**Symptom 2 — dialog:** copy/paste did not work in the configuration box's
text fields.

**Cause:** GTK binds entry copy/paste to `GDK_CONTROL_MASK`; on macOS Command is
`GDK_MOD2_MASK`, and `GtkEntry` does not handle Command+C/V, so the key did
nothing.

**Fix:** `dialog_clipboard_key()` in `unix/dialog.c`, called from
`win_key_press()`, handles Ctrl/Cmd + C/V/X plus Ctrl+Ins/Shift+Ins on the
focused widget (`GtkEditable`, and selectable `GtkLabel` for copy).
`GDK_MOD2` is treated as Command **only** under `__APPLE__`, since elsewhere it
is usually Num Lock.

**Not verified:** the physical key presses (needs a human). If the terminal
side still fails, `PUTTY_DIAG=1` prints `key: keyval=0x.... (name) state=...`
for every keystroke — that names the key macOS actually delivers.

---

## 4g. Session folders  *(done 2026-11-17)*

**Goal:** organise saved sessions in folders, in a tree, with drag and drop.

Implemented (M2, part 1):

| Where | What |
|---|---|
| `unix/storage.c`, `storage.h` | `/` in a session name separates folder names, and folders are real directories; recursive enumeration reporting folders as `Name/`; `session_mkdir()`, `session_move()` (sessions and folders, refusing to move a folder into itself), `session_rmdir()` (empty only), `session_move_target_name()`, `session_name_is_valid()` |
| `dialog.h`, `dialog.c` | `listbox.tree` flag, `listbox.drag_handler` hook, `dlg_listbox_selected_id()` / `dlg_listbox_select_id()` |
| `unix/dialog.c` | `GtkTreeStore` (a tree store with top-level rows behaves exactly like the old list store), hierarchy built from `/`, folder flag in an extra undisplayed column, bold folder rows, expand as a folder gains children, drag source + drop target calling `drag_handler` |
| `config.c` | list filled with ids, selection by id, **New Folder** button, Delete = session or empty folder, Load on a folder = beep, drag handler |
| `unix/window.c` | the right-click saved-session menu skips folder entries |
| `windows/*` | flat fallback so that front end still builds |
| `test/test_sessions.c` | new `test_sessions` target |

**Verified:** `./build-arm64/test_sessions` — see the list in
`CHANGES-FROM-UPSTREAM.md` section 9. The tree was also checked visually in the
`.app` (folders bold, triangles, nesting, empty folder).

**Added after the first cut:** the session list is multi-select, so several
sessions can be dragged into a folder in one go and Delete removes everything
selected (asking once if the selection includes sessions inside a folder); and
`sessioncmp()` now sorts folders before sessions at every level, so folders
always sit at the top of their level rather than needing a manual order.

**Not done yet (rest of M2/M3):** renaming a folder, remembering which folders
are expanded, and saved passwords via the Keychain. (Per-session comments are
done — see section 4i.)

---

## 4h. App icon  *(done 2026-11-17)*

**Goal:** ship this fork's own icon rather than the upstream PuTTY one.

`icons/putty-beter-icon.png` is the source of truth. `scripts/make-icns.sh`
converts it to `icons/PuTTY.icns` with only what macOS provides — `sips` to
resize and `iconutil` to assemble the `.iconset` — and `bundle-macos.sh` runs
it whenever the PNG is newer than the `.icns`. `unix/putty.bundle` already
copied `icons/PuTTY.icns` into `Contents/Resources` and `unix/putty.plist`
already named it in `CFBundleIconFile`, so no bundle changes were needed.

The upstream icon set (`icons/mkicon.py`, `make -C icons PuTTY.icns`) is kept
and still used as a fallback if the PNG is missing. `.gitignore` needs an
exception for the PNG, since `/icons/*.png` is otherwise ignored.

**Note:** `gtk_widget_show()` does not recurse in GTK 3. Every widget has to be
shown itself; relying on showing the parent gives an invisible child.

---

## 4i. Session comments  *(done 2026-11-17)*

**Goal:** a free-text note kept with each saved session.

| Where | What |
|---|---|
| `conf.h` | `session_comment`, `VALUE_TYPE(STR)`, `LOAD_CUSTOM, SAVE_CUSTOM` |
| `settings.c` | saved by hand under `SessionComment`, and only when non-empty; backslash, newline and CR escaped as `\\`, `\n` and `\r` |
| `dialog.h`, `dialog.c` | `editbox.multiline` flag |
| `unix/dialog.c` | a multi-line box is a `GtkTextView` in a scrolled window: word-char wrap, `accepts-tab` off, eight-line minimum, `vexpand`; changes reported by the text buffer, Return inserts a newline |
| `config.c` | the `Session/Comment` panel, directly after `Session/Logging` |
| `test/test_sessions.c` | round-trip test over six comments |

**Two things worth remembering.** A `LOAD_CUSTOM` option is skipped by the
automatic loop in `load_open_settings()`, so its default has to be set by hand —
a `Conf` entry has to exist before `conf_get_str()` can be asked for it. And the
text view needed its own `gtk_widget_show()`, because showing only the
surrounding scrolled window left it invisible: an empty sunken frame that could
not be typed into, since an invisible widget never receives the click that
would focus it.

**Verified:** the storage round trip is tested. The widget needs a human.

---

## 5. Phase 2 — O1: Session records (folders / comments / password)

**Goal:** organise saved sessions in folders, add comments, optionally save passwords.

**How sessions work today**
- Interface: `storage.h` (flat key/value).
- Unix impl: `unix/storage.c` — files in `~/.config/putty/sessions/`, name→filename with `%XX` encoding. Flat, no folders.
- Windows impl: `windows/storage.c` (registry) — needs separate handling; may be skipped for a macOS-first release.
- Session panel definition: `config.c` (~line 1904, "Session"/"savedsessions").
- GTK UI implementation: `unix/dialog.c` (4424 lines).

**Status:** folders and comments are done (sections 4g and 4i); passwords are
not started.

**Tasks**
1. ~~**Comments** (cheapest): add a `SessionComment` key; store/read via existing storage calls; show in the session list tooltip. Low risk.~~ **Done** — section 4i. (It ended up in a panel of its own rather than a tooltip, because a note can be a paragraph.) Showing it as a tooltip on the session list is still an obvious small win.
2. ~~**Folders:** allow `/` in session names → subdirectories under `sessions/`; update `make_session_filename`, `enum_settings_*` to recurse; render a tree in the GTK list.~~ **Done** — section 4g.
3. **Password** (do last, design first):
   - Never store plaintext in the session file.
   - macOS: **Keychain** (via Security framework or `security` CLI), keyed by session name.
   - Linux: libsecret, else an encrypted file with a master passphrase.
   - Add a Conf flag ("use stored credential") + a "Forget" action.
4. Handle rename/delete/duplicate across the folder tree.

**Files:** `storage.h`, `unix/storage.c`, `windows/storage.c` (if in scope), `config.c`, `unix/dialog.c`, `settings.c`.

**Verification:** create nested sessions, reload, verify tree; confirm password never appears
in plaintext on disk; confirm delete removes Keychain entry.

**Risks:** password security design (medium); cross-platform storage divergence (medium);
upstream-merge friction in `dialog.c` (medium).

---

## 6. Phase 3 — T2: ZMODEM (`sz`/`rz`)

**Goal:** `sz` on the remote host downloads a file; `rz` uploads one — over SSH, serial, telnet.

**Why it's hard:** PuTTY has **zero** ZMODEM support (verified: no `zmodem`/`zrqinit`/`lrzsz`
references anywhere). The terminal must detect the transfer start, suspend normal display
passthrough, run the protocol bidirectionally over the channel, then resume.

**Hook points**
- `term_data()` (~`terminal/terminal.c:7876`) — data *from* the server.
- `term_keyinput()` / `term_keyinputw()` — data *to* the server.
- `ldisc.c` — line discipline / local echo interaction.
- Backends: `ssh/` (SSH channel), `unix/serial.c`, `proxy/telnet.c`.

**Design sketch**
1. Watch the incoming stream for the ZMODEM init header `**\x18B00` (ZRQINIT, hex header).
2. On detection, raise a "transfer mode" flag: stop feeding the terminal emulator, feed the
   protocol engine instead; route user keystrokes (Ctrl-C to cancel) to the engine.
3. Implement the handshake/state machine: `ZRQINIT → ZRINIT → ZFILE → ZDATA → ZEOF → ZFIN`
   with CRC32 (`ZCRCW`/`ZCRCG`/`ZCRCQ`), 16/32-bit CRC negotiation, escape handling.
4. Progress UI (GTK dialog: filename, bytes, ETA, cancel).
5. File-save/upload dialogs; wire into existing `Filename`/storage helpers.

**Licensing — read this first**  *(decide before coding)*

PuTTY is **MIT**. MIT can absorb *permissive* code, so the ZMODEM component must be permissive.

| License of ZMODEM code | Usable with MIT PuTTY? | Obligations |
|---|---|---|
| Public domain / CC0 / Unlicense | ✅ Best | None |
| MIT | ✅ Best | Keep copyright notice |
| BSD-2/3, ISC, zlib | ✅ Fine | Keep notice |
| Apache-2.0 | ✅ Fine | Keep notice + NOTICE + patent grant |
| **GPL-2.0 / 3.0** | ❌ No | Relicenses the whole binary as GPL |
| LGPL | ⚠️ Awkward | Static C linking needs relinkable objects; upstream won't accept |
| AGPL | ❌ No | Worse than GPL |

**Key fact:** the **original** ZMODEM spec and Forsberg's Unix `rz`/`sz` source are
**public domain** ("No licensing, trademark, or copyright restrictions apply to the use of
the protocol, the Unix rz/sz source code and the ZMODEM name."). `lrzsz` is GPL-2.0 **only
because of Uwe Ohse's modifications** — its own README says it is built from the
public-domain rzsz package.

**Candidates**

| Source | License | Verdict |
|---|---|---|
| Forsberg original `rz.c`/`sz.c`/`zm.c` | **Public domain** | ✅ Cleanest — derive, license our version MIT |
| NuttX `apps/system/zmodem` | Apache-2.0 (C) | ✅ Usable + NOTICE, but RTOS APIs → needs porting |
| `roscopeco/mbzm` | "MIT" **but CRC code is GPL** | ❌ Contaminated — avoid / replace CRC |
| `trzsz-go` | MIT | ⚠️ Go, not linkable; protocol reference only (tmux-friendly) |
| `lrzsz` | GPL-2.0 | ❌ Relicenses the whole project |

**Decision: implement in-tree (`terminal/zmodem.c`) from the public-domain spec/original
source, licensed MIT.** No licence change, no dependency, and it is the *only* option that
stays upstream-compatible (upstream accepts MIT contributions only).
Fallback: port NuttX (Apache-2.0) with a `NOTICE`.

**Files:** new `terminal/zmodem.c` + `terminal/zmodem.h` (or `zmodem/`), edits to
`terminal/terminal.c`, `ldisc.c`, GTK dialogs in `unix/dialog.c`, CMakeLists.

**Verification:** `sz`/`rz` against a Linux host over SSH (both directions), binary integrity
(checksum), large file, cancel mid-transfer, serial line, and `rz` after `sz` abort.

**Risks:** protocol correctness (high), licensing (high — decide first), flow control over SSH
(medium), interaction with local echo/line editing (medium), scope creep into `zssh`-style
menu-driven mode (avoid).

---

## 7. Phase 4 — O2: MCP for AI

**Goal:** an AI agent shares the *same* session window; when a password prompt appears the AI
pauses, the human types into the GUI, and the AI continues.

**Why it's cheaper than it looks**
- "Stop at a prompt, human types, continue" **already exists**: `prompts_t` +
  `term_userpass_state` in `terminal/terminal.c`.
- Cross-process IPC **already exists**: `ssh/sharing.c` (2176 lines) + `unix/sharing.c` (370)
  implement upstream/downstream sharing over a socket with a shared secret and protocol version.
  Reuse this handshake/transport pattern.

**Design sketch**
1. **PuTTY side:** a local IPC server (Unix domain socket) exposing:
   `list_sessions`, `open_session`, `read_screen` (serialize the `term` screen + scrollback),
   `send_input`, `send_key`, `wait_for_prompt`, and an event stream of screen changes.
2. **MCP side:** a stdio MCP server (TypeScript on node 26, or Python on uv 3.12) that bridges
   MCP ↔ the IPC socket. Expose tools like `putty_open`, `putty_read`, `putty_write`,
   `putty_wait_idle`.
3. **Human-in-the-loop:** when the session hits a `prompts_t`, the IPC stream emits
   `prompt_pending`; the MCP tool returns "awaiting human input"; the human types in the GUI;
   the AI is notified and continues. Reuses the existing prompt machinery unchanged.

**Files:** new `unix/ipc.c` / `ipc.h` (or `unix/mcp-bridge.c`), edits to `terminal/terminal.c`
(screen serialization + prompt events), `CMakeLists.txt`; new `mcp/` directory for the server.

**Verification:** AI opens a session, runs a command, reads output; hits `sudo`/`ssh` password
prompt → human types → AI continues; screen state matches the GUI.

**Risks / security (important)**
- This hands shell access to an AI agent — needs explicit opt-in, localhost-only socket,
  file permissions `0600`, and a shared-secret handshake like `sharing.c`.
- Terminal-state serialization is fiddly (attributes, wrapping, scrollback).
- MCP spec/SDK churn.

---

## 8. Phase 5 — O3: Rust rewrite *(deferred / separate product)*

**Recommendation: do not do this as a fork.** It is a new product, not an evolution.

**Why:** PuTTY is ~200k lines covering SSH (all key types, agent, GSSAPI), crypto, terminal
emulation (Unicode, bidi), key formats, config, and a 25-year compatibility surface. A Rust
rewrite must re-earn all of it.

**If a native macOS UI is a hard requirement**, the pragmatic path is a *new frontend reusing
crates*, not a port of PuTTY:
- SSH: `russh` (or `libssh2` bindings)
- Terminal emulation: `alacritty_terminal` / `vte`
- UI: `objc2`/`cacao` for true native AppKit, or `egui`/`iced`/`Tauri` for faster iteration
- Sessions: new format, or a bridge reading PuTTY's `~/.config/putty/sessions`

**Effort:** MVP "good enough terminal" ≈ difficulty 5; parity ≈ 9.

---

## 9. Cross-cutting risks

| Risk | Impact | Mitigation |
|---|---|---|
| `lrzsz` GPL vs PuTTY MIT | Licence change of whole project | Implement ZMODEM in-tree, or find permissive impl — decide before coding |
| Universal GTK unavailable from Homebrew | Blocks a true universal `.app` | gtk-osx/jhbuild universal GTK, or ship per-arch apps |
| `gtk-mac-bundler` unmaintained (wants Python 3.14) | Phase 1 bundling friction | Use `uv` python 3.12, or patch the bundler |
| PuTTY 0.85 has no Makefile → bundling driver missing | Must write our own bundle script | Reconstruct from `putty.bundle` + `pkg-config --variable=prefix` |
| Universal `.app` needs *every* bundled Mach-O lipo'd | One missed file = broken bundle | Script walks the bundle; verify `lipo -archs` on all; keep per-arch bundles for bisecting |
| Code signing / notarization | Gatekeeper blocks distribution | Apple Developer account + hardened runtime + `notarytool`; re-sign after lipo |
| Upstream divergence | Painful merges as PuTTY releases | Keep changes additive and isolated in new files where possible |
| MCP grants shell access to AI | Security | Opt-in, `0600` localhost socket, shared-secret handshake |
| GTK3 look on macOS | Doesn't feel native | Accept for T1; only O3 gives a native look |

---

## 10. Working conventions

- **Backups before editing:** create `<filename>-NN.old` (next sequential number, start at 01);
  stop and ask if any index is ≥ 30. Skipped in folders whose `AGENTS.md` opts out.
- **Log notes:** keep `_agent/YYYY-MM-DD_HHMM.md` with *Current Target / Key Points / Next Steps*;
  update on each completed sub-task or pause.
- **Debug switches:** every new script/feature gets a debug toggle with clear stdout/stderr logging.
- **Minimal diffs:** smallest change that solves the problem; follow existing C style.
- **Preserve behaviour:** do not delete lines when modifying code — comment them out instead
  (per user rule 2026-10-30); keep existing comments unless wrong or obsolete.
- **Temp space:** use `$HOME/Downloads/temp` for scratch/cache.

---

## 11. Milestones

- [x] **M0** — native arm64 build passes (Phase 0)
- [ ] **M1** — universal signed `.app` runs on Intel + ARM (Phase 1, T1) — 1a done, 1b/1c not started
- [x] **M2** — session folders + comments (Phase 2, O1 part 1)
- [ ] **M3** — session password via Keychain (Phase 2, O1 part 2)
- [ ] **M4** — ZMODEM download works over SSH (Phase 3, T2)
- [ ] **M5** — ZMODEM upload + cancel + serial (Phase 3, T2)
- [ ] **M6** — MCP: AI drives session, human-in-the-loop at prompts (Phase 4, O2)
- [ ] **M7** *(optional)* — native macOS UI evaluation (Phase 5, O3)
