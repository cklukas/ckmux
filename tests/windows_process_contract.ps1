# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
param([Parameter(Mandatory=$true)][string]$Binary,
      [Parameter(Mandatory=$true)][string]$TemporaryRoot)
$ErrorActionPreference='Stop'
$Binary=[IO.Path]::GetFullPath($Binary)
if (-not (Test-Path -LiteralPath $Binary -PathType Leaf)) { throw 'Missing exact helper executable' }
$scope=Join-Path $TemporaryRoot ('process-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scope | Out-Null
$env:TMP=$scope
$env:TEMP=$scope
$env:TMPDIR=$scope
$unicode=[string][char]0x8A2D+[char]0x5B9A+[char]0x00E9
$log=Join-Path $scope ($unicode+'.log')
try {
    [IO.File]::WriteAllBytes($log,[Text.Encoding]::ASCII.GetBytes("existing-marker`n"))
    for ($run=0; $run -lt 2; $run++) {
        & $Binary $log
        if ($LASTEXITCODE -ne 0) { throw ('Native daemon helper failed: '+$LASTEXITCODE) }
    }
    $expected="existing-marker`nstdout-marker`nstderr-marker`nstdout-marker`nstderr-marker`n"
    if ([IO.File]::ReadAllText($log) -cne $expected) { throw 'Shared binary append streams did not preserve both writes' }
    # A failed log open must return an environmental failure, not crash or
    # report successful daemon setup. The existing file cannot be a directory.
    & $Binary (Join-Path $log 'cannot-open.log')
    if ($LASTEXITCODE -ne 1) { throw ('Failed log path was not refused: '+$LASTEXITCODE) }
    Write-Output 'Native detached CRT streams: Unicode, append, both writers, NUL input, no handle inheritance, explicit failure: PASS'
} finally {
    # Only this invocation's newly allocated scratch directory is removed.
    Remove-Item -LiteralPath $scope -Recurse -Force
}
