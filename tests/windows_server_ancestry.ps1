# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
param([Parameter(Mandatory=$true)][string]$Binary,
      [Parameter(Mandatory=$true)][string]$Probe,
      [Parameter(Mandatory=$true)][string]$TemporaryRoot)
$ErrorActionPreference='Stop'
$instance='ancestry-'+[Guid]::NewGuid().ToString('N')
$scope=Join-Path $TemporaryRoot $instance
New-Item -ItemType Directory -Path $scope | Out-Null
$env:TEMP=$scope; $env:TMP=$scope; $env:TMPDIR=$scope
$env:CKMUX_CONFIG=Join-Path $scope 'ckmux.conf'
try {
    & $Probe ([IO.Path]::GetFullPath($Binary)) $instance
    if($LASTEXITCODE -ne 0) { throw 'Actual native server ancestry contract failed' }
} finally {
    # On assertion/setup failure, stop only these two scoped test instances.
    foreach($suffix in @('-allow','-deny')) {
        $env:CKMUX_SOCKET=$instance+$suffix
        $ErrorActionPreference='Continue'
        & $Binary kill-server *> $null
        $ErrorActionPreference='Stop'
    }
    Remove-Item -LiteralPath $scope -Recurse -Force
}
