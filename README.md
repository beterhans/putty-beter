# Putty-Beter

A fork of [PuTTY](https://www.chiark.greenend.org.uk/~sgtatham/putty/) 0.85 — version **`0.85-beter.1`** — that works properly on macOS, adds an **MCP server so an AI agent can read and drive a live session**, ZMODEM file transfer, and better session management. Ships for **macOS** (arm and intel) and **Windows** (x86_64).

MIT licensed (`LICENCE`). Everything not listed here is stock upstream PuTTY.

Platforms used below: **mac arm** = macOS on Apple Silicon, **mac intel** = macOS on Intel CPUs (`x86_64`), **windows x86_64** = the Windows port (x86_64 only).

---

## 1. What is different from regular PuTTY

| Feature | Notes |
|---|---|
| **MCP server** | An AI agent (OpenCode, Claude Desktop, …) reads a live session's output, types into it, and can run a command that waits for the device to finish — including answering `--More--` pagers. Opt-in per session: *Session → Automation (MCP)*. Off by default, no socket at all until a session is offered. §3 |
| **ZMODEM `sz`/`rz`** | File transfer in both directions, over SSH and serial, with a progress window. MIT-licensed engine written in-tree (no GPL) |
| **Session folders** | Save a session as `Work/beta` → real directory under the config folder; expandable tree, drag sessions into folders |
| **Session comments** | Free-text note per session (UTF-8, stored in the `.conf`) — "prod box, ask Dana before touching" |
| **Session search** | Type `/` + text in the Saved Sessions box: matches name, host and comment; load straight from the filtered list |
| **Saved passwords** | A session can remember its login password and user (wrapped, not encrypted — v0; one attempt per connection, falls back to the prompt) |
| **Relocatable config folder** | Config lives in a plain folder you pick (windows x86_64: folder instead of the registry); `config-location.conf` names it, so one config carries between Mac and PC. Hold Option (⌥) on launch to move it again |
| **Readable exit** | Window is held open for a short countdown (default 2 s) when a session ends, so the reason it dropped can be read; per-session setting, 0 = upstream |
| **Changed defaults** (new sessions only) | Logging on by default and appending, 128×36 window, `PT Mono 14` font + CJK wide font (mac), teal palette, keepalives 120 s, IPv4, Ctrl-H backspace |
| **mac: working clipboard** | Upstream's CMake build never defined `OSX_GTK`, so macOS silently built the X11 code paths. Fixed: `Cmd+C`/`Cmd+V`, `Ctrl+Ins`/`Shift+Ins`, copy/paste in dialogs, plus the IME cursor-offset bug with its out-of-bounds read |
| **mac extras** | Finder drag-and-drop inserts a file's path; `PuTTYGen.app` GUI key generator (upstream builds only the CLI on Unix); see-through background (0–255, default colour only); new windows open on the monitor you're working on |

windows x86_64 builds the same shared features (session records, ZMODEM verified byte-for-byte, MCP over a named pipe, close countdown, defaults); its PuTTYGen is the native upstream GUI. Background transparency was attempted on Windows and dropped — no good solution (`tech_detail.md` §10).

---

## 2. Building

### mac arm

```sh
brew install cmake ninja pkgconf gtk+3 adwaita-icon-theme   # adwaita: Homebrew's gtk+3 lacks the icon set
./scripts/build-macos.sh arm64        # binaries (putty, plink, pscp, psftp, puttygen, …) -> build-arm64/
./scripts/bundle-macos.sh arm64       # PuTTY.app + PuTTYGen.app -> build-arm64/bundle-stage/unix/
```

### mac intel (x86_64)

Needs an **Intel Homebrew in `/usr/local`** — native on a mac intel machine; on mac arm it is the Rosetta copy (the `/opt/homebrew` one cannot build x86_64):

```sh
brew install cmake ninja pkgconf gtk+3 adwaita-icon-theme   # the /usr/local brew
./scripts/build-macos.sh x86_64      # -> build-x86_64/
./scripts/bundle-macos.sh x86_64     # -> build-x86_64/bundle-stage/unix/*.app
```

### Universal mac app + DMG (run on the arm Mac)

Merges the two slices into universal `.app`s and writes a disk image:

```sh
./scripts/make-universal.sh          # -> build-universal/PuTTY.app, PuTTYGen.app + build-universal/PuTTY-mac-universal.dmg
```

### windows x86_64 (run on Windows; a cross-build is fine)

```powershell
.\install-toolchain-win.ps1    # once: Git, CMake, Ninja, MSYS2 x86_64 (UCRT64)
.\scripts\build-windows.ps1    # -> putty-beter-win64\putty.exe + putty-mcp\ bridge beside it
```

One binary for **Windows 7 SP1 → 11**. Details, switches and troubleshooting: `WINDOWS-BUILD.md`.

**Build notes**

- The `.app` bundles need `gtk-mac-bundler` (a separate **GPL** project, so not vendored):
  `git clone https://gitlab.gnome.org/GNOME/gtk-mac-bundler.git tools/gtk-mac-bundler` — only for the `.app`s, not the bare binaries. See `tools/README.md`.
- The **mac intel** bundle deliberately carries no MCP bridge (an npm symlink inside `node_modules` cannot be read over the SMB share this tree lives on). `make-universal.sh` takes the bridge from the mac arm slice, so the universal app is complete — but the merge must run where symlinks are readable, i.e. on the arm Mac (`intel_arm_merge.md`).
- Apps/binaries are ad-hoc signed, not notarized: right-click → **Open** after download.
- Debug switches (all no-ops when unset): `PUTTY_DIAG=1` (events/clipboard/cursor), `PUTTY_CONFIG_DEBUG=1` (config folder), `PUTTY_MCP_DEBUG=1` (MCP), `PUTTY_SESS_SEARCH_DEBUG=1`, `PUTTY_SESSPASS_DEBUG=1` (never prints the password).

---

## 3. MCP — where the code is, and how to point OpenCode at it

```
OpenCode ──stdio MCP (JSON-RPC)──> putty-mcp (Node bridge, mcp/) ──socket/pipe──> Putty-Beter
```

- **C server** compiled into the front end: `unix/mcp.c` (mac), `windows/mcp.c` (Windows).
- **Node bridge** — all the MCP and JSON: `mcp/` (`bin/putty-mcp.mjs` + `src/*.mjs`). The socket protocol is deliberately dumb (length-prefixed fields, no JSON on the C side).

**Turn it on per session**: *Session → Automation (MCP) → Offer this session to an MCP client* (+ optionally *"Also allow an MCP client to type into this session"*), Save, then reopen the session (settings are read when the window opens).

**Where the bridge code lives in a finished build** (needs **Node 20+**, nothing else — dependencies travel with it):

| Platform | Bridge (run `node <path> mcp`) |
|---|---|
| mac arm / universal | `/Applications/PuTTY.app/Contents/Resources/putty-mcp/bin/putty-mcp.mjs` — bundled with its `node_modules`; the app is self-sufficient, and a symlink makes it reachable at `Contents/MacOS/putty-mcp` too |
| mac intel (x86_64) | not in the x86_64 bundle alone (§2); use the universal app, or a source checkout |
| windows x86_64 | `putty-beter-win64\putty-mcp\bin\putty-mcp.mjs` — staged beside `putty.exe`, dependencies installed on Windows; the folder's `SETUP-WIN.txt` has this machine's exact paths |

**Add it to OpenCode.** macOS — in `~/.config/opencode/opencode.json`:

```json
"putty-mcp": {
  "type": "local",
  "command": ["node", "/Applications/PuTTY.app/Contents/Resources/putty-mcp/bin/putty-mcp.mjs", "mcp"]
}
```

windows x86_64 — same object, with the paths from `SETUP-WIN.txt` (Windows paths need doubled backslashes in JSON):

```json
"putty-mcp": {
  "type": "local",
  "command": ["C:\\Program Files\\nodejs\\node.exe", "C:\\putty-beter-win64\\putty-mcp\\bin\\putty-mcp.mjs", "mcp"]
}
```

Restart OpenCode afterwards. From a **source checkout** instead: `cd mcp && npm install` once, then point the command at `<checkout>/mcp/bin/putty-mcp.mjs` (and rebuild — the server is compiled into the front end).

The bridge doubles as a plain CLI — no LLM in the loop, the first thing to reach for when something is wrong:

```sh
node .../putty-mcp.mjs list                  # sessions Putty-Beter is offering
node .../putty-mcp.mjs read 1                # output, with line ids
node .../putty-mcp.mjs screen 1              # the visible screen
node .../putty-mcp.mjs run 1 'show version'  # send, wait for the device to finish
```

How it works (wire protocol, capture-buffer semantics, socket location and permissions): `tech_detail.md` Appendix F. The manual the agent is given: `mcp/USER-MANUAL.md`.

---

## 4. References

| Document | What it is |
|---|---|
| [`CHANGES-FROM-UPSTREAM.md`](CHANGES-FROM-UPSTREAM.md) | Every difference from upstream PuTTY 0.85, file by file |
| [`tech_detail.md`](tech_detail.md) | The technical detail per feature: Appendices — F = MCP internals, E = ZMODEM protocol (byte-exact), D = macOS/GTK surprises |
| [`WINDOWS-BUILD.md`](WINDOWS-BUILD.md) | The windows x86_64 build: steps, switches, timings, troubleshooting |
| [`intel_arm_merge.md`](intel_arm_merge.md) | Handing the mac intel slice to the arm Mac for the universal merge + DMG |
| [`PLAN.md`](PLAN.md) | Plan, phases and results — one row per feature, mac / Windows side by side |
| [`mcp/README.md`](mcp/README.md) · [`mcp/USER-MANUAL.md`](mcp/USER-MANUAL.md) | The bridge: setup and the manual the agent is given (served to clients as `user_manual` / `putty://manual`) |
| [`tools/README.md`](tools/README.md) | Why `gtk-mac-bundler` is not vendored, and how to fetch it |

The version string `0.85-beter.1` appears in the About box on both platforms, alongside upstream's copyright and this fork's own line; it links to <https://github.com/beterhans/putty-beter>. Build output (`build-*/`) and the `gtk-mac-bundler` checkout are git-ignored.
