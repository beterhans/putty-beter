#!/bin/sh
#
# scripts/test-mcp.sh - smoke-test the MCP server and the putty-mcp bridge.
#
# It starts its own PuTTY (pterm, running a shell command with predictable
# output) against a throwaway configuration folder, then drives the server
# through the bridge CLI and checks what comes back. Nothing of yours is
# touched: PUTTYDIR points at a temporary directory, and the session ends when
# the script does.
#
# Needs a usable display, because pterm is a GUI program. The window appears
# for a few seconds.
#
#   scripts/test-mcp.sh                 quiet; one line per check
#   VERBOSE=1 scripts/test-mcp.sh       also print every command and its output
#   BUILD=build-x86_64 scripts/test-mcp.sh
#   KEEP=1 scripts/test-mcp.sh          do not delete the temporary folder
#
# Exit status is 0 only if every check passed.

set -u

BUILD=${BUILD:-build-arm64}
VERBOSE=${VERBOSE:-0}
KEEP=${KEEP:-0}

here=$(cd "$(dirname "$0")/.." && pwd)
root=$here
bridge=$root/mcp/bin/putty-mcp.mjs
[ -x "$root/$BUILD/pterm" ] || { echo "no $BUILD/pterm; run scripts/build-macos.sh" >&2; exit 2; }
[ -f "$bridge" ] || { echo "no $bridge" >&2; exit 2; }

# Deliberately short: a Unix socket address is limited to about 104 bytes, and
# the server refuses to start rather than truncate one.
work=/tmp/putty-mcp-smoke-$$

# A throwaway HOME, so that the *relocated* configuration shape is what gets
# tested: $HOME/.putty holds a config-location file naming the folder actually
# in use, and the session lives in that one. That is how a real installation
# looks after the configuration has been moved, and it is what the server has
# to write its mcp-endpoint into and the bridge has to follow.
#
# HOME rather than PUTTYDIR, deliberately: PUTTYDIR is returned directly by
# make_filename() and so bypasses the redirect altogether, which would test
# nothing.
home=$work/home
base=$home/.putty
conf=$work/config
mkdir -p "$base" "$conf/sessions" || exit 2
printf '%s\n' "$conf" > "$base/config-location"

# "hostname" is what the session is called in the list, and =1 not =yes: PuTTY
# reads its booleans with atoi(). McpWrite is on so that send/key can be
# tested; reading and writing are separate settings on purpose.
printf 'HostName=mcp-smoke\nMcpEnabled=1\nMcpWrite=1\n' \
    > "$conf/sessions/Default%20Settings"

pterm_pid=
cleanup() {
    [ -n "$pterm_pid" ] && kill "$pterm_pid" 2>/dev/null
    [ -n "$pterm_pid" ] && wait "$pterm_pid" 2>/dev/null

    # SIGTERM does not run the server's own cleanup, so its three files would
    # be left behind - harmless (the bridge skips a socket that refuses the
    # connection) but untidy, and this script is the thing that made them.
    if [ -n "$pterm_pid" ]; then
        rt=${TMPDIR:-/tmp}/putty-mcp-$(id -u)
        rm -f "$rt/sock.$pterm_pid" "$rt/token.$pterm_pid" \
              "$rt/endpoint.$pterm_pid"
    fi

    if [ "$KEEP" = 1 ]; then
        echo "kept $work"
    else
        rm -rf "$work"
    fi
}
trap cleanup EXIT INT TERM

passes=0
failures=0
check() {
    # check <description> <expected> <actual>
    if [ "$2" = "$3" ]; then
        passes=$((passes + 1))
        printf '  ok    %s\n' "$1"
    else
        failures=$((failures + 1))
        printf '  FAIL  %s\n        expected: %s\n        actual:   %s\n' \
               "$1" "$2" "$3"
    fi
}

/*
 * Progress and debug go to stderr, not stdout. `run` is called inside command
 * substitutions - `$(run list | python3 ...)` - so anything it writes to
 * stdout is fed to the JSON parser along with the JSON, and VERBOSE=1 breaks
 * the test it is meant to help with.
 */
say() { [ "$VERBOSE" = 1 ] && echo "  ...   $*" >&2 || true; }

run() {
    say "putty-mcp $*"
    out=$(HOME="$home" node "$bridge" "$@" --json 2>&1) || true
    say "$out"
    printf '%s' "$out"
}

echo "MCP smoke test"
echo "  build    $BUILD"


# ------------------------------------------------------------------
# A copy of the agent's manual lives in docs/ for humans to read. Checked
# first, so it is reported even on a machine with no display.
# ------------------------------------------------------------------

if [ -f "$root/docs/mcp-user-manual.md" ]; then
    if cmp -s "$root/mcp/USER-MANUAL.md" "$root/docs/mcp-user-manual.md"; then
        check "the manual and its docs/ copy are identical" "same" "same"
    else
        check "the manual and its docs/ copy are identical" "same" "different"
        echo "        cp mcp/USER-MANUAL.md docs/mcp-user-manual.md"
    fi
fi

# ------------------------------------------------------------------
# Start the session. The command's output is fixed, and its last line has no
# newline at all, so the "unfinished line is the prompt" case is covered.
# ------------------------------------------------------------------

# Run it from the throwaway folder: pterm has no configuration folder, so its
# default log file is a plain "putty.log" in the current directory, and we would
# rather not litter the source tree with it.
cd "$work" || exit 2
# The shell prints a few lines, sits at an unfinished prompt (so the
# "unfinished line" case is covered), then reads a line and reports it back -
# which is what proves that text typed through MCP actually reached the shell.
# It loops, so a prompt comes back after every command: that is what `run`
# needs in order to detect completion, and it is what a real device does.
HOME="$home" "$root/$BUILD/pterm" -e /bin/sh -c '
    printf "line one\n";
    printf "line two\n";
    printf "coloured \033[31mred\033[0m text\n";
    while :; do
        printf "Router>";
        IFS= read -r line || break;
        case "$line" in
            !*) eval "${line#!}" ;;
            *) printf "got: %s\n" "$line" ;;
        esac;
    done' > "$work/pterm.log" 2>&1 &
pterm_pid=$!
say "pterm pid $pterm_pid"

# Wait for the socket to appear, up to ~10s. Its name carries the pid, because
# more than one PuTTY can be offering sessions at once - so it is this pterm's
# own pid, not a fixed name, that we look for.
sock=${TMPDIR:-/tmp}/putty-mcp-$(id -u)/sock.$pterm_pid
i=0
while [ ! -S "$sock" ] && [ "$i" -lt 100 ]; do
    i=$((i + 1))
    sleep 0.1
done
if [ ! -S "$sock" ]; then
    echo "the MCP socket never appeared at $sock" >&2
    echo "--- pterm output ---" >&2
    cat "$work/pterm.log" >&2
    exit 1
fi
echo "  ok    the server came up ($sock)"
passes=$((passes + 1))

# Give the shell a moment to print everything.
sleep 1

# ------------------------------------------------------------------

ids=$(run list | python3 -c '
import sys, json
try:
    s = json.load(sys.stdin)
except Exception:
    print("BAD-JSON"); raise SystemExit
print(",".join("%d=%s" % (x["id"], x["name"]) for x in s))
')
echo "  ...   sessions: $ids"
sid=$(printf '%s' "$ids" | tr ',' '\n' | grep '=mcp-smoke$' | cut -d= -f1)
if [ -z "$sid" ]; then
    echo "FAIL: the smoke session is not in the list" >&2
    exit 1
fi
check "list finds the session by name" "$sid=mcp-smoke" \
      "$(printf '%s' "$ids" | tr ',' '\n' | grep "^$sid=")"

# ------------------------------------------------------------------
# The bridge's own tests. Discovery is where a relocated configuration is
# followed, and pterm cannot exercise that: it has no configuration folder, so
# it only ever writes its endpoint into $TMPDIR. Needs no display, so it runs
# here rather than after the window is up.
# ------------------------------------------------------------------

if node "$root/mcp/test/protocol.test.mjs" > "$work/bridge.out" 2>&1; then
    check "the bridge's own tests pass" "0 failed" \
          "$(sed -n 's/.*, \([0-9]*\) failed/\1 failed/p' "$work/bridge.out" | tail -1)"
else
    check "the bridge's own tests pass" "0 failed" "$(tail -1 "$work/bridge.out")"
    sed -n 's/^/        /p' "$work/bridge.out" >&2
fi

# The run loop and its prompt/pager/input detection, likewise with no PuTTY and
# no display: a virtual session and a virtual clock.
if node "$root/mcp/test/run.test.mjs" > "$work/run-tests.out" 2>&1; then
    check "the run tests pass" "0 failed" \
          "$(sed -n 's/.*, \([0-9]*\) failed/\1 failed/p' "$work/run-tests.out" | tail -1)"
else
    check "the run tests pass" "0 failed" "$(tail -1 "$work/run-tests.out")"
    sed -n 's/^/        /p' "$work/run-tests.out" >&2
fi

# Everything so far, in order. The ANSI escapes must be gone, and the
# unfinished line must be there with an id but without moving the cursor.
all=$(run read "$sid" | python3 -c '
import sys, json
r = json.load(sys.stdin)
print("|".join("%s:%s" % (l["id"], l["text"]) for l in r["lines"]))
print(r["cursor"], r["total_lines"], r["omitted"], file=sys.stderr)
' 2>"$work/meta")
check "read: lines, escapes stripped, unfinished line present" \
      "1:line one|2:line two|3:coloured red text|4:Router>" "$all"

check "read: the unfinished line does not move the cursor" "3 3 0" \
      "$(cat "$work/meta")"

# Following the cursor returns only the line in progress.
again=$(run read "$sid" --since 3 | python3 -c '
import sys, json
r = json.load(sys.stdin)
print("|".join("%s:%s" % (l["id"], l["text"]) for l in r["lines"]))
')
check "read --since <cursor> returns only the unfinished line" "4:Router>" "$again"

# A small budget pages, and must not hand back a fragment of the unfinished
# line: "Rout" would be worse than nothing, because it looks like the prompt.
tiny=$(run read "$sid" --max-bytes 8 | python3 -c '
import sys, json
r = json.load(sys.stdin)
print("|".join("%s:%s" % (l["id"], l["text"]) for l in r["lines"]))
print(r["omitted"], file=sys.stderr)
' 2>"$work/tiny")
check "read --max-bytes: pages down to one line, no cut fragment" "1:line one" "$tiny"
check "read --max-bytes: says how many lines are left" "2" "$(cat "$work/tiny")"

# The visible screen.
screen=$(run screen "$sid" | python3 -c '
import sys, json
s = json.load(sys.stdin)
print("|".join(x for x in s["text"].split("\n")[:4]))
print("%dx%d" % (s["cols"], s["rows"]), file=sys.stderr)
' 2>"$work/geom")
check "screen: the visible screen" \
      "line one|line two|coloured red text|Router>" "$screen"
check "screen: reports the terminal size" "128x36" "$(cat "$work/geom")"

# ------------------------------------------------------------------
# Typing. The shell is waiting at a prompt, and reads one line.
# ------------------------------------------------------------------

# Text first, then Return, which is how the CLI is meant to be used. (Not
# "$(printf 'x\n')": command substitution strips the trailing newline, so
# nothing would ever press Return.)
send_out=$(HOME="$home" node "$bridge" send "$sid" --text 'hello from mcp' 2>&1) || true
say "$send_out"
case "$send_out" in
    "sent 14 byte(s)") check "send: text is accepted" "sent 14 byte(s)" "$send_out" ;;
    *)                 check "send: text is accepted" "sent 14 byte(s)" "$send_out" ;;
esac

key_out=$(HOME="$home" node "$bridge" key "$sid" enter 2>&1) || true
say "$key_out"
check "key: a named keystroke is accepted" "sent enter" "$key_out"

# The shell's `read` should now have returned, and it prints what it got.
sleep 1
authors=$(run read "$sid" | python3 -c '
import sys, json
r = json.load(sys.stdin)
for tag in ("m", "d"):
    print(tag + "=" + "|".join(l["text"] for l in r["lines"] if l["author"] == tag))
')
say "$authors"

dev=$(printf '%s\n' "$authors" | sed -n 's/^d=//p')
case "$dev" in
    *"got: hello from mcp"*)
        check "send: the text reached the shell, which answered" "reached" "reached" ;;
    *)  check "send: the text reached the shell, which answered" "reached" "$dev" ;;
esac

# What we sent is a line of its own, tagged MCP - not merged into the device's
# prompt line, which is what makes the transcript readable when a human and an
# agent are both typing.
mcp=$(printf '%s\n' "$authors" | sed -n 's/^m=//p')
case "$mcp" in
    *"hello from mcp"*)
        check "send: our own text is tagged as MCP, not as the device" "tagged" "tagged" ;;
    *)  check "send: our own text is tagged as MCP, not as the device" "tagged" "$authors" ;;
esac

# And the prompt we interrupted is still there, as the device's own line.
case "$dev" in
    *"Router>"*)
        check "send: the interrupted prompt is kept as a device line" "kept" "kept" ;;
    *)  check "send: the interrupted prompt is kept as a device line" "kept" "$dev" ;;
esac

bad_key=$(HOME="$home" node "$bridge" key "$sid" flurble 2>&1) || true
case "$bad_key" in
    *"unknown key name"*) check "key: an unknown key name is refused" "refused" "refused" ;;
    *)                    check "key: an unknown key name is refused" "refused" "$bad_key" ;;
esac

# A session id that does not exist is refused, not guessed at.
if HOME="$home" node "$bridge" read 999 --json >/dev/null 2>"$work/err"; then
    check "an unknown session id is refused" "non-zero exit" "zero"
else
    check "an unknown session id is refused" "no such session" \
          "$(sed -n 's/^putty-mcp: //p' "$work/err" | head -1)"
fi

# ------------------------------------------------------------------
# run: send one command, wait for it to finish, print just its output.
#
# The shell above executes a line that starts with "!" and reports anything
# else back as "got: ...", which is what the send tests above rely on.
# ------------------------------------------------------------------

# How `run` says why it stopped, from its summary line on stderr:
# "(prompt, echo seen, 400ms, cursor 12)".
run_line() {
    sed -n 's/^(\([a-z_]*\).*/\1/p' "$1"
}

# The ordinary case: the device echoes the command, runs it, prints the result,
# and puts the prompt back. What comes back is the result and nothing else -
# not the echo, not the prompt we interrupted, not the prompt that returned.
run_out=$(HOME="$home" node "$bridge" run "$sid" '!echo run-ok' 2>"$work/run.err") || true
say "$run_out"
say "$(cat "$work/run.err")"
check "run: returns the command's output and nothing else" "run-ok" "$run_out"
check "run: finishes at a prompt" "prompt" "$(run_line "$work/run.err")"
case "$(cat "$work/run.err")" in
    *"echo seen"*) check "run: notices the device echoed the command" "seen" "seen" ;;
    *)             check "run: notices the device echoed the command" "seen" \
                          "$(cat "$work/run.err")" ;;
esac

# A slow command is the weakness of the quiet signal: it prints nothing for a
# second, and a bare quiet period would call it finished with no output at all.
run_slow=$(HOME="$home" node "$bridge" run "$sid" '!sleep 1; echo slow-ok' 2>"$work/slow.err") || true
say "$run_slow"
check "run: a slow command is waited for, not cut off" "slow-ok" "$run_slow"
check "run: and still finishes at a prompt" "prompt" "$(run_line "$work/slow.err")"

# A pager is the case that hangs a naive client for ever: the device stops and
# waits, and nothing about the stream says so. Real BSD `more`, deliberately,
# because it is the awkward one: given a file argument it puts the *file name*
# where the prompt goes on the first page, in reverse video, and `:` from the
# second page on. Both are measured, not assumed - see the note in detect.mjs.
i=0
while [ "$i" -lt 200 ]; do
    printf 'line %d\n' "$i"
    i=$((i + 1))
done > "$work/big.txt"

run_more=$(HOME="$home" node "$bridge" run "$sid" "!more $work/big.txt" 2>"$work/more.err") || true
say "$(cat "$work/more.err")"
case "$(cat "$work/more.err")" in
    *"page(s) answered"*) check "run: a real pager is answered without being asked" \
                                "answered" "answered" ;;
    *)                    check "run: a real pager is answered without being asked" \
                                "answered" "$(cat "$work/more.err")" ;;
esac
case "$run_more" in
    *"line 199"*) check "run: every page comes back" "all" "all" ;;
    *)            check "run: every page comes back" "all" \
                        "$(printf '%s' "$run_more" | tail -2)" ;;
esac

# Network gear's own pager, which is a different prompt from a Unix one: it
# prints `--More--` and waits for a single byte. A shell can only do that with
# the terminal in non-canonical mode, because otherwise a read waits for a whole
# line - which is exactly why this is written out rather than approximated.
#
# It is a stand-in, not a router, but it is the real byte pattern on a real pty,
# and `--More--` is the thing the plan calls the headline feature for network
# gear. What a router does that this does not is send it in a different place
# and with its own idea of how many lines fit.
cat > "$work/fakegear.sh" <<'FAKEGEAR'
i=0
while [ "$i" -lt 60 ]; do
    printf 'line %d\n' "$i"
    i=$((i + 1))
    if [ $((i % 20)) -eq 0 ] && [ "$i" -lt 60 ]; then
        stty -icanon min 1
        printf -- '--More--'
        dd bs=1 count=1 2>/dev/null
        stty icanon
        printf '\r        \r'
    fi
done
printf 'Router>'
FAKEGEAR

run_gear=$(HOME="$home" node "$bridge" run "$sid" "!sh $work/fakegear.sh" 2>"$work/gear.err") || true
say "$(cat "$work/gear.err")"
case "$(cat "$work/gear.err")" in
    *"2 page(s) answered"*) check "run: a network-gear --More-- is answered" \
                                   "answered" "answered" ;;
    *)                      check "run: a network-gear --More-- is answered" \
                                   "answered" "$(cat "$work/gear.err")" ;;
esac
case "$run_gear" in
    *"line 59"*) check "run: and the whole command comes back through it" "all" "all" ;;
    *)           check "run: and the whole command comes back through it" "all" \
                        "$(printf '%s' "$run_gear" | tail -2)" ;;
esac
check "run: and finishes at the prompt after it" "prompt" "$(run_line "$work/gear.err")"

# A command wider than the terminal. This is the case that was wrong on real
# gear: the shell emits a **bare carriage return** at every visual wrap, so that
# its own cursor bookkeeping stays right (measured: after 114 and 244 bytes of a
# 305-character command in a 128-column terminal). The capture buffer reads a
# bare CR as "back to column 0" - which is exactly what keeps a progress bar to
# one line - so every row of the echo but the last is discarded, and what is
# left is the tail of the command. That tail must not be handed back as output.
longstr=$(printf '0123456789%.0s' $(seq 1 30))
long_run=$(HOME="$home" node "$bridge" run "$sid" "!echo $longstr" 2>"$work/long.err") || true
say "$(cat "$work/long.err")"
case "$(cat "$work/long.err")" in
    *"echo seen"*) check "run: a command wider than the terminal is still seen as echoed" \
                          "seen" "seen" ;;
    *)             check "run: a command wider than the terminal is still seen as echoed" \
                          "seen" "$(cat "$work/long.err")" ;;
esac
check "run: and a wrapped echo does not leak into the output" "1 line, 300 chars" \
      "$(printf '%s' "$long_run" | python3 -c 'import sys; s=sys.stdin.read().rstrip("\n"); print("%d line, %d chars" % (len(s.split("\n")), len(s)))')"

# A prompt that wants input: the run stops and reports rather than guessing.
# Answering it is the caller's decision, and a password is never the agent's.
pw_out=$(HOME="$home" node "$bridge" run "$sid" \
    '!printf "Password: "; read x; echo pw-was-$x' 2>"$work/pw.err") || true
say "$(cat "$work/pw.err")"
check "run: a password prompt stops the run" "needs_input" "$(run_line "$work/pw.err")"
case "$(cat "$work/pw.err")" in
    *"wants a password prompt"*) check "run: and says what it wants" "yes" "yes" ;;
    *)                           check "run: and says what it wants" "yes" \
                                       "$(cat "$work/pw.err")" ;;
esac

# ...and nothing was typed at it, so the session is still waiting. Answer it as
# the human would, and the session must be usable again straight away.
HOME="$home" node "$bridge" send "$sid" hunter2 >/dev/null 2>&1 || true
HOME="$home" node "$bridge" key "$sid" enter >/dev/null 2>&1 || true
sleep 1
back=$(HOME="$home" node "$bridge" run "$sid" '!echo back-ok' 2>"$work/back.err") || true
check "run: the session works again after answering a prompt" "back-ok" "$back"

# A command that never finishes is reported as a timeout, not as an answer.
hang=$(HOME="$home" node "$bridge" run "$sid" '!sleep 3' --timeout 600 \
       2>"$work/hang.err") || true
check "run: a command that hangs is reported as a timeout" "timeout" \
      "$(run_line "$work/hang.err")"

# ------------------------------------------------------------------

echo
if [ "$failures" = 0 ]; then
    echo "$passes checks passed"
    exit 0
fi
echo "$passes passed, $failures failed"
exit 1
