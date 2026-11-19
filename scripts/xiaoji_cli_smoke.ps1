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
try {
    Copy-Item -LiteralPath $binary.FullName -Destination $standalone
    Invoke-Check @('--help') 0
    Invoke-Check @('install') 1
    Invoke-Check @('uninstall', '--force') 1
    Invoke-Check @('status') @(0, 3)
    Invoke-Check @('verify') @(0, 3)
    Invoke-Check @('uninstall', '--dry-run') 0
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
    foreach ($file in 'xiaoji.cat','xiaoji.inf','xiaoji.dll','xiaoji.exe') {
        $path = Join-Path $sandbox.FullName $file
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
    }
    [IO.Directory]::Delete($sandbox.FullName)
}
'CLI smoke passed; no devices, driver packages or certificates were installed or removed.'
exit 0
