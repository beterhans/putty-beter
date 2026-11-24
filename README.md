# PuTTY for macOS

A macOS build of [PuTTY](https://www.chiark.greenend.org.uk/~sgtatham/putty/) 0.85
(this fork's own version is **`0.85-beter.1`**), plus the fixes needed to make its
GTK front end behave properly on macOS, **ZMODEM file transfer** (`sz`/`rz`) in
both directions, and an **MCP server** so an AI agent can watch and read a live
session — see §13.

Everything not listed below is stock upstream PuTTY. MIT licensed — see `LICENCE`.

---

## Documentation index

| Document | What it is |
|---|---|
| **This file** | what the fork changes, how to build it, and how to use each feature |
| [`CHANGES-FROM-UPSTREAM.md`](CHANGES-FROM-UPSTREAM.md) | every difference from the official PuTTY 0.85 tarball, file by file, with the commands to reproduce the diff |
| [`WINDOWS-BUILD.md`](WINDOWS-BUILD.md) | **building and shipping the Windows port**: `install-toolchain-win.ps1`, `scripts/build-windows.ps1` and every switch it takes, the timings, and how to verify a build by hand |
| [`PLAN.md`](PLAN.md) | the plan: the tools, the phases, the progress and the results, for macOS and Windows side by side - one row per feature |
| [`tech_detail.md`](tech_detail.md) | the detail behind `PLAN.md`: the findings, the gotchas and the "as built" accounts, one section per feature with a macOS half and a Windows half |
| [`tech_detail.md` §13 / Appendix F](tech_detail.md#appendix-f-mcp--how-it-works) | **how the MCP works**: the files, the wire protocol, the capture-buffer semantics, the socket and its permissions, the build, testing and debugging |
| [`tech_detail.md` §8 / Appendix G](tech_detail.md#appendix-g-saved-session-search--how-it-works) | **how the saved-session search works**: the prefix rule, what is matched, the per-session cache, and how to drive the dialog by hand |
| [`mcp/README.md`](mcp/README.md) | the `putty-mcp` bridge: how to install it and point a client at it |
| [`mcp/USER-MANUAL.md`](mcp/USER-MANUAL.md) | the agent manual - what is actually served, and the file `putty-mcp manual` prints |
| [`tech_detail.md` Appendix E](tech_detail.md#appendix-e-zmodem-protocol--byte-exact-implementation-notes) | byte-exact ZMODEM protocol reference, with the licence status of every source it draws on |
| [`tech_detail.md` Appendix D](tech_detail.md#appendix-d-macos-and-gtk-what-they-do-that-you-would-not-expect) | what macOS and GTK do that you would not expect - window placement, `-geometry`, title bars, why AppleScript cannot drive GTK windows - with the measurements and the recipes for checking them |
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
build the binaries and self-contained `PuTTY.app` and `PuTTYGen.app` bundles with
GTK inside. `scripts/make-icns.sh` turns `icons/putty-beter-icon.png` and
`icons/puttygen-icon.png` into the two app icons using only `sips` and
`iconutil`, and `bundle-macos.sh` runs it whenever a PNG is newer than its
`.icns` (and always for the keygen one). An icon source must be **square with the
artwork to the edges** — macOS scales the whole canvas, so a transparent margin
comes out as an undersized icon — and the converter refuses a non-square source
rather than stretching it.

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
[`tech_detail.md` Appendix G](tech_detail.md#appendix-g-saved-session-search--how-it-works).

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
  master password is the next piece of work (`tech_detail.md` §4k).
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
| Session → seconds to hold the window open first (**new setting**) | — | **2** (§12d) |

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
[`tech_detail.md` Appendix E](tech_detail.md#appendix-e-zmodem-protocol--byte-exact-implementation-notes) for the
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

### 12c. Which monitor a new window opens on

A new session window opens on the monitor the **configuration box** (or the
window you asked for a new session from) is on, instead of on whichever screen
macOS picks.

Left to itself, macOS puts a new window at the origin of the monitor whose frame
starts at (0,0), and on a two-monitor Mac that is often *not* the primary one —
so every session opened on the same "wrong" monitor while the configuration box
sat on the other one. Measured here: the box on the primary at `1920,0`, and
every session window at `0,30` on the second display.

The monitor of the last PuTTY window you moved or resized is what counts, so
dragging the configuration box to your other screen and pressing **Open** opens
the session there, and a new session asked for from a window on the second
screen opens on the second screen. With no window to inherit from — the very
first window of the app's life, or `putty -load x` straight from a shell — it is
the **primary** monitor. A position given explicitly still wins.

`PUTTY_DIAG=1` reports the choice:
`new session window: monitor 1920,0 1920x1080, window 1040x614, placing at
2360,233`.

The platform behaviour behind all of this — why the wrong monitor is picked, what
does *not* change it, and how to measure it — is written up with its numbers in
[`tech_detail.md` Appendix D](tech_detail.md#appendix-d-macos-and-gtk-what-they-do-that-you-would-not-expect).

### 12d. A readable exit: the window is held open when a session ends

When a session ends and *Close window on exit* says the window goes, PuTTY
destroys it at once — and whatever the session printed on its way out, which is
usually the reason it dropped, goes with it. The window is now held for a short
**countdown** (two seconds by default) with a line under the terminal:

> The session has ended.  This window closes in 2 seconds.  Press any key to keep
> it open.

Any keystroke or click cancels the countdown and leaves the window open, so the
reason can be read and copied; the *next* keystroke closes the window as it always
did, and the window's close button still works straight away. The line is a GTK
label **below** the terminal, so nothing the session printed, and nothing in the
scrollback, is touched or reflowed.

* **Session → "Seconds to hold the window open first (0 = close at once)"**, a new
  box next to *Close window on exit*. **0 reproduces upstream exactly.** It is a
  per-session setting like everything else on that panel, so a session you know
  drops often can be set to skip the wait.
* The countdown runs in the GTK main loop (`g_timeout_add`), never a blocking
  sleep, so the window keeps painting and stays responsive while it counts.
  `CloseOnExit=Never` is unchanged, and so is `pterm`: it has no configuration
  panel and no saved sessions, so its user could never switch the hold off, and it
  keeps upstream's close-at-once (`platform_default_i()` in `unix/window.c`
  returns 0 for it). `plink`, `pscp` and `psftp` have no window at all.
* `PUTTY_BETER_DEBUG=exit` traces the decision on stderr: which path fired, the
  exit code, the delay, and whether a keystroke cancelled the countdown.

The case that prompted it was a saved session to a Synology NAS. `PUTTY_SESSPASS_DEBUG`
and `plink -v` show what actually happens: the login is **accepted** — `Access
granted` — the main channel opens, and then the account is refused at the session
level, the server printing `Permission denied, please try again.` with exit status
1. The window used to disappear before any of that could be read, which is why it
looked like a crash. The cause is upstream's default rather than a bug:
`CloseOnExit` is **Always** on Unix, from `platform_default_i()` in
`unix/window.c`. [`tech_detail.md`](tech_detail.md) §4n is the design of record and carries the
test results.

### 13. MCP: let an AI agent read a session

An AI agent can read a session **in the window you are looking at**, so you can
watch what it is doing, take over at any moment, and answer the prompts it
cannot. It can read the output, type into the session, and **run a command and
wait for the device to finish** — including answering a `--More--` pager by
itself, which is what makes it usable on network gear. It stops rather than
guessing at a `[confirm]` or a password prompt, and says so. Text **you** type
is tagged in the transcript, so the agent can tell your doing from the device's.

**When PuTTY itself asks you for something**, the agent is told, and waits: a
login password, a key passphrase or a `login as:` prompt is drawn **in the
session window** and answered by you. Those words look exactly like device
output, so the session reports it as a state (`needs_human`) and a command that
would otherwise be typed *into* that prompt is not sent at all — a password is
yours to give.

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

Install the app — the bridge and its dependencies travel inside `PuTTY.app`, so
there is nothing else to install — then add to
`~/.config/opencode/opencode.json`:

```json
"putty-mcp": {
  "type": "local",
  "command": ["node", "/Applications/PuTTY.app/Contents/Resources/putty-mcp/bin/putty-mcp.mjs", "mcp"]
}
```

`PuTTY.app` is self-sufficient because `bundle-macos.sh` installs a copy of the
bridge — **with its dependencies** — at `Contents/Resources/putty-mcp`, reachable
as `Contents/MacOS/putty-mcp` through a symlink, together with the bridge's
`node_modules`. So the only thing the machine needs is **Node 20 or newer** — no
`npm install`, no global packages, no paths to arrange. (The dependencies are pure
JavaScript — 0 `.node` files, 0 Mach-O, no `os`/`cpu`-constrained packages — so one
copy serves both architectures, and the universal build carries a single copy.)
`SETUP.txt` in the app spells this out. The **x86_64** build skips the bridge
entirely rather than building it without dependencies — the Intel Mac reaches this
tree over SMB, where a symlink inside `node_modules` cannot be read at all; see the
Intel notes below — and the merge takes the bridge and its dependencies from the
arm64 slice / the shared `mcp/node_modules`, saying so if neither had them.
`MCP_NODE_MODULES=0` remains for a build that wants the bridge itself but not its
packages.

Working from a source checkout instead, the client points at the checkout's own
script, and there are two more steps — the bridge's dependencies, and a rebuild,
because the server is compiled into the front end, so a `PuTTY.app` built before
this change will not have it:

```sh
# once, for the bridge's dependencies
cd mcp && npm install

# the server is compiled into the front end, so an older PuTTY.app has none
scripts/build-macos.sh arm64 && scripts/bundle-macos.sh arm64
```

and point the client at the checkout's script instead:

```json
"putty-mcp": {
  "type": "local",
  "command": ["node", "/path/to/putty-beter/mcp/bin/putty-mcp.mjs", "mcp"]
}
```

The in-app symlink is not decoration: macOS treats everything under
`Contents/MacOS` as code, and a bundle with unsigned non-code files there does not
seal; the files themselves are under `Resources`, where the seal covers them.

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
configuration folder and checks the whole path end to end (38 checks), including
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

* [`tech_detail.md` Appendix F](tech_detail.md#appendix-f-mcp--how-it-works) - **how it
  works**: the files, the wire protocol (with a snippet for driving the socket by
  hand), the capture-buffer semantics, the socket and its permissions, the build,
  and what is still missing.
* [`mcp/USER-MANUAL.md`](mcp/USER-MANUAL.md) - the manual the agent is given. This
  is the canonical copy: the bridge reads it at run time (`mcp/src/manual.mjs`) and
  serves it as the `user_manual` tool and the `putty://manual` resource.
* [`mcp/README.md`](mcp/README.md) — installing the bridge and pointing a client
  at it.
* `tech_detail.md` §13 — the design and the decisions; §7.11 for what was actually
  built, and the nine places it departed from the sketch.

---

### 14. PuTTYGen.app — a GUI key generator

Upstream builds `puttygen` as a **command-line** program on Unix; its GUI is
Windows-only (`windows/puttygen.c`). This fork ports that GUI to GTK, so macOS
gets a **PuTTYGen.app** beside PuTTY.app: generate, inspect, load, convert and
save keys by clicking, with no terminal involved.

* **Key generation** — Ed25519, Ed448, ECDSA (NIST P-256/384/521), RSA and DSA,
  with the prime-generation policy (probabilistic / proven / proven-even) and
  the "strong RSA primes" option. Generation runs on a worker thread, so the
  window stays responsive, with a progress bar and a **Cancel** that really
  stops the generator.
* **The key box** — comment, passphrase, fingerprint (SHA-256 or MD5) and the
  `authorized_keys` line for the public key, all as selectable text.
* **Open Key** loads PuTTY's `.ppk` (asking for the passphrase if it needs one),
  an OpenSSH private key (old PEM or new format) and a public-key-only file;
  importing a foreign format says so, as Windows does.
* **Save private key** writes a `.ppk` (v3 by default, with its own parameters
  dialog: Argon2 flavour, memory, time or passes, parallelism), and **Export
  OpenSSH key** / **Export ssh.com key** write the conversions. **Save public
  key** writes the RFC 4716 form. A public-key-only load disables the private
  saves.
* Every file request goes through the application's **one file chooser** (the
  one with the `Path:` box, §12), and every dialog is given an opaque title bar
  for the same reason as §12.

Two Windows features are deliberately left to the command line, which keeps all
of them: **SSH-1 keys** and **OpenSSH certificates** (add/remove/cert-info).

**Test:** `scripts/test-puttygen-gui.sh` checks the GUI against the
command-line `puttygen`, which is the oracle — for every algorithm it generates,
the GUI's fingerprint and public-key line are compared with `puttygen -l` /
`puttygen -L` on the file the GUI itself wrote, plus loading CLI-made keys in
every format, a save/reload round trip, a cancelled 4096-bit RSA generation, and
three keys generated in one process (the regression check for the PRNG being
created once). 33 checks. Set `DEBUG=1` to see each command and the raw output.

**Working on it?** `PUTTYGEN_DEBUG=1` traces every action on stderr, and the
self-test hooks (`PUTTYGEN_SELFTEST`, `PUTTYGEN_SELFTEST_LOAD`) drive the window
without a human — that is what the test script uses.

**Where to read more:** `tech_detail.md` §4m — the porting map, the design, and the "As
built" list of ten places it departed from the sketch (and why).

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
# 1.1 Compile the binaries Arm side -> build-arm64/
./scripts/build-macos.sh arm64

# 1.2 Compile the binaries Intel side
./scripts/build-macos.sh x86_64

# 2.1 Assemble the two app bundles arm side
./scripts/bundle-macos.sh arm64

# 2.2 Assemble the two app bundles Intel side
./scripts/bundle-macos.sh x86_64

# 3.1 Combine Arm and Intel for DMG
./scripts/make-universal.sh
```

**Output locations**

| What | Where |
|---|---|
| Binaries | `build-arm64/` — `putty`, `puttyapp`, `plink`, `pscp`, `psftp`, `psusan`, `puttygen`, `puttygenapp`, `pterm`, `pageant` |
| App bundles | `build-arm64/bundle-stage/unix/PuTTY.app`, `…/PuTTYGen.app` |

`bundle-macos.sh` builds **both** apps; `APPS=` overrides the list, e.g. to
rebuild one of them:

```sh
APPS="PuTTYGen:puttygen.bundle:puttygenapp:PuTTYGen.icns" ./scripts/bundle-macos.sh arm64
```

**Test the binaries without bundling:**

```sh
./build-arm64/putty -load "Default Settings"
./build-arm64/plink --version
./scripts/test-puttygen-gui.sh build-arm64     # the GUI against the CLI (33 checks)
```

> **The `.app` must be assembled with `bundle-macos.sh`.** The launcher inside
> the bundle sets `XDG_DATA_DIRS` to the bundle, which overrides GLib's fallback
> to the system GSettings schemas — so the bundle has to carry its own.
> `bundle-macos.sh` compiles 38 of them into
> `Contents/Resources/share/glib-2.0/schemas/`; without that, GTK's file chooser
> cannot find `org.gtk.Settings.FileChooser` and **opening any file picker
> aborts the process** (GLib calls `g_log_abort()`, SIGTRAP, exit 133). The bare
> binaries above are unaffected — they find the system schemas normally.

**It will not do that on a network mount.** If the stage is on an SMB/NFS/AFP
mount and you did not set `STAGE=` yourself, the script stages locally instead
(`~/Library/Caches/putty-beter/bundle-stage-<arch>`), says so, and copies the
finished `.app`s back to `build-<arch>/bundle-stage/unix/` at the end. The copy
back is per-file either way, so it pays the mount's latency: ~275 files per app for
the GTK closure, **plus the MCP bridge's ~3400 dependency files** on the arm Mac —
which measured on the Intel Mac's share is about **10 minutes per app**. The Intel
build bundles no bridge at all (see the Intel notes below), so its copy back stays
at ~275 files and about a minute; the merge takes the bridge and its dependencies
from the arm64 slice anyway (see "Universal apps", and the note in
`intel_arm_merge.md` §0). Set `STAGE=` to choose where the work happens, or
`PUTTY_BUNDLE_LOCAL_STAGE=` to change the cache directory.

**How long bundling takes, and why.** Bundling is not building: nothing is
compiled. gtk-mac-bundler copies the whole GTK closure into each `.app` — about
273 files and 44 MB, made up of 31 Homebrew formulae (47 dylibs), 26
gdk-pixbuf/immodule plugins and the Adwaita icon theme — and rewrites every
install name to point inside the bundle (~400 references, plus each library's own
id). That is a few thousand file read-modify-writes, and it happens **once per
app** (the bundler accepts exactly one bundle file per run), so `PuTTY.app` and
`PuTTYGen.app` each carry their own copy of GTK. Measured on this project: about
**1 minute per app** on a local disk, with the bundler itself ~90% of it and the
repair passes (Cellar→opt, dangling refs, `loaders.cache`, schemas, re-sign,
verify) ~6 s. A busy network mount can multiply that many times — the same work
once took 77+ minutes on an SMB share that later did it in 68 s — because the
latency lands on every one of those small writes (`codesign` also fsyncs each
file). So on a network mount, stage locally and copy the result back:

```sh
STAGE=~/tmp/stage scripts/bundle-macos.sh arm64     # or BUILD_DIR=… for the build
# then copy build-<arch>/bundle-stage/unix/*.app (or ~/tmp/stage/unix/*.app) back
```

**It only does that once.** The bundler has no notion of "that file is already
there", so `bundle-macos.sh` keeps the stage and reuses what is in it:

* an app whose GTK tree is already present is **reused in place** — only its
  binary, launcher, plist and icon are refreshed, and the binary is given the
  same inside-the-bundle install-name rewrite the bundler does;
* an app with no tree of its own **clones** the tree from the sibling app.

Both are keyed on a signature of the GTK inputs (the newest mtime of the dylibs,
plugins and icon theme, plus the arch and prefix) stored in the stage, so a
`brew upgrade` — or `BUNDLE_FULL=1` — still forces a complete run. Measured:
**~60 s per app fully bundled, ~6 s per app reused or cloned** (on a local disk;
PuTTY.app bundling, then PuTTYGen.app cloning, takes ~70 s in total). On a slow
network mount that is the difference between minutes and tens of minutes, which
is why the stage is no longer deleted at the start of every run.

**The copy back is skipped the same way.** Copying the finished apps onto a
network stage writes a marker recording which GTK inputs that copy came from, and
a later run re-copies only if the marker disagrees, the tree there is incomplete,
or the app's own plist or binary differ from the source. Those checks are a few
reads; the re-copy they avoid is minutes. (Its signature is *not* re-verified over
the network: it was verified in the local stage moments earlier, and hashing 275
files over SMB to confirm what you just copied buys nothing. The copy is checked
by counting files instead.)

**You can see what it is doing.** The bundler itself prints nothing while it
copies, so the script reports progress while it runs — every 15 s (tunable):
files of the tree so far, against the total a previous run recorded, plus the
size, the elapsed time, how many install names have been rewritten, and the file
in hand:

```
[PuTTY] bundling (0/243 files)...
[PuTTY]   progress every 15s; full detail: tail -f …/bundle-stage/bundler-PuTTY.log
[PuTTY]   0:00:10 | files 52 (peak 52) |  38M | rewrites 500 | idle 0s
[PuTTY]     now: Resources/Cellar/cairo/1.18.6/lib/libcairo.2.dylib
[PuTTY]   0:00:50 | files 242 (peak 242) |  51M | rewrites 2443 | idle 0s
[PuTTY]     now: MacOS/gdk-pixbuf-query-loaders
[PuTTY] bundler finished in 60s: 243 files, 51M
```

The copy back to a network stage reports itself too, since it is the other slow
part:

```
[PuTTY] copying 3705 files there (over a network mount this is many
[PuTTY]   small round trips, not one big write)
[PuTTY]   copy back: 1204/3705 files, 3m5s elapsed
[PuTTY] copied back in 9m38s: 3705 files,  65M, signature checked in the stage
```

(On the Intel Mac the bridge is not bundled at all, so its copy back is ~275 files
and about a minute — the universal merge supplies the bridge and its dependencies
from the arm64 slice.)

Four things to read off it: the file count (`n/total` once a previous run has
recorded the total, plus the peak, because gtk-mac-bundler's count *dips* as it
works), the size, the elapsed time, and **`rewrites`** — the number of
`install_name_tool` runs so far, which is where nearly all the time goes (about
**2400** for one app; the file count barely moves during that phase, so watch this
number and the `idle` field). `now:` names the file in hand and is only printed
when it changes. If neither the count nor the
log moves for 3 minutes, the script says so ("nothing has changed for 180s - this
looks stalled") and points at the local-stage workaround; a run that has actually
stopped otherwise looks exactly like a run that is slow. Each phase also prints
how long it took (`[PuTTYGen] gtk-mac-bundler took 54s`), and
`tail -f $STAGE/bundler-<app>.log` shows the bundler's own output (it is run with
`python -u`, so the log is not buffered). `BUNDLE_FULL=1` forces everything
through the bundler again — use it after upgrading GTK or changing
`unix/*.bundle`. `BUNDLE_PROGRESS=0` turns the progress lines off, and
`BUNDLE_PROGRESS_INTERVAL=`/`BUNDLE_STALL_WARN=` tune them.

### Universal apps, and the disk image

Once both slices exist — the arm64 bundle in `build-arm64/bundle-stage/unix/` and
the x86_64 one under `build-x86_64/` — one command merges them and packages the
result:

```sh
./scripts/make-universal.sh
```

It pairs every Mach-O in an app with its counterpart in the other arch and
`lipo -create`s them, so `PuTTY.app` and `PuTTYGen.app` each come out universal
(`x86_64 arm64`) and run natively on either Mac. It then writes a disk image whose
volume holds both apps and a symlink to `/Applications` — open it, drag both apps
across, done. The name is `DMG_NAME`'s business: these builds make
**`build-universal/PuTTY-mac-universal.dmg`** (on the command line,
`DMG_NAME=PuTTY-mac-universal.dmg`), where the script's own default is
`PuTTY-universal.dmg`.

| What | Where |
|---|---|
| Universal apps | `build-universal/PuTTY.app`, `build-universal/PuTTYGen.app` |
| Disk image | `build-universal/PuTTY-mac-universal.dmg` |

The image is a plain compressed (`UDZO`) image — no background picture and no
icon layout, which would need a mounted, Finder-arranged volume and buy nothing
for a two-app install. `MAKE_DMG=0` merges without building it; `DMG_NAME=` and
`DMG_VOLNAME=` rename it. The one-app form
(`scripts/make-universal.sh <arm64.app> <x86_64.app>`) writes
`<app>-universal.dmg` instead, so it cannot overwrite the two-app image. Every
app is checked before packaging — each Mach-O carries both slices,
`codesign --verify --deep --strict` passes, and no absolute Homebrew path leaked
into the binary — and the image is then `hdiutil verify`d.

**The apps are ad-hoc signed, not notarized.** Gatekeeper on a *different* Mac
shows the usual "unidentified developer" warning for a download, and the image
itself is not notarized. Right-click → **Open** works. The by-hand checks are in
[`intel_arm_merge.md`](intel_arm_merge.md) §7.

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

### Windows

The Windows port is built by one script, run **on Windows** (a VM is fine — it
cross-compiles, so the host's own architecture does not matter):

```powershell
# once: install Git, CMake, Ninja, MSYS2 x86_64 and mingw-w64 UCRT64
.\install-toolchain-win.ps1

# build and stage
.\scripts\build-windows.ps1
```

It is a **cross build** — the native `cmake`/`ninja` driving the x86_64 UCRT64
`gcc` — and it produces one binary for **Windows 7 SP1 → 11**
(`-D_WIN32_WINNT=0x0601`; Windows 7 also needs KB2999226), with a staged copy of
the MCP bridge beside it. `node --version` must answer, **Node 20+**, for the
bridge to be staged and smoke-tested.

**Output locations**

| What | Where |
|---|---|
| Binary | `putty-beter-win64\putty.exe` |
| Bridge | `putty-beter-win64\putty-mcp\` — mirrored from `mcp\`, with `node_modules` installed on Windows |
| Setup notes | `putty-beter-win64\SETUP-WIN.txt` — the `opencode.json` snippet and the CLI checks |

```powershell
.\scripts\build-windows.ps1 -Clean          # delete build-win\ first: full recompile
.\scripts\build-windows.ps1 -Reconfigure    # re-run the cmake configure step
.\scripts\build-windows.ps1 -ReinstallDeps  # re-run npm in the staged bridge
.\scripts\build-windows.ps1 -SkipBuild      # stage only, reuse the built putty.exe
```

`-Clean` refuses to delete anything that is not a `build-*` directory inside this
checkout. The remaining switches are `-SkipMcp`, `-SkipSmokeTest`, `-DebugOutput`
(also `BUILD_WIN_DEBUG=1`), and the overrides `-BuildDir`, `-ReleaseDir`,
`-Msys2Root`, `-NodeExe`, `-Jobs`. A second run without `-Clean` reuses the build
tree, so only changed C files are recompiled.

See [`WINDOWS-BUILD.md`](WINDOWS-BUILD.md) for the step-by-step, the timings, the
troubleshooting table and how to verify a build by hand. Debug tracing of the
running binary and bridge is separate: `PUTTY_MCP_DEBUG=1`.

---

## Intel (x86_64)

Homebrew builds GTK for a single architecture, so an Intel build needs an Intel
Homebrew in `/usr/local`. On an Intel Mac that is simply the native Homebrew; on
Apple Silicon it is a second Homebrew running under Rosetta, and the arm64 one in
`/opt/homebrew` cannot be used for an x86_64 build.

On an Intel Mac:

```sh
brew install gtk+3 cmake ninja pkgconf adwaita-icon-theme
./scripts/build-macos.sh x86_64       # -> build-x86_64/
./scripts/bundle-macos.sh x86_64      # -> build-x86_64/bundle-stage/unix/*.app
```

Cross-building x86_64 on Apple Silicon (needs Rosetta; `build-macos.sh` checks
for it, and for an x86_64 `cmake`, and tells you what is missing rather than
failing inside cmake):

```sh
arch -x86_64 /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
arch -x86_64 /usr/local/bin/brew install gtk+3 cmake ninja pkgconf adwaita-icon-theme
./scripts/build-macos.sh x86_64
```

Notes for the Intel path, since it is the one with the moving parts:

* **`PuTTYGen.app` is new here.** The x86_64 build and bundle were verified on
  the Intel Mac before it existed (see `intel_arm_merge.md`), so the first Intel
  build after this change should confirm the new `puttygenapp` target and the
  second bundle. `build-macos.sh` prints `puttygenapp -> x86_64` in its
  verification list, and `bundle-macos.sh` signs *and* verifies each app.
* **Each machine needs its own `tools/gtk-mac-bundler`** — it is not in the
  repository (GPL, gitignored). See [`tools/README.md`](tools/README.md).
* **The Intel bundle carries no MCP bridge.** `bundle-macos.sh` skips it when
  `ARCH=x86_64`: `mcp/node_modules` contains a symlink (npm's `.bin/node-which`
  shim), and from the Intel Mac this tree is on an SMB share, where a symlink
  **cannot be read at all** — `readlink`, `stat -L` and `cp -L` all fail with
  `EINVAL`, and only `lstat` works — so no copy tool can reproduce one, and
  `ditto` aborted the whole bundle run. The x86_64 bundle is a merge input only
  (measured: 275 files per app, against arm64's 3703), so this costs the shippable
  app nothing: the merged app gets the bridge and its dependencies from the arm64
  slice, which `make-universal.sh` treats as its structural authority. One
  consequence — the merge must run where symlinks are readable, i.e. on the arm
  Mac. [`tech_detail.md`](tech_detail.md) §13 has the detail.
* **A build directory cannot be moved between machines** that hold the repo at
  different paths (CMake records the source path). Both scripts detect that and
  tell you to re-run with `--clean`. `BUILD_DIR=`/`STAGE=` let you put the build
  tree on a local disk when the repo lives on a network share.
* Merging the two architectures into universal apps and building the installer
  disk image is one command — `scripts/make-universal.sh` (PLAN §1c / M8e). It
  merges **four** bundles into two universal apps and packages them as
  `build-universal/PuTTY-mac-universal.dmg`; see "Universal apps, and the disk
  image" above.
* Cross-building on Apple Silicon depends on Rosetta being present, and Apple has
  been retiring it; the native arm64 build does not.

---

## Notes

- `PuTTY.app` is ad-hoc signed, not notarized. If macOS quarantines it after
  download, use right-click → **Open**, or
  `xattr -dr com.apple.quarantine PuTTY.app`.
- This fork calls itself **`0.85-beter.1`** (not upstream's `0.85`, and not
  `Unidentified build`): that string is in each app's About box and in Finder's
  Get Info, and every About box links to
  <https://github.com/beterhans/putty-beter>. Upstream's copyright notice is kept
  as-is with this fork's own line below it — `This fork: Copyright 2026
  beterhans.` See [`CHANGES-FROM-UPSTREAM.md`](CHANGES-FROM-UPSTREAM.md) §18.
- Build output (`build-*/`) and the `gtk-mac-bundler` checkout are git-ignored.
- The defects fixed here — `OSX_GTK` never being defined, and the IME
  pre-edit cursor offset with its out-of-bounds read — are upstream PuTTY bugs
  and are worth reporting upstream. The full list is at the end of
  [`CHANGES-FROM-UPSTREAM.md`](CHANGES-FROM-UPSTREAM.md).

See [`CHANGES-FROM-UPSTREAM.md`](CHANGES-FROM-UPSTREAM.md) for every difference
from upstream PuTTY 0.85, and [`PLAN.md`](PLAN.md) for the remaining roadmap
(universal `.app`, signing/notarization, and the planned features).