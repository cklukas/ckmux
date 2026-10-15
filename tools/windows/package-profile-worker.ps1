# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
param([Parameter(Mandatory=$true)][string]$Scope)
$ErrorActionPreference='Stop'
$env:TEMP="$Scope/t"; $env:TMP=$env:TEMP; $env:TMPDIR=$env:TEMP
$code=1
try {
    $request=Get-Content -LiteralPath "$Scope/request.json" -Raw | ConvertFrom-Json
    $identity=[Security.Principal.WindowsIdentity]::GetCurrent()
    $principal=New-Object Security.Principal.WindowsPrincipal($identity)
    if($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Package worker must be a standard user, not an administrator'
    }
    if(-not $principal.IsInRole((New-Object Security.Principal.SecurityIdentifier 'S-1-5-4'))) {
        throw 'Package worker must have an interactive-logon token'
    }
    if($request.sid -cne $identity.User.Value) { throw 'Package worker changed installer identity' }
    $denied=$false
    try {
        $key=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SOFTWARE',$true)
        if($key) { $key.Dispose() }
    } catch [UnauthorizedAccessException] { $denied=$true }
      catch [Security.SecurityException] { $denied=$true }
    if(-not $denied) { throw 'Standard package worker can write protected machine settings' }
    [IO.File]::WriteAllText("$Scope/privilege-witness.txt",
        "Administrators=false; Interactive=true; HKLM-write=denied; SID="+$identity.User.Value)
    $machine=$request.installation_scope -eq 'machine'
    & "$Scope/check-package.ps1" -Zip "$Scope/input/package.zip" -Msi "$Scope/input/package.msi" `
        -BuildExecutable "$Scope/input/bin/ckmux.exe" -Architecture $request.architecture `
        -Version $request.version -TemporaryRoot "$Scope/t" `
        -LifecycleScript "$Scope/windows_server_lifecycle.ps1" -MachineInstallation:$machine *> "$Scope/package-test.log"
    $results=@(Get-ChildItem -LiteralPath "$Scope/t" -Filter record.json -Recurse -File)
    if($results.Count -ne 1) { throw 'Full checker did not produce exactly one acceptance record' }
    $result=Get-Content -LiteralPath $results[0].FullName -Raw | ConvertFrom-Json
    $steps=if($machine) { @('zip-version','zip-lifecycle','machine-access-path','machine-version','machine-lifecycle') }
        else { @('zip-version','zip-lifecycle','msi-install','msi-install-path','msi-version','msi-lifecycle',
            'msi-repair','msi-repair-path','msi-uninstall','msi-custom-install','msi-custom-install-path',
            'msi-custom-repair','msi-custom-repair-path','msi-custom-uninstall') }
    foreach($step in $steps) {
        $property=$result.PSObject.Properties["${step}_exit"]
        if($null -eq $property -or $property.Value -ne 0) { throw "Required package step missing or failed: $step" }
    }
    $expectedContext=if($machine) { 4 } else { 2 }
    if($result.exit -ne 0 -or -not $result.lifecycle_exercised -or
        $result.elevated -or $result.install_context -ne $expectedContext -or $result.user_sid -cne $request.sid -or
        ($machine -and -not $result.machine_access_verified)) {
        throw 'Package record does not prove full non-admin installation and lifecycle'
    }
    $code=0
} catch { $_ | Out-File "$Scope/error.log"; $code=1 }
finally {
    @{exit=$code; sid=([Security.Principal.WindowsIdentity]::GetCurrent().User.Value);
      profile=$env:USERPROFILE} | ConvertTo-Json | Out-File "$Scope/worker-record.json" -Encoding utf8
}
exit $code
