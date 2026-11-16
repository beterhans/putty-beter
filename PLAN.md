# putty-beter — Development Plan

Status: planning. No feature code written yet.
Base: vanilla **PuTTY 0.85** source, MIT-licensed, fresh `git init` (no commits).

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

## 5. Phase 2 — O1: Session records (folders / comments / password)

**Goal:** organise saved sessions in folders, add comments, optionally save passwords.

**How sessions work today**
- Interface: `storage.h` (flat key/value).
- Unix impl: `unix/storage.c` — files in `~/.config/putty/sessions/`, name→filename with `%XX` encoding. Flat, no folders.
- Windows impl: `windows/storage.c` (registry) — needs separate handling; may be skipped for a macOS-first release.
- Session panel definition: `config.c` (~line 1904, "Session"/"savedsessions").
- GTK UI implementation: `unix/dialog.c` (4424 lines).

**Tasks**
1. **Comments** (cheapest): add a `SessionComment` key; store/read via existing storage calls; show in the session list tooltip. Low risk.
2. **Folders:** allow `/` in session names → subdirectories under `sessions/`; update `make_session_filename`, `enum_settings_*` to recurse; render a tree in the GTK list.
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

- [ ] **M0** — native arm64 build passes (Phase 0)
- [ ] **M1** — universal signed `.app` runs on Intel + ARM (Phase 1, T1)
- [ ] **M2** — session folders + comments (Phase 2, O1 part 1)
- [ ] **M3** — session password via Keychain (Phase 2, O1 part 2)
- [ ] **M4** — ZMODEM download works over SSH (Phase 3, T2)
- [ ] **M5** — ZMODEM upload + cancel + serial (Phase 3, T2)
- [ ] **M6** — MCP: AI drives session, human-in-the-loop at prompts (Phase 4, O2)
- [ ] **M7** *(optional)* — native macOS UI evaluation (Phase 5, O3)
