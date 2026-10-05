# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
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
foreach($inputPath in @($Zip,$Msi,$BuildExecutable,$LifecycleScript)) {
    if(-not [IO.Path]::IsPathRooted($inputPath) -or -not (Test-Path -LiteralPath $inputPath -PathType Leaf)) {
        throw "An exact absolute input file is required: $inputPath"
    }
}
# MSI's native path parser does not accept mixed slash separators even when
# .NET/PowerShell can open the same file. Pass canonical Windows paths to it.
$Zip=(Resolve-Path -LiteralPath $Zip).Path
$Msi=(Resolve-Path -LiteralPath $Msi).Path
$BuildExecutable=(Resolve-Path -LiteralPath $BuildExecutable).Path
$LifecycleScript=(Resolve-Path -LiteralPath $LifecycleScript).Path
$TemporaryRoot=(Resolve-Path -LiteralPath $TemporaryRoot).Path
$identity=[Security.Principal.WindowsIdentity]::GetCurrent()
$principal=New-Object Security.Principal.WindowsPrincipal($identity)
if($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Package acceptance must run without an elevated administrator token'
}
$scope=Join-Path $TemporaryRoot ('package-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scope | Out-Null
$env:TEMP=$scope; $env:TMP=$scope; $env:TMPDIR=$scope
$record=[ordered]@{scope=$scope; architecture=$Architecture; version=$Version;
    user_sid=$identity.User.Value; elevated=$false; start=[DateTime]::UtcNow.ToString('o');
    lifecycle_exercised=$true}
$beforePath=@{user=[Environment]::GetEnvironmentVariable('PATH','User');
    machine=[Environment]::GetEnvironmentVariable('PATH','Machine')}
$installRoot=Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) ('ckmux-'+$Architecture)
$dataRoot=Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'ckmux'
$createdDataRoot=-not (Test-Path -LiteralPath $dataRoot)
if($createdDataRoot) { New-Item -ItemType Directory -Path $dataRoot | Out-Null }
$dataWitness=Join-Path $dataRoot ('package-test-'+[Guid]::NewGuid().ToString('N')+'.conf')
[IO.File]::WriteAllText($dataWitness,'package acceptance user-data preservation witness')
$record.data_witness=$dataWitness
function Data-Hashes {
    $values=@{}
    if(Test-Path -LiteralPath $dataRoot) {
        foreach($item in Get-ChildItem -LiteralPath $dataRoot -Recurse -File -Force) {
            $values[$item.FullName]=(Get-FileHash -LiteralPath $item.FullName).Hash
        }
    }
    return $values
}
$beforeData=Data-Hashes
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class CkmuxPackageInstaller {
    [DllImport("msi.dll", CharSet=CharSet.Unicode)]
    public static extern uint MsiEnumRelatedProducts(string upgrade, uint reserved,
        uint index, StringBuilder product);
    [DllImport("msi.dll", CharSet=CharSet.Unicode)]
    static extern uint MsiEnumProductsEx(string product, string user, uint contexts,
        uint index, StringBuilder installed, out uint context, StringBuilder sid, ref uint size);
    public static uint CurrentProduct(string product, StringBuilder installed,
        out uint context, StringBuilder sid, ref uint size) {
        // Keep the native null current-user selector inside C#: PowerShell's
        // string argument conversion may otherwise turn $null into empty SID.
        return MsiEnumProductsEx(product, null, 7, 0, installed, out context, sid, ref size);
    }
}
'@
function Property([object]$Database,[string]$Name) {
    $view=$Database.OpenView("SELECT ``Value`` FROM ``Property`` WHERE ``Property``='$Name'")
    try { [void]$view.Execute(); $row=$view.Fetch(); if($null -eq $row) { return '' }; return $row.StringData(1) }
    finally { [void]$view.Close() }
}
function Related-Products([string]$Upgrade) {
    $product=New-Object Text.StringBuilder 39
    $result=[CkmuxPackageInstaller]::MsiEnumRelatedProducts($Upgrade,0,0,$product)
    if($result -eq 259) { return $false }
    if($result -ne 0) { throw "Cannot inspect installed products: $result" }
    return $true
}
function Native([string]$Name,[string]$Executable,[string[]]$Arguments) {
    $record.step=$Name
    $ErrorActionPreference='Continue'
    & $Executable @Arguments *> "$scope/$Name.log"
    $code=$LASTEXITCODE; $ErrorActionPreference='Stop'
    $record["${Name}_exit"]=$code
    if($code -ne 0) { Get-Content "$scope/$Name.log"; throw "$Name failed: $code" }
}
function Installer([string]$Name,[string]$Action,[string]$Target) {
    $record.step=$Name
    $log="$scope/$Name-msi.log"
    # msiexec is a GUI-subsystem executable: explicitly own and wait for it.
    # ArgumentList concatenates arrays rather than preserving argument quoting.
    $arguments="$Action `"$Target`" /qn /norestart /l*v `"$log`""
    $process=Start-Process -FilePath $msiexec -ArgumentList $arguments -PassThru
    try {
        if(-not $process.WaitForExit(300000)) { throw "$Name exceeded the installer deadline; PID $($process.Id)" }
        $record["${Name}_exit"]=$process.ExitCode
        if($process.ExitCode -ne 0) { throw "$Name failed: $($process.ExitCode); log $log" }
    } finally { $process.Dispose() }
}
function Layout([string]$Root) {
    $required=@('bin/ckmux.exe','bin/conpty.dll',"bin/$Architecture/OpenConsole.exe",
        'bin/Microsoft.ConPTY.LICENSE.txt','bin/Microsoft.ConPTY.Runtime.json',
        'share/doc/ckmux/LICENSE','share/doc/ckmux/windows.md')
    if($Architecture -eq 'x64') { $required+='bin/arm64/OpenConsole.exe' }
    foreach($relative in $required) {
        if(-not (Test-Path -LiteralPath (Join-Path $Root $relative) -PathType Leaf)) {
            throw "Incomplete package: $relative"
        }
    }
    $runtime=Get-Content (Join-Path $Root 'bin/Microsoft.ConPTY.Runtime.json') -Raw | ConvertFrom-Json
    if($runtime.package -cne 'Microsoft.Windows.Console.ConPTY' -or
        $runtime.version -cne '1.24.261001001' -or $runtime.architecture -cne $Architecture -or
        $runtime.sha256 -cne '4d6aaddc1d2385c9f5897df28f33879f699f8f2783315d5204cf3d8c3616ac5f') {
        throw 'Packaged runtime identity differs from the qualified distribution policy'
    }
    $buildBin=Split-Path $BuildExecutable
    foreach($relative in @('ckmux.exe','conpty.dll',"$Architecture/OpenConsole.exe")) {
        if((Get-FileHash (Join-Path $Root "bin/$relative")).Hash -cne
            (Get-FileHash (Join-Path $buildBin $relative)).Hash) { throw "Stale packaged input: $relative" }
    }
    if($Architecture -eq 'x64' -and (Get-FileHash (Join-Path $Root 'bin/arm64/OpenConsole.exe')).Hash -cne
        (Get-FileHash (Join-Path $buildBin 'arm64/OpenConsole.exe')).Hash) { throw 'Stale emulation console host' }
    $files=@{}
    foreach($item in Get-ChildItem -LiteralPath $Root -Recurse -File) {
        $relative=$item.FullName.Substring($Root.Length+1).Replace('\','/')
        $files[$relative]=(Get-FileHash -LiteralPath $item.FullName).Hash
    }
    return $files
}
function Same-Layout([hashtable]$Expected,[hashtable]$Actual) {
    if($Expected.Count -ne $Actual.Count) { throw 'ZIP and installed MSI file inventories differ' }
    foreach($relative in $Expected.Keys) {
        if(-not $Actual.ContainsKey($relative) -or $Actual[$relative] -cne $Expected[$relative]) {
            throw "ZIP and installed MSI bytes differ: $relative"
        }
    }
}
function Exercise([string]$Name,[string]$Root) {
    $exe=Join-Path $Root 'bin/ckmux.exe'
    Native "$Name-version" $exe @('--version')
    if(-not (Get-Content "$scope/$Name-version.log" -Raw).Contains("ckmux $Version")) {
        throw 'Installed executable reports the wrong project version'
    }
    Native "$Name-lifecycle" 'powershell.exe' @('-NoProfile','-ExecutionPolicy','Bypass',
        '-File',$LifecycleScript,'-Binary',$exe,'-TemporaryRoot',$scope)
    if(-not $record.Contains("$Name-lifecycle_exit") -or $record["$Name-lifecycle_exit"] -ne 0) {
        throw "Required package step missing or failed: $Name-lifecycle"
    }
    if(-not (Get-Content -LiteralPath "$scope/$Name-lifecycle.log" -Raw).Contains(
        'CLI-exit persistence, Unicode logs and complete shutdown: PASS')) {
        throw "Installed lifecycle did not produce its real application witness: $Name"
    }
}
$installed=$false
$product=''
try {
    $record.zip_sha256=(Get-FileHash -LiteralPath $Zip).Hash
    $record.msi_sha256=(Get-FileHash -LiteralPath $Msi).Hash
    $zipRoot=Join-Path $scope (([string][char]0x8A2D+[char]0x5B9A)+' portable package')
    New-Item -ItemType Directory -Path $zipRoot | Out-Null
    $archive=[IO.Compression.ZipFile]::OpenRead($Zip)
    try {
        foreach($entry in $archive.Entries) {
            $destination=[IO.Path]::GetFullPath((Join-Path $zipRoot $entry.FullName))
            if(-not $destination.StartsWith($zipRoot+'\',[StringComparison]::OrdinalIgnoreCase)) {
                throw 'ZIP entry escapes its installation directory'
            }
        }
    } finally { $archive.Dispose() }
    [IO.Compression.ZipFile]::ExtractToDirectory($Zip,$zipRoot)
    $executables=@(Get-ChildItem -LiteralPath $zipRoot -Filter ckmux.exe -Recurse -File)
    if($executables.Count -ne 1 -or (Split-Path $executables[0].DirectoryName -Leaf) -cne 'bin') {
        throw 'ZIP must contain exactly one application under bin'
    }
    $portableRoot=Split-Path $executables[0].DirectoryName
    $expected=Layout $portableRoot
    $record.portable_root=$portableRoot
    $record.inventory=$expected
    Exercise 'zip' $portableRoot

    $installer=New-Object -ComObject WindowsInstaller.Installer
    $database=$installer.OpenDatabase($Msi,0)
    $product=Property $database 'ProductCode'
    $upgrade=Property $database 'UpgradeCode'
    $record.msi_properties=@{product=$product;upgrade=$upgrade;version=(Property $database 'ProductVersion');allusers=(Property $database 'ALLUSERS')}
    $expectedUpgrade=if($Architecture -eq 'arm64') { '{B90B17C0-BCF9-4506-B5BB-457245804F10}' }
        else { '{63F125D8-E36D-45C4-8E12-24E8E6A2EBA8}' }
    if($upgrade -ine $expectedUpgrade -or (Property $database 'ProductVersion') -cne $Version) {
        throw 'MSI upgrade identity or version differs from architecture-specific policy'
    }
    if((Property $database 'ALLUSERS') -ne '') { throw 'MSI is not authored as strictly per-user' }
    $template=$database.SummaryInformation(0).Property(7)
    $expectedTemplate=if($Architecture -eq 'arm64') { 'Arm64;' } else { 'x64;' }
    if(-not $template.StartsWith($expectedTemplate,[StringComparison]::OrdinalIgnoreCase)) {
        throw 'MSI platform differs from its advertised architecture'
    }
    if((Related-Products $upgrade) -or (Test-Path -LiteralPath $installRoot)) {
        throw 'Refusing to modify a pre-existing installation or installation directory'
    }
    $record.product=$product; $record.install_root=$installRoot
    $msiexec=Join-Path $env:SystemRoot 'System32/msiexec.exe'
    $installed=$true
    Installer 'msi-install' '/i' $Msi
    $registered=New-Object Text.StringBuilder 39
    $sid=New-Object Text.StringBuilder 256; [uint32]$size=256; [uint32]$context=0
    $result=[CkmuxPackageInstaller]::CurrentProduct($product,$registered,[ref]$context,$sid,[ref]$size)
    if($result -ne 0 -or $context -ne 2 -or $sid.ToString() -cne $identity.User.Value) {
        throw "Installed product is not current-user unmanaged context: $result/$context/$sid"
    }
    $record.install_context=$context
    Same-Layout $expected (Layout $installRoot)
    Exercise 'msi' $installRoot
    $repairImage=Join-Path $installRoot 'bin/ckmux.exe'
    Remove-Item -LiteralPath $repairImage
    if(Test-Path -LiteralPath $repairImage) { throw 'Repair fixture failed to remove its owned installed executable' }
    Installer 'msi-repair' '/fa' $product
    Same-Layout $expected (Layout $installRoot)
    Installer 'msi-uninstall' '/x' $product
    $installed=$false
    if((Related-Products $upgrade) -or (Test-Path -LiteralPath (Join-Path $installRoot 'bin/ckmux.exe'))) {
        throw 'Uninstall left a registered product or application executable'
    }
    if([Environment]::GetEnvironmentVariable('PATH','User') -cne $beforePath.user -or
        [Environment]::GetEnvironmentVariable('PATH','Machine') -cne $beforePath.machine) {
        throw 'Package install/repair/uninstall changed persistent PATH'
    }
    $afterData=Data-Hashes
    Same-Layout $beforeData $afterData
    $record.exit=0
} catch { $record.failure=$_.ToString(); $record.exit=1; throw }
finally {
    if($installed) {
        $ErrorActionPreference='Continue'
        try { Installer 'msi-cleanup' '/x' $product }
        catch { $record.cleanup_failure=$_.ToString() }
        $ErrorActionPreference='Stop'
    }
    if(Test-Path -LiteralPath $dataWitness) {
        Copy-Item -LiteralPath $dataWitness -Destination "$scope/user-data-witness.conf"
        Remove-Item -LiteralPath $dataWitness
    }
    if($createdDataRoot -and @(Get-ChildItem -LiteralPath $dataRoot -Force).Count -eq 0) {
        Remove-Item -LiteralPath $dataRoot
    }
    $record.end=[DateTime]::UtcNow.ToString('o')
    $record | ConvertTo-Json -Depth 6 | Out-File "$scope/record.json" -Encoding utf8
    $record | ConvertTo-Json -Depth 6
}
