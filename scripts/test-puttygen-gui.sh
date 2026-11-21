#!/usr/bin/env bash
#
# test-puttygen-gui.sh — check the GTK PuTTYgen front end against the
# command-line puttygen, which is the oracle.
#
# The GUI cannot be driven by a human in an automated run, so puttygen.c has a
# self-test hook: with PUTTYGEN_SELFTEST set, the window generates a key as
# soon as it opens, prints its fingerprint and public key, optionally saves the
# .ppk, and quits; with PUTTYGEN_SELFTEST_LOAD set it opens an existing key
# instead. Everything below goes through exactly the code the buttons use.
#
# Usage:
#   scripts/test-puttygen-gui.sh [BUILD_DIR]
#
# BUILD_DIR defaults to ./build-arm64 (see scripts/build-macos.sh).
#
# Environment:
#   DEBUG=1     show every command and the raw self-test output
#   WORK=...    scratch directory (default: ${TMPDIR:-/tmp}/puttygen-gui-test)

set -euo pipefail

DEBUG="${DEBUG:-0}"
log()  { printf '[test-puttygen-gui] %s\n' "$*" >&2; }
dbg()  { [ "$DEBUG" = "1" ] && printf '[test-puttygen-gui][debug] %s\n' "$*" >&2; return 0; }
fail() { printf '[test-puttygen-gui] FAIL: %s\n' "$*" >&2; exit 1; }

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-$SRC_DIR/build-arm64}"
WORK="${WORK:-${TMPDIR:-/tmp}/puttygen-gui-test}"

GUI="$BUILD_DIR/puttygenapp"
CLI="$BUILD_DIR/puttygen"

[ -x "$GUI" ] || fail "no GUI binary at $GUI (build it first)"
[ -x "$CLI" ] || fail "no CLI binary at $CLI"

mkdir -p "$WORK"
log "build dir : $BUILD_DIR"
log "work dir  : $WORK"

PASS=0
CHECK=0

# run_gui <env-name=value>... -- prints the self-test output
run_gui() {
    dbg "env $* $GUI"
    if [ "$DEBUG" = "1" ]; then
        env "$@" "$GUI"
    else
        env "$@" "$GUI" 2>/dev/null
    fi
}

field() { sed -n "s/^$1=//p" | head -1; }

check() {                               # check <what> <expected> <actual>
    CHECK=$((CHECK + 1))
    if [ "$2" = "$3" ]; then
        PASS=$((PASS + 1))
        log "  ok   $1"
    else
        log "  FAIL $1"
        log "       expected: $2"
        log "       actual  : $3"
        FAILED=1
    fi
}

# ---------------------------------------------------------------------------
log "1. generate every key type and compare with the CLI"

gen_and_compare() {                     # gen_and_compare <selftest> <bits|-> <label>
    local kind="$1" bits="$2" label="$3"
    local ppk="$WORK/gen-$label.ppk" out
    rm -f "$ppk"

    if [ "$bits" = "-" ]; then
        out="$(run_gui "PUTTYGEN_SELFTEST=$kind" \
                       "PUTTYGEN_SELFTEST_OUT=$ppk")"
    else
        out="$(run_gui "PUTTYGEN_SELFTEST=$kind" \
                       "PUTTYGEN_SELFTEST_BITS=$bits" \
                       "PUTTYGEN_SELFTEST_OUT=$ppk")"
    fi

    local gui_fp gui_pub cli_fp cli_pub
    gui_fp="$(printf '%s\n' "$out" | field fingerprint)"
    gui_pub="$(printf '%s\n' "$out" | field public)"
    cli_fp="$("$CLI" -l "$ppk")"
    cli_pub="$("$CLI" -L "$ppk")"

    check "$label: saved file exists" "yes" "$([ -s "$ppk" ] && echo yes || echo no)"
    check "$label: GUI fingerprint == CLI -l" "$cli_fp" "$gui_fp"
    check "$label: GUI public key == CLI -L" "$cli_pub" "$gui_pub"
}

gen_and_compare ed25519 -    ed25519
gen_and_compare ed448   -    ed448
gen_and_compare ecdsa   256  ecdsa256
gen_and_compare ecdsa   384  ecdsa384
gen_and_compare ecdsa   521  ecdsa521
gen_and_compare rsa     2048 rsa2048
gen_and_compare dsa     1024 dsa1024

# ---------------------------------------------------------------------------
log "2. load keys the CLI made, in every format the CLI can write"

load_and_compare() {                    # load_and_compare <file> <label>
    local file="$1" label="$2" out gui_fp cli_fp
    out="$(run_gui "PUTTYGEN_SELFTEST_LOAD=$file")"
    gui_fp="$(printf '%s\n' "$out" | field fingerprint)"
    cli_fp="$("$CLI" -l "$file")"
    check "$label: GUI fingerprint == CLI -l" "$cli_fp" "$gui_fp"
}

printf '' > "$WORK/empty.pass"           # an empty passphrase file

"$CLI" -q -t ed25519 -C "gui-test" --new-passphrase "$WORK/empty.pass" \
       -o "$WORK/cli.ppk" >/dev/null
load_and_compare "$WORK/cli.ppk" "load .ppk"
check "load .ppk: comment carried over" "comment=gui-test" \
      "$(run_gui "PUTTYGEN_SELFTEST_LOAD=$WORK/cli.ppk" | grep '^comment=')"

for fmt in private-openssh private-openssh-new private-sshcom; do
    f="$WORK/cli.$fmt"
    if "$CLI" -q "$WORK/cli.ppk" -O "$fmt" --new-passphrase "$WORK/empty.pass" \
             -o "$f" >/dev/null 2>&1; then
        load_and_compare "$f" "load $fmt"
    else
        log "  skip load $fmt (CLI cannot write this format for ed25519)"
    fi
done

# ---------------------------------------------------------------------------
log "3. save a public key the GUI loaded, and read it back"

out="$(run_gui "PUTTYGEN_SELFTEST_LOAD=$WORK/cli.ppk" \
               "PUTTYGEN_SELFTEST_OUT=$WORK/roundtrip.ppk")"
check "round trip: rewritten .ppk has the same fingerprint" \
      "$("$CLI" -l "$WORK/cli.ppk")" "$("$CLI" -l "$WORK/roundtrip.ppk")"

# ---------------------------------------------------------------------------
log "4. Cancel unwinds a key generation that is still running"

start_ms=$(date +%s)
cancel_out="$(env "PUTTYGEN_SELFTEST=rsa" "PUTTYGEN_SELFTEST_BITS=4096" \
                  "PUTTYGEN_SELFTEST_CANCEL=400" "$GUI" 2>&1)"
elapsed=$(( $(date +%s) - start_ms ))
check "cancel: reports that it was cancelled" "yes" \
      "$(printf '%s' "$cancel_out" | grep -q cancelled && echo yes || echo no)"
check "cancel: returned promptly (< 60s)" "yes" \
      "$([ "$elapsed" -lt 60 ] && echo yes || echo no)"

# ---------------------------------------------------------------------------
log "4b. generating a second key in the same process does not crash"

# The PRNG is created once for the process; creating it again per generation
# tripped an assert in sshrand.c and aborted on the second key.
set +e
repeat_out="$(env "PUTTYGEN_SELFTEST=ed25519" "PUTTYGEN_SELFTEST_REPEAT=3" \
                  "$GUI" 2>/dev/null)"
repeat_rc=$?
set -e
n_fp="$(printf '%s\n' "$repeat_out" | grep -c '^fingerprint=' || true)"
n_uniq="$(printf '%s\n' "$repeat_out" | grep '^fingerprint=' | sort -u | wc -l | tr -d ' ')"
check "repeat: three generations in one process" "3" "${n_fp:-0}"
check "repeat: each generation is a different key" "3" "${n_uniq:-0}"
check "repeat: exit status 0" "0" "$repeat_rc"

# ---------------------------------------------------------------------------
log "5. the file chooser is the application's own"

# It is the only dialog the GUI has with a "Path:" entry; check the source
# uses it for every file request rather than a bare GtkFileChooserDialog.
n_chooser="$(grep -c 'our_file_chooser_run' "$SRC_DIR/unix/puttygen.c" || true)"
n_bare="$(grep -c 'gtk_file_chooser_dialog_new' \
          "$SRC_DIR/unix/puttygen.c" || true)"
check "puttygen.c calls our_file_chooser_run" "yes" \
      "$([ "${n_chooser:-0}" -ge 3 ] && echo yes || echo no)"
check "puttygen.c builds no bare GtkFileChooserDialog" "0" "${n_bare:-0}"

# ---------------------------------------------------------------------------
log "results: $PASS/$CHECK checks passed"
if [ "${FAILED:-0}" = "1" ]; then
    log "SOME CHECKS FAILED"
    exit 1
fi
log "all checks passed"
