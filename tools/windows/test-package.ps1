# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
# CI fixture only: a SYSTEM bootstrap creates a disposable standard user. The
# application itself is never elevated and retains its normal containment checks.
param(
    [Parameter(Mandatory=$true)][string]$Zip,
    [Parameter(Mandatory=$true)][string]$Msi,
    [Parameter(Mandatory=$true)][string]$BuildExecutable,
    [Parameter(Mandatory=$true)][ValidateSet('x64','arm64')][string]$Architecture,
    [Parameter(Mandatory=$true)][string]$Version,
    [Parameter(Mandatory=$true)][string]$TemporaryRoot,
    [Parameter(Mandatory=$true)][string]$LifecycleScript
)
$ErrorActionPreference='Stop'
foreach($inputPath in @($Zip,$Msi,$BuildExecutable,$TemporaryRoot,$LifecycleScript)) {
    if(-not [IO.Path]::IsPathRooted($inputPath) -or -not (Test-Path -LiteralPath $inputPath)) {
        throw "An existing absolute input is required: $inputPath"
    }
}
$identity=[Security.Principal.WindowsIdentity]::GetCurrent()
$principal=New-Object Security.Principal.WindowsPrincipal($identity)
if(-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Creating the disposable CI account requires an administrator fixture host'
}
$scope=Join-Path (Resolve-Path -LiteralPath $TemporaryRoot).Path ('package-host-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scope,"$scope/t" | Out-Null
$env:TEMP="$scope/t"; $env:TMP=$env:TEMP; $env:TMPDIR=$env:TEMP
@{zip=(Resolve-Path -LiteralPath $Zip).Path; msi=(Resolve-Path -LiteralPath $Msi).Path;
  executable=(Resolve-Path -LiteralPath $BuildExecutable).Path; architecture=$Architecture;
  version=$Version; lifecycle=(Resolve-Path -LiteralPath $LifecycleScript).Path} |
    ConvertTo-Json | Out-File "$scope/request.json" -Encoding utf8
$shell=Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
$hostScript=Join-Path $PSScriptRoot 'package-profile-host.ps1'
foreach($script in @('check-package.ps1','package-profile-host.ps1','package-profile-worker.ps1')) {
    $tokens=$null; $errors=$null
    [void][Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot $script),[ref]$tokens,[ref]$errors)
    if($errors.Count -ne 0) { throw "Package fixture parse errors in ${script}: $errors" }
}
$arguments="-NoProfile -ExecutionPolicy Bypass -File `"$hostScript`" -Scope `"$scope`""
$name='ckmux-package-test-'+[Guid]::NewGuid().ToString('N')
$registered=$false
$record=[ordered]@{scope=$scope; task=$name; start=[DateTime]::UtcNow.ToString('o'); exit=1}
try {
    $action=New-ScheduledTaskAction -Execute $shell -Argument $arguments -WorkingDirectory $scope
    $taskPrincipal=New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount
    $settings=New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 14) `
        -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
    Register-ScheduledTask -TaskName $name -Action $action -Principal $taskPrincipal -Settings $settings | Out-Null
    $registered=$true
    Start-ScheduledTask -TaskName $name
    $deadline=[DateTime]::UtcNow.AddMinutes(14)
    while(-not (Test-Path -LiteralPath "$scope/host-record.json") -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 200
        $info=Get-ScheduledTaskInfo -TaskName $name
        if($info.LastRunTime.ToUniversalTime() -ge [DateTime]::Parse($record.start) -and
            (Get-ScheduledTask -TaskName $name).State -ne 'Running' -and
            [DateTime]::UtcNow -gt [DateTime]::Parse($record.start).AddSeconds(3) -and
            -not (Test-Path -LiteralPath "$scope/host-record.json")) {
            throw "Package host exited without its acceptance record: $($info.LastTaskResult)"
        }
    }
    if(-not (Test-Path -LiteralPath "$scope/host-record.json")) { throw 'Disposable package host did not finish' }
    $actual=Get-Content -LiteralPath "$scope/host-record.json" -Raw | ConvertFrom-Json
    Get-Content -LiteralPath "$scope/host-record.json"
    if($actual.exit -ne 0 -or -not $actual.account_removed -or -not $actual.profile_removed -or
        -not $actual.owned_removed -or $actual.worker_exit -ne 0) {
        throw 'Full package acceptance or disposable fixture cleanup failed'
    }
    $record.exit=0
} finally {
    if($registered) {
        if((Get-ScheduledTask -TaskName $name).State -eq 'Running') { Stop-ScheduledTask -TaskName $name }
        Unregister-ScheduledTask -TaskName $name -Confirm:$false
        if(Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue) { throw 'Owned package task was not removed' }
    }
    $record.end=[DateTime]::UtcNow.ToString('o')
    $record | ConvertTo-Json | Out-File "$scope/dispatch-record.json" -Encoding utf8
    $record | ConvertTo-Json
}
