# What this fork changes, compared with PuTTY 0.85

Baseline: the official PuTTY 0.85 source tarball,
<https://the.earth.li/~sgtatham/putty/0.85/putty-0.85.tar.gz>.

Everything listed here was written for this fork and is MIT-licensed under the
same terms as the rest of the tree (`LICENCE`). **No new external
dependencies**, and no GPL/LGPL code has been added: the one GPL-2.0 tool this
project uses, `gtk-mac-bundler`, is a build-time-only helper and is
deliberately not vendored (`tools/README.md`).

Everything not mentioned below is unmodified upstream PuTTY.

**Scope.** The narrative sections below describe the fork as it behaves on
**macOS**, which is where it was developed; that is what the feature-by-feature
accounts and their measurements are about. The **Windows port** is a difference
from upstream in its own right, and a large one: it is recorded in the
`windows/*` rows of *Modified upstream files* below, with the full file inventory
in [`tech_detail.md` Appendix A](tech_detail.md#appendix-a-what-the-windows-port-touches),
and its per-feature status in [`PLAN.md`](PLAN.md). A handful of the sentences
below name a macOS mechanism where the Windows front end has an equivalent of its
own — the readable exit's "GTK label below the terminal" is a status line across
the bottom of the terminal window on Windows, for instance — and `tech_detail.md`
is the place that says which is which.

---

## Reproducing this diff

```sh
curl -O https://the.earth.li/~sgtatham/putty/0.85/putty-0.85.tar.gz
tar xzf putty-0.85.tar.gz

# which upstream files differ, and what has been added
diff -rq putty-0.85 . -x '.git' -x 'build-*' -x '*.old' -x '_agent' -x 'issue' -x 'tools'

# a specific file
diff -u putty-0.85/unix/window.c unix/window.c
```

The first commit in this repository (`16c7c5b`) already contains the source
fixes, so `git log` alone shows only the later work.

---

## Summary

| Area | Change |
|---|---|
| macOS code paths | The CMake build never defined `OSX_GTK`, so every macOS-specific code path was silently compiled out. Now enabled, which is what makes clipboard support work at all. |
| Clipboard | Copy/paste in the terminal and in dialogs, with Command and with Ctrl+Ins/Shift+Ins, on any keyboard. |
| Keyboard input | IME pre-edit cursor offset fixed (upstream had an out-of-bounds read here). |
| Configuration folder | The configuration directory can be moved, with a one-time dialog and a redirect file. |
| Configuration box | Resizable, size remembered in the configuration folder, and the saved-session list grows with the window. |
| Session folders | Sessions can be kept in folders, shown as a tree and moved by dragging. |
| Session comments | A free-text note kept with each saved session, in a panel below Logging, with a multi-line UTF-8 box. |
| Session search | Type `/` in the Saved Sessions box and the list below is filtered, live, by session name, host name/IP or comment; folders still show as a tree. |
| Session files | A saved session is a `<name>.conf` file, and only `.conf` files are read as sessions, so Finder's `.DS_Store` and stray copies no longer turn up in the list. Folders are directories and have no extension. |
| Session passwords | A session can remember its login password — kept in the session file wrapped, injected into the password prompt so connecting does not ask, and offered for keeping after a login that succeeds. Named for both halves: the offer to remember covers the login name typed at the `login as:` prompt as well, so a session whose password is already saved stops asking for the username too. |
| ZMODEM | `sz`/`rz` file transfer in both directions, engine written in-tree from the public-domain spec, with a progress window and a working Cancel. |
| Transparent background | Window → Colours → "Transparency (0 = solid, 255 = see-through)": how much of the desktop shows through the background colour. Applied to the *default* background only, so text with a colour of its own — highlighted lines, the cursor — stays solid. **macOS only** — Windows has no good equivalent, so the feature was dropped there (section 15). |
| Window placement | A new session window opens on the monitor the configuration box (or the window a new session was asked for from) is on, instead of on whichever screen macOS picks; the primary one when there is nothing to inherit from. See section 16 — and note that `-geometry` is a no-op on macOS. |
| File chooser | One chooser for every file and folder setting, with a visible full-path box instead of GTK's Ctrl+L location bar. |
| Dialog title bars | GTK's `GtkDialog` windows get an opaque title bar; on macOS they were drawn see-through. |
| Defaults | Logging, function keys, Backspace, window size, keepalive and IP version defaults changed. |
| MCP | An AI agent can read a live session — list sessions, read their output, read the visible screen — type into it, and **run a command and wait for the device to finish** (`run`), including answering a `--More--` pager and stopping at a prompt that wants input. Text the *human* types is tagged `h` in the transcript. Off by default, per session, with reading and typing as separate opt-ins. **The bridge itself ships inside `PuTTY.app`**, with the note that says how to configure it as an MCP server (section 19). **The window's own prompts are reported** — an agent can see that PuTTY is asking the human for a password, and will not answer it (section 13). |
| PuTTYGen GUI | `puttygen` gains a **GTK front end** — `unix/puttygen.c`, a port of the Windows-only `windows/puttygen.c` — built as `puttygenapp` and shipped as `PuTTYGen.app` beside `PuTTY.app`. Generate, inspect, load, convert and save keys by clicking, on a worker thread with a working Cancel. The command-line `puttygen` is unchanged and is the oracle the GUI is tested against. |
| Version and identity | The fork's own version string — **0.85-beter.1** — in `version.h`, `LATEST.VER` and both `Info.plist`s, a clickable link to <https://github.com/beterhans/putty-beter> at the foot of every About box, and a second copyright line (`This fork: Copyright 2026 beterhans.`) below the preserved upstream notice. **Both front ends**: the Windows About box gained the same two lines and the same fork link, and `windows/putty.ico` is now the fork icon, built from the PNG by `scripts/make-ico.py`. See section 18. |
| Build tooling | `scripts/build-macos.sh`, `scripts/bundle-macos.sh` (now building **both** apps), `scripts/make-universal.sh` (universal apps + the `PuTTY-universal.dmg` installer), `scripts/make-icns.sh`, `scripts/test-puttygen-gui.sh`, `CMakePresets.json`, self-contained `PuTTY.app` and `PuTTYGen.app` with this fork's own icons, and GSettings schemas compiled into each bundle. |
| Diagnostics | `PUTTY_DIAG=1`, `PUTTY_CONFIG_DEBUG=1`, `PUTTY_MCP_DEBUG=1`, `PUTTY_SESS_SEARCH_DEBUG=1`, `PUTTY_SESSPASS_DEBUG=1` and `PUTTYGEN_DEBUG=1` runtime traces. |

---

## Modified upstream files

Line counts are `+added / -removed` against the 0.85 tarball, from
`git diff --no-index --numstat putty-0.85/<file> <file>`. They are a rough size
guide, not a contract: `git` and GNU `diff` group hunks differently, so another
tool will give slightly different numbers for the same change.

| File | +/- | What changed |
|---|---|---|
| `conf.h` | +137 / -7 | Seven default values changed (see below); `session_comment`, `session_password`, `session_name`, `zmodem_dir`, `bg_transparency`, `mcp_enabled` and `mcp_write` added |
| `config.c` | +660 / -42 | New tunnels in Connection → Tunnels preselect IPv4 instead of Auto; the Session panel reordered so the saved-session list is last and can grow; the Load/Save/Delete buttons keep their natural width; the Session/Comment panel added; the Session/File transfer panel added; the Session/Automation (MCP) panel added; the saved-session search in the Session panel (see below); the password box moved next to "Auto-login username" in Connection → Data; the Window/Colours transparency controlset; the saved-session buttons' column widened from 20% to 26% so "New Folder" is not clipped at high display scaling (only Windows notices — GTK sizes that column to its contents) |
| `dialog.h` | +83 / -0 | `vexpand` flag on `struct controlset`; `vexpand` / `force_left` flags on `struct dlgcontrol`; `multiline` and `placeholder` on the edit box; `for_directory` flag on the file selector (per front end; on Windows `vexpand` is still ignored, and `force_left` was ignored until W7 gave it `button_force_left()`) |
| `dialog.c` | +9 / -0 | Initialise those flags in `ctrl_getset()` / `ctrl_new()` / `ctrl_editbox()` / `ctrl_filesel()` |
| `settings.c` | +179 / -4 | `CtrlShiftCV` default comes from `CLIPUI_DEFAULT_CV` instead of a hardcoded `CLIPUI_NONE`, so it can differ per platform; `sessioncmp` sorts folders before sessions at every level; the session comment and the session password are saved and loaded by hand (the comment with newlines escaped, the password wrapped by `utils/sesspass.c`); `load_settings()` records which saved session the configuration came from |
| `terminal/terminal.c` | +179 / -2 | IME pre-edit cursor fix; `PUTTY_DIAG` tracing; `term_get_screen_text()`, the visible screen as plain UTF-8, for the MCP server; `term_set_input_tap()`, which reports what a human typed or pasted |
| `putty.h` | +29 / -0 | Declares `term_get_screen_text()` and `term_set_input_tap()`; `from_proxy` on `struct prompts_t`, so the SSH-to-SSH proxy's forwarded password prompt is not answered with the session's saved password |
| `test/test_conf.c` | +7 / -7 | Expected defaults updated to match the seven changed ones |
| `unix/CMakeLists.txt` | +25 / -1 | Build `unix/config-location.c`, `unix/mcp.c` and `unix/mcp-buffer.c` into `guiterminal`; link `ApplicationServices` on Apple; build the ZMODEM test targets; `no-gtk.c` moved out of `puttygen-common` into the CLI targets, and the `puttygenapp` target added (section 17) |
| `icons/Makefile` | +24 / -2 | `PuTTYgen.icns` added to `ICNS` and its rule (the `mkicon.py puttygen_icon` drawing + `macicon.py`), as the fallback for when `icons/puttygen-icon.png` is absent; `clean` no longer deletes the two checked-in icon sources |
| `unix/columns.c` | +57 / -1 | Optional "content-sized last column": a column of natural-width buttons no longer keeps its percentage of the width |
| `unix/columns.h` | +11 / -0 | The flag and the last-column width it needs |
| `unix/config-location.c` | new | See the new-files table |
| `unix/dialog.c` | +995 / -39 | `dialog_clipboard_key()`: Command/Ctrl + C/V/X/A and Ctrl+Ins/Shift+Ins in dialog text fields; resizable configuration box whose size is remembered in `gui-state`; `vexpand` / `force_left` support in the layout; tree list boxes with drag and drop; the multi-line edit box; the edit box's placeholder text; every file selector routed through the shared chooser; the configuration box reports its monitor, so a session opened from it opens on the same screen (section 16); the About box links to the fork's GitHub and shows the two labelled copyright lines (section 18) |
| `unix/gtkcompat.h` | +26 / -0 | `gtk_dialog_fix_macos_titlebar()` — gives a `GtkDialog` an opaque title bar on macOS |
| `unix/gtkmisc.h` | +21 / -0 | Declares `our_file_chooser_run()`, and the fork-identity macros for the About box: the homepage link and the fork's copyright holder (section 18) |
| `unix/main-gtk-application.c` | +9 / -0 | Call `config_location_init()` on application activation |
| `unix/main-gtk-simple.c` | +10 / -0 | Same, before `conf_new()` so the default log file name lands in the right folder |
| `unix/osxlaunch.c` | +17 / -0 | Set `GDK_PIXBUF_MODULE_FILE` and `GTK_IM_MODULE_FILE` to the bundled caches |
| `unix/platform.h` | +79 / -3 | `#ifdef OSX_GTK` → `#if defined(OSX_GTK) \|\| defined(__APPLE__)` in both blocks; macOS clipboard defaults; declarations for the configuration-folder and GUI-state APIs; the monitor a new session window should open on |
| `unix/putty.bundle` | +21 / -0 | Bundle gdk-pixbuf loaders (so `loaders.cache` points inside the app); bundle `icons/PuTTY.icns`; the Adwaita theme block is commented out (GTK ≥ 3.20 has it built in) |
| `unix/storage.c` | +1205 / -30 | Configuration-folder redirection, the `config_location_*` API, and the `gui-state` key=value store; session files are `.conf` (`INDEX_SESSIONFILE` beside `INDEX_SESSION`, which stays a folder), hidden entries are skipped, and a name beginning with `.` is refused; the redirect file is `config-location.conf` |
| `unix/unifont.c` | +1 / -0 | The font selector dialog also gets the opaque-title-bar fix |
| `unix/window.c` | +1597 / -28 | Default log file name; Finder drag-and-drop; `dup_mb_to_wc_c` fix; Command-key pass-through; `is_insert_key()` for Ctrl+Ins/Shift+Ins; `PUTTY_DIAG`; the ZMODEM front end (engine lifecycle, output divert, key routing, progress window); the MCP front end (session lifecycle, the stream tap, the screen callback, the human-input tap); the saved-session login details: the seat asks for the password after `-pw` declines, one attempt per connection, and offers to keep a freshly typed password *and* the login name typed at the `login as:` prompt after a login succeeds; the transparent background: RGBA visual, alpha on the default background colour, and a GTK-drawn title bar while the window is see-through; which monitor a new session window opens on (section 16) |
| `windows/platform.h` | +74 / -0 | `CLIPUI_DEFAULT_CV` is `CLIPUI_EXPLICIT` on Windows (the port's change; upstream leaves it `CLIPUI_NONE`), the folder-chooser, `config_state_*` and `request_file_in_dir()` declarations |
| `windows/controls.c` | +1099 / -13 | the dialog primitives the shared configuration box calls (`dlg_listbox_selected_id()`, `dlg_listbox_select_id()`, `dlg_confirm`), and everything the Windows port has since added to them: the multi-selection fix, the extended-selection style, the `EM_SETCUEBANNER` placeholder, the multi-line comment box, and the saved-session list's own drag-and-drop and folder tree |
| `windows/window.c` | +1383 / -20 | the MCP front end, the saved-session login details, the readable exit's close countdown, and the ZMODEM front end (§11) |
| `windows/dialog.c` | +453 / -43 | the resizable configuration box, and the box size remembered in `gui-state` |
| `windows/storage.c` | +61 / -0 | **not built** — superseded by `unix/storage.c`; its folder operations return "not supported" and are dead code now |
| `windows/*` (the rest) | see **`tech_detail.md` Appendix A** | `utils/defaults.c`, `utils/request_file.c`, `win-gui-seat.h`, `putty-common.rc2`, `CMakeLists.txt`, `utils/split_into_argv.c`, `putty-rc.h`, `README-msi.txt`, and the new `config-location.c`, `storage-win32-compat.h`, `jumplist-store.c`, `mcp.c` and `mcp.h`. The full inventory — 13 files differing, 5 new, 105 byte-identical — is kept in **`tech_detail.md` Appendix A** rather than repeated here, because it changes with every Windows batch |
| `storage.h` | +26 / -0 | Declarations for the folder operations and the name checks |
| `utils/CMakeLists.txt` | +2 / -0 | Build `utils/sesssearch.c` and `utils/sesspass.c` into the `utils` library |
| `CMakeLists.txt` | +48 / -1 | `test_sessions`, `test_sesssearch`, `test_sesspass`, `test_zmodem`, `test_zmodem_interop` and `test_mcp_buffer` targets |
| `proxy/sshproxy.c` | +6 / -0 | Marks a prompt it forwards to the client seat as the proxy's own (`p->from_proxy`), so the seat declines it rather than answering the *proxy's* password with the *session's* |
| `utils/prompts.c` | +1 / -0 | Initialise `from_proxy` — `new_prompts()` does not zero the structure |
| `.gitignore` | +23 / -0 | Ignore build trees, `*.old` backups, `_agent/`, `issue/`, `tools/gtk-mac-bundler/`, `.DS_Store`; keep the two icon sources, `icons/putty-beter-icon.png` and `icons/puttygen-icon.png` |
| `version.h` | — | This fork's version: `RELEASE` / `TEXTVER` / `SSHVER` are `0.85-beter.1`, and `BINARY_VERSION`'s build field is `1` (section 18) |
| `LATEST.VER` | — | `0.85-beter.1` |
| `unix/putty.plist` | +4 / -5 | `CFBundleVersion` / `CFBundleShortVersionString` are `0.85-beter.1` (was `Unidentified build`), and `NSHumanReadableCopyright` is now two lines — the preserved upstream notice plus the fork's own (section 18) |
| `README` → `README.md` | — | Replaced the generic upstream README with one about this macOS build; renamed so GitHub renders Markdown |

## New files

| File | Purpose |
|---|---|
| `CMakePresets.json` | `arm64`, `x86_64`, `debug` presets |
| `scripts/build-macos.sh` | Per-architecture build, one command |
| `scripts/build-windows.ps1` | The Windows counterpart, and more than a build: one command produces `build-win\putty.exe` **and** stages `putty-beter-win64\` — the exe plus `putty-mcp\` with dependencies installed on Windows and a generated `SETUP-WIN.txt`. Idempotent, with switches for clean/reconfigure/reinstall/stage-only. See [`WINDOWS-BUILD.md`](WINDOWS-BUILD.md) |
| `install-toolchain-win.ps1` | Sets up a Windows build host: Git, CMake, Ninja, MSYS2 x86_64 and the mingw-w64 **UCRT64** toolchain, with machine-scope PATH entries and optional Defender exclusions. Re-runnable and self-verifying |
| `WINDOWS-BUILD.md` | The Windows build reference: the one-command script, what it stages and why, the switches, prerequisites, the PowerShell 5.1 trap, a troubleshooting table, and how to verify a payload by hand |
| `scripts/bundle-macos.sh` | Builds `PuTTY.app` with gtk-mac-bundler, then has to fix up the result: normalise Cellar paths to `opt`, resolve dangling references, regenerate `loaders.cache`, **compile the GSettings schemas into the bundle**, **install the MCP bridge** (section 19), ad-hoc re-sign |
| `terminal/zmodem.c` | The ZMODEM engine: sender and receiver, written in-tree from the public-domain spec and Forsberg's original `rzsz`. Pure C with a callback interface, so it has no GTK, display or filesystem dependency and can be tested on its own |
| `terminal/zmodem.h` | The `ZmodemCallbacks` interface it talks to the front end through |
| `test/test_zmodem.c` | 135 checks: independent CRC vectors, a full loopback round trip, and the cancel / refused-file paths |
| `test/test_zmodem_interop.c` | Drives the **real** `sz` and `rz` over a pty, both directions, byte-for-byte |
| `tech_detail.md` Appendix E | Byte-exact ZMODEM protocol reference, with the licence status of every source it draws on (was `docs/zmodem-protocol-notes.md`) |
| `unix/utils/filechooser.c` | `our_file_chooser_run()` — the one file/folder chooser, with the full-path box |
| `unix/config-location.c` | The "where should the configuration live?" dialog |
| `unix/mcp.c` | The MCP server: the Unix socket, the session registry, the handshake, the length-prefixed wire protocol, and the endpoint files. Deliberately has no JSON — that lives in `mcp/` |
| `unix/mcp.h` | The front end's interface to it: one session per window, a stream tap, and a callback for the visible screen |
| `unix/mcp-buffer.c` | The capture buffer: escape stripping, line assembly, and the sliding window over absolute line ids. Free of GTK, of the terminal and of the socket, so it is testable on its own |
| `unix/mcp-buffer.h` | Its interface, and the reasoning behind the id and gap semantics |
| `test/test_mcp_buffer.c` | 95 checks over the capture buffer: escapes split across reads, a bare CR as a progress bar, chunking, eviction and the explicit gap, paging, truncation, author tags, and a sent line staying its own line |
| `mcp/` | The bridge. `bin/putty-mcp.mjs` (the CLI and the MCP server entry point), `src/protocol.mjs` (endpoint discovery and the socket protocol), `src/server.mjs` (the MCP tools and resources), `src/cli.mjs`, `src/manual.mjs`; plus `USER-MANUAL.md`, `README.md` and `package.json` |
| `mcp/USER-MANUAL.md` | The agent-facing manual, served by the `user_manual` tool and the `putty://manual` resource |
| `mcp/test/protocol.test.mjs` | 19 checks over the bridge's framing and its endpoint discovery, including following a `config-location.conf` redirect and a check that the redirect file's name still matches `CONFIG_REDIRECT_FILENAME` in `unix/storage.c`. No PuTTY, no socket, no display |
| `scripts/test-mcp.sh` | Smoke test: starts its own `pterm` against a throwaway configuration folder and drives the bridge end to end (38 checks) |
| `mcp/src/detect.mjs` | What the last line of output means: the prompt family, the pager patterns (including the two macOS pagers put where nobody expects them), the waiting-for-input patterns, and the order between them. Pure functions, so it is tested from a table of strings |
| `mcp/src/run.mjs` | The `run` poll loop: baseline, send, the fence, pager answers, `needs_input`, quiet/prompt completion, timeout, and the human-cancel rule |
| `mcp/test/run.test.mjs` | 124 checks over the two above, with a fake session on a virtual clock |
| `tech_detail.md` Appendix F | **How the MCP works**, for whoever works on it next: the files, the wire protocol with a snippet for driving the socket by hand, the capture-buffer semantics, `run` and why the waiting is in the bridge rather than the server, how human keystrokes are tagged (and why they are not written), the socket and its permissions, the build, testing, debugging, and what is still missing (was `docs/mcp.md`) |
| ~~`docs/mcp-user-manual.md`~~ | **Deleted.** It was a byte-identical copy of `mcp/USER-MANUAL.md` kept for reading; the bridge reads `mcp/USER-MANUAL.md` itself at run time, so the copy could only drift |
| `tools/README.md` | Why `gtk-mac-bundler` is not vendored, and how to fetch it |
| `test/test_sessions.c` | Tests for the session-folder storage layer |
| `utils/sesssearch.c` | The saved-session search: what counts as a search, the text a session is searched by, and the case-insensitive match. Pure - no I/O, no GTK - so it is testable on its own |
| `utils/sesssearch.h` | Its interface, and the `PUTTY_SESS_SEARCH_DEBUG` switch |
| `test/test_sesssearch.c` | Tests for the above: the prefix rule, host and comment hits, case folding, folder-name queries, and no false matches |
| `utils/sesspass.c` | The stored form of a saved session's password: `sesspass_blob_new()` / `sesspass_blob_open()` and the `v0` version tag. Pure — no I/O, no GTK — so it is testable on its own, and the tag is the seam for the encrypted `v1` that comes next |
| `utils/sesspass.h` | Its interface, and the `PUTTY_SESSPASS_DEBUG` switch |
| `test/test_sesspass.c` | 38 checks: round trips (including base64 padding and UTF-8), an empty password, and the failure modes — an unknown version, invalid base64, and a blob that must not be decodable |
| `tech_detail.md` Appendix G | **How the search works**: the prefix rule, what is matched, the per-session cache and its invalidation, how to drive the dialog from a script, and what is deliberately not done (was `docs/session-search.md`) |
| `tech_detail.md` Appendix D | **What macOS and GTK do that you would not expect**, with the numbers: a new window lands on the monitor at (0,0) rather than the primary one and `GTK_WIN_POS_CENTER` does not help; `-geometry` is compiled out (so `inst->gotpos` is dead code here); a native title bar is painted from the window's own background; GTK windows and menus are invisible to AppleScript (but keystrokes and `ESC [ 3 ; x ; y t` work). Plus the recipes for measuring window and monitor state, and two small compile traps (was `docs/macos-gtk-notes.md`) |
| `tech_detail.md` | The detail behind `PLAN.md`: findings, gotchas and "as built" accounts, one section per feature with a macOS half and a Windows half |
| `PLAN.md` | The plan: tools, phases, progress and results, both platforms |
| `icons/PuTTY.icns`, `icons/putty-*.pam` | Generated icon assets; the `.icns` is what `unix/putty.plist` asks for |
| `icons/putty-beter-icon.png` | The app icon source, converted to `icons/PuTTY.icns` at bundle time |
| `scripts/make-icns.sh` | PNG → `.icns` using only `sips` and `iconutil` |
| `unix/puttygen.c` | **The GTK PuTTYgen window**: key type/size/curve, prime policy, Generate on a worker thread with progress and a real Cancel, the key box (comment, passphrase, fingerprint, `authorized_keys` line), Open Key / Save private / Save public / the two export formats, the passphrase prompt, the `.ppk` v3 parameters dialog, About (with the fork's GitHub link and its own copyright line) and Licence. A port of `windows/puttygen.c`, with the file chooser, the opaque-title-bar fix and the threading being this fork's own — see section 17 |
| `unix/main-gtk-puttygen.c` | The `GtkApplication` main for it: the menu bar (File / Edit / Key / Help), the app id, and the dropped launcher argument |
| `unix/puttygen.bundle` | The gtk-mac-bundler description for `PuTTYGen.app` |
| `unix/puttygen.plist` | Its `Info.plist`: `CFBundleName`/`CFBundleExecutable` `PuTTYGen`, `CFBundleIconFile PuTTYgen.icns`, and its own bundle identifier |
| `scripts/test-puttygen-gui.sh` | The GUI checked against the command-line `puttygen` (33 checks): every key type, every loadable format, a save/reload round trip, a cancelled 4096-bit RSA generation, and that the file chooser is the application's own |
| `scripts/make-universal.sh` | Merges the per-architecture `.app` bundles into universal ones (`lipo -create` per Mach-O, re-signed, verified) and packages them as a drag-install `PuTTY-universal.dmg` with an `/Applications` symlink |
| `mcp/SETUP.txt` | The note installed beside the bundled MCP bridge: how to configure it as an MCP server and what it needs to run (section 19) |
| `icons/puttygen-icon.png` | The `PuTTYGen.app` icon source (1024x1024, artwork cropped to the canvas), converted to `icons/PuTTYgen.icns` at bundle time |
| `icons/PuTTYgen.icns` | Generated (gitignored) from `icons/puttygen-icon.png`; what `unix/puttygen.plist` asks for |

---

## 1. macOS code paths were disabled

`unix/platform.h` had two `#ifdef OSX_GTK` blocks, and the CMake build never
defined `OSX_GTK` (a leftover from the old Makefile-based build). Every
macOS-specific tweak was compiled out and the generic X11 code was used
instead. Both blocks now test `#if defined(OSX_GTK) || defined(__APPLE__)`.

Consequences of the fix:

| Tweak | Was off, so... | Now |
|---|---|---|
| `CLIPUI_DEFAULT_MOUSE`, `CLIPUI_DEFAULT_INS` | Mouse paste and Ctrl/Shift+Ins targeted the X11 `PRIMARY` selection, which does not exist on macOS | Target the system clipboard |
| `OSX_META_KEY_CONFIG` | No Command-key pass-through: the terminal ate `Cmd+C`/`Cmd+V` before the menu accelerators ran | Command is handled as a system modifier and passed to GTK |
| `JUST_USE_GTK_CLIPBOARD_UTF8` | The low-level `gdk_selection_*` code was used, which the source itself says fails on macOS | The `GtkClipboard` path, i.e. the macOS pasteboard |

This also uncovered a latent compile error in `clipboard_text_received` that had
never been built: `dup_mb_to_wc(…)` with four arguments, where four-argument
form is `dup_mb_to_wc_c(…)`.

## 2. Clipboard

**Terminal.** `Cmd+C`/`Cmd+V` via menu accelerators; `Ctrl+Shift+C`/`Ctrl+Shift+V`
(now defaulted to the system clipboard rather than to "do nothing");
`Ctrl+Ins`/`Shift+Ins`; and Ctrl+right-click → Copy/Paste/Copy All.

The `Ctrl+Ins` pair did not work upstream on macOS even with the setting turned
on: `key_event()` tested `event->keyval == GDK_KEY_Insert`, but macOS has no
Insert key — a PC keyboard's Insert arrives as virtual keycode 0x72
(`kVK_Help`, the Mac's "Help" key, which GDK reports as `GDK_KEY_Help`), and a
keypad Insert arrives as `GDK_KEY_KP_Insert`. `is_insert_key()` now accepts any
keyval whose GDK name contains `Insert`, plus `GDK_KEY_Help` and hardware
keycode 0x72 on macOS.

**Dialogs.** Nothing could be pasted into the configuration box's text fields,
because GTK binds entry copy/paste to Control while macOS users press Command
(`GDK_MOD2_MASK`). `dialog_clipboard_key()`, called from `win_key_press()`, now
handles Ctrl or Cmd + `C`/`V`/`X`/`A` and Ctrl+Ins/Shift+Ins on the focused
`GtkEditable` (and copy from selectable message labels). `GDK_MOD2` is only
treated as Command under `__APPLE__`, since on other Unix systems it is Num Lock.

## 3. IME pre-edit cursor offset

`term_set_preedit_text()` allocated the pre-edit line unconditionally, so an
empty pre-edit string (which GTK on macOS supplies on ordinary keystrokes) left
it non-`NULL` with zero columns. `do_paint()` then computed the cursor column as
`preedit_end - 1` — one cell too far left — and read `chars[-1]` out of bounds.
Fixed in `terminal/terminal.c`.

## 4. Relocatable configuration folder

PuTTY has always used `~/.putty`. This build offers, once, to keep the
configuration somewhere else (`~/Documents/putty-beter-config` by default).

* Base location resolution is unchanged: `$PUTTYDIR`, then an **existing**
  `$XDG_CONFIG_HOME/putty` or `~/.config/putty`, then `~/.putty`, then
  `pw_dir/.putty`, then `/.putty`.
* A base location may contain `config-location.conf`, a small text file whose
  first non-comment line is the real configuration directory. `unix/storage.c`
  reads it, so `putty`, `plink`, `pscp`, `psftp` and `puttygen` all follow the
  same directory and saved host keys stay consistent. An invalid or missing
  target is ignored (with a trace under `PUTTY_CONFIG_DEBUG=1`).
* The base folder is **created if it is not there**, because the redirect file
  lives in it and on a machine where PuTTY has never run there is nothing to
  redirect from: that is the first-run path, not an odd one. Without this,
  "Use This Folder" failed with `Unable to write ~/.putty/config-location.conf:
  No such file or directory`, and the dialog could not be completed at all.
  (Found by deleting `~/.putty` and starting the app; `test_sessions` now
  covers it, and fails if the `make_dir_path(base)` is taken out again.)
* The redirect is followed immediately: the running process switches folders
  after the dialog closes, so the very first run uses the new location.
* The dialog appears when there is no configuration yet (no `sessions`
  subdirectory), or when Option/Alt is held at launch, or when
  `PUTTY_CONFIG_RELOCATE=1`. On acceptance the existing `sessions`,
  `sshhostkeys`, `sshhostkeys.tmp`, `sshhostcas` and `randomseed` are **moved**;
  `rename()` is used where possible, `EXDEV` falls back to a recursive copy, and
  the source is only removed once the copy succeeded.
* Nothing is overwritten: an item that already exists in the destination is left
  alone and the old copy stays where it was. Cancelling records the current
  folder as configured, so the dialog is genuinely one-shot. Choosing the base
  location again removes the redirect.
* The Option-key test uses `CGEventSourceFlagsState()` (CoreGraphics, linked
  through `guiterminal`), because GDK cannot report a modifier before the first
  key event of the process.

## 5. Changed defaults

`conf.h` unless noted. These apply to **new** sessions; a saved session that
stores a key keeps its own value.

| Setting | Upstream | Here |
|---|---|---|
| Session logging | None | Printable output |
| Log file name | `putty.log` | `<config folder>/logs/&Y-&M-&D_&T-&H.txt` (`unix/window.c`) |
| If the log file exists | Ask | Always append |
| Backspace key | Control-? | Control-H |
| Function keys | `ESC[n~` (Linux) | VT100+ |
| Window size | 80×24 | 128×36 |
| Keepalive interval | 0 (off) | 120 s |
| Internet protocol version | Auto | IPv4 |
| Tunnels → IP version, new tunnel | Auto | IPv4 (`config.c`) |
| Window → Fonts, ordinary text | `client:Monospace 12` | `client:PT Mono 14` (`unix/platform.h`, `unix/window.c`) |
| Window → Fonts, wide (CJK) text | empty (use the ordinary font) | `client:Lantinghei TC Semi-Bold 14` (same) |
| Window → Colours, default foreground | `187,187,187` | `242,242,242` (`settings.c`) |
| Window → Colours, default background | `0,0,0` | `0,48,42` (`settings.c`) |
| Window → Colours, bold background | `85,85,85` | `64,96,96` (`settings.c`) |
| "Seconds to hold the window open first" (**new setting**) | — | 2 (§20) |

The palette lives in `settings.c` (`load_settings()`) rather than `conf.h`,
because `CONF_colours` is a subkey map loaded by hand: its nineteen other
entries, the ANSI sixteen in particular, are upstream's. `platform_default_s()`
does not touch `Colour*`, so that array is the single source of the default
palette. The two fonts come from `platform_default_fontspec()`; the generic
`DEFAULT_GTK_CLIENT_FONT` is deliberately *not* changed, because that is the
fallback used when a configured font cannot be loaded.

These are the owner's own settings from a saved session. The two fonts also
become **pterm**'s defaults, since `pterm` shares `unix/window.c`.

Two follow-on changes were needed:

* `conf_new()` captures the log file name, so `config_location_init()` runs
  before it in `main-gtk-simple.c`; otherwise the folder would be the
  pre-relocation one.
* PuTTY never creates a missing directory for a log file, so startup creates
  `<config folder>/logs`.

`pterm` keeps `putty.log`: it has no saved sessions and no configuration folder.

## 6. Configuration box layout and size

The Session panel is reordered so the panel ends with the saved-session list
instead of "Close window on exit", and the configuration box is resizable with
its size remembered between runs. With a long list of saved sessions, and a
window that stays the size you made it, the list is the natural thing to grow.

* `config.c`: "Close window on exit" is created before the saved-sessions
  controlset, so it appears above the list. The saved-sessions controlset is
  marked `vexpand`, and its listbox is the control that takes the spare height;
  seven rows remain the minimum.
* `dialog.h` / `dialog.c`: two flags on `dlgcontrol` (`vexpand`,
  `force_left`) and one on `controlset` (`vexpand`). Front ends that do not
  implement them ignore them.
* `unix/dialog.c`: a controlset marked `vexpand` is packed with `expand` in its
  panel, and a control marked `vexpand` becomes the `columns_vexpand` child of
  its `Columns`. `force_left` uses the existing `columns_force_left_align`, so
  a control keeps its natural width — this is what stops the Load/Save/Delete
  buttons turning into full-width bars when the window is wide.
* `unix/columns.c`, `unix/columns.h`: `columns_content_last_col()` sizes the
  last column of each block to its widest left-aligned child instead of its
  percentage. Without it the button column would still reserve 20% of the
  width and leave a gap; with it, the list reaches the buttons and the three
  buttons stay the same width, flush with the right edge, at any window size
  (checked headlessly at 600, 1000 and 2000 px: `list` 525/925/1925 px wide,
  buttons always 71 px).
* Window size: `create_config_box()` is now explicitly resizable, restores a
  remembered size with `gtk_window_set_default_size()`, tracks configure events,
  and writes the final size on `destroy`.
* `unix/storage.c`: `config_state_get_int()` / `config_state_set_int()`, a tiny
  key=value store used as `<configuration folder>/gui-state`:

  ```
  # PuTTY-beter: remembered GUI state.
  width=1200
  height=1000
  ```

  Reading is best-effort; writing rewrites the file through a temporary and a
  `rename`, keeping any other keys. `pterm` is excluded: it has no
  configuration folder, so `saved_sessions` is checked before reading or writing.

## 7. Build and packaging

* `scripts/build-macos.sh <arm64|x86_64>` — one CMake build per architecture.
* `scripts/bundle-macos.sh <arch>` — runs gtk-mac-bundler and then repairs its
  output: Homebrew `Cellar` paths are normalised to `opt`, dangling references
  resolved, `loaders.cache` regenerated (the bundler wrote it before the
  libraries were in place), and everything ad-hoc re-signed because
  `install_name_tool` invalidates signatures.
* `unix/osxlaunch.c` points `GDK_PIXBUF_MODULE_FILE` and `GTK_IM_MODULE_FILE` at
  the caches inside the bundle. Without that, gdk-pixbuf and GTK fall back to
  build-time absolute paths, which loads a **second copy of GLib** next to the
  bundled one — duplicate Objective-C classes and undefined behaviour.
* `unix/putty.bundle` lists the gdk-pixbuf loaders so the generated
  `loaders.cache` points inside the app, and bundles `icons/PuTTY.icns`.
* `scripts/bundle-macos.sh` also compiles the GSettings schemas into
  `Contents/Resources/share/glib-2.0/schemas/` (38 of them) and runs
  `glib-compile-schemas`. **Without this, opening any file chooser kills the
  app**: GTK's file chooser looks up `org.gtk.Settings.FileChooser`, and the
  launcher sets `XDG_DATA_DIRS` to the bundle, which overrides GLib's fallback
  to the system schemas — so the lookup fails, GLib calls `g_log_abort()`, and
  the process dies with SIGTRAP (exit 133). This was the cause of the "`rz`
  crashes PuTTY" bug, and it also broke the configuration box's Browse button.
* Intel (x86_64) needs an Intel Homebrew in `/usr/local`; the arm64 Homebrew in
  `/opt/homebrew` cannot produce an x86_64 GTK. **Not yet verified.** Note that
  Apple is retiring Rosetta (macOS 27 is the last release with it).
* `scripts/make-universal.sh` merges the two per-arch bundles into universal apps
  (`lipo -create` per Mach-O, then every Mach-O — `Contents/MacOS` included — and
  the bundle re-signed and verified) and packages them as a drag-install
  `build-universal/PuTTY-universal.dmg` with an `/Applications` symlink. The image
  is built with `diskutil image create from` (macOS 27 deprecates `hdiutil
  create`), falling back to `hdiutil` where that verb is absent.
* `scripts/build-windows.ps1` is the Windows build **and packaging** entry point;
  `install-toolchain-win.ps1` sets up the host. The script runs the cross build
  (native arm64 cmake/ninja driving the x86_64 UCRT64 gcc), copies `putty.exe` into
  `putty-beter-win64\`, mirrors the MCP bridge into `putty-mcp\` there, installs
  its dependencies **on Windows** (`npm ci --omit=dev`, so `node_modules/.bin` is
  native rather than the SMB-mangled mac shims), writes `SETUP-WIN.txt` from the
  real paths, and smoke-tests the bridge before reporting success. Step-by-step
  behaviour, the switches, and the PowerShell 5.1 traps: [`WINDOWS-BUILD.md`](WINDOWS-BUILD.md).

## 8. Diagnostics

These are runtime switches and cost nothing when unset.

* `PUTTY_DIAG=1` — key events (keyval, name, modifier state), clipboard
  activity, font metrics, cursor placement, ZMODEM engine traces
  (`[zmodem] …`) and file-chooser results (`[filechooser] "…" -> …`).
* `PUTTY_CONFIG_DEBUG=1` — configuration folder resolution and relocation, and
  the resulting default log file name.
* `PUTTY_MCP_DEBUG=1` — MCP traces (`[mcp] …`): whether a session offered
  itself, where the socket was created and with what permissions, the handshake,
  and every request and reply. The bridge has its own `--debug` for the other
  end of the same conversation.
* `PUTTY_SESS_SEARCH_DEBUG=1` — the saved-session search (see section 10b).
* `PUTTY_SESSPASS_DEBUG=1` — the saved-session password and the login name kept
  beside it: a blob being decoded, an injection into a password prompt, and a
  login name remembered from the `login as:` prompt (never the password itself).
* `PUTTY_BETER_DEBUG=exit` — the readable exit (§20): which path closed the
  window, the exit code, the `CloseOnExit` and delay values in force, whether the
  countdown was started, and whether a keystroke or click cancelled it.
* `ZMODEM_INTEROP_TRACE=1`, `ZMODEM_INTEROP_DUMP=1` (raw streams to
  `/tmp/zm-in-*.bin` and `zm-out-*.bin`) and `ZMODEM_INTEROP_PRINTABLE=1` —
  only for `test/test_zmodem_interop`.

---

## 9. Session folders

Sessions can be kept in folders, shown as a collapsible tree in the
configuration box, and moved between folders by dragging.

* A session name may contain `/`, which separates folder names: `Work/beta`
  is the file `<sessions>/Work/beta.conf`, and `Work` is a **real directory**.
  So folders can be empty, and the structure is visible in the Finder.
  (`make_session_filename()` already percent-encoded `/` as `%2F`, so names
  with a slash were representable but meant nothing; the encoder now treats
  `/` as a separator and encodes each component separately.)
* A saved session's file ends in **`.conf`**, and only such files are read as
  sessions: anything else that finds its way into the directory (Finder's
  `.DS_Store`, a stray copy) is ignored rather than listed. Folders are plain
  directories and have no extension, so `INDEX_SESSION` (a folder) and
  `INDEX_SESSIONFILE` (a session) are separate cases of `make_filename()`, and
  `session_move()` picks between them from the trailing `/`. A name beginning
  with `.` is refused, and hidden entries are skipped when listing, so nothing
  can be saved and then be invisible.
* `get_sesslist()` reports a folder as a name with a trailing `/`, which is
  how the front end tells folders from sessions. Enumeration is recursive
  (depth-limited, and symlinks are not followed).
* Names are validated: an empty, `.` or `..` component is refused, so a
  session name cannot escape the sessions directory, and a component may not
  begin with `.` at all, since such a file would be hidden from the listing.
  `open_settings_w()` rejects such a name outright rather than writing
  something odd.
* `session_mkdir()`, `session_move()` (sessions *and* folders, refusing to
  move a folder inside itself) and `session_rmdir()` (empty folders only)
  live in `unix/storage.c`; `session_move_target_name()` works out the name a
  drag would give, so that the fiddly part is testable without a front end.
* The dialog layer gained a `tree` flag on list boxes, a `drag_handler` hook,
  and `dlg_listbox_selected_id()` / `dlg_listbox_select_id()` /
  `dlg_listbox_selected_ids()`: with a tree, a row index is ambiguous, so the
  configuration box identifies rows by id (the index of the name in the
  session list). The plural one is what makes a drag move the whole
  selection, and what lets Delete remove several rows at once.
* The session list is a multi-select list, so a group of sessions can be
  dragged into a folder together, or deleted together. `dlg_listbox_selected_id()`
  falls back to the row under the cursor when several are selected, so Load
  and Save still act on something sensible.
* Dragging several rows needs one more workaround: GtkTreeView's own press
  handler calls `gtk_tree_view_real_set_cursor()` with `CLEAR_AND_SELECT` for
  an unmodified press, so pressing on one of several selected rows throws the
  rest of the selection away before any drag starts. `button-press-event` is
  `G_SIGNAL_RUN_LAST` and the tree view's gesture runs from the class closure
  in the *bubble* phase, so a plain `g_signal_connect()` handler sees the
  selection intact: `listbox_press_event()` in `unix/dialog.c` snapshots it
  there. The snapshot is put back by `listbox_restore_selection()` at
  **drag-begin** — not only at the drop, since the collapse is otherwise
  visible for the whole gesture — and again in the drop handler, which is what
  the control's `drag_handler` reads. `drag-end` (or the next press) clears it;
  `listbox_release_event()` deliberately does *not*, because the release that
  ends a drag can arrive before the drop does.
  The snapshot is only restored if the press was on one of the selected rows,
  so dragging an unselected row still moves just that row.
* `sessioncmp()` puts **folders before sessions at every level** (then
  compares component by component, so a folder's contents stay together),
  which is what makes the tree show folders at the top of each level.
  "Default Settings" still comes first.
* In tree mode the GTK front end builds the hierarchy from the `/` in the
  names, keeps a folder flag in an extra, undisplayed model column, shows
  folders in bold, expands them as they gain children, and drags rows onto
  folder rows.
* The tree view is the drag *source*, but the drop target is deliberately its
  **scrolled window**, not the tree view. Two reasons, both found by reading
  GTK's `gtktreeview.c`:
  - the tree view's own drag class closures (`gtk_tree_view_drag_motion`,
    `gtk_tree_view_drag_drop`) run first and, because the signal accumulator
    stops emission when a handler returns TRUE, they consume the drag signals
    — so a handler of ours could never see where the drop happened;
  - GTK's model drop inserts the dragged row as a *sibling* of the row it was
    dropped on (the parent of the destination path), so a drop on a top-level
    folder lands outside it.
  With the scrolled window as the target, GTK's model drop never runs, the
  model is untouched, and `listbox_drag_received()` turns the drop
  coordinates into a row with `gtk_tree_view_get_path_at_pos()` — not
  `get_dest_row_at_pos()`, which is drag-specific and offsets the position to
  allow for the dragged row being drawn under the cursor. The row it finds
  decides the destination: a folder row means that folder, any other row
  means the folder that row is in, and empty space below the list means the
  top level. Storage then performs the real move, and the list is rebuilt
  from disk — always, even for a no-op, so the display cannot drift.
* Deleting a folder that still has sessions in it asks first, with a
  "Delete All" / "Cancel" message box (`dlg_confirm()`, a new small core API
  implemented with the GTK message-box machinery). Escape takes the safe
  answer, and no button is the Return default, since the destructive answer
  should not be one keypress away. `session_rmdir_force()` removes the folder
  and its contents, and refuses anything that is not a folder.
* The Windows front end shows a flat list and gets `dlg_listbox_selected_id()`
  / `dlg_listbox_select_id()` implemented in terms of row indices, plus
  folder operations that report "not supported".

**Verified** by `test/test_sessions.c` (a real test target, run with
`./build-arm64/test_sessions`): plain sessions, empty folders, nesting, names
with spaces, moving sessions in and out of folders, moving whole folders
(including out of and back into a parent), refusing to clobber an existing
name, refusing to move a folder inside itself, removing empty and non-empty
folders, `../evil` being refused, and the drag-target name computation.

**Not verified:** the drag gesture itself needs a human; everything either
side of it (the move in storage, the target-name computation, the tree
display) is covered. The same goes for the delete confirmation, which needs a
click on Delete.

**Not implemented yet:** renaming a folder, remembering which folders are
expanded, and per-session saved passwords — see `PLAN.md` Phase 2. (A
per-session comment is now implemented: see section 10.)

---

## 10. Session comments

A saved session can carry a free-text note about itself. It lives in a panel of
its own, **Session → Comment**, directly below Logging, because it needs a box
big enough to type a paragraph into.

* `conf.h` gains `session_comment`, a `VALUE_TYPE(STR)` option. It is
  `LOAD_CUSTOM, SAVE_CUSTOM`, because the settings file format is one
  `key=value` per line and a comment can run to several lines. `settings.c`
  escapes `\`, newline and CR as `\\`, `\n` and `\r` on the way out and undoes
  it on the way in, so a comment survives a round trip intact.
* It is written to the session file only when it is not empty, so ordinary
  session files are unchanged. Vanilla PuTTY reading such a session ignores the
  unknown `SessionComment` key.
* Note that a `LOAD_CUSTOM` option is skipped by the automatic loop in
  `load_open_settings()`, so its empty default has to be set explicitly - a
  `Conf` entry has to exist before `conf_get_str()` can be asked for it.
* `dialog.h` gains a `multiline` flag on the edit box. `unix/dialog.c` makes a
  multi-line box a `GtkTextView` in a scrolled window: word-char wrapping,
  `accepts-tab` off so Tab still moves the focus, and a minimum of eight lines
  (the box is marked `vexpand`, so it grows into the panel's spare height).
  Return inserts a newline rather than activating the default button, and
  changes are reported by the text buffer, so it gets neither `editbox_changed`
  nor `editbox_key`.
* The value is always UTF-8, because that is what the front ends' edit boxes
  deal in.
* A `GtkTextView` is the one widget in `unix/dialog.c` that is not a plain
  `GtkEntry`, and it needs one extra line: `gtk_widget_show()` does **not**
  recurse in GTK 3, so showing only the surrounding scrolled window leaves the
  view invisible - an empty sunken frame that cannot be typed into, because an
  invisible widget never receives the click that would focus it.

**Verified:** the storage round trip, in `test/test_sessions.c`, over six
comments including empty, multi-line, backslashes, UTF-8 bytes and a backslash
at the end of a line.

**Not verified by the test suite:** the widget itself, which needs a human.

## 10b. Searching saved sessions

The **Saved Sessions** box in the Session panel doubles as a search box: if its
contents start with `/` (or `\`), the rest of the line is a query, and the list
below is filtered to the sessions that match it. A session matches when the
query appears in its **name**, its **HostName** or its **SessionComment**,
case-insensitively. `/Work` finds the sessions inside the `Work` folder,
because a session is matched by its whole path.

* A session name is a `/`-separated path and no valid path begins with `/`
  (`session_name_is_valid()` rejects an empty component), so a leading `/` is a
  character no saved session can ever use. `\` cannot appear in a session name
  at all; it is accepted as an alias and not advertised.
* `config.c` does the work: `sessionsaver_data` caches, per session, the
  lower-cased `name\nhost\ncomment` read with `open_settings_r()` and
  `read_setting_s()`, so a keystroke does not re-read the session files. The
  cache is dropped wherever the session list is rebuilt (Save, Delete, New
  Folder, drag).
* `sessionsaver_fill_list()` replaces the inline list-population loop in the
  `EVENT_REFRESH` path. It adds every row when there is no query, or only the
  matching ones when there is; rows keep their original id, so Load, Delete,
  drag and double-click keep working on a filtered list, and the rows for a
  match's parent folders are made by the existing tree code.
* While a search is on, the box's label is changed with the existing
  `dlg_label_change()` (already implemented for `CTRL_EDITBOX` in
  `unix/dialog.c`) to `Search (matched/total)`, and back to `Saved Sessions`
  when it ends. **Save** and **New Folder** beep instead of trying to save
  `/beta` as a name.
* `dialog.h` gains a `placeholder` field on the edit box, and `unix/dialog.c`
  passes it to `gtk_entry_set_placeholder_text()`, so the empty box says
  *Name/folder, or / to search*. GTK 2 has no placeholder and simply shows
  nothing; the Windows front end ignores the field, as it does `multiline`.
* The matching itself is in `utils/sesssearch.c` - a new file - so that it can
  be tested without a front end. `PUTTY_SESS_SEARCH_DEBUG=1` traces each search.

**Verified:** `test/test_sesssearch.c` (33 checks: the prefix rule, host and
comment hits, case folding, a folder-name query, no false matches), and by hand
in the built app: `/10.0.1` matched one of four sessions by host, `/dana` one
by comment, `/Work` the folder and its two children, `/zzz` nothing at all,
clearing the prefix brought the whole list and the old clear-the-box behaviour
back, and Save with a search in the box saved nothing.

## 11. ZMODEM file transfer

`sz` and `rz` now work in both directions, over SSH and on a serial line.

The engine is `terminal/zmodem.c`, a new file. It is **pure C with a callback
interface** (`ZmodemCallbacks` in `terminal/zmodem.h`) and depends on neither
GTK nor the display nor the filesystem, so it is unit-testable and would suit
any front end. It was written in-tree from the public-domain ZMODEM
specification and Chuck Forsberg's original `rzsz`, and is MIT like the rest of
this tree - no GPL component is involved. `tech_detail.md` Appendix E
records exactly which sources were used and the licence status of each;
`lrzsz` was deliberately never consulted.

* **Inbound interception** happens in `gtk_seat_output()`, on stdout only, so
  the engine gets first refusal on the stream. Everything it does not consume
  is handed back through `terminal_output`, which means that with no transfer
  running the bytes reach the terminal emulator exactly as before.
* **Outbound** goes through `backend_send()`, not `ldisc_send()`, so it does not
  go through the line editor.
* **Typed input** is held back while a transfer is running, except Ctrl-C, which
  cancels — the same key `rzsz` uses.
* A **transfer folder** setting (`Session → File transfer`) holds both
  directions: downloads are saved into it under the name the protocol carries
  in its `ZFILE` header, with no prompt, and the upload chooser starts there.
  It is written back after each transfer, so the last folder used sticks.
* A small **non-modal progress window** shows the file name, bytes and
  percentage, with a **Cancel** button.
* **Cancelling** reports `ZMODEM transfer cancelled` rather than completing: a
  file chooser that is cancelled aborts the transfer (CAN), and it is only
  treated as "no more files" — the ordinary end of a session — once a file has
  actually been sent. One `rz` sends one file; run `rz` again for another.
* A transfer cut short does **not** leave a truncated file behind.

**On Windows** the same engine is wired into the Win32 seat: `win_seat_output()`
diverts stdout to it, every typed-input path goes through a funnel that hands the
bytes to the engine instead of the terminal, and the progress window is a
modeless `IDD_ZMODEM` dialog with a real `msctls_progress32` bar. Verified the
same way, byte-for-byte, both directions. Three Windows-specific notes are in
`tech_detail.md` §12: a directory is not a valid initial file name for the common
dialog (hence `request_file_in_dir()`), editing a `.rc2` does not rebuild the
resource unless the `.rc` is touched too, and a blocking front-end callback also
stops the socket being read, so the upload chooser must be answered promptly.

**Tests.** `test/test_zmodem.c` (135 checks) covers CRC vectors from the spec, a
full loopback round trip, and the cancel and refused-file paths.
`test/test_zmodem_interop.c` drives the **real** `sz` and `rz` over a pty in both
directions and compares 250000 bytes byte-for-byte. (That one is macOS-only: it
uses `forkpty()`.)

## 12. One file chooser, and dialog title bars on macOS

Two GTK-on-macOS problems, both fixed in a way that applies to the whole app.

**Every file picker now has a full-path box.** GTK's `GtkFileChooserDialog` does
have a location bar, but it is hidden behind Ctrl+L, so there was no obvious way
to say "use this exact path" — and it is the same dialog for every file setting
in the box. `unix/utils/filechooser.c` replaces it with one chooser used
everywhere: a `Path:` entry across the top (type a full path and press Return to
use it, type a folder and press Return to go there), sane sizing, centred on its
parent, and keyboard focus taken properly. It covers the private key file and
certificate under Auth/Credentials, the GSSAPI library path, the session log
file, the transfer folder, the `rz` upload chooser and the
configuration-folder dialog.

**Dialog title bars are no longer see-through.** GTK 3 on macOS creates a
`GtkDialog`'s `NSWindow` with an **RGBA visual**, so the window is not opaque and
its background is fully transparent — and macOS paints the title bar *from that
background*. The result was traffic lights and a title drawn over whatever was
behind the window. A plain `GtkWindow`, which is what `our_dialog_new()` returns
for the configuration box, the About box and every message box, was never
affected. `gtk_dialog_fix_macos_titlebar()` in `unix/gtkcompat.h` asks for the
screen's plain system visual before the dialog is realized, which makes the
window opaque again. That is one line of ordinary GTK API at each of the few
real `GtkDialog` sites — no Objective-C and no AppKit internals.

## 13. MCP: an AI agent can read a session, and drive it

An MCP client — OpenCode, in practice — can attach to a session **in the window
the human is looking at**, read it, type into it, and **run a command and wait
for the device to finish**. Reading and typing are separate opt-ins.

**Off by default, and per session**, under *Session → Automation (MCP)*: turning
the first setting on offers the session to anything that can reach the socket,
and the second lets it be driven, so both are decisions the user makes per
session rather than something the fork decides for them. With the first off
there is no socket, no token and no file at all.

**Typing is real, and it goes through the line discipline** — the same path a
keystroke takes — so local echo and line editing behave normally. It is refused
during a ZMODEM transfer, where those bytes would be protocol rather than input.
A line a client sends gets its own line in the transcript, tagged `m`, so that
what the agent typed can be told apart from what the device printed; the
interrupted prompt is kept as a device line rather than being merged with it.

**Why it exists when `ssh` already runs commands.** An exec-per-command client
is better for a Linux shell: real exit codes, separated stderr, structural
output attribution. MCP-in-PuTTY is for what only PuTTY can do — a session that
is *already authenticated* (MFA, a jump host), network gear whose only interface
*is* an interactive CLI, a serial console where there is no second session to be
had, and a human who wants to watch and be able to take over.

**Shape.**

```
OpenCode --stdio MCP (JSON-RPC)--> putty-mcp --Unix socket--> PuTTY
             ("putty-mcp")          (mcp/, Node)             (unix/mcp.c)
```

**No JSON in the C code.** The socket speaks a list of length-prefixed fields —
`u32 nfields`, then `nfields × (u32 length, length bytes)`, big-endian, no
escaping — and the bridge does all of the MCP and JSON translation. That keeps
MCP's spec churn out of the fork, and means the socket can be driven with no LLM
in the loop at all: `putty-mcp list`, `putty-mcp read 1`, `putty-mcp screen 1`.

**The capture buffer is its own record of the output.** It is deliberately *not*
the terminal's scrollback: scrollback is a display setting, so a user can turn
it down to nothing, while MCP has to see every byte regardless. It holds roughly
the last 4 MB per session.

It is tapped in `zm_terminal_output()` — the point where the bytes are about to
be handed to the terminal — rather than in `gtk_seat_output()` where the ZMODEM
engine is fed. At the latter the bytes have not been classified yet, so the
prologue of a transfer (`**B00…`) would land in the transcript as junk and the
engine's own pass-through text would be missed.

**Line ids are absolute and never reused.** Line 1 is the first line of the
session, forever. The window slides over that numbering, so when a reader's
cursor falls out of it the reply says so explicitly —
`gap: {missing_from, missing_to}` — rather than silently losing output, and
`total_lines` is monotonic so a reader can always tell how far behind it is.

**Two details worth recording, because both are easy to get wrong:**

* A **bare carriage return** is "back to column 0", not a line ending — that is
  what a progress bar does. Only `\r\n` or `\n` ends a line, decided by looking
  at the byte *after* the CR. Otherwise a 60-second download turns into 600
  lines of output.
* The **unfinished last line** — the prompt — is reported, and does **not** move
  the cursor, because it can still grow. It is also never truncated: a fragment
  of it ("Rout") is worse than nothing. A *completed* line is truncated when it
  exceeds the whole read budget, because the cursor has to be able to move past
  it.

**The socket** lives in `$TMPDIR/putty-mcp-<uid>/`, as **one set of files per
process**: `sock.<pid>`, `token.<pid>` and `endpoint.<pid>`, with the endpoint
also written into the configuration folder as `mcp-endpoint.<pid>` (a GUI app
and a shell can disagree about `$TMPDIR`). It is `0600`, or `0660` in the
`putty-mcp` group if that group exists — plain `0660` would use macOS's default
group, `staff`, which contains every local account, and would let any local user
type into your switches. A 32-byte token in a `0600` file is required in the
handshake as well.

**Per process, because the plain `putty` and `pterm` binaries run one process per
window.** A single fixed socket name was a real bug: the second PuTTY unlinked
the first one's socket and bound the same path, so the first became silently
unreachable, and the second's cleanup then unlinked the path again. The bridge
now merges every server it finds, and a session id carries the pid
(`72377001`) so ids stay unique across processes without any shared state. The
bridge also follows a `config-location.conf` redirect when looking for the
endpoint file, so a *relocated* configuration is found — which is the shape
most installations actually have.

**The `.conf` rename left the bridge behind.** Two names in the MCP code kept
their pre-`.conf` spelling after saved sessions and the redirect file gained the
extension, and neither failure was loud:

* the bridge looked for `<base>/config-location`, which `unix/storage.c` no
  longer writes, so a *relocated* configuration was invisible to it and it fell
  back to the empty base folder;
* `scripts/test-mcp.sh` wrote its session as `Default%20Settings` and its
  redirect as `config-location`, so `pterm` read neither — it started with no
  `McpEnabled`, produced no server and no socket, and the smoke test failed at
  the first step.

`mcp/test/protocol.test.mjs` now checks the redirect file's name against
`CONFIG_REDIRECT_FILENAME` in `unix/storage.c`, because the two files are in the
same repository and this is exactly the kind of drift that is invisible until a
user relocates their configuration. The smoke test is the end-to-end half: with
both names right, `pterm` finds the session in the relocated folder, starts the
server, and the bridge follows the redirect to it.

**The agent is given a manual.** `mcp/USER-MANUAL.md` is served by the
`user_manual` tool, by the `putty://manual` resource, and as a pointer in MCP's
handshake instructions. An agent that has to guess how cursors, gaps and
unfinished lines work will get them wrong, and prose is the only way to explain
semantics that no schema can express.

**Debug:** `PUTTY_MCP_DEBUG=1` in PuTTY's environment traces the socket, the
handshake and every request and reply; `putty-mcp list --debug` traces the
bridge's side of the same conversation.

### What driving a real session found

The feature was exercised against a live SSH login to an Intel Mac, and the
session found three bugs the test suite had not. All three were in the
"obviously fine" category, which is the argument for doing it:

1. **A second PuTTY stole the first one's socket.** The socket had a single
   fixed name, but the plain `putty` and `pterm` binaries run one process per
   window. The second process unlinked the first one's socket and bound the same
   path, so the first became silently unreachable, and the second's cleanup then
   unlinked the path again. Files are now per process (`sock.<pid>` and friends)
   and the bridge merges every server it finds; session ids carry the pid so they
   stay unique across processes.
2. **A sent line was merged with the far end's echo**, and worse, our own Return
   set the buffer's "pending CR" flag — so the echo's first character was read as
   "back to column 0" and *discarded the line*, losing both the prompt and the
   command. `mcp_buffer_note_sent()` now closes any in-progress line first and
   treats a CR from us as a line ending.
3. **`read` refused to work on any session with more than about 19 lines of
   output.** A 64-field sanity limit on incoming messages is ample for requests
   (the most is four fields) but nonsense for replies: `read` returns three fields
   per line, so an ordinary 8 KB reply from a 200-line session carries 607. It
   failed as "malformed response from PuTTY", which pointed at nothing.

**And one thing that is not a bug but bites anyway:** an MCP client spawns
`putty-mcp mcp` once and keeps that process for the life of its session, so
editing the bridge has no effect until the client restarts, and a running Node
process cannot reload itself. Killing the stale process does *not* help — the
client drops the server for the rest of the session and does not respawn it
(verified). Three mitigations: the protocol version is checked at the handshake,
so a format change is reported rather than misread; the bridge warns on stderr if
its own source files are newer than its process; and the CLI always runs the
current code, because it starts a fresh process per invocation.

**Test:** `scripts/test-mcp.sh` (18 checks at the time) starts its own `pterm`
against a throwaway configuration folder and verifies the whole path, including
the escape stripping, the paging and the refusal of an unknown session id. It
needs a display and touches nothing of yours.

### 4b: waiting for a command

`run` is the tool that makes the feature useful rather than merely possible: it
sends one command, waits for the device to finish, and returns **just that
command's output** — not the echo, not the prompt that was interrupted, not the
prompt that came back. It answers a `--More--` pager by itself, which is what
makes it usable on network gear, and it *stops* rather than guessing at a
`[confirm]` or a password prompt, reporting `needs_input` instead.

**The waiting is in the bridge, not the server, and that is the main design
decision of this phase.** The socket protocol is unchanged — `run` is built from
`read` and `send`, which already existed — so no C code had to learn about
prompts. The alternative, a deferred-reply state machine in `unix/mcp.c` driven
by `schedule_timer()`, would have doubled the change, made the prompt heuristics
testable only against a real `pterm`, and grown the C side an opinion about what
a prompt looks like, which is exactly what the dumb-socket split exists to
prevent. `mcp/src/detect.mjs` and `mcp/src/run.mjs` are pure and unit-testable: a
fake session on a virtual clock tests a thirty-second timeout in microseconds.

`completed_by` is the honesty mechanism, and the reason the output can be
trusted: `prompt` (quiet *and* the last line looks like a prompt), `quiet`
(quiet, nothing prompt-shaped — probably finished), `timeout` (still talking, so
the output is partial), `needs_input`, `cancelled`, `pager_limit`.

**Human keystrokes are tagged `h`.** The hook is a tap on
`term_keyinput_internal()` — the single funnel for typing *and* pasting, which
the terminal's own generated replies bypass — so an agent can tell "the device
did this" from "the person did this". A human keystroke during a `run` cancels
it, so the two do not drive the same stream at once.

**The keystrokes are deliberately not written into the buffer.** The device's own
echo is the record of what was typed. Writing them *and* keeping the echo records
`ls` as `llss` (the echo of each character lands inside the line just written);
writing them and trying to *drop* the echo puts **a password in the transcript**,
because a device does not echo a password and nothing would suppress it. So a
password the human types never appears at all.

### What building 4b found

Six more bugs, five of them found only by testing — and three of those only by
running against a real pty, which is the argument for doing it:

1. **A slow command completed early, with no output.** `sleep 1; echo done` is
   silent for a second, and a bare quiet period called that finished. Quiet alone
   is not enough now: there must be something to judge — a prompt, or at least an
   unfinished line.
2. **`{ ...DEFAULTS, ...options }` clobbered every default.** A caller forwarding
   its optional arguments — `{ timeoutMs: opts.timeoutMs }` — passes `undefined`
   for everything the user did not mention, and the spread copied it over the
   default. The result was not an error but a silence: `quiet >= undefined` is
   never true, so the loop never decided anything, and `elapsed >= undefined`
   disabled the timeout that would have rescued it. The run hung until something
   outside killed it. The unit tests missed it because they called the loop
   directly; only a real caller exposed it. Options are now merged
   undefined-skipping, and the numeric ones checked as positive numbers, because
   `Number("abc")` is `NaN` and every comparison against `NaN` is false.
3. **macOS `more` and `less` do not print `--More--` at the first page break.**
   Given a *file argument* they put the **file name** there, in reverse video —
   which the capture buffer strips, leaving a bare path as the last line — and
   `:` from the second page on. Both were measured with a pty probe rather than
   assumed. Missing them is worse than a hang: the run gives up, the pager keeps
   waiting, and the **next command is eaten by it**, because `more` reads a
   leading `!` as "run a shell command". That is what the first live run did.
4. **The first version of that pattern matched `[y/n]` and `(yes/no)`** — it
   allowed any token containing a slash, and those are the two prompts that must
   *never* be answered on the caller's behalf. The unit tests caught it within
   seconds. Fixed by narrowing the character class to path characters, and by
   moving waiting-for-input ahead of pagers in the precedence order: when in
   doubt, do not type.
5. **The first echo design would have put a password in the transcript**, as
   above. Found by reasoning about the test rather than by the test failing.
6. **A `cur_author` reset in the wrong place** would have re-tagged a client's
   own text as the device's when it contained a Return.

7. **A command wider than the terminal lost most of its echo.** The shell emits
   a **bare carriage return** at every visual wrap so that its own cursor
   bookkeeping stays right — measured with a 305-character command in a
   128-column terminal, the bare CRs arrive after 114 and 244 bytes. The capture
   buffer reads a bare CR as "back to column 0, this line is being redrawn",
   which is what keeps a progress bar to one line, so every row but the last was
   discarded and what survived was the *tail* of the command. `run` then did not
   recognise it as the echo and handed it back as part of the output. Found on a
   real Synology DS220+ by a command long enough to wrap; the smoke test's
   commands were all short, so it had never come up. `run` now also accepts a
   long *suffix* of the command as its echo.

8. **Two `run` calls at once corrupted each other, silently.** `run` is not
   reentrant per session: the commands interleave in the one stream, and each
   run's window — "everything after my own line" — catches the other's echo and
   output. Nothing in the reply says so, and the contaminated answer looks
   perfectly plausible. Found by firing two runs in a `Promise.all` at a real
   Synology NAS, where run A came back with run B's command and output in it.
   The bridge now serialises runs per session, so the second waits.
9. **The output cap was a memory budget, not a context one.** `run` kept up to
   256 KB — about 65k tokens — and `max_output_bytes` was not on the tool at
   all, so a caller could not lower it; `max_bytes` bounds one *read*, not what
   a run accumulates, which is the number that matters. The default is 8 KB
   (about 2k tokens) now, the option is exposed on the tool and the CLI, and
   truncation reports `omitted_lines` so it is actionable. The cap also counts
   each line's newline, so it is a true bound on the string returned.

**Test:** `scripts/test-mcp.sh` (38 checks) starts its own `pterm` against a
throwaway configuration folder and verifies the whole path end to end, including
`run` against a real pty: a slow command waited for rather than cut off, a real
`more` paged through, a stand-in for network gear's own `--More--`, and a
`Password:` prompt stopping the run. It needs a display and touches nothing of
yours. `mcp/test/run.test.mjs` (159 checks) and `mcp/test/protocol.test.mjs` (19)
need neither PuTTY nor a display; `test/test_mcp_buffer.c` (110) and
`test/test_terminal.c` cover the buffer and the input tap.

**Not verified against real network gear.** The `--More--` pattern is covered by
the unit tests, by a real `more`, and by a shell stand-in that emits the real
bytes over a real pty — but not by a router or a switch. Nor has the
human-keystroke path been driven by an actual person at a keyboard: it is
unit-tested at both layers (the buffer and the tap) and not end-to-end.

**The window's own prompts are reported, not guessed (phase 4c, 2026-11-24).**
PuTTY asks for a login password, a key passphrase or a login name by drawing the
prompt **in the session's own window**, and the person at the keyboard answers it
there. Those words look exactly like device output in the transcript, so an agent
could not tell "the device is waiting for me" from "the window is waiting for the
person at the keyboard" — and only the second must never be answered. The session
now reports it, as a **state** rather than a guess at the text:

* `term_prompt_text()` (`terminal/terminal.c`, declared in `putty.h`) reads it
  from the terminal's own `prompts_t`. A prompt the saved-session password
  answered never gets that far, and is never reported — which is the point of
  reading the state rather than the words.
* `sessions` gains `needs_human` and `prompt` per session, and `read` gains
  `needs_human`: **protocol version 2 → 3**, so a mismatched pair says so.
* `run` stops with `completed_by: "needs_human"` — and, the part that matters,
  **sends nothing at all** when the window was already prompting before the run
  started. Before this it typed the command into the prompt and only noticed
  afterwards.

Verified live against a real `login as:` prompt: `putty-mcp list` showed
`[waiting for you: login as: ]`, a `run` returned `needs_human` with the command
unsent, and answering it in the window moved the state on to the password prompt.
The one thing it does not cover: a prompt that is *not* a `prompts_t` — the
host-key confirmation, and the fork's own "Remember Login" offer — is a dialog,
and is still invisible to an agent. See `tech_detail.md` §7.13.

The same change is written for the Windows port (protocol 3, the prompt field on
both replies, `mcp_get_prompt()` in `windows/window.c`) but has **not been built
or tested there**.

## 14. Saved-session passwords

A saved session can keep its login password, so that connecting to it does not
ask for one (design and reasoning: `tech_detail.md` §4k).

* **What is stored is not the password.** The session's file gets
  `SessionPassword=v0;<base64>`, written and read by hand beside
  `SessionComment` (`utils/sesspass.c`). The `v0` tag is the seam: a later
  version will encrypt the password under a master password, and a reader that
  does not know a version says so rather than guessing at it. `v0` keeps the
  password out of sight and off a plain `grep`, and that is all it does — anyone
  who can read the file can decode it, which is why the encryption is the next
  step rather than a detail.
* **The box sits next to the username** (Connection → Data → "Login details"),
  because that is where a user looks for login credentials. It is hidden like
  any password box, holds the password in the clear so it can be corrected, and
  is offered only for SSH.
* **Where it is used.** The GTK seat already asked
  `cmdline_get_passwd_input()` before showing a dialog, so a stored password is
  tried next and only then the interactive prompt. The shape test is the one
  `-pw` uses — one prompt, not echoing, destined for the server — which also
  keeps the password away from a private-key passphrase prompt and from the
  username prompt. It is tried **once per connection**, so a password the server
  rejects falls back to the dialog instead of failing the connection the way
  `-pw` does.
* **A proxy's prompt is not the session's.** An SSH-to-SSH proxy with no
  password of its own forwards its prompt to the client seat
  (`proxy/sshproxy.c`), and it has exactly the same shape — so it is marked
  (`p->from_proxy`) and declined, or the *proxy's* password would be answered
  with the *session's*.
* **The offer to remember** is made after a login that *succeeds*, and it covers
  the **login name as well as the password**: whichever of the two the session
  does not already have is what the offer names and saves. The seat notes the
  password it answered the password prompt with (`sesspass_used`) *and* the name
  it answered the `login as:` prompt with (`ssh_username_used`);
  `gtk_seat_notify_session_started()` queues a toplevel callback (a modal dialog
  must not go up in the middle of packet processing), and Yes saves the
  session's file the way Save does, with the new fields filled in. A login name
  typed at the prompt replaces the session's Auto-login username; a password
  that is already saved is left alone. `CONF_session_name` records which saved
  session the configuration came from, so there is somewhere to save it to; an
  empty name, or "Default Settings", is skipped.
* **The username prompt's shape test** is the mirror image of the password's: a
  single prompt, to the server, but *echoing* (a login name is not secret) and
  from the local client rather than the server, so a single echoing
  keyboard-interactive question cannot be mistaken for it. Both SSH-1 and SSH-2
  name their prompt set `"SSH login name"`, which is the final check. It matters
  that the offer fires on the *username* alone: once the password is saved, the
  login name was the one thing still asked for on every connection. Note that
  `CONF_username` is `CONF_TYPE_STR_AMBI`, so it is read with
  `conf_get_str_ambi()` — `conf_get_str()` asserts on it.

## 15. Transparent background

Window → Colours → **"Transparency (0 = solid, 255 = see-through)"**
(`CONF_bg_transparency`; design and reasoning: `tech_detail.md` §4l). It applies to the
*default* background colour only, so a highlighted line, a coloured `ls` entry
and the cursor stay solid and readable. `bg_alpha()` maps the setting to a cairo
alpha, and the value is clamped in `cache_conf_values()` because the edit box
does not range-check it.

Two things went wrong on the way, and both are worth knowing about.

### The title bar has to be drawn by GTK, or it goes see-through

A compositor will not blend against a window that insists on being opaque, so a
transparent background needs an **RGBA visual** on the window and on the drawing
area, GTK's own background painting turned off, and a backing surface with an
alpha channel.

The catch is the title bar, and it was measured rather than assumed (a temporary
probe read AppKit's own state while the app ran):

| Transparency | `[NSWindow isOpaque]` | `backgroundColor` alpha |
|---|---|---|
| 0 (solid) | 1 | 1 |
| 64 | 0 | **0** |

`titlebarAppearsTransparent` is 0 either way and `styleMask` is unchanged, so it
is not a flag GTK sets: macOS paints a *native* title bar over the window's own
background, and GDK makes that background fully transparent as soon as the
window is non-opaque. A solid title bar and a see-through terminal are therefore
mutually exclusive *through the native bar* — the same coupling section 12
describes for `GtkDialog`, where the fix is to ask for the system visual
instead, which is not available here because the window has to keep its alpha.

So while the background is transparent the window is given a **GTK-drawn title
bar** (`gtk_window_set_titlebar()` — client-side decoration), which GTK paints
from the theme and which therefore stays solid. It deliberately looks different
from the native one — and at transparency 0 none of this applies at all, so the
native bar is untouched.

*`gtk_window_set_titlebar()` does **not** show the bar*: it shows it only if the
window is already visible, and this window is shown with `gtk_widget_show()`
rather than `show_all()`. Without an explicit `gtk_widget_show(bar)` the title
bar never appears at all.

### A translucent fill must *replace*, not blend

The first cut painted the background with `cairo_set_source_rgba()` and the
default `OVER` operator, which lays the new frame *over* the old one instead of
replacing it. Two visible consequences, the first reported as "the text I typed
is highlighted":

* **A ghost of the cursor.** The cursor is drawn as a filled block in the cursor
  colour, so every character typed while the cursor sat on it kept a 25% tint of
  that colour — and the cell's own repaint is that same translucent fill, so
  nothing ever cleared it. With the cursor colour `(0,255,0)` and transparency
  64 (75% opaque), the ghost is exactly `(0,64,0)`: measured, not guessed.
* **The background was blended twice** per frame (the full-surface erase, then
  the per-cell fill), so a 64 setting came out 94% opaque rather than 75%.

The fix is one flag: while the *translucent default background* colour is
current, `draw_rectangle()` fills with `CAIRO_OPERATOR_SOURCE` — replacing the
pixels, alpha included — and then returns to `OVER`. Everything else still
blends as before, so glyphs, the cursor block, selections and coloured
backgrounds are untouched, and the full-surface erase (`draw_backing_rect()`)
is now a true erase, which is what guarantees a stale frame cannot survive.

Both of these were found by **rendering the terminal's backing surface
offscreen** — `cairo_surface_write_to_png()` from a temporary probe, composited
over magenta to make the alpha visible — and looking at the image. No screen
capture, and the same trick is what showed the title bar going missing.

**On Windows there is no equivalent, and the feature was dropped.** Windows offers
no per-pixel alpha for a window that GDI paints into, so the four routes that were
built and measured are all unusable: the DWM frame extension composites against an
opaque white backdrop on Windows 8 and later, not the desktop; `UpdateLayeredWindow`
does give true per-pixel alpha but *replaces* the window, taking the native title
bar with it; a colour key keeps the frame and the crisp text but is binary, so the
background becomes 100% see-through and the 0–255 amount means nothing (64 and 255
behave identically); and a uniform `LWA_ALPHA` fades the text and the title bar too.
`CONF_bg_transparency` and its Window → Colours control are still shared and still
appear in the Windows configuration box, where they do nothing. The working
colour-key build was verified pixel-for-pixel and then reverted; the measurements
are in `tech_detail.md` §10.

## 16. Which monitor a new session window opens on

`new_session_window()`, plus one hook line in the configuration box
(`unix/dialog.c`).

macOS places a new window at the origin of the monitor whose frame starts at
`(0,0)`, and that is not necessarily the primary one. On the two-monitor Mac
this was written on, the primary display is at `1920,0`, so every session window
opened at `0,30` on the *other* screen while the configuration box sat at
`2518,194` on the primary — the same wrong screen every time, which is what the
user reported.

* `gtk_remember_window_monitor()` (`unix/window.c`, called from the session
  windows' own configure handler and from `config_box_configure()` in
  `unix/dialog.c`) records the geometry of the monitor of the last PuTTY window
  that was moved or resized.
* `new_session_window()` uses it when the session has no position of its own,
  centring the window on that monitor — so a session opened from the box lands
  on the box's screen, and one asked for from a window on the second screen
  lands there too.
* Nothing remembered — the first window of a process, or `putty -load` from a
  shell — means the **primary** monitor.
* A window bigger than the monitor is clamped to its top-left corner rather than
  given a negative offset. `PUTTY_DIAG=1` prints the monitor, the window size
  and the final position.

Hoisted out of the way: on macOS `NOT_X_WINDOWS` is defined, so the
`-geometry` handling above (which parses the string with `XParseGeometry`) is
compiled *out* — `inst->gotpos` is never set on macOS and **`-geometry` has no
effect at all there**, neither position nor size. That is upstream's, and this
change deliberately leaves it alone: the new branch is the `else` of
`if (inst->gotpos)`, so if `-geometry` is ever made to work, an explicit position
still wins.

The measurements behind that, what does *not* change where a window lands, and
the recipes for checking it are in
[`tech_detail.md` Appendix D](tech_detail.md#appendix-d-macos-and-gtk-what-they-do-that-you-would-not-expect).

## 17. PuTTYGen got a GTK front end (and a second `.app`)

Upstream's GUI PuTTYgen is Windows-only (`windows/puttygen.c`); on Unix
`puttygen` is the command-line tool in `cmdgen.c`, and no GTK keygen target was
ever written. This fork ports the GUI, so macOS now gets a **`PuTTYGen.app`**
beside `PuTTY.app`.

### New code

| File | What |
|---|---|
| `unix/puttygen.c` | The window. Key type (Ed25519, Ed448, ECDSA P-256/384/521, RSA, DSA), bit count or curve, prime-generation policy and "strong RSA primes"; Generate on a `GThread` with a progress bar and a Cancel that really stops the generator; the key box (comment, passphrase, fingerprint SHA-256/MD5, the `authorized_keys` line); Open Key; Save private key; Save public key; Export OpenSSH key; Export ssh.com key; the passphrase prompt; the `.ppk` v3 parameters dialog (Argon2 flavour, memory, time or passes, parallelism); About and Licence |
| `unix/main-gtk-puttygen.c` | The `GtkApplication` wrapper: the menu bar (File / Edit / Key / Help), the app id `org.tartarus.projects.putty.macputtygen`, and the `$HOME` chdir |

Nothing cryptographic is re-implemented: `sshkeygen.h`'s generators, `import.c`,
the `ppk_*` reader/writer in `sshpubk.c`, Argon2 and `licence.h` are used as they
are. The command-line `puttygen` is untouched, and it is the oracle the GUI is
tested against (`scripts/test-puttygen-gui.sh`, 33 checks).

### The build split

`puttygen-common` used to contain `no-gtk.c` — the file whose entire purpose is
to tell `buildinfo()` that the program has no GTK. The GUI has GTK, so that file
moved out into the CLI targets (`puttygen`, `cgtest`) and `puttygenapp` is a new
target beside them, gated on `GTK_VERSION GREATER_EQUAL 3`. It links `keygen`
(which is where `import.c` lives), `crypto`, `utils` and the `puttygen-common`
objects — deliberately **not** `guiterminal`, because the one thing it wanted
from the terminal front ends, `our_file_chooser_run()`, actually lives in the
`utils` library (`unix/utils/filechooser.c`). Three symbols the terminal front
ends pick up from `console.c`/`dialog.c` have GUI versions here instead:
`modalfatalbox`, `nonfatal` and `old_keyfile_warning`.

### Six things that only show up when you run it

1. **`osxlaunch` appends an argument.** It is the unused environment-variable
   prefix the terminal front ends use to restore a clean environment for their
   shell subprocesses (`pty_osx_envrestore_prefix` in
   `main-gtk-application.c`). PuTTYGen has no subprocesses; but the argument has
   to be consumed all the same, or `GtkApplication` reads it as a file name and
   refuses to start: *"This application can not open files."*
2. **The random number generator is created once, at startup.** Two crashes
   with the same root cause. `random_setup_special()` ends in `random_create()`,
   which asserts that no PRNG exists yet — so calling it before *every* key
   generation aborted the process on the **second key**. And with the PRNG
   created only by generation, saving a key that had been *loaded* (rather than
   generated) tripped `assert(random_active > 0)` for the opposite reason. It is
   now created once in `puttygen_window_new()`, which is where
   `windows/puttygen.c` does it (`WinMain`); generation only calls
   `random_reseed()` with fresh `/dev/urandom` bytes, and saving a key just
   draws from it. No `random_ref()` anywhere. `PUTTYGEN_SELFTEST_REPEAT=<n>`
   generates n keys in one process so this stays fixed, and
   `scripts/test-puttygen-gui.sh` runs it as a check.
3. **`import_possible()` is not "can we open this file".** It answers only "is
   this a foreign format the import machinery converts", so it is false for
   PuTTY's own `.ppk` and for public-key-only files. Using it as the guard
   rejected every native `.ppk` with *"Couldn't load private key (PuTTY SSH-2
   private key)"*. The natively-loadable types are listed explicitly now.
4. **`gtk_widget_show_all()` unhides the controls you just hid.** It makes every
   child visible, including the controls belonging to other key types (bit
   count, curve picker, prime policy) and the widgets that belong to the
   "generating" state. The state is applied *after* `show_all()`; before that
   fix an Ed25519 window still displayed a bit count and a prime policy.
5. **Every dialog needs the opaque-title-bar fix.** Same macOS behaviour as
   section 12: a real `GtkDialog` gets an RGBA visual and a see-through title
   bar. The message boxes, the passphrase prompt and the parameters dialog all
   call `gtk_dialog_fix_macos_titlebar()`; About and Licence are built as plain
   `GtkWindow`s with `our_dialog_new()`, exactly as `unix/dialog.c` builds them.
6. **A replaced key has to be let go of, and a button label is literal.**
   Generating a second key, or loading a key over a generated one, used to
   overwrite `st->ssh2key.key` and leak whatever was there;
   `release_current_key()` now frees the old key first — but only when it is a
   heap key from `ppk_load_f()`/`import_ssh2()`, because a *generated* key lives
   in the `keys` union and only its internals are heap allocated (recorded in
   `st->key_in_union`). And `gtk_button_new_with_label()` takes its text
   literally — only `..._with_mnemonic()` parses `_` — so the button read
   `_Generate` until the underscore was dropped; `Generate` and `Cancel` are
   plain labels now. (The chooser's `_Cancel`/`_Open` are fine: those go through
   `gtk_dialog_add_button()`, which does parse mnemonics.)

### Packaging

`unix/puttygen.bundle` + `unix/puttygen.plist` are the second bundle
(`CFBundleName`/`CFBundleExecutable` `PuTTYGen`, its own icon and identifier),
and `scripts/bundle-macos.sh` was rewritten to loop over an app list —
`<app>:<bundle>:<binary>:<icon>`, overridable with `APPS=` — with every repair
pass (Cellar→opt, dangling references, `loaders.cache`, GSettings schemas,
re-sign, verify) turned into a function taking the app path. `BUILD_DIR` and
`STAGE` can now be overridden, which is how a bundle gets assembled on local
disk instead of over a network mount. The launcher needed no change:
`osxlaunch` derives everything from its own path and execs `<own path>-bin`, so
the same binary serves `Contents/MacOS/PuTTY` and `Contents/MacOS/PuTTYGen`.

The icon comes from a PNG, like PuTTY.app's: `icons/puttygen-icon.png` is
converted to `icons/PuTTYgen.icns` by `scripts/make-icns.sh` (`sips` +
`iconutil`), exactly as `icons/putty-beter-icon.png` produces `icons/PuTTY.icns`.
The upstream drawing route — `icons/mkicon.py`'s `puttygen_icon()` wrapped by
`icons/macicon.py` — stays in `icons/Makefile` as the fallback for when the PNG
is absent, and `bundle-macos.sh` overwrites its output whenever the PNG is
present, so a stale `mkicon`-derived `.icns` cannot reach a bundle.

**The source PNG must be square, with the artwork to the edges.** macOS scales
the whole canvas to draw a Dock icon, so a transparent margin makes the finished
icon smaller than its neighbours — and the artwork's own light background then
reads as an extra frame that the application never drew. Measured with a
full-bleed test icon (which macOS renders with no frame at all): the first
version of `icons/puttygen-icon.png` had an 11–13% transparent margin and came
out visibly undersized. `scripts/make-icns.sh` now refuses a non-square source,
because `sips -z N N` would otherwise stretch it silently.

### Deliberately not ported

`SSH-1` keys, and OpenSSH certificates (add / remove / cert-info). Both are
whole dialogs' worth of UI for things that are dead or rare on macOS, and the
command line keeps them — `cmdgen.c` still has `-t rsa1`, `--certificate`,
`--remove-certificate` and `--cert-info`.

## 18. The fork has its own version, and every About box links home

A build of the vanilla tree announces itself as `Release 0.85` or, in a bundle,
as `Unidentified build`, which tells you nothing about which fork you have.
Both are now `0.85-beter.1`.

* `version.h` — `RELEASE`, `TEXTVER` (`Release 0.85-beter.1`) and `SSHVER`
  (`-Release-0.85-beter.1`) carry the fork version, and `BINARY_VERSION`'s build
  field is `1`. `TEXTVER` is what the About box and every `--version` prints
  (`utils/version.c`).
* `LATEST.VER` is `0.85-beter.1`. (Upstream's own release tooling, `release.pl`,
  rewrites this file only when it matches `^\d+\.\d+$`, so it will not touch the
  fork string; that tooling is not used here in any case.)
* The two `Info.plist`s set `CFBundleVersion` and `CFBundleShortVersionString` to
  `0.85-beter.1`, so Finder and `mdls` show it too. Strictly, Apple wants
  `CFBundleShortVersionString` to be no more than three integers, which this is
  not; the only effect is an App Store / notarisation validation warning, and
  neither applies to an ad-hoc-signed DMG.
* **`SSHVER` changes the wire banner.** `ssh/verstring.c` builds
  `SSH-2.0-PuTTY…` from the prefix, the implementation name and `SSHVER`, and
  turns `-` into `_`, so servers now log `SSH-2.0-PuTTY_Release_0.85_beter.1`
  instead of `…_0.85`. (The 40-character banner limit in `utils/version.c` still
  holds: 33 characters.)
* **Every About box links home.** `About PuTTY` (`unix/dialog.c`) and
  `About PuTTYgen` (`unix/puttygen.c`) both gain a `GtkLinkButton` reading
  `github.com/beterhans/putty-beter`, opening
  `https://github.com/beterhans/putty-beter` (the URI and its label are the two
  macros in `unix/gtkmisc.h`). Nothing in either box named the fork before — the
  version and `buildinfo` text were plain and non-clickable, and GTK markup is
  off on that label, so the URL could not simply be added to it.
* **The copyright is now two labelled lines.** Each About box reads
  `Original author: Copyright 1997-2026 Simon Tatham. All rights reserved`, and
  directly below it `This fork: Copyright 2026 beterhans. All rights reserved`.
  The first line is still the string generated from `LICENCE` by
  `cmake/licence.cmake` (`SHORT_COPYRIGHT_DETAILS`), so the upstream MIT notice
  is preserved verbatim and in place; the second is
  `PUTTY_BETER_COPYRIGHT_HOLDER` in `unix/gtkmisc.h`. Both plists'
  `NSHumanReadableCopyright` carries the same two lines (separated by `&#10;`),
  which also replaces the old `1997-2015` year.

PuTTYGen's About box also picked up a small fix while it was being touched: it
now clears the selectable label's initial select-all
(`gtk_label_select_region(label, 0, 0)`, exactly as `unix/dialog.c` already did),
so it no longer opens as a solid blue highlight.

**On Windows** the same two things now hold for `putty.exe`
(`windows/dialog.c`, `AboutProc()`): the About box reads the same two copyright
lines — upstream's, then `This fork: Copyright 2026 beterhans. All rights
reserved` — and its `Visit Web Site` button opens
`https://github.com/beterhans/putty-beter` instead of upstream chiark. The two
macros are repeated in `windows/dialog.c`, since `unix/gtkmisc.h` is Unix-only.
The fork's **icon** replaces upstream's `windows/putty.ico` too, generated from
`icons/putty-beter-icon.png` by the new `scripts/make-ico.py` — the Windows
counterpart of `scripts/make-icns.sh`.

## 19. The MCP bridge ships inside PuTTY.app

An agent drives PuTTY through `mcp/`, a Node program that speaks MCP on stdio and
talks to a *live* PuTTY window over a Unix socket (section 13). The app now carries
a copy of it, so the app is self-describing: `scripts/bundle-macos.sh` installs
`mcp/` into every `PuTTY.app` it builds, on every run — including a run that
reuses an existing tree.

It lands at `Contents/Resources/putty-mcp`, with a symlink to it from
`Contents/MacOS/putty-mcp`, so the path to configure is the obvious
`.../Contents/MacOS/putty-mcp/bin/putty-mcp.mjs` (and the note is at
`.../putty-mcp/SETUP.txt` next to it). **The symlink is load-bearing.** macOS
treats everything under `Contents/MacOS` as nested *code*: measured, a bundle with
an unsigned non-code file there fails to seal — `codesign` says *"code object is
not signed at all"* — and `codesign --verify --deep --strict` then reports *"a
sealed resource is missing or invalid / file added"*. Files under
`Contents/Resources` are covered by the bundle seal and need no signature of their
own, and `codesign` ignores a symlink in `MacOS`. (The alternative — signing each
bridge file individually — survives neither the merge's `xattr -cr` nor a plain
copy, so it is not used.)

**Its dependencies are bundled with it**, so the app needs nothing installed:
`import '@modelcontextprotocol/sdk'` resolves by the ordinary walk-up into
`Contents/Resources/putty-mcp/node_modules`, with no absolute paths and no global
packages. That is ~3400 files and 23 MB, and it is acceptable precisely because
they are pure JavaScript — measured: 0 `.node` files, 0 Mach-O anywhere in the
tree, no `os`/`cpu`-constrained packages — so the arm64 and x86_64 copies are
identical and one copy serves both architectures in the universal build.

`MCP_NODE_MODULES=0` builds an app *without* them; `make-universal.sh` then adds
them to the merged app from the shared `mcp/node_modules` if no slice carried them
(it says so, rather than shipping a bridge that cannot load).

**The x86_64 build carries no bridge at all** (2026-11-21). `bundle-macos.sh` skips
it when `ARCH=x86_64`, so that bundle is 275 files per app, against arm64's 3703.
The reason is not size: `mcp/node_modules` contains a symlink (npm's
`.bin/node-which` shim), and the Intel Mac reads this tree over SMB, where **a
symlink cannot be read at all** — `readlink`, `stat -L`, `open` and `cp -L` all
fail with `EINVAL`, and only `lstat` works — so no copy tool can reproduce one, and
`ditto` aborted the whole bundle run
(`ditto: …/.bin/node-which: Invalid argument`). Special-casing that one link was not
worth it, because the x86_64 bundle is a merge input only: `make-universal.sh` takes
the **arm64** bundle as its structural authority, so the merged app still gets the
bridge and its dependencies, from the arm64 slice. One consequence: the merge must
run where symlinks are readable — the **arm Mac**.

`mcp/SETUP.txt`, installed beside the code, states the result: Node 20 or newer is
all a user needs. `MCP_APP=` (default `PuTTY`) chooses which app gets the bridge;
`PuTTYGen.app` has no MCP server, so it gets none.

## 20. The window is held open when a session ends ("a readable exit")

**What and why.** When a session ends and `CloseOnExit` says the window goes,
`exit_callback()` (`unix/window.c`) destroyed it at once, so the last thing the
session printed — normally the reason it dropped — was unreadable. It is now held
for a **countdown**, two seconds by default, with a line under the terminal
saying so; any keystroke or click cancels the countdown and leaves the window
open, and a later one closes it as before. Design of record: `tech_detail.md` §4n.

**New setting.** `conf.h`:

```c
CONF_OPTION(close_on_exit_delay,
    VALUE_TYPE(INT), /* in seconds */
    DEFAULT_INT(2),
    SAVE_KEYWORD("CloseOnExitDelay"),
)
```

Plain `INT` with a `SAVE_KEYWORD`, so `settings.c` saves and loads it
automatically. **0 means "close at once, exactly as upstream"**, which is what
keeps the feature honest for anyone who does not want it — and what lets a
session that drops often be set to skip the wait.

**The control** is in the Session panel, beside the *Close window on exit* radio
buttons (`config.c`), as a `ctrl_editbox` with `ED_INT` — the same shape as
*Lines of scrollback*. It is a per-session setting like the rest of that panel.

**pterm does not get it.** `pterm` shares `unix/window.c`, so it would have
inherited the two seconds with no way to switch them off — no configuration
panel, no saved sessions. `platform_default_i()` therefore returns **0** for
`CloseOnExitDelay` when `saved_sessions` is false, which leaves `pterm`'s
behaviour exactly upstream's: measured 0.20 s to run and close `pterm -e …`,
against 2.56 s for the same with `putty`. `plink`, `pscp` and `psftp` link no
window code at all. (`platform_default_i()` is reached for both tools because
`unix/main-gtk-simple.c` calls `do_defaults()` unconditionally; the name it is
handed is the save keyword, `"CloseOnExitDelay"`.)

**The countdown** is a GLib timeout (`g_timeout_add`, one second per tick), never
a blocking sleep, so the window keeps painting while it counts. It is a `GtkLabel`
that `unix/window.c` packs **below** the terminal in a `GtkVBox`, hidden until it
has something to say: writing the countdown into the terminal itself would disturb
exactly the text the feature exists to show. The tick destroys the window when the
count reaches zero; a keypress (`key_pressed()`), or a mouse press
(`button_internal()`), cancels it. `delete_inst()` removes the timeout before
freeing the `GtkFrontend` it is the user data of, and the tick clears the id
before destroying so that path cannot double-remove the source. Restarting the
session from the menu clears the line again.

Upstream's GTK behaviour — any keypress closes a window whose session has exited —
is only *deferred*: the first keypress cancels the hold, the next closes.

**What the tests found** (arm64 build, macOS 27, 2026-11-22; the trace under
`PUTTY_BETER_DEBUG=exit` was the instrument):

| case | result |
|---|---|
| default (2 s), clean end of session | held 2 s, counted down, closed — 2.64 s wall clock from launch |
| `CloseOnExitDelay=0` | `no delay: destroying the window now`, 0.55 s — upstream's behaviour |
| `CloseOnExit=Never` | unchanged: no trace, no close |
| `pterm -e …` (no settings UI) | unchanged: `delay 0`, `no delay`, 0.20 s |
| `CloseOnExitDelay=5` in a session file | loaded and honoured (`delay 5`), so the keyword round-trips |
| a keystroke during the countdown | cancelled, window stayed open (4 s were left) |
| a second keystroke | closed the window, as upstream does |
| `johnny@S-D220.local` (rejected) | the banner and `Permission denied, please try again.` held on screen |

The last row is the case from `tech_detail.md` §4n, and the SSH log is worth recording:
`plink -v` shows **`Access granted`**, `Opening main session channel`,
`Opened main channel`, `Started a shell/command`, and only then
`Permission denied, please try again.` / `Session sent command exit status 1`. So
the password is accepted and the *account* is refused at the session level; the
window closes non-cleanly (exit code 1) on the platform default `CloseOnExit=2`.
That is also why the fork's "Remember Login" offer (§14, `tech_detail.md` §4k) appears on
this connection: it is triggered by `notify_session_started`, which fires when the
main channel opens — before the server refuses the session. See §4n's "As built"
for that, and for what the code should do about it.

**In the shipped app** (built and merged on the arm Mac, 2026-11-22). Both slices of
the universal `PuTTY.app` contain the countdown, so it is there whichever Mac runs
it: `lipo -thin` each slice of `Contents/MacOS/PuTTY-bin` and `strings` finds the
line in both, and the installed `/Applications/PuTTY.app` (3705 files, seal valid)
shows the new Session box with its value of 2. The bundled app takes no session on
the command line — it is the single-instance `puttyapp` front end, which rejects
`-load`/`-raw` — so the countdown itself was driven from the same arm64 binary
through `build-arm64/`, and the image is verified by mounting (`tech_detail.md` §4m, "As
run again on the arm Mac (2026-11-22)").

## Worth reporting upstream

1. **`OSX_GTK` is never defined by the CMake build**, so every macOS-specific
   code path in `unix/platform.h` is compiled out on macOS. This disables
   clipboard support and Command-key handling in any CMake-built macOS PuTTY.
2. **IME pre-edit out-of-bounds read and cursor offset**: an empty pre-edit
   string leaves a zero-width pre-edit line, after which `do_paint()` reads
   `chars[-1]` and places the cursor one cell to the left.
3. **Ctrl+Ins/Shift+Ins cannot work on macOS** as written, because the code
   matches `GDK_KEY_Insert` and macOS reports that key as `GDK_KEY_Help`.
4. **Dialogs cannot be pasted into on macOS**: GTK's copy/paste bindings are
   Control-based and nothing handles Command.
5. **`-geometry` does nothing on a `NOT_X_WINDOWS` build.** The size and
   position are parsed inside `#ifndef NOT_X_WINDOWS` in `unix/window.c`, using
   `XParseGeometry`, and `NOT_X_WINDOWS` is defined on macOS — so
   `putty -geometry 80x24+100+100` is accepted and then silently ignored, size
   included. A parser using `sscanf` would be a dozen lines, but the position
   and gravity handling around it is X11-specific (`XValue`, `XNegative`, and
   `get_monitor_geometry` at window-creation time), so it is a change worth
   making deliberately rather than incidentally.

## Deliberately not changed

* Still GTK 3, as upstream. Only AppKit would be "universal" without bundling
  every dependency, and that is a rewrite, not a fork.
* No *writing* to a session from MCP yet, and no master password on the saved
  session passwords yet: `v0` wraps the password, it does not encrypt it — see
  `tech_detail.md` §4k.
* Copy-on-select (`CLIPUI_DEFAULT_AUTOCOPY`) is still off, matching the macOS
  Human Interface Guidelines.
