# PuTTY for macOS

A macOS build of [PuTTY](https://www.chiark.greenend.org.uk/~sgtatham/putty/) 0.85,
plus the fixes needed to make its GTK front end behave properly on macOS,
**ZMODEM file transfer** (`sz`/`rz`) in both directions, and an **MCP server**
so an AI agent can watch and read a live session — see §13.

Everything not listed below is stock upstream PuTTY. MIT licensed — see `LICENCE`.

---

## Documentation index

| Document | What it is |
|---|---|
| **This file** | what the fork changes, how to build it, and how to use each feature |
| [`CHANGES-FROM-UPSTREAM.md`](CHANGES-FROM-UPSTREAM.md) | every difference from the official PuTTY 0.85 tarball, file by file, with the commands to reproduce the diff |
| [`PLAN.md`](PLAN.md) | the roadmap, and the reasoning behind each decision — §7 is the MCP design of record |
| [`docs/mcp.md`](docs/mcp.md) | **how the MCP works**: the files, the wire protocol, the capture-buffer semantics, the socket and its permissions, the build, testing and debugging |
| [`docs/session-search.md`](docs/session-search.md) | **how the saved-session search works**: the prefix rule, what is matched, the per-session cache, and how to drive the dialog by hand |
| [`docs/mcp-user-manual.md`](docs/mcp-user-manual.md) | the manual the AI agent is given, as a plain document to read. A byte-identical copy of `mcp/USER-MANUAL.md`, checked by `scripts/test-mcp.sh` |
| [`mcp/README.md`](mcp/README.md) | the `putty-mcp` bridge: how to install it and point a client at it |
| [`mcp/USER-MANUAL.md`](mcp/USER-MANUAL.md) | the canonical copy of the agent manual (what is actually served) |
| [`docs/zmodem-protocol-notes.md`](docs/zmodem-protocol-notes.md) | byte-exact ZMODEM protocol reference, with the licence status of every source it draws on |
| [`tools/README.md`](tools/README.md) | why `gtk-mac-bundler` is not vendored, and how to fetch it |

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
`~/.putty/config-location.conf`, a one-line text file.

**Starting from nothing works.** If `~/.putty` does not exist at all — a fresh
machine, or after deleting it — it is created to hold that one-line file, along
with the folder you chose and its `sessions` subdirectory. Nothing has to exist
in advance.

Nothing is deleted unless it was copied successfully, and nothing is
overwritten: if the destination already holds one of these files, the old copy
is simply left where it was. Deleting `config-location.conf` returns to
`~/.putty`.

Hold **Option (⌥)** while launching PuTTY to reopen the dialog and move the
folder again, or set `PUTTY_CONFIG_RELOCATE=1`.

`plink`, `pscp`, `psftp` and `puttygen` read the same file, so saved host keys
and CA records stay consistent. `PUTTYDIR` (upstream) still overrides
everything.

### 5. Build tooling

`scripts/build-macos.sh`, `scripts/bundle-macos.sh` and `CMakePresets.json`
build the binaries and a self-contained `PuTTY.app` with GTK bundled inside.
`scripts/make-icns.sh` turns `icons/putty-beter-icon.png` into the app icon
using only `sips` and `iconutil`, and `bundle-macos.sh` runs it whenever the
PNG is newer than the `.icns`.

### 6. Runtime diagnostics

`PUTTY_DIAG=1` traces key events, clipboard activity, font metrics and cursor
placement to stderr. `PUTTY_CONFIG_DEBUG=1` traces the configuration folder
resolution and relocation, and the remembered configuration-box size.
`PUTTY_MCP_DEBUG=1` traces the MCP server (§13) — whether a session offered
itself, where the socket went and with what permissions, and every request and
reply. `PUTTY_SESS_SEARCH_DEBUG=1` traces the saved-session search (§9b) — the
query, the match count, and when the session files are read again. None of them
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
are in bold with a disclosure triangle and always sort to the top of their
level. Select several rows (Shift-click or ⌘-click) and drag them onto a
folder to move them all at once — the selection is restored as soon as the
drag starts, so the whole group stays highlighted and all of it moves. Under the list are **Load**, **Save**, **Delete**
and **New Folder**; New Folder takes the name from the Saved Sessions box, the
same way Save does. Delete removes everything selected, and asks first if the
selection includes sessions inside a folder, with a "Delete All" / "Cancel"
box — Escape cancels.

You can also just type a name with a slash: saving as `Work/beta` creates the
folder as it goes. Names are checked, so a session name can never escape the
sessions directory.

Each session is a **`<name>.conf` file** inside the sessions folder, and only
`.conf` files are read as sessions — so Finder's `.DS_Store`, or a stray copy
you drop in there, is ignored rather than showing up in the list. Folders are
plain directories and have no extension, so `Work/beta` is
`<sessions>/Work/beta.conf`.

### 9. Session comments

A saved session can carry a free-text note about itself, in
**Session → Comment** — a panel of its own, directly below Logging, so that
the box can be big enough to type a paragraph into. Return gives a newline,
Tab moves to the next control, and the box grows when the window does. The
text is UTF-8 and is stored with the session, so a note like
"prod box, ask Dana before touching" travels with the session file.

It is written to the session file as `SessionComment=`, only when there is
one. Newlines are escaped as `\n` on disk, since the settings file format is
one `key=value` per line.

### 9b. Searching saved sessions

The **Saved Sessions** box is also the search box. If what you type into it
starts with `/`, the rest of the line is a search rather than a name, and the
list below shrinks to the sessions that match. `\` works as the same prefix.
The box says so while it is empty: *Name/folder, or / to search*.

A session matches when the text appears in its **name**, its **host name or IP
address**, or its **comment** — case-insensitively. A query that names a folder
finds everything inside it too, because a session is matched by its whole path
(`/Work` finds `Work/beta`).

| Typed | What you get |
|---|---|
| `/` | every session (search mode with nothing to look for yet) |
| `/10.0.1` | the sessions whose host is in that range |
| `/dana` | a session whose comment says "ask Dana before touching" |
| `/Work` | the `Work` folder and the sessions in it |
| `/zzz` | nothing at all — the list is empty and the box's label says so |

While a search is on, the box's label reads **`Search (matched/total)`** and
the whole list comes back as soon as you clear the prefix, or by deleting back
to nothing. Load, Delete, drag and double-click all work on a filtered list,
so searching for a session and then loading it is two steps, not three. **Save**
and **New Folder** beep while a search is on: `/beta` is not a session name.

`PUTTY_SESS_SEARCH_DEBUG=1` traces each search — the query, the match count and
whether the session files had to be read again — to stderr. For the design, the
cache and how to drive the dialog from a script, see
[`docs/session-search.md`](docs/session-search.md).

### 9c. Saving a session's login details

A saved session can remember its login password, so connecting to it does not
ask. The box is in **Connection → Data**, directly under *Auto-login username*,
and it is hidden the way a password box should be. Leave it empty for no saved
password; clear it and press **Save** to forget one.

* **Connecting** uses the saved password only if the server asks for one, and
  only once: if the server rejects it (the password changed, say) you get the
  ordinary prompt instead of a failed connection.
* **A login that succeeds** offers to keep what it took to get there —
  **Remember Login** — after the password you typed, the login name you typed at
  the `login as:` prompt, or both, whichever the session does not already have;
  Yes writes them into that session, and Restart Session then logs straight in.
  The login name is offered **on its own** as well, which is the case that
  matters once the password is already saved: the name was still being asked for
  on every connection.
* A login name typed at the prompt **replaces** the session's *Auto-login
  username* — it is the name that actually logged in. A password already saved
  is left alone.
* **What is on disk is wrapped, not encrypted**: the session file holds
  `SessionPassword=v0;<base64>`. That keeps it off a plain `grep` and out of
  sight, but anyone who can read the file can decode it. Encrypting it under a
  master password is the next piece of work (`PLAN.md` §4k).
* `PUTTY_SESSPASS_DEBUG=1` traces a decode, an injection and a remembered login
  name to stderr — never the password itself.

### 10. Changed defaults

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
| Window → Fonts → ordinary text | `client:Monospace 12` | **`client:PT Mono 14`** |
| Window → Fonts → wide (CJK) text | none (use the ordinary font) | **`client:Lantinghei TC Semi-Bold 14`** |
| Window → Colours → default foreground / background / bold background | `187,187,187` / `0,0,0` / `85,85,85` | **`242,242,242`** / **`0,48,42`** / **`64,96,96`** |

The ANSI palette (Colour 6–21), the cursor colours, *Allow terminal to specify
ANSI colours*, *Bold-coloured text is shown in bold* and the other Font boxes
(bold, wide bold) keep upstream's defaults. The background-transparency setting
is left at 0: only the colours and the fonts are taken from that session.

### 11. ZMODEM file transfer

`sz` and `rz` work in both directions — over SSH, and on a serial line. Type
`rz` on the remote end to send a file up, or `sz <file>` to bring one down.

The engine is written in-tree from the public-domain ZMODEM specification and
Chuck Forsberg's original `rzsz`, and is MIT like the rest of this tree; no GPL
component is involved. See
[`docs/zmodem-protocol-notes.md`](docs/zmodem-protocol-notes.md) for the
byte-exact protocol reference and the licence status of every source used.

* **Downloads** need no prompt: the protocol carries the file name, so `sz`
  just saves into the transfer folder.
* **Uploads** open a file chooser with a full-path box.
* **`Session → File transfer`** sets the folder for both directions, defaulting
  to `~/Downloads`. It is written back after each transfer, so the last folder
  you used sticks.
* A small **progress window** shows the file name, bytes and percentage, with a
  **Cancel** button. Ctrl-C in the terminal cancels too, as in `rzsz`.
* **Cancelling reports a cancellation**, not a completion. One `rz` sends one
  file; run `rz` again to send another.
* A transfer cut short does **not** leave a truncated file behind.

`./build-arm64/test_zmodem` runs the engine's own tests (135 checks), and
`./build-arm64/test_zmodem_interop` drives the real `sz` and `rz` over a pty in
both directions, byte-for-byte.

### 12. File choosers, and dialog title bars

Every place PuTTY asks for a file or a folder — the private key file, the
certificate, the GSSAPI library, the log file name, the transfer folder, the
`rz` upload, the configuration folder — uses **one chooser with a `Path:` box
across the top**. Type a full path and press Return to use it, or type a folder
and press Return to go there. GTK's own dialog keeps that behind Ctrl+L, which
is not discoverable.

GTK's dialog windows also had **see-through title bars** on macOS: GTK gives a
`GtkDialog` a transparent window background, and macOS paints the title bar from
it. Fixed with one line of ordinary GTK API per dialog (no Objective-C) — see
`gtk_dialog_fix_macos_titlebar()` in `unix/gtkcompat.h`. The configuration box,
About box and message boxes were never affected.

### 12b. A see-through background

**Window → Colours → Transparency** (0 = solid, 255 = see-through) lets the
desktop show through the terminal's background colour. Only the *default*
background is affected, so highlighted lines, coloured `ls` entries and the
cursor stay solid and readable. It needs a compositing window manager, which
macOS has; if there is none, the window stays solid and says so on stderr.

The setting takes effect when a session is **opened**, because the window must be
given an alpha channel before it is shown. Changing the *amount* with the session
running does repaint at once (the Event Log says so if the window was opened
solid).

Two macOS realities, both found the hard way:

* **The title bar becomes a GTK-drawn one.** A see-through terminal means a
  non-opaque window, and macOS paints a *native* title bar from the window's own
  background — which GDK makes fully transparent as soon as the window is
  non-opaque (measured: `isOpaque` 0, `backgroundColor` alpha 0). So the native
  bar goes see-through with it and the title is left written on the desktop. A
  solid bar and a see-through terminal cannot both have the native bar, so while
  transparency is on, the window is given a GTK-drawn title bar instead. It
  looks different — that is the price — and at transparency 0 nothing changes at
  all, so the native bar is untouched.
* **The background is *put down*, not blended over.** A translucent fill laid
  over what is already in the window lets the previous frame show through: every
  character typed while the cursor sat on it kept a tint of the cursor colour,
  which is exactly what "the text I typed is highlighted" turned out to be.
  Fills of the default background now replace the pixels instead — which also
  means the setting is as transparent as it says (the background was being
  blended twice, so 64 came out 94% opaque rather than 75%).

`CHANGES-FROM-UPSTREAM.md` §15 has the measurements and the mechanics.

---

### 13. MCP: let an AI agent read a session

An AI agent can read a session **in the window you are looking at**, so you can
watch what it is doing, take over at any moment, and answer the prompts it
cannot. It can read the output, type into the session, and **run a command and
wait for the device to finish** — including answering a `--More--` pager by
itself, which is what makes it usable on network gear. It stops rather than
guessing at a `[confirm]` or a password prompt, and says so. Text **you** type
is tagged in the transcript, so the agent can tell your doing from the device's.

It is **off by default and per session** — *Session → Automation (MCP) → Offer
this session to an MCP client* — because turning it on offers the session to
anything that can reach the socket. With it off, there is no socket, no token
and no file at all.

**Why this exists when `ssh` already runs commands.** An exec-per-command client
is better for a Linux shell: it gets real exit codes and separated stderr. MCP
in PuTTY is for what only PuTTY can do — a session that is already
authenticated, network gear whose only interface *is* an interactive CLI, a
serial console where there is no second session to be had, and a human who wants
to watch.

**How it is put together:**

```
OpenCode --stdio MCP (JSON-RPC)--> putty-mcp --Unix socket--> PuTTY
             ("putty-mcp")          (mcp/, Node)             (unix/mcp.c)
```

The socket protocol is deliberately dumb — a list of length-prefixed fields, no
JSON, no escaping — because the C side has no JSON parser and should not grow
one. All of the MCP and JSON lives in `mcp/`, so MCP's spec churn never touches
the fork, and the socket can be driven with no LLM in the loop at all.

**The capture buffer is its own record of the output**, not the terminal's
scrollback: scrollback is a display setting, and MCP must see every byte. Line
ids are **absolute and never reused**, so a client can tell exactly what it
missed when its cursor falls out of the retained window, and the reply says so
explicitly rather than losing output silently.

**Setting it up:**

```sh
# once, for the bridge's dependencies
cd mcp && npm install

# and rebuild the app: the server is compiled into the front end, so a
# PuTTY.app built before this change will not have it
scripts/build-macos.sh arm64 && scripts/bundle-macos.sh arm64
```

then add to `~/.config/opencode/opencode.json`:

```json
"putty-mcp": {
  "type": "local",
  "command": ["node", "/path/to/putty-beter/mcp/bin/putty-mcp.mjs", "mcp"]
}
```

It is also a plain command-line client, which is the first thing to reach for
when something is wrong — no LLM involved:

```sh
node mcp/bin/putty-mcp.mjs list              # sessions PuTTY is offering
node mcp/bin/putty-mcp.mjs read 1            # the output, with line ids
node mcp/bin/putty-mcp.mjs read 1 --since 12 # follow from a cursor
node mcp/bin/putty-mcp.mjs screen 1          # the visible screen
node mcp/bin/putty-mcp.mjs run 1 'show version'   # wait for it, print the output
node mcp/bin/putty-mcp.mjs manual            # what the agent reads
```

`run` is the one to reach for: it sends a command, waits for the device to
finish, and prints **just that command's output** — not the echo, not the prompt
you interrupted, not the prompt that came back. It answers a `--More--` pager by
itself (the thing that hangs a naive client on network gear), and it *stops*
rather than guessing at a `[confirm]` or a password prompt. Its summary line on
stderr says why it decided the command had finished, so `--debug` shows the
reasoning:

```
$ node mcp/bin/putty-mcp.mjs run 1 'show version'
Cisco IOS Software, ...
(prompt, echo seen, 410ms, cursor 42)
```

**What you type is tagged in the transcript.** A line the device printed is
`d`, one an MCP client sent is `m`, and one **you** typed is `h` — so an agent
can tell your doing from the device's, and a keystroke of yours during a `run`
cancels it rather than letting the two of you drive the same stream. A password
you type is never in the transcript at all, because the device does not echo
one.

The agent is given a **user manual** — the `user_manual` tool, the
`putty://manual` resource, and a pointer in MCP's handshake instructions — so it
does not have to guess how cursors, gaps and unfinished prompt lines work.

**The socket** lives in `$TMPDIR/putty-mcp-<uid>/` (`sock`, `token`,
`endpoint`; the endpoint is also copied into the configuration folder as
`mcp-endpoint`). It is `0600`, or `0660` in the `putty-mcp` group if that group
exists — plain `0660` would use macOS's default group, which contains every
local account.

**Debug:** `PUTTY_MCP_DEBUG=1` in PuTTY's environment traces the socket, the
handshake and every request and reply; `putty-mcp list --debug` traces the
bridge's side.

**Test:** `scripts/test-mcp.sh` starts its own `pterm` against a throwaway
configuration folder and checks the whole path end to end (36 checks), including
`run` against a real pty: a slow command is waited for rather than cut off, a
real `more` is paged through, and a `Password:` prompt stops the run. It needs
a display, and touches nothing of yours. `mcp/test/protocol.test.mjs` and
`mcp/test/run.test.mjs` cover the bridge's framing, discovery and the run loop
with no PuTTY at all — the run loop against a fake session on a virtual clock,
so a thirty-second timeout is tested in microseconds.

**Working on the bridge?** An MCP client spawns `putty-mcp mcp` **once** and
keeps that process for the whole life of its session, so an edit under `mcp/` is
invisible until the client is restarted — and the old code keeps working, just
wrongly. Killing the process does not help: the client drops the server and does
not respawn it. Use the CLI meanwhile (it starts a fresh process every time), and
restart the client when you are done.

**Where to read more:**

* [`docs/mcp.md`](docs/mcp.md) — **how it works**: the files, the wire protocol
  (with a snippet for driving the socket by hand), the capture-buffer semantics,
  the socket and its permissions, the build, and what is still missing.
* [`docs/mcp-user-manual.md`](docs/mcp-user-manual.md) — the manual the agent is
  given, as a plain document. `mcp/USER-MANUAL.md` is the canonical copy, and
  the two are kept identical by a check in the smoke test.
* [`mcp/README.md`](mcp/README.md) — installing the bridge and pointing a client
  at it.
* `PLAN.md` §7 — the design and the decisions; §7.11 for what was actually
  built, and the nine places it departed from the sketch.

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

> **The `.app` must be assembled with `bundle-macos.sh`.** The launcher inside
> the bundle sets `XDG_DATA_DIRS` to the bundle, which overrides GLib's fallback
> to the system GSettings schemas — so the bundle has to carry its own.
> `bundle-macos.sh` compiles 38 of them into
> `Contents/Resources/share/glib-2.0/schemas/`; without that, GTK's file chooser
> cannot find `org.gtk.Settings.FileChooser` and **opening any file picker
> aborts the process** (GLib calls `g_log_abort()`, SIGTRAP, exit 133). The bare
> binaries above are unaffected — they find the system schemas normally.

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