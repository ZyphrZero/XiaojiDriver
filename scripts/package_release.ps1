param(
    [string]$DriverDir = "$PSScriptRoot\..\x64\driver",
    [Parameter(Mandatory)][string]$OutputDir,
    [string]$Tag
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
$name = "xiaoji-driver-$version-win-x64-unsigned.zip"
$archive = Join-Path $output $name
$checksum = Join-Path $output 'SHA256SUMS.txt'
if ((Test-Path -LiteralPath $archive) -or (Test-Path -LiteralPath $checksum)) { throw "Release files already exist in $output." }
$notices = 'LICENSE', 'third_party/HIDMaestro/LICENSE', 'third_party/HIDMaestro/NOTICE.md', 'third_party/HIDMaestro/UPSTREAM.txt'
$tar = Join-Path ([Environment]::SystemDirectory) 'tar.exe'
& $tar -a -cf $archive -C (Resolve-Path -LiteralPath $DriverDir).Path @driverFiles -C $root @notices
if ($LASTEXITCODE -ne 0) { throw "Could not create release archive (tar exit $LASTEXITCODE)." }
$entries = @(& $tar -tf $archive)
if ($LASTEXITCODE -ne 0 -or (Compare-Object ($driverFiles + $notices) $entries -CaseSensitive)) { throw 'Unexpected release archive contents.' }
$hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText($checksum, "$hash  $name`n", [Text.Encoding]::ASCII)
Write-Output "$name SHA256 $hash"
