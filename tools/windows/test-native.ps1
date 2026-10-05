# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
param(
    [Parameter(Mandatory=$true)][string]$BuildDirectory,
    [Parameter(Mandatory=$true)][string]$TemporaryRoot,
    [string]$Configuration='Release',
    [string]$CTestPath='',
    [string]$TestRegex='',
    [switch]$Worker,
    [switch]$ForceScheduledHost
)
$ErrorActionPreference='Stop'
$BuildDirectory=(Resolve-Path -LiteralPath $BuildDirectory).Path
$TemporaryRoot=(Resolve-Path -LiteralPath $TemporaryRoot).Path
$env:TEMP=$TemporaryRoot; $env:TMP=$TemporaryRoot; $env:TMPDIR=$TemporaryRoot
if(!$CTestPath) { $CTestPath=(Get-Command ctest -ErrorAction Stop).Source }
$CTestPath=(Resolve-Path -LiteralPath $CTestPath).Path
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class CkmuxNativeTestHost {
    [DllImport("kernel32.dll", SetLastError=true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool IsProcessInJob(IntPtr process, IntPtr job,
        [MarshalAs(UnmanagedType.Bool)] out bool member);
}
'@
function In-Job {
    $member=$false
    if(![CkmuxNativeTestHost]::IsProcessInJob([Diagnostics.Process]::GetCurrentProcess().Handle,
        [IntPtr]::Zero,[ref]$member)) { throw 'Cannot query native test-host containment' }
    return $member
}
if($Worker) {
    $code=1
    try {
        $env:TEMP=$TemporaryRoot; $env:TMP=$TemporaryRoot; $env:TMPDIR=$TemporaryRoot
        $pathFile=Join-Path $TemporaryRoot 'worker-path.txt'
        if(Test-Path -LiteralPath $pathFile) { $env:PATH=[IO.File]::ReadAllText($pathFile) }
        # A scheduler may retain a permissive job. The actual native fixtures
        # verify their created child's independence before installing controlled
        # jobs; host membership alone does not establish breakaway eligibility.
        Write-Output "Native CTest worker: ambient_job=$(In-Job) scope=$TemporaryRoot"
        $arguments=@('--test-dir',$BuildDirectory,'-C',$Configuration,'--output-on-failure','-j2')
        if($TestRegex) { $arguments+=@('-R',$TestRegex) }
        $ErrorActionPreference='Continue'
        & $CTestPath @arguments *> (Join-Path $TemporaryRoot 'ctest.log')
        $code=$LASTEXITCODE
        $ErrorActionPreference='Stop'
    } catch {
        $_ | Out-File (Join-Path $TemporaryRoot 'host-error.log')
    } finally {
        [IO.File]::WriteAllText((Join-Path $TemporaryRoot 'exit.txt'),[string]$code)
    }
    exit $code
}

$contained=In-Job
$scope=Join-Path $TemporaryRoot ('native-test-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scope | Out-Null
$shell=Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
[IO.File]::WriteAllText((Join-Path $scope 'worker-path.txt'),$env:PATH)
$arguments="-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Worker -BuildDirectory `"$BuildDirectory`" -TemporaryRoot `"$scope`" -Configuration `"$Configuration`" -CTestPath `"$CTestPath`""
if($TestRegex) { $arguments+=" -TestRegex `"$TestRegex`"" }
$record=[ordered]@{build=$BuildDirectory; configuration=$Configuration; ctest=$CTestPath;
    ambient_job=$contained; scheduled=($contained -or $ForceScheduledHost.IsPresent);
    scope=$scope; started=[DateTime]::UtcNow.ToString('o')}
$name='ckmux-native-test-'+[Guid]::NewGuid().ToString('N')
$record.task_name=$name
$registered=$false
try {
    if(!$record.scheduled) {
        $child=Start-Process -FilePath $shell -ArgumentList $arguments -PassThru
        try {
            if(!$child.WaitForExit(1200000)) { $child.Kill(); throw 'Native test worker timed out' }
        } finally { $child.Dispose() }
    } else {
        # Test infrastructure only. No application launch, library API or
        # containment assertion is weakened; run the exact complete inventory.
        $action=New-ScheduledTaskAction -Execute $shell -Argument $arguments -WorkingDirectory $scope
        $user=[Security.Principal.WindowsIdentity]::GetCurrent().Name
        $principal=New-ScheduledTaskPrincipal -UserId $user -LogonType S4U -RunLevel Limited
        $settings=New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 20) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
        Register-ScheduledTask -TaskName $name -Action $action -Principal $principal -Settings $settings | Out-Null
        $registered=$true
        Write-Output "Native CTest: same-user limited scheduled host; ambient_job=$contained task=$name scope=$scope"
        Start-ScheduledTask -TaskName $name
        $deadline=[DateTime]::UtcNow.AddMinutes(21)
        while(!(Test-Path -LiteralPath (Join-Path $scope 'exit.txt')) -and [DateTime]::UtcNow -lt $deadline) {
            Start-Sleep -Milliseconds 200
        }
    }
    foreach($log in @('ctest.log','host-error.log')) {
        if(Test-Path -LiteralPath (Join-Path $scope $log)) { Get-Content -LiteralPath (Join-Path $scope $log) }
    }
    $result=Join-Path $scope 'exit.txt'
    if(!(Test-Path -LiteralPath $result)) { throw 'Native test worker did not write an exit result' }
    $code=[int](Get-Content -LiteralPath $result -Raw)
    $record.exit=$code; $record.finished=[DateTime]::UtcNow.ToString('o')
    $record | ConvertTo-Json | Out-File (Join-Path $scope 'record.json') -Encoding utf8
    if($code -ne 0) { throw "Native CTest failed: $code" }
} finally {
    if($registered) {
        if((Get-ScheduledTask -TaskName $name).State -eq 'Running') { Stop-ScheduledTask -TaskName $name }
        Unregister-ScheduledTask -TaskName $name -Confirm:$false
    }
}
