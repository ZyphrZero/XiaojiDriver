param([string]$HostExe = "$PSScriptRoot\..\build\xiaoji-host.exe")
$ErrorActionPreference = 'Stop'

function Get-TrustSnapshot {
    foreach ($store in 'My','Root','TrustedPublisher') {
        Get-ChildItem -LiteralPath "Cert:\LocalMachine\$store" |
            Where-Object Subject -eq 'CN=xiaoji UMDF2 Local Test' |
            Sort-Object Thumbprint | ForEach-Object { "$store/$($_.Thumbprint)" }
    }
    $key = 'HKLM:\SOFTWARE\XiaojiUmdf2LocalSigning'
    if (Test-Path -LiteralPath $key) { (Get-ItemProperty -LiteralPath $key).Thumbprint }
}

$before = @(Get-TrustSnapshot) -join ';'
$binaryDir = (Get-Item -LiteralPath $HostExe).Directory.FullName
$dir = New-Item -ItemType Directory -Path (Join-Path ([IO.Path]::GetTempPath()) ([guid]::NewGuid().ToString('N')))
$exe = Join-Path $dir.FullName 'xiaoji-host.exe'
$tester = Join-Path $dir.FullName 'xiaoji-test-cli.exe'
try {
    Copy-Item -LiteralPath $HostExe -Destination $exe
    Push-Location -LiteralPath $dir.FullName
    try {
        & $exe self-test
        if ($LASTEXITCODE -ne 0) { throw "host self-test exit=$LASTEXITCODE" }
        & $exe repeat-test
        if ($LASTEXITCODE -ne 0) { throw "private IPC repeat-test exit=$LASTEXITCODE" }
        $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
        if ($principal.IsInRole([Security.Principal.SecurityIdentifier]::new('S-1-5-32-544'))) {
            $token = [guid]::NewGuid().ToString('N')
            $cancel = [Threading.EventWaitHandle]::new($true, [Threading.EventResetMode]::ManualReset, "Local\XiaojiRepeatStop-$token")
            try {
                $result = & $exe repeat-key never-a-published-package 27 10 100 $token $PID
                if ($LASTEXITCODE -ne 0 -or $result -ne 'STOPPED 0') { throw 'Pre-cancelled repeat did not stop before device access' }
            } finally { $cancel.Dispose() }
        }
        & $exe install --check
        if ($LASTEXITCODE -ne 0) { throw "embedded package check exit=$LASTEXITCODE" }
        if ((Get-ChildItem -Force | Measure-Object).Count -ne 1) { throw 'Check wrote to the distribution directory' }
        Remove-Item -LiteralPath $exe
        Copy-Item -LiteralPath (Join-Path $binaryDir 'xiaoji-test-cli.exe') -Destination $tester
        & $tester
        if ($LASTEXITCODE -ne 0) { throw "standalone tester default exit=$LASTEXITCODE" }
        'CANCEL' | & $tester install-test
        if ($LASTEXITCODE -ne 2) { throw "noninteractive install-test exit=$LASTEXITCODE" }
        & $tester invalid-command
        if ($LASTEXITCODE -ne 2) { throw "unknown tester command exit=$LASTEXITCODE" }
        & $tester devices
        if ($LASTEXITCODE -notin 0,3) { throw "device query exit=$LASTEXITCODE" }
        if (-not (Test-Path -LiteralPath 'HKLM:\SOFTWARE\XiaojiUmdf2LocalSigning')) {
            & $tester verify
            if ($LASTEXITCODE -ne 1) { throw "missing signing state incorrectly accepted: exit=$LASTEXITCODE" }
        }
    } finally { Pop-Location }
} finally {
    if (Test-Path -LiteralPath $exe) { Remove-Item -LiteralPath $exe }
    if (Test-Path -LiteralPath $tester) { Remove-Item -LiteralPath $tester }
    [IO.Directory]::Delete($dir.FullName)
}
if ($before -cne (@(Get-TrustSnapshot) -join ';')) { throw 'Offline check changed machine signing state' }
'Standalone host and offline signing-package smoke test passed.'
