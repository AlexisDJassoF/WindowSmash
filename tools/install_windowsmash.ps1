<#
.SYNOPSIS
Sets WindowSmash up on this PC in one go: what "Install WindowSmash.bat" runs.

.DESCRIPTION
Steps, each skipped when it is already done, so running it again carries on where it stopped:
  1. check     64-bit Windows 10 or 11, a processor with AVX2, the installed RAM, the free space
               on this drive (about 60 GB for the build). A failed check stops with a plain message.
  2. python    a Python 3.10 or newer with the packages (deps\python, then python on the PATH, then
               the py launcher); otherwise python.org's embeddable Python 3.14 into deps\python, with
               pip and requirements.txt. Every later Python call uses the one found.
  3. compiler  clang-cl, found and tried by tools\build_game.py --check; otherwise the Visual Studio
               Build Tools with the C++ workload and the Clang component, through winget (a few GB),
               or the missing components added to a Visual Studio already there; without winget, the
               manual step and its link.
  4. sdl3      deps\SDL3 from the newest SDL3-devel-<version>-VC.zip on GitHub, checked against the
               SHA-256 the release lists (controllers other than Xbox pads).
  5. codecs    nothing to fetch: the music and movie codecs are built from third_party\ during the
               game build (tools\codec_libs.py).
  6. files     a window asking where your game files, keys and firmware are, filled in from yuzu's
               or Ryujinx's folders when they have them, showing what was found, and your online
               name (online_name.txt), so the first start asks nothing.
  7. extras    a desktop shortcut to Play SSBU.bat and, if ticked, a Windows Firewall rule for
               build\ssbu.exe (asks for administrator rights once).
  8. build     tools\build_game.py with those folders: the game's program, fonts, data, DLC,
               fighters, then the build itself (30 minutes to a few hours).
  9. shaders   if ticked, Prepare Shaders.bat in a window of its own, at low priority.
Everything is written to install.log next to this folder's launchers.

Why the embeddable Python and not `winget install Python.Python.3.14`: the embeddable zip goes
into deps\python and touches nothing else on the PC (no PATH, no file associations, no second
Python for other programs to find), it needs no installer run, and it is 11 MB. Its drawbacks do
not matter here: it has no tkinter (this wizard is WinForms) and no pip until get-pip.py adds it.
winget's package would be the choice for a Python the user wants to keep for other things.

.PARAMETER DryRun
Say what each step would do and do nothing: no downloads, no installs, no wizard, no build.
.PARAMETER Game
.PARAMETER Keys
.PARAMETER Firmware
The folders, instead of the wizard's auto-detection (the wizard still shows them unless -NoWizard).
.PARAMETER OnlineName
The online name to write into online_name.txt.
.PARAMETER NoWizard
Ask in this window instead of opening the wizard.
.PARAMETER SkipBuild
Stop after the setup steps, before building the game.
.PARAMETER Jobs
Passed to the build: how many files to compile at a time (default: by the installed RAM).
#>
[CmdletBinding()]
param(
    [switch]$DryRun,
    [string]$Game,
    [string]$Keys,
    [string]$Firmware,
    [string]$OnlineName,
    [switch]$NoWizard,
    [switch]$SkipBuild,
    [int]$Jobs
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Root = Split-Path -Parent $PSScriptRoot
$Script:Python = $null            # the Python command: @(exe) or @('py', '-3')
$Script:WizardSnapshot = $null    # set to a .png path to have the wizard save a picture of itself and accept (tests)

# ---- what to fetch, and from where -----------------------------------------------------------
$PythonMinor = '3.14'
$PythonFallbackVersion = '3.14.8'   # when python.org's listing cannot be read
$PythonFtp = 'https://www.python.org/ftp/python/'
$GetPipUrl = 'https://bootstrap.pypa.io/get-pip.py'
$SdlReleasesUrl = 'https://api.github.com/repos/libsdl-org/SDL/releases?per_page=30'
$SdlFallback = @{   # when GitHub's API cannot be read: a known release and its listed SHA-256
    Version = '3.4.18'
    Url = 'https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-devel-3.4.18-VC.zip'
    Sha256 = '78d84602ae616cfe26b33a73b7d3b9b6b56e8a6650e53d92c9c6ca938f747acd'
}
$VsBuildToolsId = 'Microsoft.VisualStudio.BuildTools'   # Visual Studio 2026's; 2022's is Microsoft.VisualStudio.2022.BuildTools
$VsBuildToolsUrl = 'https://visualstudio.microsoft.com/visual-cpp-build-tools/'
$VsInstallerDir = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer'
$NeedDiskGb = 60
$MinRamGb = 8
$FirewallRuleName = 'WindowSmash (ssbu.exe)'
$UserAgent = 'WindowSmash-installer'

# ---- pure functions: what the steps decide by (tested in tests/test_installer.py) --------------
function Get-PythonEmbedUrl {
    <# The newest 3.<minor>.x on python.org's listing (its "3.14.8/" links), or the fallback. #>
    param([string]$Listing, [string]$Minor, [string]$Fallback, [string]$Base = $PythonFtp)
    $versions = @()
    foreach ($m in [regex]::Matches($Listing, 'href="(\d+\.\d+\.\d+)/"')) {
        $v = $m.Groups[1].Value
        if ($v.StartsWith($Minor + '.')) { $versions += [version]$v }
    }
    $pick = $Fallback
    if ($versions.Count -gt 0) { $pick = ($versions | Sort-Object -Descending | Select-Object -First 1).ToString() }
    return @{ Version = $pick; Url = "$Base$pick/python-$pick-embed-amd64.zip" }
}

function Select-Sdl3Asset {
    <# The newest release (not a prerelease) with an SDL3-devel-3.x.y-VC.zip, as the GitHub API lists
       them: its version, the zip's URL and the SHA-256 the API gives for it (digest), if any. #>
    param($Releases)
    $best = $null
    foreach ($r in $Releases) {
        if ($r.prerelease -or $r.draft) { continue }
        foreach ($a in $r.assets) {
            $m = [regex]::Match($a.name, '^SDL3-devel-(3\.\d+\.\d+)-VC\.zip$')
            if (-not $m.Success) { continue }
            $v = [version]$m.Groups[1].Value
            if ($null -eq $best -or $v -gt $best.Version) {
                $sha = $null
                if ($a.PSObject.Properties['digest'] -and $a.digest -match '^sha256:([0-9a-fA-F]{64})$') { $sha = $Matches[1].ToLower() }
                $best = @{ Version = $v; Url = $a.browser_download_url; Sha256 = $sha; Name = $a.name }
            }
        }
    }
    return $best
}

function Get-VsBuildToolsArguments {
    <# winget's arguments for the Build Tools with the C++ workload and the Clang component, installed
       passively (a progress window, no questions) and waited for. #>
    param([string]$Id)
    $override = '--passive --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Llvm.Clang --includeRecommended'
    return @('install', '--id', $Id, '--exact', '--source', 'winget', '--accept-package-agreements',
             '--accept-source-agreements', '--disable-interactivity', '--override', $override)
}

function Get-VsModifyArguments {
    <# The Visual Studio installer's arguments to add the C++ workload and Clang to an edition
       already installed (vswhere's installationPath and productId). #>
    param([string]$InstallPath, [string]$ProductId)
    $workload = 'Microsoft.VisualStudio.Workload.NativeDesktop'
    if ($ProductId -match 'BuildTools') { $workload = 'Microsoft.VisualStudio.Workload.VCTools' }
    return @('modify', '--installPath', $InstallPath, '--add', $workload, '--add', 'Microsoft.VisualStudio.Component.VC.Llvm.Clang',
             '--includeRecommended', '--passive', '--norestart', '--wait')
}

function Get-FolderCandidates {
    <# Where the emulators keep keys, firmware and installed games, most likely first. #>
    param([string]$AppData, [string]$UserProfile)
    $keys = @(); $firmware = @(); $game = @()
    foreach ($name in @('yuzu', 'eden', 'sudachi', 'citron', 'suyu')) {   # yuzu and the forks with its layout
        $keys += "$AppData\$name\keys"
        $firmware += "$AppData\$name\nand\system\Contents\registered"
        $game += "$AppData\$name\nand\user\Contents\registered"
        $game += "$AppData\$name\sdmc\Nintendo\Contents\registered"
    }
    $keys += "$AppData\Ryujinx\system"
    $firmware += "$AppData\Ryujinx\bis\system\Contents\registered"
    $game += "$AppData\Ryujinx\bis\user\Contents\registered"
    $game += "$AppData\Ryujinx\sdcard\Nintendo\Contents\registered"
    $keys += "$UserProfile\.switch"
    return @{ Keys = $keys; Firmware = $firmware; Game = $game }
}

function Resolve-FirstExisting {
    <# The first of the paths for which the test says yes, or $null. #>
    param([string[]]$Paths, [scriptblock]$Exists)
    foreach ($p in $Paths) {
        if (& $Exists $p) { return $p }
    }
    return $null
}

function Test-Avx2ByName {
    <# Whether a processor has AVX2, from its name, when Windows cannot be asked (before 20H1):
       $true, $false, or $null when the name does not say. #>
    param([string]$Name)
    if ($Name -match 'Ryzen|EPYC|Threadripper') { return $true }
    if ($Name -match 'Core\(TM\)\s+Ultra|Core\s+Ultra') { return $true }
    $m = [regex]::Match($Name, 'Core\(TM\)\s+i[3579]-(\d{4,5})')
    if ($m.Success) { return ([int]$m.Groups[1].Value -ge 4000) }   # 4th generation (Haswell) and newer
    $m = [regex]::Match($Name, '\bi[3579]-(\d{4,5})')
    if ($m.Success) { return ([int]$m.Groups[1].Value -ge 4000) }
    if ($Name -match 'Core\(TM\)2|Core\(TM\)\s+i[3579]\s+\d{3}\b|Phenom|Athlon\s+(64|II|X[24])\b|Atom|A[468]-\d{4}|A10-\d{4}') { return $false }
    return $null
}

function Get-FirewallRuleCommand {
    <# The PowerShell that adds the inbound allow rule for the game (run with administrator rights). #>
    param([string]$Exe, [string]$Name)
    return "New-NetFirewallRule -DisplayName '$Name' -Direction Inbound -Program '$Exe' -Action Allow -Profile Any | Out-Null"
}

function Test-PythonVersionText {
    <# Whether "3.14.6"-like text names a Python WindowSmash can use (3.10 or newer, 3.x). #>
    param([string]$Text)
    $m = [regex]::Match($Text, '(\d+)\.(\d+)')
    if (-not $m.Success) { return $false }
    $v = [version]("{0}.{1}" -f $m.Groups[1].Value, $m.Groups[2].Value)
    return ($v -ge [version]'3.10' -and $v.Major -eq 3)
}

function Get-DiskNeedGb {
    <# How much free space the build still needs: the full amount, less what dump\ and build\ hold already. #>
    param([double]$NeedGb, [double]$AlreadyGb)
    return [math]::Max(0, $NeedGb - $AlreadyGb)
}

# ---- helpers ---------------------------------------------------------------------------------------
function Write-Step { param([string]$Text) Write-Host ''; Write-Host $Text -ForegroundColor Cyan }
function Write-Info { param([string]$Text) Write-Host "  $Text" }
function Write-Warn { param([string]$Text) Write-Host "  $Text" -ForegroundColor Yellow }
function Write-Would { param([string]$Text) Write-Host "  (dry run) would $Text" -ForegroundColor DarkGray }

function Invoke-Download {
    param([string]$Url, [string]$Dest, [string]$What)
    Write-Info "downloading $What"
    Write-Info "  from $Url"
    $dir = Split-Path -Parent $Dest
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
    $partial = "$Dest.partial"
    $old = $ProgressPreference
    $ProgressPreference = 'SilentlyContinue'   # (the progress bar makes Windows PowerShell's downloads very slow)
    try {
        Invoke-WebRequest -Uri $Url -OutFile $partial -UseBasicParsing -Headers @{ 'User-Agent' = $UserAgent }
    } finally {
        $ProgressPreference = $old
    }
    Move-Item -LiteralPath $partial -Destination $Dest -Force
    Write-Info ("  {0:N1} MB" -f ((Get-Item -LiteralPath $Dest).Length / 1MB))
}

function Get-Json {
    param([string]$Url)
    return Invoke-RestMethod -Uri $Url -UseBasicParsing -Headers @{ 'User-Agent' = $UserAgent; 'Accept' = 'application/vnd.github+json' }
}

function Get-Text {
    param([string]$Url)
    return (Invoke-WebRequest -Uri $Url -UseBasicParsing -Headers @{ 'User-Agent' = $UserAgent }).Content
}

function Test-FileSha256 {
    param([string]$File, [string]$Expected)
    $actual = (Get-FileHash -LiteralPath $File -Algorithm SHA256).Hash.ToLower()
    return ($actual -eq $Expected.ToLower())
}

function Invoke-Python {
    <# Runs the chosen Python with these arguments, output to this window; returns its exit code. #>
    param([string[]]$Arguments)
    $exe = $Script:Python[0]
    $pre = @()
    if ($Script:Python.Count -gt 1) { $pre = @($Script:Python[1..($Script:Python.Count - 1)]) }
    & $exe @pre @Arguments | Out-Host   # (to the window, not into this function's result)
    return $LASTEXITCODE
}

function Get-PythonOutput {
    <# Runs the chosen Python and returns what it printed (errors included), and the exit code. #>
    param([string[]]$Arguments)
    $exe = $Script:Python[0]
    $pre = @()
    if ($Script:Python.Count -gt 1) { $pre = @($Script:Python[1..($Script:Python.Count - 1)]) }
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & $exe @pre @Arguments 2>&1 | ForEach-Object { "$_" }
        return @{ Output = ($out -join "`n"); Code = $LASTEXITCODE }
    } finally {
        $ErrorActionPreference = $old
    }
}

function Test-PythonCommand {
    <# The version of a Python command (@(exe, args...)), or $null when it does not run. #>
    param([string[]]$Command)
    try {
        $old = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        $exe = $Command[0]
        $pre = @()
        if ($Command.Count -gt 1) { $pre = @($Command[1..($Command.Count - 1)]) }
        # (no double quotes in what is passed to a program: Windows PowerShell hands them over unescaped)
        $out = & $exe @pre '-c' 'import sys; print(sys.version)' 2>&1 | ForEach-Object { "$_" }
        $ErrorActionPreference = $old
        if ($LASTEXITCODE -ne 0) { return $null }
        $text = ($out -join ' ').Trim().Split(' ')[0]   # "3.14.8" of "3.14.8 (tags/v3.14.8:..., Sep 30 2026) [MSC ...]"
        if (Test-PythonVersionText $text) { return $text }
        return $null
    } catch {
        return $null
    }
}

# ---- step 1: the PC ---------------------------------------------------------------------------------
function Step-CheckPc {
    Write-Step '[1/9] Check this PC'
    $os = Get-CimInstance Win32_OperatingSystem
    $build = [int]$os.BuildNumber
    Write-Info "$($os.Caption) ($($os.Version), $($os.OSArchitecture))"
    if ($os.OSArchitecture -notmatch '64') { throw 'WindowSmash needs 64-bit Windows; this Windows is not 64-bit.' }
    if ([version]$os.Version -lt [version]'10.0') { throw 'WindowSmash needs Windows 10 or 11.' }

    $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
    $name = ($cpu.Name -replace '\s+', ' ').Trim()
    $avx2 = $null
    if ($build -ge 19041) {   # Windows 10 version 2004 and later can be asked about AVX2 (feature 40)
        $sig = '[DllImport("kernel32.dll")] public static extern bool IsProcessorFeaturePresent(int feature);'
        $k = Add-Type -MemberDefinition $sig -Name 'Cpu' -Namespace 'WindowSmashSetup' -PassThru
        $avx2 = [bool]$k::IsProcessorFeaturePresent(40)
    } else {
        $avx2 = Test-Avx2ByName $name
    }
    if ($avx2 -eq $true) { Write-Info "$name - has AVX2" }
    elseif ($avx2 -eq $false) { throw "The processor ($name) has no AVX2, which the game needs: WindowSmash cannot run on it." }
    else { Write-Warn "$name - whether it has AVX2 is unknown; the game will only start if it does (the setup guide's step 1 says which do)" }

    $ramGb = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB)
    if ($ramGb -lt $MinRamGb) { throw "This PC has $ramGb GB of RAM; WindowSmash needs at least $MinRamGb GB (16 GB is comfortable)." }
    if ($ramGb -lt 16) { Write-Warn "$ramGb GB of RAM: enough, but the build will be slow (16 GB is comfortable)" } else { Write-Info "$ramGb GB of RAM" }

    $drive = [IO.Path]::GetPathRoot($Root)
    $free = (Get-PSDrive -Name $drive.Substring(0, 1)).Free / 1GB
    $already = 0
    foreach ($d in @('dump', 'build')) {
        $p = Join-Path $Root $d
        if (Test-Path -LiteralPath $p) { $already += (Get-ChildItem -LiteralPath $p -Recurse -File -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum / 1GB }
    }
    $need = Get-DiskNeedGb $NeedDiskGb $already
    Write-Info ("{0:N0} GB free on {1}, {2:N0} GB still needed for the build" -f $free, $drive, $need)
    if ($free -lt $need) {
        throw ("Only {0:N0} GB are free on {1}; the build needs about {2:N0} GB more. Free some space, or move WindowSmash to a drive with room." -f $free, $drive, $need)
    }
    $gpu = (Get-CimInstance Win32_VideoController | Select-Object -First 1).Name
    if ($gpu) { Write-Info "graphics: $gpu (needs DirectX 11 and an up-to-date driver)" }
}

# ---- step 2: Python -----------------------------------------------------------------------------------
function Find-Python {
    <# The first Python 3.10+ that runs: deps\python, python on the PATH, the py launcher. #>
    $own = Join-Path $Root 'deps\python\python.exe'
    $candidates = @(, @($own)) + @(, @('python')) + @(, @('py', '-3'))
    foreach ($c in $candidates) {
        if ($c[0] -eq $own -and -not (Test-Path -LiteralPath $own)) { continue }
        $v = Test-PythonCommand $c
        if ($v) { return @{ Command = $c; Version = $v } }
    }
    return $null
}

function Test-PythonPackages {
    <# Whether the chosen Python has what the tools import (pycryptodomex, and a zstd). #>
    $r = Get-PythonOutput @('-c', 'import Cryptodome')
    if ($r.Code -ne 0) { return $false }
    $r = Get-PythonOutput @('-c', 'import importlib.util as u, sys; sys.exit(0 if u.find_spec(''compression.zstd'') or u.find_spec(''zstandard'') else 1)')
    return ($r.Code -eq 0)
}

function Step-Python {
    Write-Step '[2/9] Python'
    $found = Find-Python
    if ($found) {
        $Script:Python = $found.Command
        Write-Info "Python $($found.Version): $($found.Command -join ' ')"
    } else {
        $target = Join-Path $Root 'deps\python'
        $listing = ''
        try { $listing = Get-Text $PythonFtp } catch { Write-Warn "python.org's listing could not be read ($($_.Exception.Message)); taking $PythonFallbackVersion" }
        $pick = Get-PythonEmbedUrl $listing $PythonMinor $PythonFallbackVersion
        if ($DryRun) {
            Write-Would "download Python $($pick.Version) (embeddable, $($pick.Url)) into deps\python, add pip, and install requirements.txt there"
            return
        }
        Write-Info "no Python 3.10 or newer found: setting up Python $($pick.Version) in deps\python (touches nothing else on the PC)"
        $zip = Join-Path $Root "deps\python-$($pick.Version)-embed-amd64.zip"
        if (-not (Test-Path -LiteralPath $zip)) { Invoke-Download $pick.Url $zip "Python $($pick.Version)" }
        if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Recurse -Force }
        Expand-Archive -LiteralPath $zip -DestinationPath $target
        Remove-Item -LiteralPath $zip -Force
        # the embeddable Python ignores site-packages until its ._pth file says "import site"
        foreach ($pth in Get-ChildItem -LiteralPath $target -Filter 'python3*._pth') {
            $text = [IO.File]::ReadAllText($pth.FullName)
            [IO.File]::WriteAllText($pth.FullName, $text.Replace('#import site', 'import site'))
        }
        $Script:Python = @((Join-Path $target 'python.exe'))
        $v = $null
        foreach ($try in 1..5) {   # (a just-extracted program can be busy for a moment: the antivirus looks at it first)
            $v = Test-PythonCommand $Script:Python
            if ($v) { break }
            Start-Sleep -Seconds 2
        }
        if (-not $v) { throw "deps\python\python.exe does not run after extracting $($pick.Url)." }
        Write-Info "Python $v in deps\python"
    }
    # pip, for the embeddable Python (the others have it)
    $pip = Get-PythonOutput @('-m', 'pip', '--version')
    if ($pip.Code -ne 0) {
        if ($DryRun) { Write-Would 'add pip with get-pip.py' } else {
            $getpip = Join-Path $Root 'deps\get-pip.py'
            Invoke-Download $GetPipUrl $getpip 'get-pip.py (adds pip)'
            if ((Invoke-Python @($getpip, '--no-warn-script-location')) -ne 0) { throw 'get-pip.py failed (see above).' }
            Remove-Item -LiteralPath $getpip -Force
        }
    }
    if (Test-PythonPackages) {
        Write-Info 'packages: installed'
    } elseif ($DryRun) {
        Write-Would 'run pip install -r requirements.txt'
    } else {
        Write-Info 'installing the packages (requirements.txt)'
        if ((Invoke-Python @('-m', 'pip', 'install', '-r', (Join-Path $Root 'requirements.txt'))) -ne 0) { throw 'pip could not install the packages (see above).' }
        if (-not (Test-PythonPackages)) { throw 'The packages are still missing after pip install.' }
    }
}

# ---- step 3: the compiler ---------------------------------------------------------------------------
function Test-Compiler {
    $r = Get-PythonOutput @((Join-Path $Root 'tools\build_game.py'), '--check')
    return @{ Ok = ($r.Code -eq 0); Output = $r.Output }
}

function Get-VsInstances {
    <# Visual Studio editions installed here, from vswhere: @{Path; Product}. #>
    $vswhere = Join-Path $VsInstallerDir 'vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { return @() }
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $json = & $vswhere -all -products '*' -format json 2>$null | Out-String
    $ErrorActionPreference = $old
    if (-not $json.Trim()) { return @() }
    $out = @()
    foreach ($i in ($json | ConvertFrom-Json)) { $out += @{ Path = $i.installationPath; Product = $i.productId } }
    return $out
}

function Step-Compiler {
    Write-Step '[3/9] The compiler (Visual Studio Build Tools with Clang)'
    $check = Test-Compiler
    if ($check.Ok) {
        foreach ($line in ($check.Output -split "`n")) { if ($line -match 'clang-cl|Python') { Write-Info $line.Trim() } }
        return
    }
    Write-Info ($check.Output -split "`n" | Where-Object { $_ -match 'ERROR' } | Select-Object -First 1)
    $instances = Get-VsInstances
    $setup = Join-Path $VsInstallerDir 'setup.exe'
    if ($instances.Count -gt 0 -and (Test-Path -LiteralPath $setup)) {
        $inst = $instances[0]
        $modify = Get-VsModifyArguments $inst.Path $inst.Product
        if ($DryRun) { Write-Would "add the C++ workload and Clang to the Visual Studio in $($inst.Path): setup.exe $($modify -join ' ')"; return }
        Write-Info "adding the C++ workload and Clang to the Visual Studio in $($inst.Path) (a progress window opens; this downloads up to a few GB)"
        $p = Start-Process -FilePath $setup -ArgumentList $modify -Wait -PassThru
        if ($p.ExitCode -notin @(0, 3010)) { throw "The Visual Studio installer stopped with code $($p.ExitCode). Open Visual Studio Installer, click Modify, tick 'Desktop development with C++' and 'C++ Clang tools for Windows', then run this again." }
    } else {
        $winget = Get-Command winget -ErrorAction SilentlyContinue
        if (-not $winget) {
            throw ("winget is not available, so the Build Tools cannot be installed from here. Download them from $VsBuildToolsUrl, " +
                   "tick 'Desktop development with C++' and, under Installation details, 'C++ Clang tools for Windows', install, then run this again.")
        }
        $install = Get-VsBuildToolsArguments $VsBuildToolsId
        if ($DryRun) { Write-Would "run: winget $($install -join ' ')"; return }
        Write-Info 'installing the Visual Studio Build Tools with the C++ workload and Clang through winget'
        Write-Info 'this downloads a few GB and takes a while; a progress window opens, and this window waits for it'
        & $winget.Source @install
        $code = $LASTEXITCODE
        if ($code -eq -1978335189) {   # already installed, but without what we need: add it
            $instances = Get-VsInstances
            if ($instances.Count -eq 0 -or -not (Test-Path -LiteralPath $setup)) { throw "winget says the Build Tools are installed, but they could not be found. Open Visual Studio Installer, click Modify, tick 'Desktop development with C++' and 'C++ Clang tools for Windows', then run this again." }
            $inst = $instances[0]
            $p = Start-Process -FilePath $setup -ArgumentList (Get-VsModifyArguments $inst.Path $inst.Product) -Wait -PassThru
            if ($p.ExitCode -notin @(0, 3010)) { throw "The Visual Studio installer stopped with code $($p.ExitCode)." }
        } elseif ($code -notin @(0, 3010)) {
            throw "winget stopped with code $code. Install the Build Tools by hand from $VsBuildToolsUrl (tick 'Desktop development with C++' and 'C++ Clang tools for Windows'), then run this again."
        }
    }
    $check = Test-Compiler
    if (-not $check.Ok) {
        throw ("The compiler still does not work after the install:`n" + $check.Output +
               "`nA restart may be needed. After it, run Install WindowSmash.bat again.")
    }
    foreach ($line in ($check.Output -split "`n")) { if ($line -match 'clang-cl') { Write-Info $line.Trim() } }
}

# ---- step 4: SDL3 ----------------------------------------------------------------------------------------
function Step-Sdl3 {
    Write-Step '[4/9] SDL3 (controllers other than Xbox pads)'
    $dir = Join-Path $Root 'deps\SDL3'
    if ((Test-Path -LiteralPath (Join-Path $dir 'include\SDL3\SDL_gamepad.h')) -and (Test-Path -LiteralPath (Join-Path $dir 'lib\x64\SDL3.dll'))) {
        $v = ''
        $h = Join-Path $dir 'include\SDL3\SDL_version.h'
        if (Test-Path -LiteralPath $h) {
            $t = [IO.File]::ReadAllText($h)
            $ma = [regex]::Match($t, '#define SDL_MAJOR_VERSION\s+(\d+)'); $mi = [regex]::Match($t, '#define SDL_MINOR_VERSION\s+(\d+)'); $mc = [regex]::Match($t, '#define SDL_MICRO_VERSION\s+(\d+)')
            if ($ma.Success -and $mi.Success -and $mc.Success) { $v = " $($ma.Groups[1].Value).$($mi.Groups[1].Value).$($mc.Groups[1].Value)" }
        }
        Write-Info "deps\SDL3: already there$v"
        return
    }
    $asset = $null
    try { $asset = Select-Sdl3Asset (Get-Json $SdlReleasesUrl) } catch { Write-Warn "GitHub's release list could not be read ($($_.Exception.Message))" }
    if (-not $asset) { $asset = @{ Version = [version]$SdlFallback.Version; Url = $SdlFallback.Url; Sha256 = $SdlFallback.Sha256; Name = (Split-Path -Leaf $SdlFallback.Url) } }
    if ($DryRun) { Write-Would "download SDL3 $($asset.Version) ($($asset.Url)) into deps\SDL3, checking its SHA-256 ($($asset.Sha256))"; return }
    $zip = Join-Path $Root "deps\$($asset.Name)"
    if (-not (Test-Path -LiteralPath $zip)) { Invoke-Download $asset.Url $zip "SDL3 $($asset.Version)" }
    if ($asset.Sha256) {
        if (-not (Test-FileSha256 $zip $asset.Sha256)) { Remove-Item -LiteralPath $zip -Force; throw "The downloaded $($asset.Name) does not match the SHA-256 the release lists. Run this again to download it afresh." }
        Write-Info 'SHA-256 matches the release'
    } else {
        Write-Warn 'the release lists no SHA-256 for it; taking the download as it is'
    }
    $extract = Join-Path $Root 'deps\SDL3-extract'
    if (Test-Path -LiteralPath $extract) { Remove-Item -LiteralPath $extract -Recurse -Force }
    Expand-Archive -LiteralPath $zip -DestinationPath $extract
    $inner = Get-ChildItem -LiteralPath $extract -Directory | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'include\SDL3\SDL_gamepad.h') } | Select-Object -First 1
    if (-not $inner) { throw "$($asset.Name) does not hold include\SDL3 as expected." }
    if (Test-Path -LiteralPath $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
    Move-Item -LiteralPath $inner.FullName -Destination $dir
    Remove-Item -LiteralPath $extract -Recurse -Force
    Remove-Item -LiteralPath $zip -Force
    Write-Info "deps\SDL3: SDL3 $($asset.Version)"
}

# ---- step 5: codecs ---------------------------------------------------------------------------------------
function Step-Codecs {
    Write-Step '[5/9] Music and movie codecs'
    $opus = Join-Path $Root 'third_party\opus\src\opus_decoder.c'
    $vpx = Join-Path $Root 'third_party\libvpx\vp9\vp9_dx_iface.c'
    if ((Test-Path -LiteralPath $opus) -and (Test-Path -LiteralPath $vpx)) {
        Write-Info 'libopus and libvpx are in third_party and are built during the game build: nothing to download'
    } else {
        Write-Warn 'third_party\opus or third_party\libvpx is missing: the game will build without music or movies (download WindowSmash again to get them)'
    }
}

# ---- step 6: the game files ------------------------------------------------------------------------------
function Get-SavedFolders {
    <# Last time's answers (dump\build_folders.txt, written by build_game.py). #>
    $saved = @{}
    $f = Join-Path $Root 'dump\build_folders.txt'
    if (Test-Path -LiteralPath $f) {
        foreach ($line in [IO.File]::ReadAllLines($f)) {
            $i = $line.IndexOf('=')
            if ($i -gt 0) { $saved[$line.Substring(0, $i)] = $line.Substring($i + 1) }
        }
    }
    return $saved
}

function Get-DetectedFolders {
    <# The folders to suggest: last time's, else the emulators', else nothing. #>
    $saved = Get-SavedFolders
    $c = Get-FolderCandidates $env:APPDATA $env:USERPROFILE
    $exists = { param($p) Test-Path -LiteralPath $p -PathType Container }
    $keys = $null; $firmware = $null; $game = $null
    if ($saved.ContainsKey('keys')) { $keys = $saved['keys'] } else { $keys = Resolve-FirstExisting ($c.Keys | Where-Object { Test-Path -LiteralPath (Join-Path $_ 'prod.keys') }) $exists }
    if ($saved.ContainsKey('firmware')) { $firmware = $saved['firmware'] } else { $firmware = Resolve-FirstExisting $c.Firmware $exists }
    if ($saved.ContainsKey('game')) { $game = $saved['game'] } else { $game = Resolve-FirstExisting $c.Game $exists }
    return @{ Game = $game; Keys = $keys; Firmware = $firmware }
}

function Test-KeysFolder {
    param([string]$Folder)
    if (-not $Folder -or -not (Test-Path -LiteralPath $Folder -PathType Container)) { return @{ Ok = $false; Text = 'Choose the folder with prod.keys and title.keys.' } }
    $prod = Test-Path -LiteralPath (Join-Path $Folder 'prod.keys')
    $title = Test-Path -LiteralPath (Join-Path $Folder 'title.keys')
    if (-not $prod) { return @{ Ok = $false; Text = 'There is no prod.keys here.' } }
    if (-not $title) { return @{ Ok = $true; Text = 'prod.keys found; no title.keys (needed for games bought from the eShop).' } }
    return @{ Ok = $true; Text = 'prod.keys and title.keys found.' }
}

function Test-FirmwareFolder {
    param([string]$Folder)
    if (-not $Folder -or -not (Test-Path -LiteralPath $Folder -PathType Container)) { return @{ Ok = $false; Text = 'Choose the folder with your firmware (a few hundred .nca files).' } }
    $n = @(Get-ChildItem -LiteralPath $Folder -Filter '*.nca' -ErrorAction SilentlyContinue).Count
    if ($n -eq 0) { return @{ Ok = $false; Text = 'There are no .nca files here.' } }
    return @{ Ok = $true; Text = "$n .nca files (the game's fonts are taken out of them)." }
}

function Test-GameFolder {
    <# What tools\gamefiles.py finds in the folder, with the keys. #>
    param([string]$Folder, [string]$KeysFolder)
    if (-not $Folder -or -not (Test-Path -LiteralPath $Folder -PathType Container)) { return @{ Ok = $false; Text = 'Choose the folder with your game files.' } }
    if (-not $KeysFolder -or -not (Test-Path -LiteralPath (Join-Path $KeysFolder 'prod.keys'))) { return @{ Ok = $false; Text = 'The keys folder is needed to read the game files.' } }
    $r = Get-PythonOutput @((Join-Path $Root 'tools\gamefiles.py'), $Folder, '--keys', $KeysFolder, '--json')
    try {
        $scan = $r.Output | ConvertFrom-Json
    } catch {
        return @{ Ok = $false; Text = ('Could not read the folder: ' + ($r.Output -split "`n" | Select-Object -Last 1)) }
    }
    $lines = @($scan.summary)
    if (Test-Path -LiteralPath (Join-Path $Folder 'romfs.bin')) { $lines += 'romfs.bin found (the game data, used as it is).' }
    if ($scan.program) { return @{ Ok = $true; Text = ($lines -join ' ') } }
    return @{ Ok = $false; Text = (($lines + @("Problem: $($scan.problem)")) -join ' ') }
}

function Show-Wizard {
    <# The setup window: three folders with what was found in each, the online name, the options.
       Returns @{Game; Keys; Firmware; OnlineName; Shaders; Shortcut; Firewall}, or $null if cancelled. #>
    param([hashtable]$Detected, [string]$DefaultName)
    Add-Type -AssemblyName System.Windows.Forms
    Add-Type -AssemblyName System.Drawing
    [Windows.Forms.Application]::EnableVisualStyles()

    $form = New-Object Windows.Forms.Form
    $form.Text = 'WindowSmash setup'
    $form.AutoScaleMode = [Windows.Forms.AutoScaleMode]::Dpi
    $form.AutoScaleDimensions = New-Object Drawing.SizeF(96, 96)
    $form.ClientSize = New-Object Drawing.Size(760, 560)
    $form.FormBorderStyle = [Windows.Forms.FormBorderStyle]::FixedDialog
    $form.MaximizeBox = $false
    $form.MinimizeBox = $false
    $form.StartPosition = [Windows.Forms.FormStartPosition]::CenterScreen
    $form.Font = New-Object Drawing.Font('Segoe UI', 9.75)
    $ico = Join-Path $Root 'docs\assets\windowsmash.ico'
    if (Test-Path -LiteralPath $ico) { $form.Icon = New-Object Drawing.Icon($ico) }

    $intro = New-Object Windows.Forms.Label
    $intro.Text = 'Where are your game files? Folders found on this PC are filled in; change any of them with Browse. Each line below a folder says what was found in it.'
    $intro.Location = New-Object Drawing.Point(16, 12)
    $intro.Size = New-Object Drawing.Size(728, 40)
    $form.Controls.Add($intro)

    $rows = @{}
    $y = 58
    foreach ($spec in @(
        @{ Key = 'Game'; Title = 'Game files'; Hint = "The game's .nca files (from yuzu, Ryujinx, or your own dump folder), with romfs.bin if you have one" },
        @{ Key = 'Keys'; Title = 'Keys'; Hint = "The folder with prod.keys and title.keys" },
        @{ Key = 'Firmware'; Title = 'Firmware'; Hint = "The folder with your Switch firmware's .nca files (for the fonts)" }
    )) {
        $title = New-Object Windows.Forms.Label
        $title.Text = "$($spec.Title):  $($spec.Hint)"
        $title.Location = New-Object Drawing.Point(16, $y)
        $title.Size = New-Object Drawing.Size(728, 20)
        $title.Font = New-Object Drawing.Font('Segoe UI', 9.75, [Drawing.FontStyle]::Bold)
        $form.Controls.Add($title)
        $box = New-Object Windows.Forms.TextBox
        $box.Location = New-Object Drawing.Point(16, ($y + 24))
        $box.Size = New-Object Drawing.Size(620, 26)
        $box.Text = [string]$Detected[$spec.Key]
        $form.Controls.Add($box)
        $browse = New-Object Windows.Forms.Button
        $browse.Text = 'Browse...'
        $browse.Location = New-Object Drawing.Point(644, ($y + 22))
        $browse.Size = New-Object Drawing.Size(100, 29)
        $form.Controls.Add($browse)
        $status = New-Object Windows.Forms.Label
        $status.Location = New-Object Drawing.Point(16, ($y + 54))
        $status.Size = New-Object Drawing.Size(728, 40)
        $status.ForeColor = [Drawing.Color]::DimGray
        $form.Controls.Add($status)
        $rows[$spec.Key] = @{ Box = $box; Status = $status; Browse = $browse }
        $y += 100
    }

    $nameLabel = New-Object Windows.Forms.Label
    $nameLabel.Text = 'Your online name (what other players see; kept in online_name.txt):'
    $nameLabel.Location = New-Object Drawing.Point(16, $y)
    $nameLabel.Size = New-Object Drawing.Size(500, 20)
    $form.Controls.Add($nameLabel)
    $nameBox = New-Object Windows.Forms.TextBox
    $nameBox.Location = New-Object Drawing.Point(520, ($y - 2))
    $nameBox.Size = New-Object Drawing.Size(224, 26)
    $nameBox.Text = $DefaultName
    $nameBox.MaxLength = 32
    $form.Controls.Add($nameBox)
    $y += 36

    $checks = @{}
    foreach ($spec in @(
        @{ Key = 'Shortcut'; Text = 'Put a shortcut to Play SSBU.bat on the desktop'; On = $true },
        @{ Key = 'Firewall'; Text = 'Allow the game through Windows Firewall now (asks for administrator rights once; needed for playing online)'; On = $true },
        @{ Key = 'Shaders'; Text = 'After the build, prepare the shaders in the background (minutes to two hours at low priority; stops the first-time stutter of effects)'; On = $true }
    )) {
        $cb = New-Object Windows.Forms.CheckBox
        $cb.Text = $spec.Text
        $cb.Checked = $spec.On
        $cb.Location = New-Object Drawing.Point(16, $y)
        $cb.Size = New-Object Drawing.Size(728, 24)
        $form.Controls.Add($cb)
        $checks[$spec.Key] = $cb
        $y += 28
    }

    $note = New-Object Windows.Forms.Label
    $note.Text = 'Then the game is built, which takes 30 minutes to a few hours. If it stops, run Install WindowSmash.bat again: it carries on where it stopped.'
    $note.Location = New-Object Drawing.Point(16, ($y + 4))
    $note.Size = New-Object Drawing.Size(728, 40)
    $form.Controls.Add($note)

    $check = New-Object Windows.Forms.Button
    $check.Text = 'Check files'
    $check.Location = New-Object Drawing.Point(16, 516)
    $check.Size = New-Object Drawing.Size(120, 32)
    $form.Controls.Add($check)
    $ok = New-Object Windows.Forms.Button
    $ok.Text = 'Set up and build'
    $ok.Location = New-Object Drawing.Point(488, 516)
    $ok.Size = New-Object Drawing.Size(150, 32)
    $form.Controls.Add($ok)
    $cancel = New-Object Windows.Forms.Button
    $cancel.Text = 'Cancel'
    $cancel.Location = New-Object Drawing.Point(644, 516)
    $cancel.Size = New-Object Drawing.Size(100, 32)
    $cancel.DialogResult = [Windows.Forms.DialogResult]::Cancel
    $form.Controls.Add($cancel)
    $form.CancelButton = $cancel

    $results = @{}
    $refresh = {
        foreach ($k in @('Keys', 'Firmware', 'Game')) { $rows[$k].Status.Text = 'Checking...' }
        $form.Refresh()
        $results['Keys'] = Test-KeysFolder $rows['Keys'].Box.Text.Trim().Trim('"')
        $results['Firmware'] = Test-FirmwareFolder $rows['Firmware'].Box.Text.Trim().Trim('"')
        $results['Game'] = Test-GameFolder $rows['Game'].Box.Text.Trim().Trim('"') $rows['Keys'].Box.Text.Trim().Trim('"')
        foreach ($k in @('Keys', 'Firmware', 'Game')) {
            $rows[$k].Status.Text = $results[$k].Text
            if ($results[$k].Ok) { $rows[$k].Status.ForeColor = [Drawing.Color]::DarkGreen } else { $rows[$k].Status.ForeColor = [Drawing.Color]::Firebrick }
        }
    }
    foreach ($k in @('Game', 'Keys', 'Firmware')) {
        $row = $rows[$k]
        $row.Browse.Add_Click({
            $dlg = New-Object Windows.Forms.FolderBrowserDialog
            $dlg.Description = "Choose the folder"
            if ($row.Box.Text -and (Test-Path -LiteralPath $row.Box.Text)) { $dlg.SelectedPath = $row.Box.Text }
            if ($dlg.ShowDialog($form) -eq [Windows.Forms.DialogResult]::OK) { $row.Box.Text = $dlg.SelectedPath; & $refresh }
        }.GetNewClosure())
    }
    $check.Add_Click({ & $refresh })
    $ok.Add_Click({
        & $refresh
        $bad = @()
        foreach ($k in @('Game', 'Keys', 'Firmware')) { if (-not $results[$k].Ok) { $bad += $k } }
        if ($bad.Count -gt 0) {
            [Windows.Forms.MessageBox]::Show($form, "Please fix: $($bad -join ', ') (see the line under each folder).", 'WindowSmash setup', 'OK', 'Warning') | Out-Null
            return
        }
        if (-not $nameBox.Text.Trim()) { $nameBox.Text = $DefaultName }
        $form.DialogResult = [Windows.Forms.DialogResult]::OK
        $form.Close()
    })
    $form.Add_Shown({ $form.Activate(); & $refresh })
    if ($Script:WizardSnapshot) {   # (for checking the window without a person: save a picture of it, then accept it)
        $timer = New-Object Windows.Forms.Timer
        $timer.Interval = 1500
        $timer.Add_Tick({
            $timer.Stop()
            $bmp = New-Object Drawing.Bitmap($form.Width, $form.Height)
            $form.DrawToBitmap($bmp, (New-Object Drawing.Rectangle(0, 0, $form.Width, $form.Height)))
            $bmp.Save($Script:WizardSnapshot, [Drawing.Imaging.ImageFormat]::Png)
            $form.DialogResult = [Windows.Forms.DialogResult]::OK
            $form.Close()
        })
        $timer.Start()
    }

    if ($form.ShowDialog() -ne [Windows.Forms.DialogResult]::OK) { return $null }
    return @{
        Game = $rows['Game'].Box.Text.Trim().Trim('"'); Keys = $rows['Keys'].Box.Text.Trim().Trim('"'); Firmware = $rows['Firmware'].Box.Text.Trim().Trim('"')
        OnlineName = $nameBox.Text.Trim(); Shortcut = $checks['Shortcut'].Checked; Firewall = $checks['Firewall'].Checked; Shaders = $checks['Shaders'].Checked
    }
}

function Read-Answers {
    <# The same questions in this window, for -NoWizard. #>
    param([hashtable]$Detected, [string]$DefaultName)
    $answers = @{}
    foreach ($spec in @(@{ Key = 'Game'; Q = 'Folder with your game files' }, @{ Key = 'Keys'; Q = 'Folder with prod.keys and title.keys' }, @{ Key = 'Firmware'; Q = "Folder with your firmware's .nca files" })) {
        $default = [string]$Detected[$spec.Key]
        while ($true) {
            $prompt = $spec.Q
            if ($default) { $prompt += " [$default]" }
            $a = (Read-Host $prompt).Trim().Trim('"')
            if (-not $a) { $a = $default }
            $r = switch ($spec.Key) { 'Keys' { Test-KeysFolder $a } 'Firmware' { Test-FirmwareFolder $a } default { Test-GameFolder $a $answers['Keys'] } }
            Write-Host "  $($r.Text)"
            if ($r.Ok) { $answers[$spec.Key] = $a; break }
        }
    }
    $n = (Read-Host "Your online name [$DefaultName]").Trim()
    if (-not $n) { $n = $DefaultName }
    $answers['OnlineName'] = $n
    $answers['Shortcut'] = ((Read-Host 'Put a shortcut on the desktop? [Y/n]').Trim() -notmatch '^n')
    $answers['Firewall'] = ((Read-Host 'Allow the game through Windows Firewall (asks for administrator rights)? [Y/n]').Trim() -notmatch '^n')
    $answers['Shaders'] = ((Read-Host 'Prepare the shaders after the build (minutes to two hours, low priority)? [Y/n]').Trim() -notmatch '^n')
    return $answers
}

function Step-GameFiles {
    Write-Step '[6/9] Your game files'
    $detected = Get-DetectedFolders
    if ($Game) { $detected.Game = $Game }
    if ($Keys) { $detected.Keys = $Keys }
    if ($Firmware) { $detected.Firmware = $Firmware }
    foreach ($k in @('Game', 'Keys', 'Firmware')) {
        if ($detected[$k]) { Write-Info "$($k.ToLower()): $($detected[$k])" } else { Write-Info "$($k.ToLower()): not found on this PC (to be chosen)" }
    }
    $nameFile = Join-Path $Root 'online_name.txt'
    $default = $OnlineName
    if (-not $default -and (Test-Path -LiteralPath $nameFile)) { $default = ([IO.File]::ReadAllText($nameFile)).Trim() }
    if (-not $default) { $default = 'Player' + (Get-Random -Minimum 1000 -Maximum 9999) }
    if ($DryRun) {
        if ($detected.Game -and $detected.Keys) { Write-Info ('game folder check: ' + (Test-GameFolder $detected.Game $detected.Keys).Text) }
        Write-Would "open the setup window with these folders and the online name '$default', then build with the answers"
        return @{ Game = $detected.Game; Keys = $detected.Keys; Firmware = $detected.Firmware; OnlineName = $default; Shortcut = $true; Firewall = $false; Shaders = $false }
    }
    $answers = $null
    if (-not $NoWizard) {
        try { $answers = Show-Wizard $detected $default } catch { Write-Warn "the setup window could not open ($($_.Exception.Message)); asking here instead"; $NoWizard = $true }
        if (-not $NoWizard -and -not $answers) { throw 'Cancelled. Run Install WindowSmash.bat again when you are ready.' }
    }
    if ($NoWizard) { $answers = Read-Answers $detected $default }
    if (($answers.OnlineName -and $answers.OnlineName -ne $default) -or -not (Test-Path -LiteralPath $nameFile)) {
        [IO.File]::WriteAllText($nameFile, $answers.OnlineName + "`r`n")
    }
    Write-Info "online name: $($answers.OnlineName)"
    return $answers
}

# ---- step 7: shortcut and firewall -----------------------------------------------------------------------
function Get-DesktopFolder {
    return [Environment]::GetFolderPath('Desktop')
}

function Step-Extras {
    param([hashtable]$Answers)
    Write-Step '[7/9] Shortcut and firewall'
    $lnk = Join-Path (Get-DesktopFolder) 'Play SSBU.lnk'
    if (-not $Answers.Shortcut) {
        Write-Info 'shortcut: not wanted'
    } elseif (Test-Path -LiteralPath $lnk) {
        Write-Info "shortcut: already on the desktop"
    } elseif ($DryRun) {
        Write-Would "put $lnk on the desktop, pointing at Play SSBU.bat"
    } else {
        $ws = New-Object -ComObject WScript.Shell
        $s = $ws.CreateShortcut($lnk)
        $s.TargetPath = Join-Path $Root 'Play SSBU.bat'
        $s.WorkingDirectory = $Root
        $s.IconLocation = (Join-Path $Root 'docs\assets\windowsmash.ico') + ',0'
        $s.Description = 'Super Smash Bros. Ultimate (WindowSmash)'
        $s.Save()
        Write-Info "shortcut: $lnk"
    }
    $exe = Join-Path $Root 'build\ssbu.exe'
    if (-not $Answers.Firewall) {
        Write-Info 'firewall rule: not wanted (Windows will ask the first time the game goes online)'
    } elseif (Get-NetFirewallRule -DisplayName $FirewallRuleName -ErrorAction SilentlyContinue) {
        Write-Info "firewall rule: already there ($FirewallRuleName)"
    } elseif ($DryRun) {
        Write-Would "add the firewall rule '$FirewallRuleName' for $exe, with administrator rights"
    } else {
        $cmd = Get-FirewallRuleCommand $exe $FirewallRuleName
        Write-Info 'adding the firewall rule: Windows now asks for administrator rights'
        try {
            $p = Start-Process -FilePath 'powershell.exe' -Verb RunAs -Wait -PassThru -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', $cmd)
            if ($p.ExitCode -eq 0) { Write-Info "firewall rule: $FirewallRuleName" } else { Write-Warn "the firewall rule could not be added (code $($p.ExitCode)); Windows will ask the first time the game goes online" }
        } catch {
            Write-Warn 'no administrator rights given: Windows will ask the first time the game goes online'
        }
    }
}

# ---- step 8: the build ---------------------------------------------------------------------------------------
function Step-Build {
    param([hashtable]$Answers)
    Write-Step '[8/9] Build the game'
    $build = @((Join-Path $Root 'tools\build_game.py'), '--game', $Answers.Game, '--keys', $Answers.Keys, '--firmware', $Answers.Firmware)
    if ($Jobs) { $build += @('--jobs', "$Jobs") }
    if ($SkipBuild) { Write-Info '-SkipBuild: not building'; return $false }
    if ($DryRun) { Write-Would ('run: ' + ($Script:Python -join ' ') + ' ' + ($build -join ' ')); return $false }
    Write-Info 'the program, fonts, data, DLC and fighters, then the build: 30 minutes to a few hours'
    if ((Invoke-Python $build) -ne 0) { throw 'The build stopped (the message above says why). Fix that, then run Install WindowSmash.bat again: it carries on where it stopped.' }
    return $true
}

# ---- step 9: shaders --------------------------------------------------------------------------------------------
function Step-Shaders {
    param([hashtable]$Answers, [bool]$Built)
    Write-Step '[9/9] Shaders'
    if (-not $Answers.Shaders) { Write-Info 'not wanted (Prepare Shaders.bat does it any time)'; return }
    if ($DryRun -or -not $Built) { Write-Would 'start Prepare Shaders.bat in a window of its own'; return }
    Start-Process -FilePath (Join-Path $Root 'Prepare Shaders.bat') -WorkingDirectory $Root
    Write-Info 'Prepare Shaders.bat runs in its own window at low priority; you can play meanwhile, and close it any time'
}

function Main {
    $log = Join-Path $Root 'install.log'
    try { Start-Transcript -Path $log -Append | Out-Null } catch { }
    try {
        Write-Host 'WindowSmash setup' -ForegroundColor Cyan
        Write-Host "  $Root"
        if ($DryRun) { Write-Host '  dry run: saying what each step would do, doing nothing' -ForegroundColor DarkGray }
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
        Step-CheckPc
        Step-Python
        if (-not $Script:Python) { Write-Host ''; Write-Host '(dry run) the later steps need Python; stopping here' -ForegroundColor DarkGray; return 0 }
        Step-Compiler
        Step-Sdl3
        Step-Codecs
        $answers = Step-GameFiles
        Step-Extras $answers
        $built = Step-Build $answers
        Step-Shaders $answers $built
        Write-Host ''
        if ($built) {
            Write-Host 'Done. Start the game with Play SSBU.bat, or the shortcut on the desktop.' -ForegroundColor Green
        } else {
            Write-Host 'Setup done.' -ForegroundColor Green
        }
        return 0
    } catch {
        Write-Host ''
        Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
        if ($_.ScriptStackTrace) { Write-Host $_.ScriptStackTrace -ForegroundColor DarkGray }
        Write-Host "(everything above is also in $log)"
        return 1
    } finally {
        try { Stop-Transcript | Out-Null } catch { }
    }
}

if ($MyInvocation.InvocationName -ne '.') {
    exit (Main)
}
