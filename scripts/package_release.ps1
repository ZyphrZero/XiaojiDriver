param(
    [string]$DriverDir = "$PSScriptRoot\..\x64\driver",
    [string]$SdkDir,
    [Parameter(Mandatory)][string]$OutputDir,
    [string]$Tag,
    [string]$CliDir
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$driverFiles = 'xiaoji.inf', 'xiaoji.dll', 'xiaoji.cat'
foreach ($name in $driverFiles) {
    if ((Get-Item -LiteralPath (Join-Path $DriverDir $name)).Length -le 0) { throw "Empty driver file: $name" }
}
$infVersion = @(Select-String -LiteralPath (Join-Path $DriverDir 'xiaoji.inf') -Pattern '^\s*DriverVer\s*=\s*[\d/]+\s*,\s*(\d+(?:\.\d+){3})\s*$')
if ($infVersion.Count -ne 1) { throw 'Expected exactly one four-part DriverVer in xiaoji.inf.' }
$version = $infVersion[0].Matches[0].Groups[1].Value
$dllVersion = (Get-Item -LiteralPath (Join-Path $DriverDir 'xiaoji.dll')).VersionInfo.FileVersionRaw.ToString()
if ($version -ne $dllVersion) { throw "INF version $version does not match DLL version $dllVersion." }
if ($Tag -and $Tag -cne "v$version") { throw "Expected tag v$version, got $Tag." }

$output = [IO.Directory]::CreateDirectory($OutputDir).FullName
$checksum = Join-Path $output 'SHA256SUMS.txt'
if (Test-Path -LiteralPath $checksum) { throw "Release checksum already exists in $output." }
$notices = 'LICENSE', 'third_party/HIDMaestro/LICENSE', 'third_party/HIDMaestro/NOTICE.md', 'third_party/HIDMaestro/UPSTREAM.txt'
$tar = Join-Path ([Environment]::SystemDirectory) 'tar.exe'
function Write-ReleaseArchive($name, $directory, $files, $repoFiles) {
    $archive = Join-Path $output $name
    if (Test-Path -LiteralPath $archive) { throw "Release archive already exists: $archive" }
    & $tar -a -cf $archive -C (Resolve-Path -LiteralPath $directory).Path @files -C $root @repoFiles
    if ($LASTEXITCODE -ne 0) { throw "Could not create $name (tar exit $LASTEXITCODE)." }
    $entries = @(& $tar -tf $archive)
    if ($LASTEXITCODE -ne 0 -or (Compare-Object ($files + $repoFiles) $entries -CaseSensitive)) { throw "Unexpected archive contents: $name" }
    $hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    return "$hash  $name"
}
$lines = @(Write-ReleaseArchive "xiaoji-driver-$version-win-x64-unsigned.zip" $DriverDir $driverFiles $notices)
if ($CliDir) {
    $lines += Write-ReleaseArchive "xiaoji-cli-$version-win-x64.zip" $CliDir @('xiaoji.exe') ($notices + 'docs/cli.md')
}
if ($SdkDir) {
    $sdkFiles = 'xiaoji_client.dll', 'xiaoji_client.lib', 'xiaoji_client.h', 'xiaoji.py'
    $sdkDocs = $notices + @('docs/developer.md', 'docs/cli.md', 'sdk/examples/status.c', 'sdk/examples/Status.cs')
    $sdkDocs += @('sdk/rust/Cargo.toml', 'sdk/rust/Cargo.lock', 'sdk/rust/build.rs', 'sdk/rust/src/lib.rs', 'sdk/rust/examples/status.rs')
    $lines += Write-ReleaseArchive "xiaoji-client-sdk-$version-win-x64.zip" $SdkDir $sdkFiles $sdkDocs
}
[IO.File]::WriteAllText($checksum, ($lines -join "`n") + "`n", [Text.Encoding]::ASCII)
$lines
