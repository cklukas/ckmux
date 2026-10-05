# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
param([Parameter(Mandatory=$true)][string]$Scope)
$ErrorActionPreference='Stop'
$env:TEMP="$Scope/t"; $env:TMP=$env:TEMP; $env:TMPDIR=$env:TEMP
# Test-only primary-token hosting. No application launch code, security policy,
# existing user's account or existing user's profile is changed by this fixture.
Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Text;
public static class CkmuxPackageProfileHost {
    [StructLayout(LayoutKind.Sequential)] struct Security { public int length; public IntPtr descriptor; public int inherit; }
    [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)] struct Startup {
        public int cb; public string reserved,desktop,title; public uint x,y,width,height,xchars,ychars,fill,flags;
        public ushort show,reservedSize; public IntPtr reservedBytes,input,output,error;
    }
    [StructLayout(LayoutKind.Sequential)] struct Child { public IntPtr process,thread; public uint pid,tid; }
    [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)] struct Profile {
        public int size,flags; public string user,path,defaultPath,server,policy; public IntPtr handle;
    }
    [DllImport("advapi32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool CreateProcessAsUserW(IntPtr token,string image,StringBuilder command,IntPtr processSecurity,IntPtr threadSecurity,bool inherit,uint flags,IntPtr environment,string cwd,ref Startup startup,out Child child);
    [DllImport("advapi32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool LogonUserW(string user,string domain,IntPtr password,uint type,uint provider,out IntPtr token);
    [DllImport("advapi32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool ConvertStringSecurityDescriptorToSecurityDescriptor(string text,uint revision,out IntPtr descriptor,out uint size);
    [DllImport("userenv.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool LoadUserProfileW(IntPtr token,ref Profile profile);
    [DllImport("userenv.dll",SetLastError=true)] static extern bool UnloadUserProfile(IntPtr token,IntPtr profile);
    [DllImport("userenv.dll",SetLastError=true)] static extern bool CreateEnvironmentBlock(out IntPtr environment,IntPtr token,bool inherit);
    [DllImport("userenv.dll")] static extern bool DestroyEnvironmentBlock(IntPtr environment);
    [DllImport("user32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr CreateWindowStation(string name,uint flags,uint access,ref Security security);
    [DllImport("user32.dll")] static extern IntPtr GetProcessWindowStation();
    [DllImport("user32.dll",SetLastError=true)] static extern bool SetProcessWindowStation(IntPtr station);
    [DllImport("user32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr CreateDesktop(string name,IntPtr device,IntPtr mode,uint flags,uint access,ref Security security);
    [DllImport("user32.dll")] static extern bool CloseDesktop(IntPtr desktop);
    [DllImport("user32.dll")] static extern bool CloseWindowStation(IntPtr station);
    [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr handle,uint timeout);
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool GetExitCodeProcess(IntPtr handle,out uint code);
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool TerminateProcess(IntPtr handle,uint code);
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool IsProcessInJob(IntPtr process,IntPtr job,out bool member);
    [DllImport("kernel32.dll",SetLastError=true)] static extern uint ResumeThread(IntPtr thread);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr memory);
    static void Require(bool value,string operation) { if(!value) throw new Win32Exception(Marshal.GetLastWin32Error(),operation); }
    public static uint Run(string user,string sid,IntPtr password,string image,string worker,string scope) {
        IntPtr token=IntPtr.Zero,descriptor=IntPtr.Zero,station=IntPtr.Zero,desktop=IntPtr.Zero;
        IntPtr original=GetProcessWindowStation(),environment=IntPtr.Zero;
        Child child=new Child(); Profile profile=new Profile(); bool switched=false,finished=false;
        try {
            Require(LogonUserW(user,".",password,2,0,out token),"Log on disposable standard user interactively");
            string owner=WindowsIdentity.GetCurrent().User.Value; uint length;
            Require(ConvertStringSecurityDescriptorToSecurityDescriptor(
                "D:(A;;GA;;;"+owner+")(A;;GA;;;"+sid+")(A;;GA;;;SY)S:(ML;;NW;;;ME)",
                1,out descriptor,out length),"Owned private station ACL");
            Security security=new Security(); security.length=Marshal.SizeOf(typeof(Security)); security.descriptor=descriptor;
            string name="ckmux_package_"+Guid.NewGuid().ToString("N");
            station=CreateWindowStation(name,0,0xF037F,ref security); Require(station!=IntPtr.Zero,"Create owned station");
            Require(SetProcessWindowStation(station),"Select owned station"); switched=true;
            desktop=CreateDesktop("validation",IntPtr.Zero,IntPtr.Zero,0,0xF01FF,ref security);
            Require(desktop!=IntPtr.Zero,"Create owned desktop");
            Require(SetProcessWindowStation(original),"Restore caller station"); switched=false;
            profile.size=Marshal.SizeOf(typeof(Profile)); profile.flags=1; profile.user=user;
            Require(LoadUserProfileW(token,ref profile),"Load only disposable user's profile");
            Require(CreateEnvironmentBlock(out environment,token,false),"Create disposable user's environment");
            Startup startup=new Startup(); startup.cb=Marshal.SizeOf(typeof(Startup)); startup.desktop=name+"\\validation";
            StringBuilder command=new StringBuilder("\""+image+"\" -NoProfile -ExecutionPolicy Bypass -File \""+worker+"\" -Scope \""+scope+"\"");
            const uint noWindow=0x08000000, suspended=0x00000004, unicode=0x00000400, breakaway=0x01000000;
            // The OS must allow this test worker to leave its parent's job. A
            // refusal fails this fixture; no application guard is disabled.
            Require(CreateProcessAsUserW(token,image,command,IntPtr.Zero,IntPtr.Zero,false,
                noWindow|suspended|unicode|breakaway,environment,scope,ref startup,out child),"Create standard-user worker suspended");
            bool member; Require(IsProcessInJob(child.process,IntPtr.Zero,out member),"Query all worker jobs");
            if(member) throw new InvalidOperationException("Standard-user worker remains contained; fixture is not independent");
            Console.WriteLine("Verified standard-user worker outside every job before resume; PID="+child.pid);
            Require(ResumeThread(child.thread)!=0xffffffff,"Resume independently hosted standard-user worker");
            uint wait=WaitForSingleObject(child.process,720000);
            if(wait!=0) throw new InvalidOperationException("Owned package worker timeout: "+wait);
            uint code; Require(GetExitCodeProcess(child.process,out code),"Read actual worker exit"); finished=true; return code;
        } finally {
            bool cleanup=true;
            if(!finished && child.process!=IntPtr.Zero && WaitForSingleObject(child.process,0)!=0) {
                cleanup=TerminateProcess(child.process,995) && WaitForSingleObject(child.process,5000)==0;
            }
            if(switched) cleanup=SetProcessWindowStation(original) && cleanup;
            if(child.thread!=IntPtr.Zero) cleanup=CloseHandle(child.thread) && cleanup;
            if(child.process!=IntPtr.Zero) cleanup=CloseHandle(child.process) && cleanup;
            if(environment!=IntPtr.Zero) cleanup=DestroyEnvironmentBlock(environment) && cleanup;
            if(profile.handle!=IntPtr.Zero) cleanup=UnloadUserProfile(token,profile.handle) && cleanup;
            if(token!=IntPtr.Zero) cleanup=CloseHandle(token) && cleanup;
            if(desktop!=IntPtr.Zero) cleanup=CloseDesktop(desktop) && cleanup;
            if(station!=IntPtr.Zero) cleanup=CloseWindowStation(station) && cleanup;
            if(descriptor!=IntPtr.Zero) cleanup=LocalFree(descriptor)==IntPtr.Zero && cleanup;
            if(!cleanup) throw new InvalidOperationException("Native disposable package host cleanup failed");
        }
    }
}
'@
$request=Get-Content -LiteralPath "$Scope/request.json" -Raw | ConvertFrom-Json
$owned=Join-Path ([Environment]::GetFolderPath('CommonDocuments')) ('ckmux-package-'+[Guid]::NewGuid().ToString('N'))
$name='CKP_'+[Guid]::NewGuid().ToString('N').Substring(0,12)
$password=ConvertTo-SecureString ('Ck!'+[Guid]::NewGuid().ToString('N')+'9z') -AsPlainText -Force
$created=$false; $sid=''; $secret=[IntPtr]::Zero
$record=[ordered]@{owned=$owned; account=$name; start=[DateTime]::UtcNow.ToString('o'); exit=1}
try {
    New-Item -ItemType Directory -Path $owned,"$owned/input/bin","$owned/t" | Out-Null
    $user=New-LocalUser -Name $name -Password $password -AccountNeverExpires -Description 'Disposable ckmux release-package acceptance'
    $created=$true; $sid=$user.SID.Value; $record.sid=$sid
    Add-LocalGroupMember -Group (Get-LocalGroup -SID 'S-1-5-32-545') -Member $user
    & icacls.exe $owned /grant "*$($sid):(OI)(CI)M" *> "$Scope/owned-acl.log"
    if($LASTEXITCODE -ne 0) { throw 'Cannot grant owned fixture files to disposable user' }
    Copy-Item -LiteralPath $request.zip -Destination "$owned/input/package.zip"
    Copy-Item -LiteralPath $request.msi -Destination "$owned/input/package.msi"
    $buildBin=Split-Path $request.executable
    $binaries=@('ckmux.exe','conpty.dll',"$($request.architecture)/OpenConsole.exe")
    if($request.architecture -eq 'x64') { $binaries+='arm64/OpenConsole.exe' }
    foreach($relative in $binaries) {
        $destination=Join-Path "$owned/input/bin" $relative
        [void][IO.Directory]::CreateDirectory((Split-Path $destination))
        Copy-Item -LiteralPath (Join-Path $buildBin $relative) -Destination $destination
    }
    foreach($script in @('check-package.ps1','package-profile-worker.ps1')) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $script) -Destination $owned
    }
    Copy-Item -LiteralPath $request.lifecycle -Destination "$owned/windows_server_lifecycle.ps1"
    @{version=$request.version; architecture=$request.architecture; sid=$sid} |
        ConvertTo-Json | Out-File "$owned/request.json" -Encoding utf8
    $inputs=@(Get-ChildItem -LiteralPath "$owned/input" -Recurse -File | ForEach-Object {
        @{path=$_.FullName; hash=(Get-FileHash -LiteralPath $_.FullName).Hash}
    })
    $inputs | ConvertTo-Json | Out-File "$Scope/input-hashes.json" -Encoding utf8
    $secret=[Runtime.InteropServices.Marshal]::SecureStringToGlobalAllocUnicode($password)
    $shell=Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
    $record.worker_exit=[CkmuxPackageProfileHost]::Run($name,$sid,$secret,$shell,"$owned/package-profile-worker.ps1",$owned)
    foreach($inputFile in $inputs) {
        if((Get-FileHash -LiteralPath $inputFile.path).Hash -cne $inputFile.hash) { throw 'Package test changed its input bytes' }
    }
    if($record.worker_exit -ne 0) { throw "Standard-user package worker failed: $($record.worker_exit)" }
    $record.exit=0
} catch { $record.failure=$_.ToString(); $record.exit=1 }
finally {
    if($secret -ne [IntPtr]::Zero) { [Runtime.InteropServices.Marshal]::ZeroFreeGlobalAllocUnicode($secret) }
    $password.Dispose()
    try {
        if($created) {
            $remaining=@(Get-CimInstance Win32_Process | Where-Object {
                (Invoke-CimMethod -InputObject $_ -MethodName GetOwnerSid -ErrorAction SilentlyContinue).Sid -eq $sid
            })
            foreach($process in $remaining) { Stop-Process -Id $process.ProcessId -Force -ErrorAction Stop }
            $profile=Get-CimInstance Win32_UserProfile | Where-Object { $_.SID -eq $sid }
            if($profile) {
                if($profile.Loaded -or (Split-Path $profile.LocalPath -Leaf) -cne $name) {
                    throw 'Disposable profile is loaded or has an unexpected path; preserving it for inspection'
                }
                Remove-CimInstance -InputObject $profile
            }
            if(Get-CimInstance Win32_UserProfile | Where-Object { $_.SID -eq $sid }) { throw 'Disposable profile remains' }
            $record.profile_removed=$true
            if((Get-LocalUser -Name $name).SID.Value -cne $sid) { throw 'Disposable account identity changed' }
            Remove-LocalUser -Name $name
            if(Get-LocalUser -Name $name -ErrorAction SilentlyContinue) { throw 'Disposable account remains' }
            $record.account_removed=$true
        }
        if(Test-Path -LiteralPath $owned) {
            if(@(Get-ChildItem -LiteralPath $owned -Recurse -Force | Where-Object {
                $_.Attributes -band [IO.FileAttributes]::ReparsePoint
            }).Count -ne 0) { throw 'Unexpected reparse point in disposable workspace; preserving it' }
            # Preserve compact, byte-identical scripts, records and logs before
            # deleting disposable extracted packages and copied binaries.
            foreach($file in Get-ChildItem -LiteralPath $owned -Recurse -File -Force) {
                if($file.Extension -notin @('.log','.json','.txt','.ps1','.conf')) { continue }
                $relative=$file.FullName.Substring($owned.Length+1)
                $destination=Join-Path "$Scope/evidence" $relative
                [void][IO.Directory]::CreateDirectory((Split-Path $destination))
                Copy-Item -LiteralPath $file.FullName -Destination $destination
                if((Get-FileHash -LiteralPath $file.FullName).Hash -cne (Get-FileHash -LiteralPath $destination).Hash) {
                    throw 'Compact package evidence differs from its original bytes'
                }
            }
            if($created -and (-not $record.account_removed -or -not $record.profile_removed)) {
                throw 'Refusing workspace cleanup before disposable identity cleanup'
            }
            Remove-Item -LiteralPath $owned -Recurse -Force
            $record.owned_removed=-not (Test-Path -LiteralPath $owned)
        }
    } catch { $record.cleanup_failure=$_.ToString(); $record.exit=1 }
    $record.end=[DateTime]::UtcNow.ToString('o')
    $record | ConvertTo-Json -Depth 5 | Out-File "$Scope/host-record.json" -Encoding utf8
    $record | ConvertTo-Json -Depth 5
}
exit $record.exit
