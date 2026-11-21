# intel_arm_merge.md — handing the Intel slice to the arm Mac

**Purpose.** The x86_64 ("Intel") slices of `PuTTY.app` **and** `PuTTYGen.app` are
built and signed on the Intel Mac. This document is the handover to the arm Mac
side, whose job is to merge each with its arm64 slice into one universal `.app` —
Phase 1c of `PLAN.md`.

**Everything referenced here lives in this repo** (`build-arm64/…`,
`build-x86_64/…`, `scripts/…`). Nothing is on the Intel Mac's local SSD; that
scratch workspace was deleted. If you find a path outside the repo in this
document, it is a mistake — tell whoever wrote it.

Written 2026-11-20 on the Intel Mac (`MacMini2018`, macOS 15.8.2, x86_64).

**Committed state at handover** (on `main`):

| | |
|---|---|
| fix for `scripts/bundle-macos.sh` (§3) | **`fd49206`** — *"Sign the bundle's Contents/MacOS binaries on x86_64"* |
| this document | **`0ca60de`** — *"Hand over the Intel slice: intel_arm_merge.md"* |
| the GTK PuTTYgen design (§4m, M8) | **`75c87d3`** — *"PLAN.md: design the GTK PuTTYgen front end"* |
| pushed to the remote? | **no** — the Intel Mac has no git credentials; **push from the arm Mac** (see §9) |

Oldest-to-newest: `fd49206` → `0ca60de` → `75c87d3`. All three are local only.

**Nothing has been committed since.** As of **2026-11-21** the working tree also
holds uncommitted work from both sides: the arm side's GTK PuTTYgen port
(`unix/puttygen.c`, `unix/main-gtk-puttygen.c`, `unix/puttygen.bundle`,
`unix/puttygen.plist`, the `icons/` and `scripts/` changes, `PLAN.md`), and the
Intel side's `scripts/make-universal.sh`, the `bundle-macos.sh` fixes described in
§0, and this document. Check `git status` before committing so the two sets do not
end up in one unexplained commit.
Note the third: it is a *design*, not built code — §4m and M8 in `PLAN.md`
describe a `PuTTYGen.app` that does not exist yet, and when it does, the tables
in §1 and §7 of this document grow a second app.

---

## 0. Update — 2026-11-21, Intel Mac: two apps, and the merge script exists

**Everything below §0 was written for one app.** Since then the arm side built the
GTK PuTTYgen front end (`PLAN.md` §4m) and rewrote `scripts/bundle-macos.sh` to
bundle several apps in one run. Two things follow, and both are now handled:

* **There are four bundles, not two.** `PuTTY.app` and `PuTTYGen.app`, each with
  an arm64 slice (`build-arm64/bundle-stage/unix/`) and an x86_64 slice
  (`build-x86_64/bundle-stage/unix/`). The merge is per app: two `lipo` runs over
  two pairs, producing two universal apps. Wherever this document says
  "PuTTY.app", read "each app in the list"; §6 and §7 below have been superseded
  on that point by the script that now exists.

* **`scripts/make-universal.sh` now exists** — written on the Intel Mac
  2026-11-21, not on the arm side as §6 originally proposed. It merges **both
  apps** by default, and (added 2026-11-21 on the arm Mac) packages the result as
  a drag-install disk image:

  ```sh
  scripts/make-universal.sh                  # both apps, then the DMG
  APPS=PuTTYGen scripts/make-universal.sh    # one of them
  scripts/make-universal.sh <arm64.app> <x86_64.app> [<out.app>]   # explicit
  ```

  Defaults: `build-arm64/bundle-stage/unix` + `build-x86_64/bundle-stage/unix` →
  `build-universal/<name>.app`, then `build-universal/PuTTY-universal.dmg`. It
  walks the arm64 bundle as the structural authority, `lipo -create`s every Mach-O
  that has a counterpart, copies non-Mach-O through (noting the ones that differ —
  §4's six), **fails loudly** on a Mach-O that exists in only one slice, re-signs
  every Mach-O **including `Contents/MacOS`** (the §3 trap) with no `|| true`, then
  checks: every Mach-O carries both slices, `codesign --verify --deep --strict`
  passes, and no absolute Homebrew path leaked into the main binary. It refuses to
  start if either slice is already universal, is the same arch twice, or has a
  stray entry in its root.

* **The merge has now been run end to end on the arm Mac (2026-11-21)** — this was
  the one step §9 called out as never executed. Both apps came out **275 files, 75
  Mach-O, all `x86_64 arm64`**, seal valid, no Homebrew leak; `PuTTYGen` generates
  three keys. The disk image was then mounted and checked: `PuTTY.app`,
  `PuTTYGen.app` and an `Applications -> /Applications` symlink at the volume
  root, both apps sealing clean, and the universal `PuTTYGen` running from inside
  the mounted image. The image is a plain UDZO (no background or icon layout, which
  needs a mounted, Finder-arranged volume); it is built with `diskutil image
  create from`, because macOS 27 deprecates `hdiutil create` — the script prefers
  the new verb and falls back to `hdiutil` where it is absent. `MAKE_DMG=0` skips
  it; the one-app form writes `<app>-universal.dmg` so it cannot overwrite the
  suite image.

* **Four failure modes were found on the Intel side and fixed** in
  `scripts/bundle-macos.sh` (backups `-05.old`, `-06.old`):

  1. **A rebuild that did not replace the previous app nested it inside the new
     one** — `PuTTY.app/.PuTTY.app`. That entry sits in the bundle root, outside
     the signature seal, so `codesign --verify` said *"unsealed contents present
     in the bundle root"* and Gatekeeper refused the app while the build logged
     `done`. The x86_64 `PuTTY.app` was unopenable because of exactly this. Now
     `clear_app` insists the old app is gone before rebuilding (a `rm -rf` on SMB
     is where this starts), `app_complete` rejects a bundle with a stray root
     entry, and `verify` names the offender and fails the build.
  2. **A failed `codesign --verify --deep --strict` was only a `WARNING`** — which
     is why an unopenable bundle was reported as a successful build. Now fatal.
  3. **The copy-back path** — which only the Intel Mac takes, because there
     `/Volumes/Backup` is SMB and the script stages on local disk — had both
     hazards: an unguarded `rm -rf` before `cp -R` (which nests), and an "already
     up to date" test that could not see a stray root entry. Both fixed; the
     copy-back now also requires `app_complete`, so a bad copy fails the build.
  4. **`com.apple.quarantine` on the built apps** — several hundred files in this
     repository carry it (attribute agent `Brave`), including `unix/putty.plist`
     and `unix/puttygen.plist`, and `cp` propagates it into
     `Contents/Info.plist` of every bundle built from them. **Correction:** an
     earlier draft of this section claimed that alone makes Finder refuse the
     app. It does not — measured on the Intel Mac, a bundle with a valid seal and
     a quarantined `Info.plist` **launches normally**. The unopenable app was the
     nested entry in item 1, full stop. It is still a download marker with no
     place in a build product (it is what makes a *fetched* copy look "damaged"
     to Gatekeeper), so `bundle-macos.sh` and `make-universal.sh` now clear
     extended attributes on the bundle they produce, and `xattr -cr <app>` fixes
     one that appears anyway.

* **The x86_64 slices were rebuilt on the Intel Mac on 2026-11-21 and verified.**
  `PuTTY.app` and `PuTTYGen.app`: each **275 files, 75 Mach-O, all `x86_64`**,
  bundle root containing only `Contents`, `codesign --verify` seal good, and both
  **launch natively there**. §4's table below has been updated with these
  numbers. (The two apps' file sets differ in exactly six paths — each app's
  launcher, its `-bin` and its `.icns` — which is by design; the merge pairs
  *within* an app, never across apps.)

* **`build-arm64/` no longer exists** — it has been removed from the share, so
  there is **no arm64 slice to merge at the moment**. §2 already said to rebuild
  arm64 first because its provenance was uncertain; that is now *mandatory*
  rather than prudent, and it is the one reason `make-universal.sh` has not been
  run end to end. Its guard paths have been tested (missing bundle, same arch
  twice, stray root entry — all exit non-zero) and `bash -n` is clean, but the
  first real merge is the arm side's to do.

* **Per-app facts the merge depends on** (verified 2026-11-21, both arches):

  | App | `CFBundleExecutable` | `Contents/MacOS/<exe>-bin` | `CFBundleIdentifier` |
  |---|---|---|---|
  | `PuTTY.app` | `PuTTY` | the `puttyapp` binary | `org.tartarus.projects.putty.macputty` |
  | `PuTTYGen.app` | `PuTTYGen` | the `puttygenapp` binary | `org.tartarus.projects.putty.macputtygen` |

  The launcher is the same `osxlaunch` binary under each app's own name
  (`unix/osxlaunch.c` execs `<its own path>-bin`), so `Contents/MacOS/PuTTY` and
  `Contents/MacOS/PuTTYGen` are byte-identical launchers and merge cleanly.

---

## 1. What the Intel side produced

*(Superseded by §0 for the current state: there are two apps at 275 files each,
and `build-arm64/` is now gone. This table is the 2026-11-20 origin record.)*

| Artefact | Path (in this repo) | State |
|---|---|---|
| x86_64 build tree | `build-x86_64/` | 536/536 targets, all tests pass |
| **x86_64 app bundle** | `build-x86_64/bundle-stage/unix/PuTTY.app` | **275 files, 75 Mach-O, all `x86_64`, ad-hoc signed** |
| arm64 app bundle (pre-existing) | `build-arm64/bundle-stage/unix/PuTTY.app` | 273 files, 75 Mach-O, all `arm64`, ad-hoc signed — **and now deleted; §0**, rebuild it |

Verified on the share, on the x86_64 bundle:

```
$ codesign --verify --deep --strict --verbose=1 build-x86_64/bundle-stage/unix/PuTTY.app
… valid on disk
… satisfies its Designated Requirement
$ codesign -dv …/PuTTY.app | grep Signature
Signature=adhoc
```

The two bundles are now side by side in the **same relative layout**, which is
what makes `lipo` pairing possible:

```
build-arm64/bundle-stage/unix/PuTTY.app
build-x86_64/bundle-stage/unix/PuTTY.app
```

---

## 1a. How the pieces reach you — git carries the scripts, the share carries the bundles

Two different paths, and it matters which is which:

| What | In git? | How you get it |
|---|---|---|
| the `bundle-macos.sh` fix (§3) | **yes** — commit `fd49206` | `git pull` |
| this document | yes — the commit after `fd49206` | `git pull` |
| **the x86_64 app bundle** | **no** — `.gitignore:152`, `/build-*/` | the shared working tree only |
| the arm64 app bundle | no — same rule | already yours (you built it) |
| `tools/gtk-mac-bundler/` | **no** — `.gitignore:164`, `/tools/gtk-mac-bundler/` | already in the working tree; re-clone if absent |

Everything in the lower half of that table is **physically present** in this
working tree, which *is* your disk — `//hans@MacMiniM5p.local/Backup` is the arm
Mac serving its own volume — so it is already on your side. It simply will not
appear from a fresh `git clone`, and it is not in the commit.

**So: work in this tree, not in a clone.** If you ever do need a clone, bring the
ignored pieces across by hand:

```sh
git clone <remote> putty-beter && cd putty-beter
git clone https://gitlab.gnome.org/GNOME/gtk-mac-bundler.git tools/gtk-mac-bundler
# then copy build-arm64/ and build-x86_64/ from the shared tree
```

**Sanity check, run this first on the arm Mac** — it confirms you have the fixes
and both Intel slices, before any merging:

```sh
git log -1 --format='%h %s'                 # expect a 2026-11-21 commit or later
grep -n 'stray_root_entries\|clear_app' scripts/bundle-macos.sh   # the §0 fixes
for n in PuTTY PuTTYGen; do
    a="build-x86_64/bundle-stage/unix/$n.app"
    test -d "$a" && echo "$n slice: present"
done
lipo -archs build-x86_64/bundle-stage/unix/PuTTY.app/Contents/MacOS/PuTTY-bin      # x86_64
lipo -archs build-x86_64/bundle-stage/unix/PuTTYGen.app/Contents/MacOS/PuTTYGen-bin # x86_64
```

---

## 2. **Read this before merging: rebuild arm64 first**

**The arm64 build directory no longer exists** (§0) — so this is no longer a
question of provenance, there is simply nothing to merge until you rebuild. Two
occurrences, and either order works:

```sh
./scripts/build-macos.sh arm64
./scripts/bundle-macos.sh arm64
```

On the arm Mac that is a couple of minutes, because the repository is on local
disk there (see §8 for why that matters on the Intel side). This also picks up
the §3 fix and everything in §0.

The rest of this section is the original 2026-11-20 note, kept for the record —
the arm64 bundle's provenance *was* uncertain before it was deleted:

* `build-arm64/bundle-stage/unix/PuTTY.app` — mtime `2026-11-19 13:36:27`
* commit `e1b0388` ("Open a new session window on the monitor the configuration
  box is on", touching `unix/window.c`, `unix/dialog.c`, `unix/platform.h`) —
  `2026-11-19 13:38:06`, i.e. **90 seconds later**
* `build-arm64/puttyapp` — mtime `2026-11-19 13:41:21` (rebuilt after the commit)

The bundled `PuTTY-bin` and the `build-arm64/puttyapp` of the day were the **same
size** (1,549,800 bytes) and differed only in the load-command region where
`install_name_tool` writes, so it was *probably* current — but it could not be
proven from metadata, and a stale slice fails silently. That is why rebuilding
was the advice then; now there is nothing there to rebuild from.

---

## 3. Required repo change: the x86_64-only signing fix

`scripts/bundle-macos.sh` **must contain** this — it is committed as **`fd49206`**,
so a `git pull` on your side brings it in:

```diff
 log "normalising Cellar -> opt references"
 cd "$APP"
-find "$RES" -type f > "$STAGE/macho-list.txt"
+# The list is every Mach-O in the bundle, not only those under Resources: the
+# two executables in Contents/MacOS must be re-signed as well. On arm64 the
+# linker ad-hoc signs them by itself, so leaving them out went unnoticed; the
+# x86_64 linker does not, and an unsigned nested binary makes the bundle-level
+# codesign below fail - which the "|| true" there hides, leaving the .app
+# unsigned on Intel.
+find Contents -type f > "$STAGE/macho-list.txt"
```

**Why it matters, because it affects the merge too.** The arm64 linker ad-hoc
signs by default (`flags=0x20002(adhoc, linker-signed)`); the x86_64 linker does
not (`build-x86_64/puttyapp` → *"code object is not signed at all"*). With the
old one-liner, `Contents/MacOS/PuTTY-bin` was never signed, the bundle-level
`codesign` failed, and `2>&1 || true` hid it — the Intel `.app` came out
**unsigned**. The same trap will bite `make-universal.sh` if it re-signs only
`Contents/Resources`: **sign `Contents/MacOS/*` too, and do not hide `codesign`
failures.**

Backup of the pre-fix script: `scripts/bundle-macos.sh-03.old`.

---

## 4. Measured facts about the two bundles

Both were counted on the share, file by file.

*(Originally measured 2026-11-20 on `PuTTY.app` alone. The x86_64 column was
**re-measured 2026-11-21** on the rebuilt pair — `PuTTY.app` and `PuTTYGen.app`
are both 275 files / 75 Mach-O — and is unchanged. The arm64 column is the
2026-11-20 figure for `PuTTY.app`; the arm64 bundle is gone, so re-measure it
after the rebuild in §2. The per-app file sets differ only in the three
app-specific paths — launcher, `-bin`, `.icns` — so these totals apply to either
app.)*

| | arm64 | x86_64 |
|---|---|---|
| files (total) | 273 | 275 |
| Mach-O | **75** | **75** |
| `.dylib` | 47 | 47 |
| `.so` (loaders + immodules + printbackends) | 26 | 26 |
| `.svg` / `.png` | 102 / 48 | 102 / 50 |

**Files present in only one bundle:**

```
only in arm64 : (none)
only in x86_64 : Contents/Resources/share/icons/hicolor/256x256/apps/disconnected.png
                 Contents/Resources/share/icons/hicolor/256x256/apps/scrcpy.png
```

Those two PNGs are other Homebrew packages' icons that happen to exist on the
Intel box; they are **not** Mach-O and are harmless. The merge must copy
non-Mach-O extras through and only **fail loudly on a Mach-O** that exists in
one bundle but not the other.

**Files that are common but differ in content** — exactly six, all non-Mach-O,
all expected:

| File | Why it differs | What to do |
|---|---|---|
| `Contents/_CodeSignature/CodeResources` | signature manifest; hashes the bundle | ignore — regenerated by the final `codesign` |
| `Contents/Resources/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache` | generated by `bundle-macos.sh`; ordering/whitespace | pick either — it is `@executable_path`-relative, so both are valid |
| `Contents/Resources/lib/gtk-3.0/3.0.0/immodules.cache` | generated by gtk-mac-bundler | pick either, same reason |
| `Contents/Resources/share/glib-2.0/schemas/org.gnome.desktop.background.gschema.xml` | `gsettings-desktop-schemas` differs between the machines | pick either — cosmetic; the runtime file is `gschemas.compiled`, which did **not** differ |
| `Contents/Resources/share/glib-2.0/schemas/org.gnome.desktop.screensaver.gschema.xml` | same as above | same |
| `Contents/Resources/share/icons/hicolor/icon-theme.cache` | GTK icon-theme cache, built from each machine's icon set | pick either; GTK rebuilds it if it is stale |

**Everything else is byte-identical.** So the merge script may treat "common
non-Mach-O file differs" as *report-and-continue*, not as an error — but it
should still report it, so a genuine surprise is visible.

---

## 5. The merge contract

1. **Same source revision.** Both slices must come from the same git commit.
   Intel: built 2026-11-20 from `7d1d5b7` (working tree clean apart from the
   §3 script fix). Rebuild arm64 per §2 and record both revisions.
2. **Identical relative layout.** Confirmed above: 75 Mach-O each, and the
   Mach-O paths match one-to-one.
3. **Bundle-relative install names.** `unix/putty.bundle` runs with
   `<run-install-name-tool/>`, so all dylib references are
   `@executable_path/../Resources/…`. Nothing embeds an absolute Homebrew path,
   which is why the two trees pair up without path surgery. (This is the
   assumption `PLAN.md` §4 "Why B is workable here" rests on — re-verify it if
   the merge ever produces a bundle that will not launch.)

---

## 6. Spec for `scripts/make-universal.sh`

**Superseded by §0: the script now exists.** Written on the Intel Mac 2026-11-21,
it merges both apps by default and implements the algorithm below, with the
loop-in-the-current-shell shape this section recommends (here-strings, so an
`exit 1` really exits). It was run end to end on the arm Mac 2026-11-21. One
addition beyond this spec: after the merge it packages the result as a
drag-install disk image (§0). What follows is kept as the design rationale.

Original spec (for one app, arm side to write):

### Interface

```sh
scripts/make-universal.sh [<arm64.app> <x86_64.app> [<out.app>]]
```

Defaults, so a bare run does the right thing after §2:

```
<arm64.app>  build-arm64/bundle-stage/unix/PuTTY.app
<x86_64.app> build-x86_64/bundle-stage/unix/PuTTY.app
<out.app>    build-universal/PuTTY.app
```

House style, matching the other scripts: `set -euo pipefail`, a `DEBUG=1`
switch, `log()` to stderr, an `-h/--help` that prints the header comment.

### Algorithm

1. **Preflight.** Both inputs exist and are directories. `lipo -archs` on each
   `Contents/MacOS/PuTTY-bin` must report exactly one arch, and they must
   differ — otherwise you are merging something with itself, or a bundle that
   is already universal.
2. **Fresh output.** `rm -rf` the output bundle, then `mkdir -p`.
3. **Walk the arm64 bundle as the structural authority.** For every entry
   (files *and* symlinks), compute the relative path `rel`:
   * present in both, and `file` says Mach-O → `lipo -create arm/x86 -output out`
   * present in both, non-Mach-O → copy the arm64 copy; if the bytes differ,
     print a `note:` (see §4 — six files are expected to differ)
   * present in arm64 only → copy it through
   * symlink → recreate the link, do not dereference
4. **Overlay the x86_64-only entries.** For every path in the x86_64 bundle
   that has no arm64 counterpart:
   * Mach-O → **`ERROR` and exit non-zero** (a Mach-O with no pair cannot be
     made universal; this is the "fail loudly" case `PLAN.md` §4 asks for)
   * otherwise → copy through (this is where the two stray PNGs land)
5. **Re-sign inside-out.** `lipo` invalidates every signature it touches:
   * every Mach-O under `Contents/Resources` **and both executables in
     `Contents/MacOS`** — see §3, this is the trap
   * then the bundle itself: `codesign --force --sign - "$OUT"`
   * do **not** use `|| true` here; a silent signing failure is exactly the bug
     §3 documents
6. **Verify** (§7) and print a summary; exit non-zero on any failure.

### Reference implementation

Adapt as you like — this is the shape, not gospel. It is deliberately explicit
about the two things that silently go wrong (unsigned `Contents/MacOS`, and
Mach-O files with no pair).

```bash
#!/usr/bin/env bash
# make-universal.sh — merge per-arch PuTTY.app bundles into one universal .app.
set -euo pipefail

DEBUG="${DEBUG:-0}"
log() { printf '[make-universal] %s\n' "$*" >&2; }

ARM="${1:-build-arm64/bundle-stage/unix/PuTTY.app}"
X86="${2:-build-x86_64/bundle-stage/unix/PuTTY.app}"
OUT="${3:-build-universal/PuTTY.app}"

for a in "$ARM" "$X86"; do
    [ -d "$a" ] || { log "ERROR: not a bundle: $a"; exit 1; }
done
log "arm64 : $ARM"
log "x86_64: $X86"
log "out   : $OUT"

is_macho() { file -b "$1" 2>/dev/null | grep -q 'Mach-O'; }

rm -rf "$OUT"
mkdir -p "$OUT"

# --- 1. arm64 is the structural authority -------------------------------
( cd "$ARM" && find . \( -type f -o -type l \) -print0 ) |
while IFS= read -r -d '' rel; do
    src_a="$ARM/${rel#./}"
    src_x="$X86/${rel#./}"
    dst="$OUT/${rel#./}"
    mkdir -p "$(dirname "$dst")"

    if [ -L "$src_a" ]; then                      # symlink: recreate as-is
        cp -P "$src_a" "$dst"
    elif [ ! -e "$src_x" ]; then                  # arm64-only
        cp "$src_a" "$dst"
    elif is_macho "$src_a"; then                  # the point of the exercise
        if ! is_macho "$src_x"; then
            log "ERROR: $rel is Mach-O in arm64 but not in x86_64"; exit 1
        fi
        lipo -create "$src_a" "$src_x" -output "$dst"
    else                                          # common, not Mach-O
        cp "$src_a" "$dst"
        if ! cmp -s "$src_a" "$src_x"; then
            log "note: $rel differs between the two bundles; kept the arm64 copy"
        fi
    fi
done

# --- 2. x86_64-only entries --------------------------------------------
( cd "$X86" && find . \( -type f -o -type l \) -print0 ) |
while IFS= read -r -d '' rel; do
    [ -e "$ARM/${rel#./}" ] && continue
    src_x="$X86/${rel#./}"
    dst="$OUT/${rel#./}"
    if is_macho "$src_x"; then
        log "ERROR: $rel is Mach-O but exists only in x86_64 - cannot merge"
        exit 1
    fi
    log "note: $rel exists only in x86_64; copied through"
    mkdir -p "$(dirname "$dst")"
    cp -P "$src_x" "$dst"
done

# --- 3. re-sign inside-out (lipo invalidated everything) ----------------
log "re-signing Mach-O files (deepest first), then the bundle"
( cd "$OUT" && find Contents/Resources -type f -print0 ) |
while IFS= read -r -d '' f; do
    is_macho "$f" && codesign --force --sign - "$f" >/dev/null 2>&1 || true
done
# Contents/MacOS is NOT under Resources - this is the x86_64 trap from §3.
for f in "$OUT/Contents/MacOS/"*; do
    [ -e "$f" ] || continue
    if is_macho "$f"; then
        codesign --force --sign - "$f" || { log "ERROR: cannot sign $f"; exit 1; }
    fi
done
codesign --force --sign - "$OUT" || { log "ERROR: cannot sign the bundle"; exit 1; }

log "done: $OUT"
```

Two deliberate details, do not "simplify" them away:

* the `Contents/MacOS` loop is **separate** from the `Resources` walk — that is
  precisely the omissible step that produced an unsigned Intel bundle (§3);
* the bundle-level `codesign` is **not** wrapped in `|| true`.

The two `while` loops run in subshells, so a `exit 1` inside them only kills the
subshell — if that matters, restructure with a `for` loop over a here-string, or
use `set -o pipefail` plus a flag file. Worth getting right, since "fail loudly"
is half the point of the script.

Actually, prefer this shape for the error paths:

```bash
list=$( cd "$ARM" && find . \( -type f -o -type l \) -print )
while IFS= read -r rel; do
    ...
done <<<"$list"
```

That keeps the loop in the current shell, so `exit 1` really exits.

---

## 7. Verification — do all of these

(Once per app: `make-universal.sh` merges `PuTTY.app` and `PuTTYGen.app`, so run
this twice — `APP=build-universal/PuTTY.app`, then `APP=build-universal/PuTTYGen.app`.
`make-universal.sh` already performs all four checks itself and exits non-zero if
any fails; this section is what you run by hand to see the evidence.)

For the disk image, the by-hand check is to mount it and look:

```sh
hdiutil verify build-universal/PuTTY-universal.dmg
MP=$(hdiutil attach build-universal/PuTTY-universal.dmg -nobrowse -readonly \
        | grep -o '/Volumes/.*' | head -1)
ls -la "$MP"                 # PuTTY.app  PuTTYGen.app  Applications -> /Applications
codesign --verify --deep --strict "$MP/PuTTYGen.app"
lipo -archs "$MP/PuTTYGen.app/Contents/MacOS/PuTTYGen-bin"      # x86_64 arm64
hdiutil detach "$MP"
```

```sh
APP=build-universal/PuTTY.app

# 1. every Mach-O carries both slices; expect only "x86_64 arm64" lines
while IFS= read -r f; do
    file -b "$f" | grep -q Mach-O && printf '%s\t%s\n' "$(lipo -archs "$f")" "$f"
done < <(find "$APP" -type f) | awk '{print $1}' | sort -u
# expect exactly: x86_64 arm64

# 2. no Mach-O left behind with a single slice
find "$APP" -type f -exec sh -c 'file -b "$1" | grep -q Mach-O || exit 0
    [ "$(lipo -archs "$1")" = "x86_64 arm64" ] || echo "NOT UNIVERSAL: $1"' _ {} \;

# 3. signature
codesign --verify --deep --strict --verbose=2 "$APP"
codesign -dv "$APP" | grep -E 'Signature|Sealed'

# 4. no absolute Homebrew path leaked into the bundle
otool -L "$APP/Contents/MacOS/PuTTY-bin" | grep -E '/usr/local|/opt/homebrew' && echo "LEAK"
```

Then **launch it on real hardware**: natively on the arm Mac, and on an Intel
Mac (this one) — **not** through Rosetta, and not with `arch -x86_64` forcing
the arm binary. On this Intel box the universal build simply runs the x86_64
slice, which is the end-to-end check that the Intel side is real.

`PLAN.md` §4 lists the same steps (its items 5 and 9).

---

## 8. Traps worth carrying over

* **Do not build or bundle over SMB from the Intel Mac.** `/Volumes/Backup` is
  the arm Mac's disk served over SMB (`//hans@MacMiniM5p.local/Backup`).
  Bundling there took **> 77 minutes** for this same input; the same bundling on
  the Intel box's local SSD took **2 min 17 s** — a ~34× difference, all of it
  filesystem latency (a single small `.c` compiled in 17 s). On the arm Mac the
  repo is local, so this does not apply to you; it is the reason the Intel side
  used a local stage. A `STAGE_DIR` override for `bundle-macos.sh` would make
  that painless instead of ad-hoc (not written yet).
* **Homebrew has no x86_64 macOS bottles any more** (Tier 3 since Sept 2026:
  `gtk+3`'s bottle stanza lists only `arm64_*` and `x86_64_linux`). So
  `gtk+3` is a **source build on Intel, on any host** — including under Rosetta
  on an arm Mac. `gtk+3 3.24.52` is what the Intel slice was built against;
  keep the arm slice on the same version or the bundles will diverge more than
  §4 describes.
* **The GTK source build needs a DocBook XSL catalog**, or it dies near the end
  at the man-page step with `xsltproc --nonet … cannot parse`. Homebrew's
  `post_install` scripts that create `/usr/local/etc/xml/catalog` do not
  complete on this macOS (`xmlcatalog --del` returns non-zero for an absent
  entry and the scripts use `set -e`), leaving a 0-byte catalog. Workaround used
  on the Intel side: populate `/usr/local/etc/xml/catalog` by hand, then
  `XML_CATALOG_FILES=/usr/local/etc/xml/catalog brew install gtk+3`. Homebrew's
  `superenv` does not strip that variable, so it reaches the build.
* **Rosetta is being retired** (macOS 27 is the last release with it; macOS 28
  drops it for general applications) — which is exactly why the universal
  `.app` still matters for Intel Macs on macOS ≤ 27, and why the arm64 slice
  must stay native.
* **The version string is `0.85-beter.1` (set 2026-11-21).** `version.h`,
  `LATEST.VER` and both `Info.plist`s carry it, and every About box links to
  <https://github.com/beterhans/putty-beter> (`PLAN.md` §4 item 6,
  `CHANGES-FROM-UPSTREAM.md` §18). **This makes the two slices below stale**:
  they were built before it, so the x86_64 slice still announces `Unidentified
  build` and has no About link — rebuild *both* slices from one revision before
  merging, or the universal app will carry a different version per architecture.
  (`SSHVER` also changes: the banner is now
  `SSH-2.0-PuTTY_Release_0.85_beter.1`.)
* **Not notarized.** Both bundles are ad-hoc signed only, so Gatekeeper on a
  *different* Mac needs right-click → Open; the local build launches normally
  here. Notarization (`notarytool`, hardened runtime, a Developer account) is
  still open — `PLAN.md` §4 items 8–9. Note that quarantine is **not** the
  problem it looks like: see §0 item 4 — several hundred files in this repository
  carry `com.apple.quarantine` (a browser download) and it propagates into every
  bundle's `Info.plist`, but a valid seal plus a quarantined `Info.plist` still
  launches. Both scripts now clear xattrs on what they produce anyway.

---

## 9. Checklist for the arm Mac side

- [ ] Confirm you have the fixes and both Intel slices — run the check in §1a.
      **See §0 first**: the x86_64 slices were rebuilt 2026-11-21 and
      `bundle-macos.sh` changed again.
- [ ] `./scripts/build-macos.sh arm64` — record the revision and toolchain.
- [ ] `./scripts/bundle-macos.sh arm64` — **there is no arm64 bundle at all right
      now** (`build-arm64/` was deleted, §2); on the arm Mac the repo is on local
      disk, so this takes minutes, not hours.
- [ ] Record `lipo -archs` of each slice's `Contents/MacOS/<exe>-bin`:
      `build-arm64/…` → `arm64`, `build-x86_64/…` → `x86_64`, for **both** apps.
- [x] `scripts/make-universal.sh` is **written** (Intel side, 2026-11-21) — §6 —
      and **run end to end** (arm Mac, 2026-11-21, §0): two universal apps plus
      `build-universal/PuTTY-universal.dmg`. Nothing left to write or run here
      unless a slice is rebuilt.
- [x] Work through §7 until clean — done for the merged apps, and the disk image
      additionally mounted and inspected (both apps present, seal valid, universal
      `PuTTYGen` runs from the mounted volume).
- [ ] Launch both results on the arm Mac **and** hand them back to the Intel Mac to
      launch — the universal `PuTTYGen` was launched on the arm Mac (generates
      three keys); the Intel-side launch is still to do.
- [ ] **Push.** The Intel Mac has no credentials, so its commits are local only until
      you run `git push origin main`.
- [ ] Only then: signing identity, hardened runtime, notarization. (The version
      strings are **done** — `0.85-beter.1` in `version.h`, `LATEST.VER` and both
      plists, with an About link home; see §8. Rebuild both slices so the merged
      app is one version, not two.)
- [ ] Decide whether `PLAN.md` §4's phase status lines and `README.md`'s "This
      path is not yet verified" need further updating — the phase 1b/1c lines were
      refreshed 2026-11-21 but `README.md` was not.
