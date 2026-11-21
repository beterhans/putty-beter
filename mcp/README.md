# putty-mcp

A bridge between an MCP client and a **running PuTTY window**.

An agent launches `putty-mcp mcp` and speaks MCP on stdio; this program turns
that into requests over a Unix socket to PuTTY. The point is that the agent
drives the session **in the window the human is looking at**, so the human can
watch, take over, and answer the prompts an agent cannot.

It can **read** (list sessions, read their output, read the visible screen) and
**type** (`send` text, `key` one keystroke). Reading and writing are separate
opt-ins in PuTTY, so a session can be offered for reading without being
driveable. What is *not* here yet is anything that waits: `send` returns
immediately, and prompt detection, `run` and pager auto-answer are phase 4b.

* For the agent-facing documentation, see **[USER-MANUAL.md](USER-MANUAL.md)**.
  It is served to the client by the `user_manual` tool and the
  `putty://manual` resource.
* For the design and the decisions behind it, see `PLAN.md` section 7 in the
  putty-beter source.

## How it fits together

```
OpenCode ──stdio MCP (JSON-RPC)──> putty-mcp ──Unix socket, length-prefixed fields──> PuTTY.app
                                       (this)                                        (unix/mcp.c)
```

The socket protocol is deliberately dumb — a list of length-prefixed fields, no
JSON, no escaping — because the other end is C and has no JSON parser, and
should not grow one. All of the MCP and JSON lives in this package, so MCP's
spec churn never touches the fork. The upside is that the socket can be driven
with no LLM in the loop at all, which is exactly what the CLI subcommands are
for.

## Setting it up

1. **Build.** `scripts/build-macos.sh arm64` (or `cmake --build build-arm64`).
2. **Turn MCP on for a session.** In PuTTY: *Session → Automation (MCP) → Offer
   this session to an MCP client*, then Save. Tick *Also allow an MCP client to
   type into this session* as well if the agent should be able to drive it.
   Both are off by default, because turning the first one on offers the session
   to anything that can reach the socket, and the second one lets it be driven.
   The settings are read when the window is created, so **reopen the session**
   after changing them.
3. **Install the bridge's dependencies** (once):
   ```sh
   cd mcp && npm install
   ```
4. **Point your MCP client at it.** For OpenCode, add to
   `~/.config/opencode/opencode.json`:
   ```json
   "putty-mcp": {
     "type": "local",
     "command": ["node", "/path/to/putty-beter/mcp/bin/putty-mcp.mjs", "mcp"]
   }
   ```

**PuTTY.app carries a copy too.** `bundle-macos.sh` installs this directory into
every PuTTY.app it builds, at `Contents/Resources/putty-mcp` — reachable at the
same path under `Contents/MacOS/putty-mcp`, through a symlink, because that is
where a runnable helper is expected. It is **source only**: the npm packages are
not bundled, because `npm install` inside the .app would change the bundle and
break its code signature. Install them once, globally, and let the app's copy see
them:

```sh
npm install -g '@modelcontextprotocol/sdk@^1.32.2' 'zod@^3.25.76'
ln -s "$(npm root -g)" /Applications/node_modules
```

Node does not consult a global location by itself — it looks for `node_modules` in
every directory *above* the file it runs — so that one symlink is what makes the
in-app path work; nothing is written inside the app, and re-installing the app
does not undo it. A client can then point straight at
`/Applications/PuTTY.app/Contents/Resources/putty-mcp/bin/putty-mcp.mjs`. That
copy carries `SETUP.txt`, which is steps 1–4 above with the in-bundle paths
filled in, including the alternative that pins the exact tested versions (copy
the folder out and `npm install` there, using the shipped `package-lock.json`).
You do not need any of it if you have the source — but it is what makes the app
self-describing.

## Using it without an agent

The same program is a plain command-line client, which is the first thing to
reach for when something is wrong:

```sh
node bin/putty-mcp.mjs list              # which sessions PuTTY is offering
node bin/putty-mcp.mjs read 1            # a session's output, with line ids
node bin/putty-mcp.mjs read 1 --since 12 # follow from a cursor
node bin/putty-mcp.mjs screen 1          # the visible screen
node bin/putty-mcp.mjs send 1 'ls -la'   # type into a session (needs McpWrite)
node bin/putty-mcp.mjs key 1 enter       # one keystroke: enter, ctrl-c, up, ...
node bin/putty-mcp.mjs version           # PuTTY build and protocol version
node bin/putty-mcp.mjs manual            # what the agent reads
node bin/putty-mcp.mjs read 1 --json     # the raw result
```

`send` does **not** add a newline — send the text, then `key 1 enter`. (And do
not write `send 1 "$(printf 'ls\n')"`: command substitution strips the trailing
newline, so nothing would ever press Return.)

Or link it: `npm link`, and `putty-mcp list` works anywhere.

## Where the socket lives

PuTTY writes `$TMPDIR/putty-mcp-<uid>/` containing **one set of files per
process**, named after the pid:

| file | what |
|---|---|
| `sock.<pid>` | the Unix socket, `0660` for the `putty-mcp` group if that group exists, otherwise `0600` |
| `token.<pid>` | `0600`; the 32-byte handshake token, hex |
| `endpoint.<pid>` | `0600`; socket path, token path, pid and protocol version |

It also writes the same endpoint file into the configuration folder as
`mcp-endpoint.<pid>`, because a GUI app and a shell can disagree about
`$TMPDIR`. (`pterm` has no configuration folder, so it only writes the `$TMPDIR`
one.)

**Per process, because more than one PuTTY can be offering sessions.** The
plain `putty` and `pterm` binaries run one process per window, each with its own
server; a single fixed socket name would let the second one unlink the first
one's socket. The bridge therefore reads *every* endpoint file it can find and
merges the session lists, which is why a session id looks like `72377001` — the
leading digits are the pid, so ids stay unique across processes.

The bridge looks for endpoints here, newest file first:

1. `--endpoint`, `$PUTTY_MCP_ENDPOINT`
2. every `mcp-endpoint*` in each configuration folder — `$PUTTYDIR`, `~/.putty`,
   `$XDG_CONFIG_HOME/putty`, `~/.config/putty` — **and whatever each one's
   `config-location.conf` file redirects to**, which is how a relocated
   configuration is found
3. every `endpoint*` in `$TMPDIR/putty-mcp-<uid>/`
4. a guessed `$TMPDIR/putty-mcp-<uid>/sock`, with `--socket`/`--token-file`
   overriding anything

A candidate whose socket is gone, or that refuses the connection, is skipped —
so files left behind by a PuTTY that was killed are harmless and do not need
removing.

**A socket only exists while at least one session has MCP turned on.** With the
setting off there is no socket, no token and no file.

### The dedicated group

`0660` on its own would be worse than useless on macOS, where the default group
is `staff` and contains every local account — any local user could then type
into your switches. So the socket uses the `putty-mcp` group if it exists, and
falls back to owner-only `0600` if it does not:

```sh
# To let a second account drive this PuTTY (both must be in the group):
sudo dseditgroup -o create putty-mcp
sudo dseditgroup -o edit -a youruser -t user putty-mcp
```

The bridge does not need the group: it runs as you, and reads the token file.

## If you change the bridge, restart the client

An MCP client spawns `putty-mcp mcp` **once** and keeps that process for the
whole life of its session, and a running Node process cannot reload itself. So
editing anything in this directory has no effect until the client is restarted —
and the old code keeps working, just wrongly, which is worse than failing.

* The bridge warns on stderr if it notices it is stale (its own source files are
  newer than the process), so check the client's MCP log.
* The protocol version is checked at the handshake, so a *format* change is
  reported as `unsupported protocol version` rather than misread.
* The CLI is unaffected: it starts a fresh process per invocation, so it always
  runs the current code. Use it when you need current behaviour mid-session.

**Do not just kill the process.** The client notices the server died and drops
it for the rest of the session; it does not respawn it. Restart the client.

## Debugging

* `PUTTY_MCP_DEBUG=1` in **PuTTY's** environment traces the socket, the
  handshake, every request and every response on stderr.
* `putty-mcp list --debug` traces the bridge's side of the same conversation.
* `PUTTY_CONFIG_DEBUG=1` traces where PuTTY thinks its configuration folder is.

## Licence

MIT, like the rest of PuTTY-beter.
