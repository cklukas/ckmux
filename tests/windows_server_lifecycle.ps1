# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
param([Parameter(Mandatory=$true)][string]$Binary,
      [Parameter(Mandatory=$true)][string]$TemporaryRoot)
$ErrorActionPreference='Stop'
$Binary=[IO.Path]::GetFullPath($Binary)
if (-not (Test-Path -LiteralPath $Binary -PathType Leaf)) { throw 'Missing exact application executable' }
$instance='lifecycle-'+[Guid]::NewGuid().ToString('N')
$scope=Join-Path $TemporaryRoot $instance
$unicode=[string][char]0x8A2D+[char]0x5B9A+[char]0x00E9
$configRoot=Join-Path $scope $unicode
New-Item -ItemType Directory -Path $configRoot | Out-Null
$env:TMP=$scope
$env:TEMP=$scope
$env:TMPDIR=$scope
$env:CKMUX_SOCKET=$instance
$env:CKMUX_CONFIG=Join-Path $configRoot 'ckmux.conf'
function Run-CLI([string]$name,[string[]]$Arguments,[int]$ExpectedExit=0) {
    $log=Join-Path $scope ($name+'.log')
    $ErrorActionPreference='Continue'
    & $Binary @Arguments *> $log
    $code=$LASTEXITCODE
    $ErrorActionPreference='Stop'
    if ($code -ne $ExpectedExit) { Get-Content $log; throw ('CLI '+$name+' failed: '+$code) }
    return (Get-Content $log -Raw)
}
function Owned-Servers {
    @(Get-CimInstance Win32_Process | Where-Object {
        $_.ExecutablePath -and [String]::Equals($_.ExecutablePath,$Binary,[StringComparison]::OrdinalIgnoreCase) -and
        $_.CommandLine -and $_.CommandLine.Contains('--server') -and $_.CommandLine.Contains($instance)
    })
}
$clients=@()
$childProcesses=@()
$passed=$false
try {
    $absent=Run-CLI 'absent' @('ls') 1
    if (-not $absent.Contains('no server is running') -or @(Owned-Servers).Count -ne 0) {
        throw 'Read-only discovery did not preserve server absence'
    }
    # These are eight real application starter clients, not a mutex fixture.
    for ($index=0; $index -lt 8; $index++) {
        # Own the process handle from creation through exit. Start-Process's
        # later Process lookup can lose ExitCode for these very short clients.
        $client=New-Object Diagnostics.Process
        $client.StartInfo.FileName=$Binary
        $client.StartInfo.Arguments='new -s win-native-'+$index
        $client.StartInfo.UseShellExecute=$false
        $client.StartInfo.CreateNoWindow=$true
        $client.StartInfo.RedirectStandardOutput=$true
        $client.StartInfo.RedirectStandardError=$true
        if (-not $client.Start()) { $client.Dispose(); throw 'Starter could not be created' }
        $clients+=$client
    }
    foreach ($client in $clients) {
        if (-not $client.WaitForExit(15000)) { throw ('Starter timed out: '+$client.Id) }
        $index=[Array]::IndexOf($clients,$client)
        $output=$client.StandardOutput.ReadToEnd()
        $problem=$client.StandardError.ReadToEnd()
        [IO.File]::WriteAllText("$scope/new-$index.out",$output)
        [IO.File]::WriteAllText("$scope/new-$index.err",$problem)
        if ($client.ExitCode -ne 0) { throw ('Starter failed: '+$client.Id+' exit '+$client.ExitCode+' '+$problem) }
    }
    for ($index=0; $index -lt 8; $index++) {
        if ((Get-Content "$scope/new-$index.out" -Raw).Trim() -cne ('win-native-'+$index)) {
            throw ('Wrong session returned to starter '+$index)
        }
    }
    $servers=@(Owned-Servers)
    if ($servers.Count -ne 1) { throw 'Concurrent clients did not leave exactly one detached server' }
    $serverPid=$servers[0].ProcessId
    $children=@(Get-CimInstance Win32_Process | Where-Object {
        $_.ParentProcessId -eq $serverPid -and $_.Name -eq 'cmd.exe'
    })
    if ($children.Count -ne 8) { throw 'Detached server did not own eight actual native shell children' }
    foreach ($child in $children) {
        $process=Get-Process -Id $child.ProcessId -ErrorAction Stop
        # Acquire and retain a kernel handle while the known child is alive.
        # Later PID lookup can observe a recycled identity, and server exit
        # alone does not prove every member's exit notification has arrived.
        $null=$process.Handle
        $childProcesses+=$process
    }
    $children | Select-Object ProcessId,ParentProcessId,CreationDate,ExecutablePath |
        ConvertTo-Json | Out-File (Join-Path $scope 'children.json')
    $listed=Run-CLI 'list' @('ls')
    for ($index=0; $index -lt 8; $index++) {
        if (-not $listed.Contains('win-native-'+$index)) { throw 'Session disappeared after starter exit' }
    }
    if (@(Owned-Servers).Count -ne 1) { throw 'Read-only list lost detached server' }
    foreach ($child in $childProcesses) {
        if ($child.HasExited) { throw 'Native child died with a CLI client' }
    }
    $log=Join-Path $configRoot ($instance+'.log')
    if (-not (Get-Content $log -Raw).Contains('ckmux server: listening')) { throw 'Unicode log path lost real server diagnostics' }
    $null=Run-CLI 'shutdown' @('kill-server')
    $deadline=[DateTime]::UtcNow.AddSeconds(10)
    do {
        $remaining=@(Owned-Servers)
        if ($remaining.Count -eq 0) { break }
        Start-Sleep -Milliseconds 50
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($remaining.Count -ne 0) { throw 'Explicit shutdown left the owned server running' }
    foreach ($child in $childProcesses) {
        $remainingMilliseconds=[Math]::Max(0,[int]($deadline-[DateTime]::UtcNow).TotalMilliseconds)
        if (-not $child.WaitForExit($remainingMilliseconds)) {
            throw ('Explicit shutdown left owned native child '+$child.Id+' running past the shutdown deadline')
        }
    }
    if (-not (Get-Content $log -Raw).Contains('ckmux server: stopped')) { throw 'Orderly shutdown diagnostic missing' }
    $passed=$true
    Write-Output ('Native application: eight concurrent starters, one server '+$serverPid+', eight cmd children, CLI-exit persistence, Unicode logs and complete shutdown: PASS')
} finally {
    foreach ($client in $clients) { $client.Dispose() }
    foreach ($child in $childProcesses) { $child.Dispose() }
    if (@(Owned-Servers).Count -gt 0) {
        $ErrorActionPreference='Continue'
        & $Binary kill-server *> "$scope/cleanup.log"
        $ErrorActionPreference='Stop'
    }
    if ($passed) { Remove-Item -LiteralPath $scope -Recurse -Force }
    else { Write-Output ('Failure evidence retained: '+$scope) }
}
