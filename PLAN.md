# putty-beter — Development Plan

**Status.** In development on two platforms. macOS is the origin: the fork builds
and runs there, and its features are done except the universal signed `.app` and
the later MCP phases. Windows is a **port**: an x86_64 `putty.exe` builds and runs,
and of the seven Windows batches (**W1–W7**) **W1 (MCP)**, **W2** (ZMODEM `sz`/`rz`,
both directions), **W3** (the close countdown, the multi-line comments box, the
remembered box size), **W4** (the session-folder tree and drag-and-drop),
**W5 (saved-session passwords)** and **W7 (the fork icon and the About box)** are
done — W7 landed on 2026-11-24. **W6 (background transparency) was attempted on
2026-11-24 and dropped: Windows has no good solution** — see
[§10](tech_detail.md#10-background-transparency) of `tech_detail.md` for the four
routes that were built and measured. That leaves the port complete apart from that
one deliberate exclusion.

This file is the **plan**: the tools, the phases, the progress and the results, for
both platforms, **one row per feature**. Everything else — the findings, the
gotchas, the "as built" accounts, the protocol notes — is in
**[`tech_detail.md`](tech_detail.md)**, which every row below links into.

Base: vanilla **PuTTY 0.85**, MIT-licensed.
Repo: <https://github.com/beterhans/putty-beter>

---

## 1. Tools

### macOS — the origin platform

| Tool | Version | Used for |
|---|---|---|
| cmake | 4.5.0 | build |
| ninja | 1.13.2 | build |
| pkg-config (pkgconf) | 3.0.10 | GTK discovery |
| gtk+-3.0 | 3.24.52 | the UI (there is no Cocoa front end) |
| halibut | 1.3 | documentation (`brew install halibut`) |
| perl | 5.34.1 | `licence.pl`, for docs |
| git | 2.54.0 | |
| gtk-mac-bundler | GNOME gitlab `fbc6ffd` | bundling `.app`s — in-project at `tools/gtk-mac-bundler`, git-ignored, GPL-2.0 |
| python3 | 3.9.6 | gtk-mac-bundler, `scripts/test-mcp.sh` |
| node / npm | v26.11.1 / 11.20.0 | the MCP bridge |

Build entry point: `scripts/build-macos.sh` (+ `CMakePresets.json`, `native` and
`universal` presets).

### Windows — an x86_64 cross build, on an ARM64 host

| Tool | Version | Used for |
|---|---|---|
| MSYS2 (x86_64) | — | the toolchain container |
| mingw-w64 **UCRT64** toolchain | — | compiles the x86_64 binary |
| cmake / ninja | native **arm64** | drive the cross build |
| node | v26.13.1 | the MCP bridge |

**Host:** the user's Mac mini / VMware **Windows 11 Pro ARM64** VM, so the x86_64
output is a **cross** build — native arm64 cmake/ninja driving the x86_64 UCRT64
gcc. Installed machine-wide by `install-toolchain-win.ps1`.

**Target:** one binary for **Windows 7 SP1 → 11**, `-D_WIN32_WINNT=0x0601`.
**CRT:** UCRT, so Windows 7 needs **KB2999226** (mingw cannot static-link the UCRT).

**Build recipe** (PowerShell, with the UCRT64 bin directory first on `PATH`):

```powershell
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release `
      -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc `
      -DCMAKE_C_FLAGS="-D_WIN32_WINNT=0x0601"
cmake --build build-win -j
```

**One command instead** — the same build plus the ship folder (`putty.exe` +
`putty-mcp/` with its dependencies + `SETUP-WIN.txt`):

```powershell
.\scripts\build-windows.ps1
```

Output: `build-win\putty.exe`, staged into `putty-beter-win64\`. Full reference:
[`WINDOWS-BUILD.md`](WINDOWS-BUILD.md).

Toolchain detail, the ruled-out host, and the Windows 7 compatibility plan:
[tech_detail.md §1](tech_detail.md#1-build-baseline-and-toolchain).

---

## 2. Phases

| Phase | Goal | Delivered | State |
|---|---|---|---|
| **0** | Build baseline | native arm64 build, scripted entry point | ✅ macOS · ✅ Windows (**W0**) |
| **1** (T1) | macOS Intel + ARM | per-arch builds, universal merge, DMG | ⚠️ 1a done; **1b/1c (signed universal `.app`) not started** |
| **2** (O1) | Session records: folders, comments, passwords | folders, comments, search, **v0** passwords | ✅ v0 both platforms; v1 encrypted blob not built |
| **3** (T2) | ZMODEM `sz`/`rz` | both directions, byte-for-byte | ✅ macOS · ✅ Windows (**W2**) |
| **4** (O2) | MCP for AI | server + bridge; `sessions`, `read`, `screen`, `send`, `key`, `run`, and the human-in-the-loop state | ✅ macOS 4a/4b/4c · ✅ Windows (**W1**); 4d–4e pending. **4c is written for Windows too but has not been built or tested there** — see §6 |
| — | **Windows port** (W0–W7) | W0, W1, W2, W3, W4, W5, W7, the config folder, defaults, config box, transfer Browse | **complete** — W7 done 2026-11-24 · **W6 dropped** — no good solution (§10) |

The phase model is deliberate: order by "how much of the base is kept" and "cheap
wins first", so there is always a working build. Expensive items (ZMODEM, MCP IPC)
come after the cheap ones.

---

## 3. Progress — one row per feature, macOS and Windows side by side

`✅` done · `⚠️` partial · `❌` not done · `n/a` not applicable on that platform.

| # | Feature | macOS | Windows | Result / evidence | Detail |
|---|---|---|---|---|---|
| 1 | Build baseline and toolchain | ✅ | ✅ 2026-11-23 | both build; macOS native arm64, Windows x86_64 cross-built on the ARM64 VM; test suite + cryptsuite green on each | [§1](tech_detail.md#1-build-baseline-and-toolchain) |
| 2 | Relocatable configuration folder | ✅ 2026-11-17 | ✅ 2026-11-23 | configuration lives in a **folder**, not the registry/plist; `config-location.conf` names it, so a config carries between Mac and PC | [§2](tech_detail.md#2-relocatable-configuration-folder) |
| 3 | Changed default settings | ✅ 2026-11-17 | ✅ 2026-11-23 | colours, backspace, function keys, window size, keepalive, IP version, log name, transfer folder. Windows keeps its own fonts **by design**. The Windows batch also fixed a **Session-panel crash** | [§3](tech_detail.md#3-changed-default-settings-and-the-session-panel-crash) |
| 4 | Configuration box layout and size | ✅ 2026-11-17 | ✅ 2026-11-23 | box fits its contents, is resizable, minimum size enforced, and **the size is remembered** in `<config folder>/gui-state` on both; 35 panels, 0 overflow | [§4](tech_detail.md#4-configuration-box-layout-and-size) |
| 5 | Clipboard, and drag-and-drop | ✅ 2026-11-16 | n/a | macOS uses the fork's clipboard layer (`CLIPUI_SYSTEM`); Windows already routes `Ctrl+Ins`/`Shift+Ins` to the system clipboard, so it sets `CLIPUI_EXPLICIT` and needs no port | [§5](tech_detail.md#5-clipboard-and-drag-and-drop) |
| 6 | Session folders | ✅ 2026-11-17 | ✅ 2026-11-23 | folders are real directories under `<config folder>/sessions`; Save / Delete / New Folder, **drag-and-drop into a folder** and an **expandable tree** all work on both. Windows draws the tree into a list box (a TreeView cannot multi-select) and re-orders rows into tree order as it fills them, because `sessioncmp()` does not actually keep a folder's entries together. The **New Folder** button no longer clips at high display scaling: its column was widened and Windows now honours the shared `force_left` flag (W7) | [§6](tech_detail.md#6-session-folders) |
| 7 | Session comments | ✅ 2026-11-17 | ✅ 2026-11-23 | comment round-trips through shared `settings.c` on both; the Windows box is now **multi-line**, and stores LF-only text like the mac's, so a comment written on one platform reads correctly on the other | [§7](tech_detail.md#7-session-comments) |
| 8 | Saved-session search | ✅ 2026-11-19 | ✅ free | matches name, host and comment. Windows fires on **focus-out**, not per keystroke — accepted, see §18 | [§8](tech_detail.md#8-saved-session-search) |
| 9 | Saved-session passwords | ✅ v0 2026-11-19 | ✅ 2026-11-23 | one attempt per connection before the prompt, falls back to the dialog on rejection, offers to remember after a successful login. **v1** (Argon2id-encrypted blob) unbuilt on **both** | [§9](tech_detail.md#9-saved-session-passwords) |
| 10 | Background transparency | ✅ 2026-11-19 | ❌ **dropped** | alpha on the default background colour only. **W6 attempted 2026-11-24 and dropped: no good solution on Windows.** The DWM frame route composites against an opaque white backdrop on Win11 (not the desktop); `UpdateLayeredWindow` gives true per-pixel alpha but replaces the whole window, title bar included; a colour key keeps the frame and the crisp text but is all-or-nothing, so the background is 100% see-through and the 0–255 amount means nothing. The working colour-key implementation was built, verified, and then reverted | [§10](tech_detail.md#10-background-transparency) |
| 11 | A readable exit (close countdown) | ✅ 2026-11-22 | ✅ 2026-11-23 | hold the window open so a dropped session's reason can be read: the delay (`CloseOnExitDelay`, default 2 s) is honoured on both, any keystroke or click cancels the hold, and `PUTTY_BETER_DEBUG=exit` traces the decision | [§11](tech_detail.md#11-a-readable-exit-close-countdown) |
| 12 | ZMODEM `sz`/`rz` | ✅ 2026-11-17 | ✅ 2026-11-23 | **both directions, byte-for-byte, on both platforms.** macOS verified against real `lrzsz`; Windows verified the same way — download 250 000 B and 5 000 000 B, upload 5 046 272 B, md5 identical each time, driven over the fork's own MCP bridge against a Mac mini running `lrzsz` 0.13.1. Windows adds a modeless Win32 progress window, and Cancel removes the partial file | [§12](tech_detail.md#12-zmodem-file-transfer) |
| 13 | MCP for AI | ✅ 4a/4b 2026-11-18 · ✅ 4c 2026-11-24 | ✅ 2026-11-23 (4c: written, not yet built) | agent drives the session in the window the human watches. macOS over a Unix socket, Windows over a **named pipe**; verified end to end on both, including two PuTTYs at once. **4c** adds the human-in-the-loop state: a prompt PuTTY itself is asking the person at the keyboard (`needs_human`) is reported by `sessions` and `read`, and `run` refuses to send at all while it is up | [§13](tech_detail.md#13-mcp-for-ai) |
| 14 | App icon and branding | ✅ | ✅ 2026-11-24 | macOS has the fork `.icns`, the `0.85-beter.1` version string and the two-line About box with its link home; Windows now ships the fork icon too — `icons/putty-beter-icon.png` → a multi-size `windows/putty.ico`, built by the new `scripts/make-ico.py` — and the About box carries the fork's own copyright line beside upstream's and points its web link at the fork. The **version string** is already right on both — `version.h` is shared | [§14](tech_detail.md#14-app-icon-and-branding) |
| 15 | GTK PuTTYgen | ✅ 2026-11-21 | n/a | second `.app` beside `PuTTY.app`, both arches, in the universal DMG. **Deliberately not ported** — the native Windows `puttygen` builds in-tree | [§15](tech_detail.md#15-gtk-puttygen-macos-only) |

The **fine-grained** version of this table — every individual change in
`CHANGES-FROM-UPSTREAM.md`, row by row — is
[Appendix C](tech_detail.md#appendix-c-the-parity-matrix-fine-grained).

---

## 4. Results

What has actually been built and run. The macOS milestone checklist is
[Appendix I](tech_detail.md#appendix-i-the-macos-milestone-checklist-m0m9).

### macOS

| Milestone | Result |
|---|---|
| **M0** build baseline | native arm64 build passes |
| **M1** universal signed `.app` | **1a done** (per-arch builds, universal merge, DMG mounted and verified); 1b/1c not started |
| **M2 / M2b** session folders, comments, search | done |
| **M3v0** saved passwords | save + inject without a master password; tests green |
| **M4 / M5** ZMODEM | download and upload both verified against real `lrzsz`, 250 000 bytes byte-for-byte; **serial untested** |
| **M6a / M6b** MCP | socket + `sessions`/`read`/`screen`/`send`/`key`, then `run` with prompt detection and pager auto-answer, verified against a real `pterm` |
| **M6c** human-in-the-loop | the `needs_human` state, read from the terminal's own prompt; live-verified against a real `login as:` prompt (and the run that refuses to type into it) |
| **M8** GTK PuTTYgen | arm64 + x86_64, both bundled, merged universal (`275 files / 75 Mach-O`, all `x86_64 arm64`), in `PuTTY-mac-universal.dmg` |
| **M9** readable exit | countdown present in **both** slices of the universal build |

### Windows

| Batch | Result |
|---|---|
| **W0** baseline | x86_64 `putty.exe` builds; **test suite + cryptsuite green natively on Windows** |
| configuration folder (§12) | folder-based, no registry key ever appeared |
| config box (§7.2) / defaults + crash (§7.3) | resizable box, defaults parity, Session-panel crash fixed |
| transfer folder (§7.4) | Browse now opens `#32770 "Browse For Folder"`; sweep 17 / 0 |
| **W1** MCP (§7.5) | verified end to end: `version`, `sessions`, `screen`, `read`, `run`, `send`, `key`, two PuTTYs with distinct ids, a stale endpoint skipped, and a clean exit leaving no token, no endpoint file and no directory |
| **W5** saved passwords (§7.6) | verified: one attempt per connection, wrong password falls back to the prompt, login name captured; the manual config-box password field restored |
| **W2** ZMODEM (§12) | verified end to end against real `lrzsz` over SSH, **both directions byte-for-byte**: `sz` 250 000 B and 5 000 000 B, `rz` 5 046 272 B, md5 identical; the progress window shows live bytes and a moving bar; **Cancel** removes the partial file; typed input and MCP `send` are both refused mid-transfer; the upload chooser opens in the transfer folder |
| **W6** background transparency (§10) | **attempted and dropped — no good solution.** Four routes built and measured: the DWM frame extension (on Win11 it composites against an opaque white backdrop, not the desktop), `UpdateLayeredWindow` (true per-pixel alpha, but it replaces the whole window, so the native title bar goes), a colour key (works, keeps the frame and crisp text, but is binary — background 100% see-through, so 0–255 means nothing), and uniform `LWA_ALPHA` (graded, but fades the text too). The colour-key build was verified pixel-for-pixel — on → the terminal area samples the desktop colour, off → its own colour, and the live toggle through *Change Settings* flips both — and then **reverted**, so `putty.exe` is byte-for-byte its pre-W6 size |
| **W7** branding (§14) | **done 2026-11-24.** `windows/putty.ico` is now the fork icon, generated from `icons/putty-beter-icon.png` by the new dependency-free `scripts/make-ico.py` (16/32/48/64/128/256, PNG-compressed entries) — verified: Explorer, the taskbar and the config box's title bar all show it. The About box now reads *Original author: Copyright 1997-2026 Simon Tatham…* then *This fork: Copyright 2026 beterhans…*, and its **Visit Web Site** button opens `github.com/beterhans/putty-beter` instead of upstream chiark. Same batch: the **New Folder** button no longer clips at 200% scaling |
| regression net | after each batch: C sweep **15 passed / 2 skipped by design / 0 failed**, cryptsuite **90 OK** |

---

## 5. Constraints

| # | Constraint |
|---|---|
| **G1** | **x86_64 `putty.exe`, Windows 7 SP1 → 11** — one binary across all of it |
| **G2** | **The MCP bridge ships beside the exe** — `putty-mcp/` (bin + src + node_modules) in the same folder |
| **G3** | **All macOS features on Windows** — two deliberate exclusions: PuTTYgen (the native Windows `puttygen` builds) and **background transparency** (no good solution on Windows — see §10) |
| **G4** | **Must not affect or break the macOS side** — one source tree; Windows work is additive |
| **G5** | **Build host** — the Mac mini / VMware Windows 11 ARM64 VM, where x86_64 is a cross build |

The G4 rules that keep the macOS side safe, the batch ordering, and the
"deliberately not ported" list are in
[§16](tech_detail.md#16-goals-and-constraints).

**Working conventions** (backups, `_agent/` notes, debug switches, minimal diffs,
preserve-don't-delete, temp space): [§18](tech_detail.md#18-working-conventions).

---

## 6. Next

1. **Windows W1's last test-plan item** — a real OpenCode session over the G2
   config (`SETUP-WIN.txt`, the `opencode.json` snippet, does not exist yet).
2. **macOS M1b/M1c** — the signed universal `.app`; and MCP 4d–4e
   (audit log, read-only/confirm modes, the GUI badge, `open_session`).
3. **Windows 4c needs a build on the VM.** The port was mirrored (protocol 3,
   the prompt field on `sessions` and `read`, `mcp_get_prompt()` in
   `windows/window.c`), but it has **not been compiled or run** — there is no
   mingw toolchain here. Run `scripts/build-windows.ps1` and the Windows smoke
   test before trusting it. Until then the Windows bridge in the staged payload
   and the Windows exe must travel together (they are both built by that script).
4. **Unresolved:** no Windows 7 SP1 machine to verify on; the W5 happy path needs
   a real credential; **W2 has no serial test** on either platform.
5. **Two ZMODEM findings carried into §12, neither a defect of the port:** a
   front-end callback that blocks (the upload chooser) also stops the socket
   being read, so a slow answer can desynchronise a handshake that retransmits;
   and `lrzsz`'s post-abort ENQ makes the terminal inject its answerback string
   into the shell. Both are shared with the macOS front end.
6. **Known gap, not a batch of its own:** the fork's `vexpand` flag — which makes
   the session list and the comment box absorb spare height — is honoured by GTK
   but **not** by the Windows layout, so making the configuration box taller
   leaves the panel at its natural height with empty space below. See
   tech_detail.md §4.

**Just closed:** **M6c** — the human-in-the-loop state, `needs_human`: an agent
can now tell that PuTTY itself is asking the person at the keyboard for a
password or a login name, and `run` will not send anything into that prompt
(§7.13). Before it, on Windows: **W7** (the fork icon and the About box), and the
**"New Folder" button clipping** it carried — Windows now honours the shared
`force_left` flag, so the session buttons keep their natural width, and their
column was widened from 20% to 26%.

---

## 7. Detail

Everything that is *detail* is in **[`tech_detail.md`](tech_detail.md)**:

| Part | Contents |
|---|---|
| 1 | The features, one section each, macOS half and Windows half: [1–15](tech_detail.md#part-1--the-features) |
| 2 | Cross-cutting: [goals and constraints](tech_detail.md#16-goals-and-constraints), [risks](tech_detail.md#17-cross-cutting-risks), [conventions](tech_detail.md#18-working-conventions), [packaging](tech_detail.md#19-packaging-and-ship-layout), [test plan](tech_detail.md#20-test-plan) |
| 3 | Appendices: [what the Windows port touches](tech_detail.md#appendix-a-what-the-windows-port-touches) · [build log](tech_detail.md#appendix-b-build-log--the-windows-port-entry-by-entry) · [parity matrix](tech_detail.md#appendix-c-the-parity-matrix-fine-grained) · [macOS/GTK surprises](tech_detail.md#appendix-d-macos-and-gtk-what-they-do-that-you-would-not-expect) · [ZMODEM byte-exact notes](tech_detail.md#appendix-e-zmodem-protocol--byte-exact-implementation-notes) · [MCP internals](tech_detail.md#appendix-f-mcp--how-it-works) · [search internals](tech_detail.md#appendix-g-saved-session-search--how-it-works) · [putty-mcp manual](tech_detail.md#appendix-h-putty-mcp-user-manual) · [macOS milestones](tech_detail.md#appendix-i-the-macos-milestone-checklist-m0m9) |
