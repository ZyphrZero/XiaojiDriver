param([Parameter(Mandatory)][string]$OutDir, [Parameter(Mandatory)][string]$SdkBin,
      [Parameter(Mandatory)][string]$WdkBin, [switch]$Gui)
$ErrorActionPreference = 'Stop'
$resources = @()
function EmbeddedFile($dir, $name) {
    $file = Get-Item -LiteralPath (Join-Path $dir $name) -ErrorAction Stop
    '/resource:{0},{1}' -f $file.FullName, $name
}
foreach ($name in 'xiaoji.dll','xiaoji.inf') { $resources += EmbeddedFile $OutDir $name }
foreach ($name in 'signtool.exe','signtool.exe.manifest','mssign32.dll','wintrust.dll','wintrust.dll.ini',
    'appxsip.dll','appxpackaging.dll','opcservices.dll','Microsoft.Windows.Build.Signing.mssign32.dll.manifest',
    'Microsoft.Windows.Build.Signing.wintrust.dll.manifest','Microsoft.Windows.Build.Appx.AppxSip.dll.manifest',
    'Microsoft.Windows.Build.Appx.AppxPackaging.dll.manifest','Microsoft.Windows.Build.Appx.OpcServices.dll.manifest') {
    $resources += EmbeddedFile $SdkBin $name
}
foreach ($name in 'Inf2Cat.exe','WindowsProtectedFiles.xml','Microsoft.UniversalStore.HardwareWorkflow.Cabinets.dll',
    'Microsoft.UniversalStore.HardwareWorkflow.Catalogs.dll','Microsoft.UniversalStore.HardwareWorkflow.InfReader.dll',
    'Microsoft.UniversalStore.HardwareWorkflow.SubmissionBuilder.dll') {
    $resources += EmbeddedFile $WdkBin $name
}
$compiler = "$env:SystemRoot\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
$flags = '/nologo','/platform:x64','/optimize+','/codepage:65001'
$repoRoot = Split-Path -Parent $PSScriptRoot
$setupSources = "$repoRoot\host\LocalSetup.cs", "$repoRoot\host\DriverStore.cs"
$sources = $setupSources + @("$repoRoot\tests\XiaojiTest.cs", "$repoRoot\tests\InputObserver.cs")
if ($Gui) {
    $resources += EmbeddedFile $OutDir 'xiaoji-host.exe'
    $entry = '/target:winexe','/main:TestWindow',"/out:$OutDir\xiaoji-test.exe"
    $sources += "$repoRoot\tests\TestWindow.cs"
} else {
    & $compiler @flags "/out:$OutDir\xiaoji-setup.exe" @resources @setupSources
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $entry = '/target:exe','/main:XiaojiTest',"/out:$OutDir\xiaoji-test-cli.exe"
}
& $compiler @flags @entry /reference:System.Windows.Forms.dll /reference:System.Drawing.dll @resources @sources
exit $LASTEXITCODE
