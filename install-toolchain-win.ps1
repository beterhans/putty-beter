<#
.SYNOPSIS
    putty-beter - global (machine-wide) Windows build-toolchain installer.

.DESCRIPTION
    Installs, machine-wide, everything needed to build an x86_64 putty.exe for
    Windows 7 SP1 -> Windows 11 on this Windows 11 ARM64 host (tech_detail.md section 1).

    What it installs (all "global" / all-users):
      1. Git for Windows (ARM64)        -> C:\Program Files\Git        (winget, --scope machine)
      2. CMake 4.x (ARM64, native)      -> C:\Program Files\CMake      (winget, --scope machine, ADD_CMAKE_TO_PATH=System)
      3. Ninja 1.13.2 (ARM64, native)   -> C:\Program Files\Ninja      (manual: winget's ninja package is portable/per-user)
      4. MSYS2 x86_64                   -> C:\msys64                   (x86_64 on purpose: the ARM64 MSYS2 has no x86_64 GCC)
      5. mingw-w64 UCRT64 toolchain     -> C:\msys64\ucrt64            (pacman: x86_64-w64-mingw32-gcc, binutils, windres)

    System-wide changes it makes: 3 new folders under C:\Program Files, C:\msys64,
    and Machine-scope PATH entries for Git\cmd, CMake\bin and Ninja.
    MSYS2 is deliberately NOT added to PATH (tech_detail.md section 1: it shadows Windows binaries).
    Nothing is deleted or overwritten; re-running is safe (idempotent).

    Admin: installing machine-wide needs an elevated shell. Without -NoElevate the
    script re-launches itself through UAC and waits, then verifies in the parent.

.PARAMETER Check
    Verify only - no downloads, no installs. Use this to re-check the box later.

.PARAMETER SmokeTest
    After installing, compile+run a 2-line hello.exe with the target flags
    (-D_WIN32_WINNT=0x0601 -static-libgcc) and print its imported DLLs. This is the
    tech_detail.md section 1 pre-check: if Defender quarantines the exe or kills gcc, STOP.

.PARAMETER AddDefenderExclusions
    Add Windows Defender path exclusions for the build dir (+ C:\msys64). Optional,
    security-relevant: it stops Defender scanning build output. Needs admin.

.PARAMETER Trace
    Debug switch: echo every external command (and its exit code) before/after it runs.

.PARAMETER NoElevate
    Never try to self-elevate; fail instead if not admin.

.PARAMETER Msys2Root
    MSYS2 install root. Default C:\msys64.

.PARAMETER NinjaDir
    Ninja install dir. Default C:\Program Files\Ninja.

.PARAMETER BuildDir
    Build tree used by -SmokeTest / -AddDefenderExclusions. Default C:\putty-beter-win.

.PARAMETER LogPath
    Log file. Default %USERPROFILE%\Downloads\temp\install-toolchain-win.log

.EXAMPLE
    pwsh -ExecutionPolicy Bypass -File .\install-toolchain-win.ps1
    Install everything (prompts UAC), then verify.

.EXAMPLE
    pwsh -ExecutionPolicy Bypass -File .\install-toolchain-win.ps1 -Check
    Just report what is installed and which architecture each tool is.

.EXAMPLE
    pwsh -ExecutionPolicy Bypass -File .\install-toolchain-win.ps1 -SmokeTest -Trace
    Install + run the hello.exe pre-check with command tracing.
#>

[CmdletBinding()]
param(
    [switch]$Check,
    [switch]$SmokeTest,
    [switch]$AddDefenderExclusions,
    [switch]$Trace,
    [switch]$NoElevate,
    [switch]$ElevatedChild,   # internal: set by the UAC re-launch, never by hand
    [string]$Msys2Root = 'C:\msys64',
    [string]$NinjaDir  = 'C:\Program Files\Ninja',
    [string]$BuildDir  = 'C:\putty-beter-win',
    [string]$LogPath   = (Join-Path $env:USERPROFILE 'Downloads\temp\install-toolchain-win.log')
)

$ErrorActionPreference = 'Continue'
$script:Fail = 0

# --- versions pinned to what winget/official releases served on 2026-11-23 -----
$GitUrl    = 'https://github.com/git-for-windows/git/releases/download/v2.56.1.windows.1/Git-2.56.1-arm64.exe'
$Msys2Url  = 'https://github.com/msys2/msys2-installer/releases/download/2026-09-27/msys2-x86_64-20260927.exe'
$NinjaUrl  = 'https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-winarm64.zip'
$MsysPackages = @('mingw-w64-ucrt-x86_64-toolchain')

# ================================ helpers =====================================
$logDir = Split-Path -Parent $LogPath
if ($logDir -and -not (Test-Path $logDir)) { New-Item -ItemType Directory -Path $logDir -Force | Out-Null }

function Write-Log {
    param([string]$Message, [string]$Level = 'INFO')
    $line = '{0} [{1,-5}] {2}' -f (Get-Date -Format 'HH:mm:ss'), $Level, $Message
    switch ($Level) {
        'ERROR' { Write-Host $line -ForegroundColor Red;    $script:Fail = 1 }
        'WARN'  { Write-Host $line -ForegroundColor Yellow }
        'OK'    { Write-Host $line -ForegroundColor Green }
        'STEP'  { Write-Host ''; Write-Host $line -ForegroundColor Cyan }
        default { Write-Host $line }
    }
    try { Add-Content -Path $LogPath -Value $line -Encoding UTF8 } catch { }
}

# Run an external command, honouring -Trace. Returns exit code.
function Invoke-External {
    param([string]$FilePath, [string[]]$Arguments = @(), [switch]$AllowFail)
    if ($Trace) { Write-Log ("TRACE cmd: `"{0}`" {1}" -f $FilePath, ($Arguments -join ' ')) 'TRACE' }
    & $FilePath @Arguments 2>&1 | ForEach-Object { Write-Host "      $_" }
    $code = $LASTEXITCODE
    if ($Trace) { Write-Log "TRACE exit: $code" 'TRACE' }
    if ($code -ne 0 -and -not $AllowFail) { Write-Log "command failed (exit $code): $FilePath" 'ERROR' }
    return $code
}

# PE machine type, so we can prove "x86_64 output, arm64 host tools"
function Get-PEArch {
    param([string]$Path)
    try {
        $fs = [System.IO.File]::OpenRead($Path)
        try {
            $br = New-Object System.IO.BinaryReader($fs)
            $fs.Position = 0x3C
            $peOff = $br.ReadInt32()
            if ($peOff -le 0 -or $peOff -gt $fs.Length - 8) { return 'not-PE' }
            $fs.Position = $peOff
            if ($br.ReadUInt32() -ne 0x00004550) { return 'not-PE' }
            $machine = $br.ReadUInt16()
            switch ($machine) {
                0x014c  { 'x86' }
                0x8664  { 'x86_64' }
                0xAA64  { 'ARM64' }
                0x01c4  { 'ARM' }
                default { ('unknown(0x{0:X})' -f $machine) }
            }
        } finally { $fs.Dispose() }
    } catch { 'err' }
}

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Add-MachinePath {
    param([string[]]$Path)
    $cur   = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    $parts = @($cur -split ';' | Where-Object { $_ -ne '' })
    $added = @()
    foreach ($p in $Path) { if ($parts -notcontains $p) { $parts += $p; $added += $p } }
    if ($added.Count -gt 0) {
        [Environment]::SetEnvironmentVariable('Path', ($parts -join ';'), 'Machine')
        Write-Log "Machine PATH += $($added -join ', ')" 'OK'
    } else {
        Write-Log "Machine PATH already has: $($Path -join ', ')"
    }
    # refresh this session
    $env:PATH = ([Environment]::GetEnvironmentVariable('Path','Machine') + ';' +
                 [Environment]::GetEnvironmentVariable('Path','User'))
}

function Get-FirstLine {
    param([string]$FilePath, [string[]]$Arguments = @())
    if (-not (Test-Path $FilePath)) { return '<missing>' }
    try { ((& $FilePath @Arguments 2>&1 | Select-Object -First 1) -as [string]) } catch { '<error>' }
}

# ============================ elevation ======================================
$isAdmin = Test-Admin
$hostInfo = 'host={0} os={1} build={2} admin={3}' -f `
    [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture,
    (Get-CimInstance Win32_OperatingSystem).Caption,
    [Environment]::OSVersion.Version,
    $isAdmin
Write-Log $hostInfo 'STEP'

if (-not $Check -and -not $isAdmin -and -not $NoElevate) {
    $shell = if (Get-Command pwsh -ErrorAction SilentlyContinue) { (Get-Command pwsh).Source } else { 'powershell.exe' }
    $childArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"",
                   '-ElevatedChild', '-Msys2Root', "`"$Msys2Root`"", '-NinjaDir', "`"$NinjaDir`"",
                   '-LogPath', "`"$LogPath`"")
    if ($SmokeTest)             { $childArgs += '-SmokeTest' }
    if ($AddDefenderExclusions) { $childArgs += '-AddDefenderExclusions' }
    if ($Trace)                 { $childArgs += '-Trace' }
    Write-Log "not elevated -> re-launching through UAC: $shell" 'WARN'
    try {
        Start-Process -FilePath $shell -ArgumentList $childArgs -Verb RunAs -Wait
    } catch {
        Write-Log "elevation cancelled/failed: $($_.Exception.Message)" 'ERROR'
    }
    Write-Log 'install pass finished - verifying in this (non-elevated) session' 'STEP'
    $Check = $true   # never install from the non-elevated parent
}

if (-not $Check -and -not $isAdmin) {
    Write-Log 'admin rights required for machine-wide installs; re-run from an elevated shell (or drop -NoElevate)' 'ERROR'
    exit 1
}

# ============================ verification ===================================
function Invoke-Verify {
    Write-Log 'VERIFY' 'STEP'
    $rows = New-Object System.Collections.ArrayList
    function Add-Row([string]$Tool, [string]$Path, [string]$Ver) {
        $arch = if ($Path -and (Test-Path $Path)) { Get-PEArch $Path } else { '-' }
        [void]$rows.Add([pscustomobject]@{ Tool = $Tool; Arch = $arch; Path = $Path; Version = $Ver })
    }

    # prefer the installed locations, fall back to PATH lookups
    $gitExe  = @('C:\Program Files\Git\cmd\git.exe', (Get-Command git -ErrorAction SilentlyContinue).Source) |
               Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
    $cmExe   = @('C:\Program Files\CMake\bin\cmake.exe', (Get-Command cmake -ErrorAction SilentlyContinue).Source) |
               Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
    $njExe   = @((Join-Path $NinjaDir 'ninja.exe'), (Get-Command ninja -ErrorAction SilentlyContinue).Source) |
               Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
    $gccExe  = Join-Path $Msys2Root 'ucrt64\bin\x86_64-w64-mingw32-gcc.exe'
    # MSYS2 binutils installs these UNPREFIXED in ucrt64\bin; the prefixed copies
    # live in ucrt64\x86_64-w64-mingw32\bin (verified 2026-11-23)
    $wrExe   = Join-Path $Msys2Root 'ucrt64\bin\windres.exe'
    $bashExe = Join-Path $Msys2Root 'usr\bin\bash.exe'
    $odExe   = Join-Path $Msys2Root 'ucrt64\bin\objdump.exe'

    Add-Row 'git'      $gitExe (Get-FirstLine $gitExe @('--version'))
    Add-Row 'cmake'    $cmExe  (Get-FirstLine $cmExe  @('--version'))
    Add-Row 'ninja'    $njExe  (Get-FirstLine $njExe  @('--version'))
    Add-Row 'gcc'      $gccExe (Get-FirstLine $gccExe @('--version'))
    Add-Row 'windres'  $wrExe  (Get-FirstLine $wrExe  @('--version'))
    Add-Row 'objdump'  $odExe  (Get-FirstLine $odExe  @('--version'))
    Add-Row 'msys2-sh' $bashExe (Get-FirstLine $bashExe @('-lc','uname -m'))

    $rows | Format-Table -AutoSize | Out-String -Width 200 | ForEach-Object { Write-Host $_ }

    Push-Location $env:TEMP   # cmd-based shims (npm.cmd) cannot run with a UNC cwd
    try {
        Write-Log ("node  : " + (Get-FirstLine 'C:\Program Files\nodejs\node.exe' @('--version')))
        Write-Log ("npm   : " + (Get-FirstLine 'C:\Program Files\nodejs\npm.cmd' @('--version')))
    } finally { Pop-Location }

    $need = @($gitExe, $cmExe, $njExe, $gccExe, $wrExe, $bashExe) | Where-Object { -not ($_ -and (Test-Path $_)) }
    if ($need.Count -gt 0) {
        Write-Log "missing: $($need -join ', ')" 'ERROR'
    } else {
        Write-Log 'all toolchain components present' 'OK'
    }
    # the compiler must be an x86_64 host tool (it produces x86_64 code for Win7+)
    if ((Test-Path $gccExe) -and (Get-PEArch $gccExe) -ne 'x86_64') {
        Write-Log "unexpected gcc host arch: $(Get-PEArch $gccExe) - expected x86_64" 'WARN'
    }
}

# ============================ install steps ==================================
function Install-Winget {
    param([string]$Id, [string]$What)
    Write-Log "STEP: $What ($Id, machine scope)" 'STEP'
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) { Write-Log 'winget not found' 'ERROR'; return }
    Invoke-External 'winget' @('install','--id',$Id,'--exact','--source','winget','--scope','machine',
                               '--silent','--accept-package-agreements','--accept-source-agreements') | Out-Null
}

function Install-Ninja {
    Write-Log "STEP: Ninja (ARM64, native) -> $NinjaDir" 'STEP'
    if (Test-Path (Join-Path $NinjaDir 'ninja.exe')) { Write-Log 'ninja already installed' 'OK' }
    else {
        $zip = Join-Path $env:TEMP 'ninja-winarm64.zip'
        try {
            Write-Log "downloading $NinjaUrl"
            Invoke-WebRequest -Uri $NinjaUrl -OutFile $zip -UseBasicParsing
            New-Item -ItemType Directory -Path $NinjaDir -Force | Out-Null
            Expand-Archive -Path $zip -DestinationPath $NinjaDir -Force
            Write-Log "extracted to $NinjaDir" 'OK'
        } catch { Write-Log "ninja install failed: $($_.Exception.Message)" 'ERROR'; return }
    }
    Add-MachinePath @($NinjaDir)
}

function Install-Msys2 {
    Write-Log "STEP: MSYS2 x86_64 -> $Msys2Root" 'STEP'
    if (Test-Path (Join-Path $Msys2Root 'usr\bin\bash.exe')) {
        Write-Log 'MSYS2 already installed'
    } else {
        $exe = Join-Path $env:TEMP 'msys2-x86_64-20260927.exe'
        try {
            Write-Log "downloading $Msys2Url"
            Invoke-WebRequest -Uri $Msys2Url -OutFile $exe -UseBasicParsing
        } catch { Write-Log "MSYS2 download failed: $($_.Exception.Message)" 'ERROR'; return }
        # Qt Installer Framework CLI: non-interactive install
        Invoke-External $exe @('in','--confirm-command','--accept-messages','--root',($Msys2Root -replace '\\','/')) | Out-Null
    }
    if (-not (Test-Path (Join-Path $Msys2Root 'usr\bin\bash.exe'))) {
        Write-Log "MSYS2 not present at $Msys2Root - run the installer manually (https://www.msys2.org) and re-run this script" 'ERROR'
        return
    }

    $bash = Join-Path $Msys2Root 'usr\bin\bash.exe'
    $env:MSYSTEM = 'UCRT64'
    Write-Log 'pacman -Syu (base update)'
    Invoke-External $bash @('-lc','pacman -Syu --noconfirm') | Out-Null
    Write-Log ("pacman -S --needed --noconfirm " + ($MsysPackages -join ' '))
    Invoke-External $bash @('-lc',("pacman -S --needed --noconfirm " + ($MsysPackages -join ' '))) | Out-Null
}

function Install-DefenderExclusions {
    Write-Log 'STEP: Defender exclusions (optional)' 'STEP'
    $paths = @($BuildDir, $Msys2Root)
    try {
        $pref = Get-MpPreference -ErrorAction Stop
        foreach ($p in $paths) {
            if ($pref.ExclusionPath -contains $p) { Write-Log "exclusion already present: $p" }
            else { Add-MpPreference -ExclusionPath $p -ErrorAction Stop; Write-Log "exclusion added: $p" 'OK' }
        }
        Write-Log 'note: this stops Defender scanning build output under those paths' 'WARN'
    } catch { Write-Log "could not set exclusions: $($_.Exception.Message)" 'ERROR' }
}

function Invoke-SmokeTest {
    Write-Log 'STEP: hello.exe pre-check (tech_detail.md section 1)' 'STEP'
    $gcc = Join-Path $Msys2Root 'ucrt64\bin\x86_64-w64-mingw32-gcc.exe'
    $od  = Join-Path $Msys2Root 'ucrt64\bin\objdump.exe'
    if (-not (Test-Path $gcc)) { Write-Log "compiler missing: $gcc" 'ERROR'; return }
    # cc1.exe / collect2.exe live in ucrt64\lib\gcc\... and need the ucrt64\bin DLLs
    # on PATH. Without it they die SILENTLY with 0xC0000135 (exit 1, no output) -
    # that looks exactly like the old tech_detail.md section 1 Defender block but is not one.
    $env:PATH = (Join-Path $Msys2Root 'ucrt64\bin') + ';' + $env:PATH
    $work = Join-Path $env:USERPROFILE 'Downloads\temp\hello-smoke'
    New-Item -ItemType Directory -Path $work -Force | Out-Null
    $c = Join-Path $work 'hello.c'
    "#include <stdio.h>`r`nint main(void) { printf(`"hello x86_64 ok\`n`"); return 0; }`r`n" | Set-Content -Path $c -Encoding ASCII
    $exe = Join-Path $work 'hello.exe'
    Invoke-External $gcc @('-D_WIN32_WINNT=0x0601','-static-libgcc','-O2',$c,'-o',$exe) | Out-Null
    if (-not (Test-Path $exe)) { Write-Log 'compile produced no exe - Defender may have killed gcc (STOP)' 'ERROR'; return }
    Write-Log "hello.exe PE arch: $(Get-PEArch $exe) (expected x86_64)"
    Write-Log 'running hello.exe ...'
    Invoke-External $exe @() | Out-Null
    if ($LASTEXITCODE -eq 0) { Write-Log 'hello.exe ran OK' 'OK' }
    else { Write-Log "hello.exe did not run cleanly (exit $LASTEXITCODE) - check Defender (STOP)" 'ERROR' }
    if (Test-Path $od) {
        Write-Log 'imports (Win7 must have every DLL listed, except api-ms-win-crt-*/ucrtbase = KB2999226):'
        Invoke-External $od @('-p',$exe) | Out-Null
    }
}

# ================================ main =======================================
if ($Check) {
    Write-Log 'CHECK ONLY - nothing will be installed' 'WARN'
    Invoke-Verify
} else {
    Write-Log 'INSTALL PASS (machine-wide)' 'STEP'
    Install-Winget 'Git.Git'       'Git for Windows'
    Install-Winget 'Kitware.CMake' 'CMake'
    Install-Ninja
    Install-Msys2
    if ($AddDefenderExclusions) { Install-DefenderExclusions }
    Write-Log 'refreshing session PATH'
    $env:PATH = ([Environment]::GetEnvironmentVariable('Path','Machine') + ';' +
                 [Environment]::GetEnvironmentVariable('Path','User'))
    Invoke-Verify
    if ($SmokeTest) { Invoke-SmokeTest }
}

Write-Log "log written to $LogPath"
if ($script:Fail -ne 0) { Write-Log 'FINISHED WITH ERRORS - see log above' 'ERROR'; exit 1 }
Write-Log 'DONE - toolchain ready. Next: W0 (clone/robocopy + cmake -G Ninja).' 'OK'
exit 0
