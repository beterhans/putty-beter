# MCP: how it works

How the MCP server in this fork is put together, for whoever works on it next.
The *user-facing* documentation is [`mcp/USER-MANUAL.md`](../mcp/USER-MANUAL.md)
(what the AI reads, and readable by humans too); the *why* is `PLAN.md` §7,
including §7.11 for what was actually built. This file is the *how*: the files,
the protocol, the semantics, the socket, and the build.

Status: **phase 4a, plus `send`/`key`.** An agent can list sessions, read their output
and read the visible screen. It cannot type into a session yet (4b), cannot
open one (4e), and there is no audit log or policy layer (4d).

---

## 1. What it is for

An AI agent reads a session **in the window the human is looking at**, so the
human can watch, take over, and answer the prompts an agent cannot.

It is not a replacement for an SSH exec tool. Use `ssh-connect` for a Linux
shell — real exit codes, separated stderr, structural output attribution. This
is for a session that is already authenticated, network gear whose only
interface *is* an interactive CLI, a serial console with no second session to be
had, and a human who wants to see what is going on.

## 2. Shape

```
OpenCode ──stdio MCP (JSON-RPC)──> putty-mcp ──Unix socket──> PuTTY
             ("putty-mcp")          (mcp/, Node)             (unix/mcp.c)
```

Three rules keep it maintainable:

1. **No JSON in the C code.** The socket speaks a list of length-prefixed
   fields; `mcp/` does all of the MCP and JSON translation. MCP's spec churn
   therefore never touches the fork.
2. **The capture buffer is not the terminal's scrollback.** Scrollback is a
   display setting and can be turned down to nothing; MCP has to see every byte.
3. **The socket only exists while a session offers itself.** With the setting
   off there is no socket, no token and no file at all.

## 3. The files

| File | What is in it |
|---|---|
| `unix/mcp.c` | the socket server, the session registry, the handshake, the wire protocol, the endpoint files. GTK-aware but JSON-free |
| `unix/mcp.h` | the front end's whole interface: one session per window, a stream tap, a callback for the visible screen |
| `unix/mcp-buffer.c` | the capture buffer: escape stripping, line assembly, and the sliding window over absolute line ids. **No GTK, no terminal, no socket** — this is deliberate, and is what makes it testable |
| `unix/mcp-buffer.h` | its interface, and the id/cursor/gap semantics in prose |
| `test/test_mcp_buffer.c` | 95 checks over the buffer. Links only `utils` |
| `unix/window.c` | the front end's half: `McpSession *mcp` in `GtkFrontend`, `mcp_start()` / `mcp_stop()` / `mcp_get_view()`, and the tap |
| `conf.h`, `config.c` | `CONF_mcp_enabled` (`McpEnabled`), and the `Session/Automation (MCP)` panel |
| `mcp/bin/putty-mcp.mjs` | the entry point: `putty-mcp mcp` (the MCP server) and the human subcommands |
| `mcp/src/protocol.mjs` | endpoint discovery, framing, the socket client, and the command wrappers |
| `mcp/src/server.mjs` | the MCP tools and resources, and the `instructions` |
| `mcp/src/cli.mjs` | `list` / `read` / `screen` / `version` / `ping` / `manual` |
| `mcp/src/manual.mjs` | loads the manual, and holds the short `instructions` |
| `mcp/test/protocol.test.mjs` | 10 checks over the framing and the endpoint discovery. No PuTTY, no socket, no display |
| `mcp/USER-MANUAL.md` | the manual, served to the agent. **Canonical** |
| `docs/mcp-user-manual.md` | a byte-identical copy of it, for reading |
| `scripts/test-mcp.sh` | the end-to-end smoke test (18 checks) |

## 4. The build

The MCP code is compiled into **`guiterminal`**, so it is in every GTK front end
(`putty`, `puttyapp`, `pterm`, `ptermapp`) and in nothing else:

```cmake
add_sources_from_current_dir(guiterminal
  ...
  mcp.c mcp-buffer.c)
```

The buffer is also compiled straight into `test_mcp_buffer`, exactly as
`terminal/zmodem.c` is compiled into `test_zmodem`, so the test needs only
`utils`.

```sh
cmake --build build-arm64                  # binaries
scripts/build-macos.sh arm64               # + the usual checks
scripts/bundle-macos.sh arm64              # the .app (must be re-run: the
                                           # server is inside the front end)
```

The bridge is not built — it is Node, run from the source tree:

```sh
cd mcp && npm install        # once; @modelcontextprotocol/sdk + zod
```

`package-lock.json` is committed, so the SDK version is pinned.
`mcp/node_modules/` is git-ignored (23 MB).

**Nothing needs bundling for the bridge**, but the *app* must be rebuilt for a
new `.app` to have the server in it: `unix/mcp.c` is linked into `puttyapp`,
which is what the bundle runs. To check a bundle really has it:

```sh
APP=build-arm64/bundle-stage/unix/PuTTY.app
nm -a "$APP/Contents/MacOS/PuTTY-bin" | grep mcp_session_new
codesign --verify --deep --strict "$APP" && echo ok
```

Confirmed 2026-11-18: `scripts/bundle-macos.sh arm64` produces
`build-arm64/bundle-stage/unix/PuTTY.app` (arm64, ad-hoc signed, signature
verifies) whose `Contents/MacOS/PuTTY-bin` exports `mcp_session_new`,
`mcp_server_running`, `mcp_buffer_read` and `term_get_screen_text`. The bundle
stage lives inside the build directory, which is git-ignored, so re-bundling
never disturbs an app you are running.

## 5. The wire protocol

Both directions are the same, everything big-endian:

```
message := u32 nfields
           nfields * field
field   := u32 length, then `length` bytes
```

A field is a byte string — no escaping, no quoting, no NULs needed. Integers
travel as **decimal text**, so the two ends never have to agree on an integer
width. Limits: at most 64 fields, 1 MB per field, 4 MB per message. Anything
malformed, and any failed handshake, **drops the connection**.

Requests, `field 0` being the command:

| Request | Reply |
|---|---|
| `hello <protocol-version> <token>` | `ok <version>` — must be the first message |
| `sessions` | `ok <count>` then `<id> <name>` per session |
| `read <id> <since> <max-bytes>` | `ok <cursor> <total-lines> <missing-from> <missing-to> <omitted> <nlines>` then `<id> <author> <text>` per line |
| `screen <id>` | `ok <text> <cols> <rows>` |
| `send` | `ok <bytes-sent>` — refused unless the session allows typing |
| `key` | `ok` — refused unless the session allows typing |
| `version` | `ok <putty-version> <protocol>` |
| `ping` | `ok` |

Failures are `error <message>`.

* `sessions` reports `<id> <name> <ro|rw>` — the mode is there so a client knows
  before it tries whether it may type.
* An **empty** `since` means "from the start of the buffer"; an empty
  `max-bytes` means the server's default (8192).
* An empty `missing-from` **and** `missing-to` means nothing was lost.
* `author` is `d` (device), `h` (human at the keyboard) or `m` (an MCP client).
  `h` is not produced yet — see §10.

The protocol version is **2**, and the handshake checks it. Both ends ship
together, so a mismatch is a mistake rather than a compatibility question, and
it is reported as `unsupported protocol version` rather than misread. That
matters in practice: a client holds one `putty-mcp` process for the life of its
session, so a bridge built before a protocol change says so clearly instead of
appearing to find no server at all.

`send` and `key` are refused in two cases: the session does not allow typing, or
a ZMODEM transfer is running — during a transfer the stream is protocol and the
terminal is not seeing it, so typed bytes would corrupt the transfer rather than
do anything useful.

### Driving it by hand

The point of a dumb protocol is that it can be poked without the bridge:

```sh
RT="${TMPDIR:-/tmp}/putty-mcp-$(id -u)"
python3 - "$RT/sock" "$RT/token" <<'EOF'
import socket, struct, sys
def pack(fields):
    out = struct.pack(">I", len(fields))
    for f in fields:
        b = f.encode()
        out += struct.pack(">I", len(b)) + b
    return out
def recv(s):
    def need(n):
        b = b""
        while len(b) < n:
            b += s.recv(n - len(b))
        return b
    return [need(struct.unpack(">I", need(4))[0]).decode()
            for _ in range(struct.unpack(">I", need(4))[0])]

s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
s.sendall(pack(["hello", "1", open(sys.argv[2]).read().strip()]))
print("handshake:", recv(s))
s.sendall(pack(["sessions"]))
print("sessions: ", recv(s))
EOF
```

`printf`/`nc` work too — the fields are `\x00\x00\x00\x01` (one field) then
`\x00\x00\x00\x07sessions` — but the shell quoting is not worth it.

## 6. The capture buffer

The part to understand before changing anything. `mcp-buffer.h` has the same
explanation next to the code.

**Line ids are absolute and never reused.** Line 1 is the first line the session
ever printed, forever. The window slides over that numbering: when it fills, the
oldest lines are dropped and **nothing is renumbered**, so a window can hold
101…20100 and its first line is still id 101.

**`read` returns lines with an id greater than `since`**, plus a new `cursor`.
Feed the cursor back as the next `since`.

**Eviction is never silent.** A cursor that has fallen out of the window gets an
explicit gap — `missing_from`/`missing_to` — rather than quietly losing output.
`total_lines` is monotonic, so `total_lines - cursor` is always "how far behind
am I". `omitted` counts lines that are still held but did not fit this call's
budget: the reply **pages**, it does not truncate.

**Two budgets.** The read budget (`max_bytes`, default 8 KB) bounds one call;
the buffer budget (~4 MB per session, `MCP_BUFFER_DEFAULT_BUDGET`) bounds what
history exists at all. The plan also allows a total cap across sessions, which
is not implemented yet.

**The unfinished last line is reported, and does not move the cursor.** A shell
or a router sits at a prompt with no newline after it; that line is usually the
most important one. It is reported on every read, because it can still grow —
and it is **never truncated**, because a fragment of it ("Rout") would invite
the caller to believe the prompt is shorter than it is. A *completed* line is
truncated when it exceeds the whole budget, because the cursor has to be able to
move past it.

**A bare carriage return is "back to column 0", not a line ending.** That is
what a progress bar does. Only `\n`, or `\r\n`, ends a line — decided by looking
at the byte *after* the CR. Otherwise a slow download becomes one line per
redraw.

**Escapes are stripped before line assembly**: CSI, OSC (BEL or ST terminated),
DCS, and the `ESC <intermediate> <final>` forms like `ESC ( B`. This is a state
machine, not a regular expression, because a sequence can straddle a read
boundary and an OSC string can contain anything up to its terminator.

**Long lines are cut into 4 KB chunks** (`MCP_BUFFER_CHUNK`) so that one
unterminated line cannot fill the whole budget. The chunks are ordinary lines.

## 6a. Typing

`send` and `key` go through **`ldisc_send()`**, the same path a keystroke takes,
so local echo, line editing and the terminal's own handling all behave exactly
as they would for the person at the keyboard. The ZMODEM engine's send path
deliberately bypasses ldisc for the opposite reason: during a transfer those
bytes are protocol, not input.

Typing is a **separate opt-in** from reading (`McpWrite` against `McpEnabled`).
Offering a session to be read should not quietly grant the ability to drive it,
and the two settings have different consequences.

**A sent line is its own line in the transcript.** `mcp_buffer_note_sent()`
closes any line the far end is part-way through before appending what we sent,
and it treats a CR from us as a line ending rather than leaving it pending for
the next byte to interpret. Both matter, and the second one is subtle:

* Without the first, our text is merged into the device's prompt line
  (`Router>show version`) and cannot be told apart from device output, which
  defeats the point of tagging.
* Without the second, our Return sets the "pending CR" flag, and the far end's
  **echo** of the next character is then read as "back to column 0" — which
  *discards* the line, losing both the prompt and our command. That was a real
  bug, and it is what `test_sent_lines` in `test/test_mcp_buffer.c` pins down.

So a command shows up as: the interrupted prompt as a device line, then our
command as an `m` line, then the echo as a device line, then the output. The
screen still shows it as one line — `screen()` is for what it literally looks
like, `read` is the transcript.

## 7. The tap

`gtk_seat_output()` is where the **ZMODEM engine** is fed. MCP taps
`zm_terminal_output()` instead — the callback that hands bytes to `term_data()`
— and that is deliberate:

* `zm_terminal_output()` is called for exactly the bytes the terminal is about
  to be shown, which is the correct definition of what MCP should capture.
* At `gtk_seat_output()` the bytes have not been classified yet, so a transfer's
  prologue (`**B00…`) would land in the transcript as junk, and the engine's own
  pass-through text ("rz waiting to receive…") would be missed.

Only stdout is captured. `mcp_session_feed()` is a no-op when `inst->mcp` is
NULL, so with MCP off the stream is byte-identical to upstream.

## 8. The socket, and who may use it

`$TMPDIR/putty-mcp-<uid>/`, **one set of files per process**, named after the
pid:

| File | Mode | What |
|---|---|---|
| `sock.<pid>` | `0660`, or `0600` | the Unix socket |
| `token.<pid>` | `0600` | 32 random bytes from `/dev/urandom`, hex. Required in the handshake |
| `endpoint.<pid>` | `0600` | `socket`, `token`, `pid`, `version` — one per line |

The same endpoint file is also written into the **configuration folder** as
`mcp-endpoint.<pid>`, because a GUI app and a shell can disagree about
`$TMPDIR`. (`pterm` has no configuration folder, so it only writes the
`$TMPDIR` one.)

### Why per process, and why ids carry the pid

The plain `putty` and `pterm` binaries run **one process per window**, and each
process runs its own server. A single fixed socket name would be actively
harmful: the second PuTTY would unlink the first one's socket and bind the same
path, leaving the first silently unreachable — and, when the second exited, its
cleanup would unlink the path again. That was a real bug, found by running two
PuTTYs at once.

So the names are per-process, and **the bridge merges every server it finds**.
That is why a session id looks like `72377001`: the server's id base is
`pid * 1000`, so ids stay unique across processes without any shared state or
locking. The number is machine-facing; the `name` is what a human reads.

The bridge does not rely on the arithmetic, though: `read` and `screen` ask
each server in turn and take the first that does not say "no such session".
That keeps routing correct whatever the ids look like.

**Stale files are harmless and are not swept up.** A PuTTY that is killed
rather than closed never runs its cleanup, so its three files stay behind. The
bridge skips a candidate whose socket refuses the connection, and a new PuTTY
gets its own names, so nothing needs removing. (The smoke test removes its own,
because it is the thing that kills a PuTTY.) If the pile ever bothers you:
`rm -f $TMPDIR/putty-mcp-$(id -u)/*`. A sweep from inside the server was
rejected: a recycled pid would make it delete a *live* PuTTY's socket, which is
the very bug being avoided.

**Permissions.** `0660` alone would be worse than useless on macOS, where the
default group is `staff` and contains every local account — any local user could
then drive your switches. So the socket uses the **`putty-mcp` group** if it
exists, and falls back to owner-only `0600` if it does not:

```sh
sudo dseditgroup -o create putty-mcp
sudo dseditgroup -o edit -a youruser -t user putty-mcp
```

The bridge does not need the group: it runs as you and reads the token file.

**Why not the configuration folder, and why not TCP.** A Unix socket cannot live
on a synced or network filesystem, and `~/Documents` may be iCloud Drive. There
is no TCP listener at all in v1: if a second machine ever needs this, bind
`127.0.0.1` only and reach it through an SSH tunnel — never `0.0.0.0`, which
would put "type into my switches" on the LAN.

**Endpoint discovery** collects *every* candidate, not just the first, because
there can be several servers:

1. `--endpoint <file>`, and `$PUTTY_MCP_ENDPOINT`
2. every `mcp-endpoint*` file in each configuration folder, where the
   configuration folder is `$PUTTYDIR`, `~/.putty`, `$XDG_CONFIG_HOME/putty`
   and `~/.config/putty` — **and, after each of those, whatever its
   `config-location` redirect file points at**, mirroring `config_redirect_read()`
   in `unix/storage.c`. Without that the endpoint file of a *relocated*
   configuration would never be found, which is the shape most installations
   actually have.
3. every `endpoint*` file in `$TMPDIR/putty-mcp-<uid>/`
4. a guessed `$TMPDIR/putty-mcp-<uid>/sock`, with `--socket` / `--token-file`
   overriding

Newest file first, so a freshly started PuTTY is tried first. A candidate whose
socket file is gone is skipped, and so is one that refuses the connection.

`mcp/test/protocol.test.mjs` covers this without a display or a PuTTY: framing,
following a redirect, `~` expansion in a redirect, a redirect naming nothing,
and several processes with one stale. Run it with
`node mcp/test/protocol.test.mjs`, or as part of `scripts/test-mcp.sh`.

## 9. Configuration

`McpEnabled` in `conf.h`, off by default, per session, exposed as
*Session → Automation (MCP) → Offer this session to an MCP client*.

**The setting is read when the window is created**, so changing it needs the
session to be reopened. That is the single most confusing thing about the
feature for a new user, and the manual warns about it.

**Gotcha:** PuTTY reads its booleans with `atoi()`, so a hand-written session
file needs `McpEnabled=1`. `yes` silently reads as false. This cost an hour
during development; `scripts/test-mcp.sh` says why in a comment.

## 10. What is not done

| | Where it goes |
|---|---|
| `run`, `wait` — echo + quiet + prompt detection, and `needs_input` for `[confirm]`, `Password:`, `--More--` | 4b |
| Pager auto-answer for `--More--` | 4b — the headline feature for network gear |
| **Human keystrokes tagged `h`** | 4b. What *we* send is tagged `m` and the device is `d`, but text typed by the human is still reported as `d` |
| A human keystroke cancelling an in-flight operation | 4b. With fire-and-forget `send` there is no in-flight operation to cancel; it becomes necessary with `run` |
| Audit log, read-only/confirm modes, a GUI "AI is driving" badge | 4d |
| `open_session`, fleet runs, ZMODEM tools, a shadow `Terminal` for correct cursor addressing | 4e |

`send` and `key` were **pulled forward out of 4b** because they are what makes
the feature useful at all, and they are small: the plan's §7.7 already listed
them as separate tools from `run`. Everything that makes typing *safe* —
waiting, prompt detection, `needs_input`, cancelling — is still 4b, and until
then the caller does the waiting and the deciding by reading.

The plan's reasoning for each is in `PLAN.md` §7.5–§7.10.

## 11. Testing and debugging

```sh
./build-arm64/test_mcp_buffer         # 95 checks, no display, no socket
node mcp/test/protocol.test.mjs       # 10 checks on discovery and framing
./scripts/test-mcp.sh                 # 18 checks end to end; needs a display
cd test && PUTTY_TESTCRYPT=../build-arm64/testcrypt python3 cryptsuite.py
```

`scripts/test-mcp.sh` starts its own `pterm` against a throwaway `HOME` (a short
path under `/tmp`, because a Unix socket address is limited to about 104 bytes)
and drives the bridge. It touches nothing of yours, and it cleans up its own
socket files, which its `SIGTERM` would otherwise leave behind. `KEEP=1` keeps
the temporary folder; `VERBOSE=1` shows every command and its output.

It uses `HOME` rather than `PUTTYDIR` on purpose: `PUTTYDIR` is returned
directly by `make_filename()` and so bypasses the `config-location` redirect,
which would mean testing nothing. With `HOME` set, `$HOME/.putty/config-location`
is followed exactly as a real relocated installation is.

It is also safe to run while a PuTTY of your own is offering sessions — that is
what per-process sockets buy — and it looks its session up by name rather than
assuming an id.

### Changing the bridge while a client is running

**An MCP client spawns `putty-mcp mcp` once and keeps that process for the whole
life of its session.** Editing anything in `mcp/` therefore has no effect until
the client is restarted, and a running Node process cannot reload itself. This
is the single most annoying thing about working on the bridge, and the failure
mode is quiet: the old code keeps working, just wrongly. A field-count limit
that was too small for a `read` reply looked exactly like a malformed response
from PuTTY.

Three mitigations, all in place:

1. **The protocol version is checked at handshake.** A format change produces
   `unsupported protocol version`, not silence. Bump `MCP_PROTO_VERSION` in
   `unix/mcp.c` and `PROTOCOL_VERSION` in `mcp/src/protocol.mjs` together.
2. **The bridge warns when it is stale.** On the first request it compares its
   own source files' mtimes against its start time and writes to stderr:
   *"this bridge process started before N of its own source file(s) were last
   changed … restart the MCP client"*. Clients log MCP stderr, so it turns up
   in the client's log rather than nowhere.
3. **The CLI always runs the current code**, because it starts a fresh process
   for every invocation. When you need current behaviour mid-session, drive the
   session with `node mcp/bin/putty-mcp.mjs …` — that is what the CLI is for,
   and it is how the live testing of this feature was done.

**Killing the stale process does not help.** Verified by experiment: the client
notices the server has died and *drops it for the rest of the session*; it does
not respawn it on the next call. So `pkill -f "putty-mcp.mjs mcp"` leaves you
with no `putty-mcp` tools at all until the client is restarted. Restart the
client instead.

### Switches, both ends of the same conversation

* `PUTTY_MCP_DEBUG=1` in **PuTTY's** environment — whether a session offered
  itself, where the socket went and with what permissions, the handshake, and
  every request and reply.
* `putty-mcp list --debug` — the bridge's side of it.

Both were what found the bug where the bridge exited the moment it connected.
