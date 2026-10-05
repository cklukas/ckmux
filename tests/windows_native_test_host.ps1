# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
param(
    [Parameter(Mandatory=$true)][string]$TestHostPath,
    [Parameter(Mandatory=$true)][string]$TemporaryRoot,
    [string]$CTestPath=''
)
$ErrorActionPreference='Stop'
if(!$CTestPath) { $CTestPath=(Get-Command ctest -ErrorAction Stop).Source }
$CTestPath=(Resolve-Path -LiteralPath $CTestPath).Path
$cmake=Join-Path (Split-Path $CTestPath) 'cmake.exe'
if(!(Test-Path -LiteralPath $cmake)) { throw 'Exact companion CMake missing' }
$scope=Join-Path $TemporaryRoot ('test-host-negative-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path "$scope/build","$scope/tmp" | Out-Null
$command=$cmake.Replace('\','/')
$fixture="# Copyright (c) 2026 C. Klukas. All rights reserved.`n# SPDX-License-Identifier: MIT`nadd_test(native_expected_failure `"$command`" -E false)`n"
[IO.File]::WriteAllText((Join-Path $scope 'build/CTestTestfile.cmake'),$fixture)
$caught=$false
try {
    & $TestHostPath -BuildDirectory "$scope/build" -TemporaryRoot "$scope/tmp" -CTestPath $CTestPath -ForceScheduledHost *> "$scope/driver.log"
} catch {
    $caught=$true
    $_ | Out-File "$scope/expected-error.log"
}
if(!$caught) { throw 'Native test host concealed an actual failing CTest entry' }
$records=@(Get-ChildItem "$scope/tmp/native-test-*/record.json")
if($records.Count -ne 1) { throw 'No exact test-worker exit evidence' }
$record=Get-Content -LiteralPath $records[0].FullName -Raw | ConvertFrom-Json
if($record.exit -ne 8 -or !$record.scheduled) { throw 'Actual CTest failure did not propagate from scheduled worker' }
$log=Get-Content -LiteralPath (Join-Path $record.scope 'ctest.log') -Raw
if(!$log.Contains('native_expected_failure') -or !$log.Contains('0% tests passed')) {
    throw 'Negative fixture did not actually execute and fail'
}
if(Get-ScheduledTask -TaskName $record.task_name -ErrorAction SilentlyContinue) {
    throw 'Native test host left its temporary scheduled task registered'
}
Write-Output "Native test-host negative: real failing entry, CTest exit8, parent failure and exact task removal PASS; evidence=$scope"
