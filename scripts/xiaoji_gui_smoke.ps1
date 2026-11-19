param([string]$GuiExe = "$PSScriptRoot\..\build\xiaoji-gui.exe", [switch]$VerifyInstalled)
$ErrorActionPreference = 'Stop'
if ([Threading.Thread]::CurrentThread.ApartmentState -ne 'STA') { throw 'Run with powershell.exe -STA' }
Add-Type -AssemblyName System.Windows.Forms
$path = (Get-Item -LiteralPath $GuiExe).FullName
$bytes = [IO.File]::ReadAllBytes($path)
$pe = [BitConverter]::ToInt32($bytes, 60)
if ([BitConverter]::ToUInt16($bytes, $pe + 24 + 68) -ne 2) { throw 'Expected a Windows GUI subsystem executable' }
$assembly = [Reflection.Assembly]::LoadFrom($path)
if ($assembly.GetManifestResourceNames() -notcontains 'xiaoji-host.exe') { throw 'Missing embedded input host' }
$oldOut = [Console]::Out
$oldError = [Console]::Error
if ($assembly.EntryPoint.DeclaringType.Name -ne 'DriverWindow') { throw 'Expected the driver manager entry point' }
$form = [Activator]::CreateInstance($assembly.EntryPoint.DeclaringType)
try {
    $form.Opacity = 0
    $form.ShowInTaskbar = $false
    $form.Show()
    $form.CreateControl()
    $status = $form.Controls.Find('Status', $true)[0]
    $log = $form.Controls.Find('Log', $true)[0]
    $initial = $status.Text
    foreach ($name in 'Offline','Devices','Verify','Install','Uninstall','Keyboard','Mouse','RepeatStart') {
        $button = $form.Controls.Find($name, $true)[0]
        if (!$button.Visible -or !$button.Enabled -or !$button.Text) { throw "Missing or disabled GUI action: $name" }
    }
    $key = $form.Controls.Find('RepeatKey', $true)[0]
    $count = $form.Controls.Find('RepeatCount', $true)[0]
    $gap = $form.Controls.Find('RepeatGap', $true)[0]
    $stop = $form.Controls.Find('RepeatStop', $true)[0]
    if ($key.SelectedItem -ne 'X' -or $key.Items.Count -ne 37 -or $count.Value -ne 10 -or $gap.Value -ne 100 -or $stop.Enabled) { throw 'Incorrect idle repeat controls' }
    if ($count.Minimum -ne 1 -or $count.Maximum -ne 1000 -or $gap.Minimum -ne 10 -or $gap.Maximum -ne 5000) { throw 'Invalid repeat bounds' }
    $signingState = 'HKLM:\SOFTWARE\XiaojiUmdf2LocalSigning'
    $hadSigner = Test-Path -LiteralPath $signingState
    if ($hadSigner) {
        # No actual published OEM INF can match this value. Exercise rejection without sending input.
        $form.GetType().GetField('installedNames','Instance,NonPublic').SetValue($form,'never-a-published-package')
    }
    $checks = @('Offline','Devices','Keyboard','Mouse','Keyboard','RepeatStart')
    if ($VerifyInstalled) { $checks += 'Verify' }
    foreach ($name in $checks) {
        $action = $form.Controls.Find($name, $true)[0]
        $previousLog = $log.Text
        $action.PerformClick()
        $deadline = [datetime]::UtcNow.AddSeconds(45)
        do {
            if ([datetime]::UtcNow -gt $deadline) { throw "GUI action timed out: $name" }
            Start-Sleep -Milliseconds 20
            [Windows.Forms.Application]::DoEvents()
        } until ($action.Enabled)
        [Windows.Forms.Application]::DoEvents()
        if ($log.Text -eq $previousLog -or $status.Text -eq $initial) { throw "GUI results were not displayed: $name" }
        if ($name -eq 'Offline' -and $log.Text -notmatch 'Catalog generation complete|xiaoji\.cat') { throw 'Missing real Inf2Cat output' }
        if ($name -eq 'Offline' -and !$status.Text.StartsWith(([string][char]0x901a + [char]0x8fc7))) { throw $log.Text }
        if ($name -eq 'Verify' -and ($log.Text -notmatch 'Driver Store.*oem[0-9]+\.inf' -or
            $status.Text -eq (-join ([char[]](0x672a,0x901a,0x8fc7))))) { throw $log.Text }
        if ($name -in 'Keyboard','Mouse','RepeatStart') {
            if ($status.Text -ne (-join ([char[]](0x672a,0x901a,0x8fc7)))) { throw "Uninstalled input did not fail: $name" }
            if ((Test-Path -LiteralPath $signingState) -ne $hadSigner) { throw 'Input changed the signing state' }
            if ([Windows.Forms.Application]::OpenForms.Count -ne 1) { throw 'Input displayed another window' }
        }
        if ($name -eq 'RepeatStart' -and ($stop.Enabled -or !$key.Enabled)) { throw 'Repeat controls did not recover after failure' }
        if (!$log.ReadOnly) { throw 'Diagnostic log must be read-only' }
    }
    if ($form.GetType().GetField('admin','Instance,NonPublic').GetValue($form)) {
        $start = $form.Controls.Find('RepeatStart', $true)[0]
        $start.PerformClick()
        if (!$stop.Enabled -or $start.Enabled -or $key.Enabled) { throw 'Repeat did not enter its active state' }
        $stop.PerformClick()
        if ($stop.Enabled) { throw 'Stop did not signal cancellation' }
        $deadline = [datetime]::UtcNow.AddSeconds(15)
        while (!$start.Enabled) {
            [Windows.Forms.Application]::DoEvents()
            if ([datetime]::UtcNow -gt $deadline) { throw 'Cancellation did not finish' }
            Start-Sleep -Milliseconds 10
        }
        if (!$key.Enabled -or $stop.Enabled) { throw 'Repeat did not return to idle' }
    }
} finally {
    $form.Dispose()
    [Console]::SetOut($oldOut)
    [Console]::SetError($oldError)
}
'GUI smoke passed: offline, device query, install/uninstall controls and repeated nonmodal rejection without real input.'
