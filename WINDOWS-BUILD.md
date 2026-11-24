# Building and shipping on Windows

One command builds the x86_64 `putty.exe` and stages the folder that is handed to
the user — the exe **plus a ready-to-run MCP bridge beside it**.

```powershell
.\scripts\build-windows.ps1
```

Output:

```
C:\putty-beter\build-win\putty.exe            the build output (windows/CMakeLists.txt
                                              sets the output dir to the build root)

C:\putty-beter\putty-beter-win64\             the ship folder — this is what you zip
  putty.exe                                   x86_64, Windows 7 SP1 .. 11
  putty-mcp\                                  the MCP bridge, dependencies included
    bin\putty-mcp.mjs
    src\*.mjs
    node_modules\                             installed here, on Windows
    package.json  package-lock.json
    README.md  USER-MANUAL.md
  SETUP-WIN.txt                               generated: opencode.json snippet + help
```

The folder is self-contained: Node is the only system requirement, and
`SETUP-WIN.txt` is written with *this* machine's real paths, so the folder explains
itself on the machine it was built on.

---

## What changed on 2026-11-24

**No C source was touched.** The change is tooling and documentation only.

1. **New: `scripts/build-windows.ps1`.** Previously the Windows build was four
   hand-typed commands (PLAN.md §1) and the ship folder was assembled by hand:
   copy `putty.exe`, `robocopy` the bridge, run `npm install`, and remember that
   `SETUP-WIN.txt` did not exist yet. Now one command does all of it, and it is
   safe to re-run.
2. **`putty-mcp\` is installed, not just copied.** `mcp/bin` and `mcp/src` are
   mirrored (so a deleted module does not linger), and `npm ci --omit=dev` runs
   **in the payload**, which is what makes `node_modules` and `.bin` genuinely
   Windows-native. The checkout's `mcp/node_modules` is never copied unless npm
   cannot reach the network at all (then it is used as a fallback — the
   dependencies are pure JavaScript).
3. **`SETUP-WIN.txt` now exists.** `tech_detail.md` §7.5 and §19 items 11–12
   listed it as an open gap ("only the Mac's `SETUP.txt` exists"). It is generated
   from the real paths on every build, BOM-less, and sits at the top of the ship
   folder beside `putty.exe`.
4. **Docs updated in the same session:** `PLAN.md` §1 (the one-command line),
   `tech_detail.md` §7.5 / §19 items 11–12 / §20 ship layout, and
   `CHANGES-FROM-UPSTREAM.md` §7 + the *New files* table.

## What the script does

| # | Step | Notes |
|---|---|---|
| 1 | Toolchain check | Prepends `C:\msys64\ucrt64\bin` to `PATH` (MSYS2 is deliberately off the machine PATH — it shadows Windows binaries). Verifies gcc, windres, cmake, ninja, and Node ≥ 20 |
| 2 | Configure | `cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=<ucrt64 gcc> -DCMAKE_C_FLAGS="-D_WIN32_WINNT=0x0601"`. Skipped when the tree already exists (`-Reconfigure` / `-Clean` to force) |
| 3 | Build | `cmake --build build-win -j <cores>`; reports the elapsed time and the exe size |
| 4 | Stage the exe | Copies `build-win\putty.exe` → `putty-beter-win64\putty.exe`, and prints old → new size |
| 5 | Stage the bridge | Mirrors `mcp/bin`, `mcp/src`, `package.json`, `package-lock.json`, `README.md`, `USER-MANUAL.md`; runs `npm ci --omit=dev` when `node_modules` is missing or `-ReinstallDeps` |
| 6 | Generate `SETUP-WIN.txt` | Real node path + payload path, the opencode.json snippet (backslashes doubled for JSON), how to offer a session, CLI checks, and the debugging switches |
| 7 | Smoke-test | `node putty-mcp\bin\putty-mcp.mjs help` and `manual` must exit 0. No PuTTY needed |
| 8 | Summary | Tree with sizes, the exe path, and a ready-to-paste bridge command |

Timing on this machine (4 vCPU): **167 s** cold (474 targets), **~1 s** when
nothing changed, **~4 s** with `-ReinstallDeps`.

## Switches

| Switch | Effect |
|---|---|
| `-Clean` | Delete the build tree first (full recompile). Refuses any directory not named `build-*`, and refuses the checkout itself |
| `-Reconfigure` | Re-run the cmake configure step |
| `-ReinstallDeps` | Re-run `npm ci` in the staged payload |
| `-SkipBuild` | Stage only, reusing the existing `putty.exe` |
| `-SkipMcp` | Build and stage the exe alone |
| `-SkipSmokeTest` | Do not run the bridge |
| `-DebugOutput` | Trace every external command, every resolved path, every copy. Also `BUILD_WIN_DEBUG=1`, matching the other scripts' `*_DEBUG` variables |
| `-BuildDir`, `-ReleaseDir`, `-Msys2Root`, `-NodeExe`, `-Jobs` | Override the defaults (`build-win`, `putty-beter-win64`, `C:\msys64`, auto-detected node, cpu count) |

Exit code is **0** on success and **1** on any failure, with a single `ERROR:`
line (a top-level `trap` keeps PowerShell stack traces out of the output).

## Prerequisites

The toolchain is installed by `install-toolchain-win.ps1` (Git, CMake, Ninja,
MSYS2 x86_64, mingw-w64 **UCRT64**). Node 20+ must be on `PATH` for the bridge —
`node --version` should answer. Nothing else: the script checks all of it and says
what is missing instead of failing halfway.

Target: one binary for **Windows 7 SP1 → 11** (`-D_WIN32_WINNT=0x0601`, UCRT, so
Windows 7 needs KB2999226). This is a **cross build** on the ARM64 host: native
arm64 cmake/ninja driving the x86_64 UCRT64 gcc.

## Useful behaviours to know

* **Idempotent.** The second run skips configure, recompiles only what changed,
  and leaves the installed payload alone — so it is cheap to run after every edit.
* **Guarded deletes.** `-Clean` only ever removes `build-*` directories inside the
  checkout, and the payload mirror only removes `<ship>\putty-mcp\bin` and `src`,
  both checked to be inside the payload. Nothing else is deleted.
* **A foreign build tree is rejected** before anything is built: if
  `build-win\CMakeCache.txt` names a compiler that is not mingw, the script stops
  and suggests `-Clean` / `-Reconfigure` rather than mixing object files.
* **Windows PowerShell 5.1 and pwsh 7** are both supported (`#Requires -Version 5.1`).
  One 5.1 quirk is handled deliberately: with `2>&1`, a native command's stderr
  becomes an ErrorRecord, which `$ErrorActionPreference='Stop'` would turn into an
  abort on a *successful* run — the smoke test lowers it and judges by
  `$LASTEXITCODE` only.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `mingw-w64 UCRT64 toolchain not found` | MSYS2 not at `C:\msys64` — run `install-toolchain-win.ps1`, or pass `-Msys2Root` |
| `the build tree was configured with a different compiler` | An old tree from MSVC/another gcc: `-Clean` (full rebuild) or `-Reconfigure` |
| `node not found` | Install Node 20+, pass `-NodeExe`, or build the exe only with `-SkipMcp` |
| `npm ci` failed, then `npm install` failed | Offline: the script falls back to the checkout's `mcp/node_modules`; connect once and re-run for a clean install |
| `node ... help failed` | The payload's `node_modules` is broken — `-ReinstallDeps` |
| `ConPTY not available; cannot build Windows pterm` | Expected on this host; `pterm` is not part of the Windows ship |
| `Could NOT find Perl` at configure | Expected; the generated charset tables are in the tree |
| `-Clean refused: '…' is not a build-* directory` | Correct behaviour — `-Clean` only removes build trees |

## Verifying the result by hand

```powershell
.\putty-beter-win64\putty.exe                      # the GUI starts, fork icon, About box

cd .\putty-beter-win64
node putty-mcp\bin\putty-mcp.mjs help              # the bridge runs (no PuTTY needed)
node putty-mcp\bin\putty-mcp.mjs list              # sessions PuTTY is offering
```

`list` prints nothing (and exits 1 with a clear message) when no session is
offered — offer one in *Session → Automation (MCP)* and reopen the session.
Stale `%TEMP%\putty-mcp-<pid>\` endpoint records from killed processes are
skipped automatically; they do not need removing.

## Where the detail lives

| Topic | File |
|---|---|
| Why the Windows port works this way, toolchain traps, W0–W7 | `tech_detail.md` §1, §7.5, §8, §19, §20 |
| The ship layout (exe + bridge, G2) | `tech_detail.md` §20 |
| Plan, phases and results | `PLAN.md` §1, §2 |
| Every difference from upstream PuTTY 0.85 | `CHANGES-FROM-UPSTREAM.md` |
| The bridge itself (protocol, CLI, tools) | `mcp/README.md`, `mcp/USER-MANUAL.md` |
| Toolchain installation on a fresh host | `install-toolchain-win.ps1` |