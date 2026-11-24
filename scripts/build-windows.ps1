#Requires -Version 5.1
<#
.SYNOPSIS
    One-command Windows build: putty.exe + a usable putty-mcp/ bridge beside it.

.DESCRIPTION
    Builds the x86_64 Windows binary (cross build: x86_64 mingw-w64 UCRT64 gcc
    driven by the native cmake/ninja) and stages the ship folder described in
    tech_detail.md section 9:

        <ReleaseDir>/
          putty.exe                     the build output, copied
          putty-mcp/                    the MCP bridge, with node_modules
            bin/putty-mcp.mjs
            src/*.mjs
            node_modules/               installed here, ON WINDOWS
            README.md  USER-MANUAL.md
          SETUP-WIN.txt                 generated: opencode.json snippet + help

    Everything is idempotent: a second run reuses the build tree and the staged
    bridge, so only changed C files are recompiled and npm is not re-run.

.PARAMETER Clean
    Delete the build tree first (full recompile). Refuses to touch anything that
    is not a build-* directory inside this checkout.

.PARAMETER Reconfigure
    Re-run the cmake configure step even if the build tree already exists.

.PARAMETER ReinstallDeps
    Re-run npm in the staged putty-mcp/ (else it is only done when node_modules
    is missing).

.PARAMETER SkipBuild
    Stage only: reuse the putty.exe already in the build tree. Useful to redo
    the ship folder alone.

.PARAMETER SkipMcp
    Build and stage putty.exe only, no bridge.

.PARAMETER SkipSmokeTest
    Do not run `node putty-mcp.mjs help/manual` after staging.

.PARAMETER DebugOutput
    Verbose tracing (every external command, every resolved path). Also enabled
    by setting BUILD_WIN_DEBUG=1, which matches the other scripts' *_DEBUG vars.

.EXAMPLE
    .\scripts\build-windows.ps1

.EXAMPLE
    .\scripts\build-windows.ps1 -Clean -DebugOutput

.NOTES
    Target: one binary for Windows 7 SP1 .. 11 (-D_WIN32_WINNT=0x0601, UCRT).
    Toolchain: C:\msys64\ucrt64 (override with -Msys2Root) + cmake + ninja.
    Debug tracing of the running PuTTY/bridge is separate: PUTTY_MCP_DEBUG=1.
#>
[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$Reconfigure,
    [switch]$ReinstallDeps,
    [switch]$SkipBuild,
    [switch]$SkipMcp,
    [switch]$SkipSmokeTest,
    [switch]$DebugOutput,
    [string]$BuildDir   = 'build-win',
    [string]$ReleaseDir = 'putty-beter-win64',
    [string]$Msys2Root  = 'C:\msys64',
    [string]$NodeExe    = '',
    [int]$Jobs          = 0
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Any uncaught terminating error: one clean line, no PowerShell stack trace.
trap {
    Write-Err2 $_.Exception.Message
    exit 1
}

# ---------------------------------------------------------------- logging ---

$script:DebugOn = $DebugOutput.IsPresent -or ($env:BUILD_WIN_DEBUG -eq '1')
$script:StepNo  = 0
$script:Started = Get-Date

function Write-Step {
    param([string]$Text)
    $script:StepNo++
    Write-Host ''
    Write-Host ("[{0}] {1}" -f $script:StepNo, $Text) -ForegroundColor Cyan
}

function Write-Info {
    param([string]$Text)
    Write-Host "    $Text"
}

function Write-Debug2 {
    param([string]$Text)
    if ($script:DebugOn) { Write-Host "    [dbg] $Text" -ForegroundColor DarkGray }
}

function Write-Warn2 {
    param([string]$Text)
    Write-Host "    WARNING: $Text" -ForegroundColor Yellow
}

function Write-Err2 {
    param([string]$Text)
    Write-Host "    ERROR: $Text" -ForegroundColor Red
}

function Write-Ok {
    param([string]$Text)
    Write-Host "    OK: $Text" -ForegroundColor Green
}

# Run an external program, trace it, and fail loudly on a non-zero exit code.
function Invoke-Tool {
    param(
        [Parameter(Mandatory = $true)][string]$Exe,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory = $true)][string]$What
    )
    Write-Debug2 ("{0}: {1} {2}" -f $What, $Exe, ($Arguments -join ' '))
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw ("{0} failed (exit code {1}): {2} {3}" -f `
            $What, $LASTEXITCODE, $Exe, ($Arguments -join ' '))
    }
}

# Refuse to delete a path that is not inside $Parent (defence for Remove-Item).
function Assert-Inside {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Parent
    )
    $full   = [System.IO.Path]::GetFullPath($Path).TrimEnd('\')
    $parent = [System.IO.Path]::GetFullPath($Parent).TrimEnd('\')
    if (-not $full.StartsWith($parent + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
        throw ("refusing to delete '{0}': not inside '{1}'" -f $full, $parent)
    }
    if ($full -eq $parent) {
        throw ("refusing to delete '{0}': that is the parent itself" -f $full)
    }
    Write-Debug2 "deleting (in scope): $full"
}

# ------------------------------------------------------------------- setup ---

$root = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path (Join-Path $root 'CMakeLists.txt'))) {
    throw ("cannot find CMakeLists.txt above '{0}' - run this script from the putty-beter checkout" -f $PSScriptRoot)
}

$buildPath = Join-Path $root $BuildDir
$relPath   = Join-Path $root $ReleaseDir

Write-Host ''
Write-Host 'putty-beter - Windows build' -ForegroundColor White
Write-Info "checkout : $root"
Write-Info "build dir: $buildPath"
Write-Info "ship dir : $relPath"
Write-Info ("debug    : {0}" -f $(if ($script:DebugOn) { 'on' } else { 'off (use -DebugOutput or BUILD_WIN_DEBUG=1)' }))

# --------------------------------------------------------------- clean up ---

if ($Clean) {
    Write-Step 'Clean (removing the build tree)'
    if (Test-Path $buildPath) {
        # Guard: only a build-* directory inside this checkout may go.
        $leaf = Split-Path $buildPath -Leaf
        if ($leaf -notlike 'build-*') {
            throw ("-Clean refused: '{0}' is not a build-* directory" -f $leaf)
        }
        if ([System.IO.Path]::GetFullPath($buildPath).TrimEnd('\') -eq [System.IO.Path]::GetFullPath($root).TrimEnd('\')) {
            throw '-Clean refused: the build directory is the checkout itself'
        }
        Write-Info "deleting $buildPath"
        Remove-Item -Recurse -Force $buildPath
        Write-Ok 'build tree removed'
    } else {
        Write-Info 'nothing to delete (no build tree yet)'
    }
}

# ------------------------------------------------------------- toolchain ----

Write-Step 'Toolchain'

$ucrtBin = Join-Path $Msys2Root 'ucrt64\bin'
if (-not (Test-Path $ucrtBin)) {
    Write-Err2 "mingw-w64 UCRT64 toolchain not found: $ucrtBin"
    Write-Info 'Install it with .\install-toolchain-win.ps1, or pass -Msys2Root <root>.'
    exit 1
}
# MSYS2 is deliberately NOT on the machine PATH (it shadows Windows binaries),
# so put it in front for this process only. cc1.exe/collect2.exe need these DLLs.
$env:PATH = $ucrtBin + ';' + $env:PATH
Write-Debug2 "PATH[0] = $ucrtBin"

$gcc     = Join-Path $ucrtBin 'x86_64-w64-mingw32-gcc.exe'
$windres = Join-Path $ucrtBin 'windres.exe'
foreach ($tool in @($gcc, $windres)) {
    if (-not (Test-Path $tool)) {
        Write-Err2 "missing tool: $tool"
        Write-Info 'Re-run the MSYS2 pacman step from install-toolchain-win.ps1.'
        exit 1
    }
}

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue)
$ninja = (Get-Command ninja -ErrorAction SilentlyContinue)
if (-not $cmake) { Write-Err2 'cmake not found on PATH (expected C:\Program Files\CMake\bin)'; exit 1 }
if (-not $ninja) { Write-Err2 'ninja not found on PATH (expected C:\Program Files\Ninja)'; exit 1 }

$gccVer = (& $gcc --version | Select-Object -First 1)
Write-Info "gcc   : $gccVer"
Write-Debug2 "gcc path: $gcc"
Write-Info ("cmake : {0} ({1})" -f $cmake.Source, (& $cmake.Source --version | Select-Object -First 1))
Write-Info ("ninja : {0} ({1})" -f $ninja.Source, (& $ninja.Source --version | Select-Object -First 1))

if ($Jobs -le 0) { $Jobs = [Environment]::ProcessorCount }
Write-Info "jobs  : $Jobs"

# Node is needed for the bridge: to install its deps and to smoke-test it.
$node = $null
if ($NodeExe) {
    if (-not (Test-Path $NodeExe)) { Write-Err2 "-NodeExe not found: $NodeExe"; exit 1 }
    $node = $NodeExe
} else {
    $nodeCmd = (Get-Command node -ErrorAction SilentlyContinue)
    if ($nodeCmd) { $node = $nodeCmd.Source }
}
if (-not $node) {
    Write-Warn2 'node not found - the bridge cannot be staged with its dependencies'
    Write-Info 'Install Node 20+ (https://nodejs.org) or pass -NodeExe <path>, or use -SkipMcp.'
} else {
    $nodeFull = [System.IO.Path]::GetFullPath($node)
    $nodeVer  = (& $nodeFull --version).Trim()
    $nodeMajor = 0
    if ($nodeVer -match '^v(\d+)') { $nodeMajor = [int]$Matches[1] }
    Write-Info "node  : $nodeFull ($nodeVer)"
    if ($nodeMajor -lt 20) {
        Write-Err2 "Node 20 or newer is required by the bridge (found $nodeVer)"
        exit 1
    }
}

# --------------------------------------------------------------- configure --

if (-not $SkipBuild) {

    $cacheFile = Join-Path $buildPath 'CMakeCache.txt'
    $needConfigure = $Reconfigure.IsPresent -or (-not (Test-Path (Join-Path $buildPath 'build.ninja')))

    if (-not $needConfigure -and (Test-Path $cacheFile)) {
        # An existing tree configured for a different compiler cannot be reused.
        # (The cache spells the type STRING or FILEPATH depending on cmake, so
        # match any type.)
        $cachedCompiler = Select-String -Path $cacheFile -Pattern '^CMAKE_C_COMPILER:[A-Z]+=(.+)$' -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($cachedCompiler -and ($cachedCompiler.Matches[0].Groups[1].Value -notlike '*mingw32-gcc*')) {
            Write-Err2 "the build tree was configured with a different compiler:"
            Write-Info $cachedCompiler.Matches[0].Groups[1].Value
            Write-Info 'Re-run with -Clean (full rebuild) or -Reconfigure.'
            exit 1
        }
    }

    if ($needConfigure) {
        Write-Step 'Configure (cmake -G Ninja, Release, Win7 SP1 target)'
        $cfgArgs = @(
            '-S', $root,
            '-B', $buildPath,
            '-G', 'Ninja',
            '-DCMAKE_BUILD_TYPE=Release',
            "-DCMAKE_C_COMPILER=$gcc",
            '-DCMAKE_C_FLAGS=-D_WIN32_WINNT=0x0601'
        )
        Write-Info ("cmake {0}" -f ($cfgArgs -join ' '))
        Invoke-Tool -Exe $cmake.Source -Arguments $cfgArgs -What 'cmake configure'
        Write-Ok 'configured'
    } else {
        Write-Step 'Configure (skipped - reusing the existing build tree)'
        Write-Info 'use -Reconfigure to regenerate it, -Clean to start over'
    }

    Write-Step 'Build'
    $t0 = Get-Date
    Invoke-Tool -Exe $cmake.Source -Arguments @('--build', $buildPath, '-j', "$Jobs") -What 'cmake build'
    $secs = [int]((Get-Date) - $t0).TotalSeconds
    Write-Ok ("built in {0}s" -f $secs)
} else {
    Write-Step 'Build (skipped - -SkipBuild)'
}

$exeSrc = Join-Path $buildPath 'putty.exe'
if (-not (Test-Path $exeSrc)) {
    Write-Err2 "build produced no putty.exe at $exeSrc"
    Write-Info 'Check the compiler output above; on a first run the tree may need -Reconfigure.'
    exit 1
}
$exeInfo = Get-Item $exeSrc
if ($exeInfo.Length -lt 500KB) {
    Write-Warn2 ("putty.exe is only {0:N0} bytes - that looks wrong" -f $exeInfo.Length)
}
Write-Info ("putty.exe: {0:N0} bytes  {1}" -f $exeInfo.Length, $exeInfo.LastWriteTime)

# ------------------------------------------------------------------ stage ---

Write-Step 'Stage the ship folder'

if (-not (Test-Path $relPath)) {
    New-Item -ItemType Directory -Path $relPath -Force | Out-Null
    Write-Info "created $relPath"
}

$exeDst = Join-Path $relPath 'putty.exe'
$oldLen = $null
if (Test-Path $exeDst) { $oldLen = (Get-Item $exeDst).Length }
Copy-Item -Path $exeSrc -Destination $exeDst -Force
if ($null -ne $oldLen) {
    Write-Info ("putty.exe replaced: {0:N0} -> {1:N0} bytes" -f $oldLen, (Get-Item $exeDst).Length)
} else {
    Write-Info ("putty.exe copied: {0:N0} bytes" -f (Get-Item $exeDst).Length)
}
Write-Ok "exe at $exeDst"

# -------------------------------------------------------------- the bridge --

$mcpStaged = Join-Path $relPath 'putty-mcp'
if ($SkipMcp) {
    Write-Step 'Bridge (skipped - -SkipMcp)'
} else {
    Write-Step 'Stage putty-mcp (the MCP bridge)'

    $mcpSrc = Join-Path $root 'mcp'
    foreach ($need in @('bin', 'src', 'package.json', 'package-lock.json')) {
        if (-not (Test-Path (Join-Path $mcpSrc $need))) {
            Write-Err2 "bridge source missing: $(Join-Path $mcpSrc $need)"
            exit 1
        }
    }

    if (-not (Test-Path $mcpStaged)) { New-Item -ItemType Directory -Path $mcpStaged -Force | Out-Null }

    # Mirror bin/ and src/ so deleted or renamed modules do not linger.
    foreach ($sub in @('bin', 'src')) {
        $srcDir = Join-Path $mcpSrc $sub
        $dstDir = Join-Path $mcpStaged $sub
        if (Test-Path $dstDir) {
            Assert-Inside -Path $dstDir -Parent $mcpStaged
            Remove-Item -Recurse -Force $dstDir
        }
        Copy-Item -Path $srcDir -Destination $dstDir -Recurse -Force
        $n = (Get-ChildItem $dstDir -Recurse -File | Measure-Object).Count
        Write-Debug2 "copied $sub ($n files) -> $dstDir"
    }
    foreach ($file in @('package.json', 'package-lock.json', 'README.md', 'USER-MANUAL.md')) {
        if (Test-Path (Join-Path $mcpSrc $file)) {
            Copy-Item -Path (Join-Path $mcpSrc $file) -Destination (Join-Path $mcpStaged $file) -Force
        } else {
            Write-Debug2 "not present in mcp/: $file"
        }
    }
    Write-Info "source copied to $mcpStaged"

    # Dependencies: installed HERE, on Windows. They are pure JavaScript (no
    # .node files), but a tree copied from a mac has unix .bin shims, so a fresh
    # install from the committed lockfile is the honest thing to ship.
    $nm = Join-Path $mcpStaged 'node_modules'
    $needDeps = $ReinstallDeps.IsPresent -or (-not (Test-Path $nm))
    if ($needDeps) {
        if (-not $node) {
            Write-Err2 'node is required to install the bridge dependencies (-SkipMcp avoids this)'
            exit 1
        }
        $npmCmd = (Get-Command npm -ErrorAction SilentlyContinue)
        if (-not $npmCmd) {
            Write-Err2 'npm not found next to node; cannot install the bridge dependencies'
            exit 1
        }

        $installed = $false
        Push-Location $mcpStaged
        try {
            Write-Info 'npm ci --omit=dev (pinned by package-lock.json) ...'
            Invoke-Tool -Exe $npmCmd.Source -Arguments @('ci', '--omit=dev', '--no-audit', '--no-fund') -What 'npm ci'
            $installed = $true
        } catch {
            Write-Warn2 "$($_.Exception.Message)"
            Write-Info 'falling back to npm install ...'
            try {
                Invoke-Tool -Exe $npmCmd.Source -Arguments @('install', '--omit=dev', '--no-audit', '--no-fund') -What 'npm install'
                $installed = $true
            } catch {
                Write-Warn2 "$($_.Exception.Message)"
            }
        } finally {
            Pop-Location
        }

        if (-not $installed) {
            # Offline fallback: the checkout's tree is pure JS and works as-is.
            $srcNm = Join-Path $mcpSrc 'node_modules'
            if (Test-Path $srcNm) {
                Write-Warn2 "npm failed - copying the checkout's node_modules instead (offline fallback)"
                Copy-Item -Path $srcNm -Destination $nm -Recurse -Force
                $installed = $true
            }
        }
        if (-not $installed) {
            Write-Err2 'the bridge has no dependencies - it will not start'
            Write-Info 'Connect to the network once and re-run, or see mcp/README.md step 4.'
            exit 1
        }
        $nPkgs = (Get-ChildItem $nm -Directory | Measure-Object).Count
        Write-Ok ("dependencies installed ({0} packages)" -f $nPkgs)
    } else {
        Write-Info 'node_modules already present (use -ReinstallDeps to refresh)'
    }

    $mjs = Join-Path $mcpStaged 'bin\putty-mcp.mjs'
    if (-not $SkipSmokeTest) {
        Write-Step 'Smoke-test the bridge (no PuTTY needed)'
        if (-not $node) { Write-Warn2 'skipped: no node'; }
        else {
            # The CLI writes its diagnostics to stderr. With 2>&1 under Windows
            # PowerShell 5.1 each such line becomes an ErrorRecord, and with
            # $ErrorActionPreference='Stop' that would abort on a *successful*
            # run - so let them through and judge by the exit code instead.
            $savedEap = $ErrorActionPreference
            $ErrorActionPreference = 'Continue'
            try {
                foreach ($sub in @('help', 'manual')) {
                    $out  = @(& $node $mjs $sub 2>&1)
                    $code = $LASTEXITCODE
                    if ($code -ne 0) {
                        Write-Err2 "node $mjs $sub failed (exit $code)"
                        $out | Select-Object -First 20 | ForEach-Object { Write-Info "$_" }
                        exit 1
                    }
                    $lines = $out.Count
                    Write-Debug2 "$sub -> exit 0, $lines lines"
                    Write-Info ("{0,-6} exit 0, {1} lines" -f $sub, $lines)
                }
            } finally {
                $ErrorActionPreference = $savedEap
            }
            Write-Ok 'the bridge runs'
        }
    }

    # SETUP-WIN.txt: the ship folder's own instructions, with real paths, so the
    # folder is self-describing (macOS has the equivalent SETUP.txt).
    $setupPath = Join-Path $relPath 'SETUP-WIN.txt'
    $nodeForJson = if ($node) { $node } else { 'C:\Program Files\nodejs\node.exe' }
    $nodeJson = $nodeForJson -replace '\\', '\\'
    $mjsJson  = $mjs       -replace '\\', '\\'
    $nodeShown = if ($node) { $node } else { '<full path to node.exe>' }

    $setup = @'
putty-mcp - the MCP bridge that ships beside putty.exe
=====================================================

putty.exe is the terminal; this folder is the other end of it. An MCP client
(OpenCode, Claude Desktop, ...) runs the program below, which speaks MCP on
stdio and talks to a *running* PuTTY window over a named pipe - so the agent
works in the window you are looking at, and you can watch it, take over, and
answer what it cannot.

    the program:  bin\putty-mcp.mjs        (run it with node)


1. What you need
-------------------------------------------------------------------------------
Node.js 20 or newer - that is all, and this machine already has it:

    node --version        # want v20 or newer

    detected here: __NODE_SHOWN__

Nothing to install. The bridge's npm packages are in `node_modules` next to
this file, so Node finds them by itself: no `npm install`, no global packages.
They were installed on this machine when the folder was built.


2. Offer a session to it (in PuTTY)
-------------------------------------------------------------------------------
The pipe only exists while at least one session is offered:

  a. Load the session in PuTTY.
  b. Session -> Automation (MCP) -> "Offer this session to an MCP client".
     "Also allow an MCP client to type into this session" lets the agent drive
     it as well; leave it off for read-only.
  c. Save, then reopen the session - the settings are read when the window
     opens.

Both switches are off by default: the first offers the session to anything that
can reach the pipe, the second lets it be typed into.


3. Point the client at it
-------------------------------------------------------------------------------
For OpenCode, add this to opencode.json inside the "mcp" object:

    "putty-mcp": {
      "type": "local",
      "command": [
        "__NODE_JSON__",
        "__MJS_JSON__",
        "mcp"
      ]
    }

(Windows paths in JSON need doubled backslashes, as above.) Then restart
OpenCode. Any other MCP client works the same way: it runs

    <node> <...>\putty-mcp\bin\putty-mcp.mjs mcp

and speaks MCP on stdio.


4. Checking it without an agent
-------------------------------------------------------------------------------
The same program is a plain command-line client - the first thing to reach for
when something is wrong. From this folder:

    node putty-mcp\bin\putty-mcp.mjs list       # sessions PuTTY is offering
    node putty-mcp\bin\putty-mcp.mjs read 1     # a session's output, with line ids
    node putty-mcp\bin\putty-mcp.mjs screen 1   # the visible screen
    node putty-mcp\bin\putty-mcp.mjs version    # PuTTY build and protocol version
    node putty-mcp\bin\putty-mcp.mjs help

`list` printing nothing means no session is offered (step 2), or PuTTY is not
running.


5. If something is wrong
-------------------------------------------------------------------------------
  * The endpoint record lives in %TEMP%\putty-mcp-<pid>\ (one folder per PuTTY
    process) and is also written into PuTTY's configuration folder as
    mcp-endpoint.<pid>. A stale one is harmless: connecting fails and it is
    skipped.
  * PUTTY_MCP_DEBUG=1 in PuTTY's environment traces the pipe, the handshake and
    every request on stderr.
  * PUTTY_CONFIG_DEBUG=1 traces where PuTTY thinks its configuration folder is.
  * node bin\putty-mcp.mjs list --debug traces the bridge's side.
  * An MCP client starts the bridge once and keeps it: after changing anything
    in this folder, restart the client - do not just kill the process.

More
-------------------------------------------------------------------------------
  * README.md       - the setup in more depth: where the pipe lives, the wire
                      protocol, and debugging both ends.
  * USER-MANUAL.md  - what the agent is told to read before it acts.
  * In the source: tech_detail.md section 13 and PLAN.md section 7.

This file is generated by scripts\build-windows.ps1. Re-running the script
refreshes this folder; a copy you make will not change, so re-copy after an
upgrade.
'@
    $setup = $setup.Replace('__NODE_SHOWN__', $nodeShown)
    $setup = $setup.Replace('__NODE_JSON__', $nodeJson)
    $setup = $setup.Replace('__MJS_JSON__', $mjsJson)

    # Written without a BOM: a .txt with one shows as junk in some editors.
    [System.IO.File]::WriteAllText($setupPath, $setup, (New-Object System.Text.UTF8Encoding($false)))
    Write-Info "wrote $setupPath"
    Write-Ok "bridge at $mcpStaged"
}

# ---------------------------------------------------------------- summary ---

$totalSecs = [int]((Get-Date) - $script:Started).TotalSeconds
Write-Host ''
Write-Host ("Done in {0}s. Ship folder: {1}" -f $totalSecs, $relPath) -ForegroundColor Green

Get-ChildItem $relPath | Sort-Object Name | ForEach-Object {
    if ($_.PSIsContainer) {
        $size = (Get-ChildItem $_.FullName -Recurse -File -ErrorAction SilentlyContinue |
                 Measure-Object -Property Length -Sum).Sum
        Write-Host ("  {0,-22} {1,12:N0} bytes  (folder)" -f $_.Name, $size)
    } else {
        Write-Host ("  {0,-22} {1,12:N0} bytes" -f $_.Name, $_.Length)
    }
}

Write-Host ''
Write-Info "run it : $exeDst"
if (-not $SkipMcp) {
    Write-Info ("bridge : node `"{0}`" `"{1}`" list" -f $nodeShown, (Join-Path $mcpStaged 'bin\putty-mcp.mjs'))
    Write-Info "setup  : $(Join-Path $relPath 'SETUP-WIN.txt')"
}
Write-Host ''