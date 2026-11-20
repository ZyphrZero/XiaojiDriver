param(
    [string]$Cli = "$PSScriptRoot\..\x64\cli\xiaoji.exe",
    [string]$UnsignedInf,
    [string]$SignedInf
)
$ErrorActionPreference = 'Stop'
$binary = Get-Item -LiteralPath $Cli
if ($binary.Length -gt 1MB) { throw 'CLI exceeded 1 MiB; do not embed setup tools or the GUI.' }
$pe = [IO.File]::ReadAllBytes($binary.FullName)
if ([BitConverter]::ToUInt16($pe, [BitConverter]::ToInt32($pe, 60) + 24 + 68) -ne 3) { throw 'Expected a console executable.' }
$sandbox = New-Item -ItemType Directory -Path (Join-Path ([IO.Path]::GetTempPath()) ([Guid]::NewGuid().ToString('N')))
$standalone = Join-Path $sandbox.FullName 'xiaoji.exe'
function Invoke-Check([string[]]$Arguments, [int[]]$Expected) {
    & $standalone @Arguments
    if ($LASTEXITCODE -notin $Expected) { throw "Unexpected exit $LASTEXITCODE for $Arguments (expected $Expected)" }
}
function Read-JsonLine([string[]]$Arguments, [int]$ExitCode) {
    $errFile = Join-Path $sandbox.FullName 'stderr.txt'
    $lines = @(& $standalone @Arguments 2>$errFile)
    if ($LASTEXITCODE -ne $ExitCode -or $lines.Count -ne 1 -or (Get-Item -LiteralPath $errFile).Length) {
        throw "Expected one JSON line, silent stderr and exit $ExitCode for $Arguments; got $LASTEXITCODE : $lines"
    }
    return ($lines[0] | ConvertFrom-Json -ErrorAction Stop)
}
function Get-InstallSnapshot {
    foreach ($store in 'My','Root','TrustedPublisher') {
        Get-ChildItem -LiteralPath "Cert:\LocalMachine\$store" | Where-Object Subject -eq 'CN=xiaoji UMDF2 Local Test' |
            Sort-Object Thumbprint | ForEach-Object { "$store/$($_.Thumbprint)" }
    }
    $key = 'HKLM:\SOFTWARE\XiaojiUmdf2LocalSigning'
    if (Test-Path -LiteralPath $key) { (Get-ItemProperty -LiteralPath $key | Select-Object -Property * -ExcludeProperty PS* | ConvertTo-Json -Compress) }
    Get-ChildItem -LiteralPath "$env:windir\INF" -Filter 'oem*.inf' -Name | Sort-Object
    Get-ChildItem -LiteralPath $env:ProgramFiles -Filter 'xiaoji-umdf2-*' -Name | Sort-Object
    & $standalone status --json
    if ($LASTEXITCODE -ne 0) { throw 'Cannot snapshot fixed devices' }
}
try {
    Copy-Item -LiteralPath $binary.FullName -Destination $standalone
    Invoke-Check @('--help') 0
    Invoke-Check @('install') 1
    Invoke-Check @('uninstall', '--force') 1
    Invoke-Check @('status') @(0, 3)
    Invoke-Check @('verify') @(0, 3)
    Invoke-Check @('uninstall', '--dry-run') 0
    $status = Read-JsonLine @('status','--json') 0
    if ($status.type -ne 'status' -or $status.state -notin 'ready','not_installed','needs_upgrade','broken' -or
        $status.elevated -isnot [bool]) { throw 'Invalid machine status' }
    $before = @(Get-InstallSnapshot) -join "`n"
    $packageDir = if ($UnsignedInf) { (Get-Item -LiteralPath $UnsignedInf).Directory.FullName } else { $sandbox.FullName }
    $failure = Read-JsonLine @('install','--local-sign',$packageDir,'--json') 2
    if ($failure.type -ne 'error' -or $failure.step -ne 'verify' -or $failure.detail -notlike '*--trust-local-certificate*') { throw 'Missing consent not rejected' }
    if ($before -cne (@(Get-InstallSnapshot) -join "`n")) { throw 'Rejected install changed system state' }
    $principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
    if (!$status.elevated -and !$principal.IsInRole([Security.Principal.SecurityIdentifier]::new('S-1-5-32-544'))) {
        $failure = Read-JsonLine @('uninstall','--json') 2
        if ($failure.type -ne 'error' -or $failure.detail -notlike '*administrator*') { throw 'Expected elevation error' }
    } else { 'Skipped uninstall --json: elevated process.' }
    Invoke-Check @('verify', (Join-Path $sandbox.FullName 'xiaoji.inf')) 2
    if ($UnsignedInf) { Invoke-Check @('verify', $UnsignedInf) 2 }
    if ($SignedInf) {
        $package = (Get-Item -LiteralPath $SignedInf).Directory
        Invoke-Check @('verify', $SignedInf) 0
        foreach ($file in 'xiaoji.cat','xiaoji.inf','xiaoji.dll') {
            Copy-Item -LiteralPath (Join-Path $package.FullName $file) -Destination $sandbox.FullName
        }
        $inf = Join-Path $sandbox.FullName 'xiaoji.inf'
        [IO.File]::AppendAllText($inf, "`r`n; Changed after signing`r`n")
        Invoke-Check @('verify', $inf) 2
        Copy-Item -LiteralPath $SignedInf -Destination $inf -Force
        [IO.File]::WriteAllBytes((Join-Path $sandbox.FullName 'xiaoji.dll'), [byte[]](1, 2, 3))
        Invoke-Check @('verify', $inf) 2
    }
} finally {
    foreach ($file in 'xiaoji.cat','xiaoji.inf','xiaoji.dll','xiaoji.exe','stderr.txt') {
        $path = Join-Path $sandbox.FullName $file
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
    }
    [IO.Directory]::Delete($sandbox.FullName)
}
'CLI smoke passed; no devices, driver packages or certificates were installed or removed.'
exit 0
