param(
    [Parameter(Mandatory)][string]$Tester,
    [Parameter(Mandatory)][ValidatePattern('^oem[0-9]+\.inf$')][string]$PublishedInf,
    [ValidateSet('Different', 'NoBuildCache')][string]$Expected = 'Different'
)
$ErrorActionPreference = 'Stop'
$assembly = [Reflection.Assembly]::LoadFrom((Get-Item -LiteralPath $Tester).FullName)
$setup = $assembly.GetType('LocalSetup', $true)
$store = $assembly.GetType('DriverStore', $true)
$probe = $assembly.GetType('XiaojiTest', $true)
$payloadType = $assembly.GetType('LocalSetup+EmbeddedPackage', $true)
$methods = [Reflection.BindingFlags]'Static,NonPublic'
$fields = [Reflection.BindingFlags]'Instance,NonPublic'
$state = 'HKLM:\SOFTWARE\XiaojiUmdf2LocalSigning'
$before = (Get-ItemProperty -LiteralPath $state).InstalledHashes -join ','
$summary = $probe.GetMethod('VerifyInstalled', $methods).Invoke($null, $null)
$suffix = if ($Expected -eq 'Different') { '与当前构建不同' } else { '未比对当前构建' }
if (!$summary.StartsWith('已安装') -or !$summary.EndsWith($suffix)) { throw "Unexpected verification result: $summary" }

$payload = [Activator]::CreateInstance($payloadType, $true)
try {
    $tools = $payloadType.GetField('Tools', $fields).GetValue($payload)
    $files = $payloadType.GetField('Package', $fields).GetValue($payload)
    $template = [IO.Path]::Combine($files, 'xiaoji.inf')
    $bound = $probe.GetMethod('VerifiedBindings', $methods).Invoke($null, @($tools, $files))
    if (!$bound.ContainsKey($PublishedInf)) { throw 'Expected OEM INF is not bound or did not pass signature verification' }
    $published = $store.GetMethod('FromBinding', $methods).Invoke($null, @($PublishedInf, $template))
    $storeFolder = [IO.Path]::GetDirectoryName($published.GetType().GetField('InfPath', $fields).GetValue($published))
    $resolve = $setup.GetMethod('ResolveImport', $methods)
    $exact = $resolve.Invoke($null, @($tools, $storeFolder))
    if (!$exact.GetType().GetField('IsExactBuild', $fields).GetValue($exact)) { throw 'Real Driver Store files must match themselves' }
    $cache = $setup.GetMethod('CheckLocalCache', $methods).Invoke($null, @($tools, $files, $true))
    if ($Expected -eq 'Different') {
        if (!$cache) { throw 'A real signed cache for the newer build is required to reproduce this case' }
        $reused = $resolve.Invoke($null, @($tools, $cache))
        $reusedType = $reused.GetType()
        if ($reusedType.GetField('IsExactBuild', $fields).GetValue($reused)) { throw 'Reused package falsely classified as exact' }
        $actual = $reusedType.GetField('Hashes', $fields).GetValue($reused)
        if (($actual -join ',') -ne ($bound[$PublishedInf] -join ',')) { throw 'Removal receipt did not come from the actual verified files' }
        if (!$probe.GetMethod('FindPackages', $methods).Invoke($null, [object[]]@(,$actual)).Contains($PublishedInf)) {
            throw 'Exact removal lookup could not recover the real published package'
        }
    } elseif ($cache) { throw 'Expected this build not to have a signed cache' }

    $sameFamily = $store.GetMethod('SameFamily', $methods)
    $templateText = [IO.File]::ReadAllText($template)
    $updatedInf = [IO.Path]::Combine($files, 'version-only.inf')
    $unrelatedInf = [IO.Path]::Combine($files, 'different-service.inf')
    $version = 2,0,0,1 -join '.'
    [IO.File]::WriteAllText($updatedInf, ($templateText -replace '(?im)^DriverVer[ \t]*=.*$', "DriverVer=11/19/2026,$version"))
    [IO.File]::WriteAllText($unrelatedInf, ($templateText -replace '(?im)^UmdfService\s*=.*$', 'UmdfService=other,other_Service'))
    if (!$sameFamily.Invoke($null, @($updatedInf, $template)) -or $sameFamily.Invoke($null, @($unrelatedInf, $template))) {
        throw 'INF family detection must allow DriverVer changes, but reject a different UMDF service'
    }
    $unpublished = [string[]]$bound[$PublishedInf].Clone()
    $unpublished[0] = $setup.GetMethod('FileHash', $methods).Invoke($null, @($unrelatedInf))
    try {
        $store.GetMethod('ResolveStaged', $methods).Invoke($null, [object[]]@(,$unpublished)) | Out-Null
        throw 'Missing driver was accepted as staged'
    } catch {
        if ($_.Exception.InnerException -isnot [IO.IOException]) { throw }
    }
} finally { $payload.Dispose() }
if ($before -ne ((Get-ItemProperty -LiteralPath $state).InstalledHashes -join ',')) { throw 'Read-only check changed the install receipt' }
"Installed verification passed: $PublishedInf; $summary. No installation, uninstallation, or input was performed."
