# Run in the VM's interactive session after opening the real KSword Plugins page.
param([string]$Root='C:\ksword\debugger-tests\20260930')
$ErrorActionPreference='Stop'
$report=Join-Path $Root 'gui-tabs.log'
function Check($Value,[string]$Message) {if(!$Value){throw $Message};"PASS $Message" | Add-Content $report}
Set-Content $report ''
try {
Add-Type -TypeDefinition @'
using System; using System.Text; using System.Collections.Generic; using System.Runtime.InteropServices;
public static class GuestWindows {
 public delegate bool EnumProc(IntPtr h, IntPtr p);
 [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc f, IntPtr p);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint p);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h,uint f);
 [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h,int i);
 [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
 [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int c);
 [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll",CharSet=CharSet.Unicode,EntryPoint="SendMessageW")] public static extern IntPtr ReadText(IntPtr h,uint m,IntPtr n,StringBuilder s);
 [DllImport("gdi32.dll",CharSet=CharSet.Unicode,EntryPoint="GetObjectW")] public static extern int GetObject(IntPtr h,int n,byte[] data);
 public static IntPtr[] Children(IntPtr h) { var result=new List<IntPtr>();EnumChildWindows(h,(c,p)=>{result.Add(c);return true;},IntPtr.Zero);return result.ToArray(); }
 public static string Class(IntPtr h) { var s=new StringBuilder(256);GetClassName(h,s,256);return s.ToString(); }
 public static string Text(IntPtr h) { var s=new StringBuilder(65536);ReadText(h,13,(IntPtr)65536,s);return s.ToString(); }
 public static string Font(IntPtr h) { var f=SendMessage(h,49,IntPtr.Zero,IntPtr.Zero);var data=new byte[92];if(GetObject(f,92,data)!=92)return "";return Encoding.Unicode.GetString(data,28,64).TrimEnd('\0'); }
}
'@
$gui=Get-Process -Id ([int](Get-Content "$Root\gui.pid"))
$deadline=(Get-Date).AddSeconds(20)
do {
    $clients=@(Get-Process | Where-Object { $_.ProcessName -in @('cheatengine-x86_64','x64dbg') -and $_.Path.StartsWith("$Root\plugin\",[StringComparison]::OrdinalIgnoreCase) })
    if($clients.Count -ne 2){Start-Sleep -Milliseconds 200}
} while($clients.Count -ne 2 -and (Get-Date) -lt $deadline)
Check ($clients.Count -eq 2) 'both production debugger GUIs started from their plugin payloads'
foreach($client in $clients) {
    Check ($client.MainWindowHandle -ne 0 -and ([GuestWindows]::GetWindowLong($client.MainWindowHandle,-16) -band 0x40000000) -eq 0) "$($client.ProcessName) is a standalone top-level window"
    Check ([GuestWindows]::GetAncestor($client.MainWindowHandle,2) -ne $gui.MainWindowHandle) "$($client.ProcessName) is independent of KSword's native window"
    [GuestWindows]::ShowWindow($client.MainWindowHandle,6) | Out-Null
}
$helpers=Get-Process | Where-Object { $_.ProcessName -in @('KswordCheatEngineLauncher','KswordX96dbgLauncher') -and $_.Path.StartsWith("$Root\plugin\",[StringComparison]::OrdinalIgnoreCase) }
Check ($helpers.Count -eq 2) 'both real PluginHost control/log processes connected'
$children=[GuestWindows]::Children($gui.MainWindowHandle)
foreach($helper in $helpers) {
    $owned=@($children | Where-Object { $owner=0;[GuestWindows]::GetWindowThreadProcessId($_,[ref]$owner)|Out-Null;$owner -eq $helper.Id })
    Check ($owned.Count -ge 4) "$($helper.ProcessName) provides an embedded control/log surface"
    $editor=@($owned | Where-Object {[GuestWindows]::Class($_) -eq 'Edit'})[0]
    Check ([GuestWindows]::Font($editor) -eq 'Consolas') "$($helper.ProcessName) log uses Consolas"
    $text=[GuestWindows]::Text($editor)
    Check ($text -match 'backend|后端') "$($helper.ProcessName) forwarded real backend startup logs"
    $toggleId=if($helper.ProcessName -eq 'KswordCheatEngineLauncher'){7101}else{4101}
    $toggle=@($owned | Where-Object {[GuestWindows]::Class($_) -eq 'Button' -and [GuestWindows]::GetDlgCtrlID($_) -eq $toggleId})[0]
    Check ([GuestWindows]::IsWindowEnabled($toggle)) "$($helper.ProcessName) HVM control is available"
    if($helper.ProcessName -eq 'KswordCheatEngineLauncher') {
        $state="$env:TEMP\ksword-ce-bridge-$($helper.Id).status.log.state";$selection=2;$revision=0
    } else {
        $session=Get-ChildItem "$Root\plugin\x96dbg\sessions" -Directory | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        $state=Join-Path $session.FullName 'backend.log.state';$selection=3;$revision=1
    }
    $before=(Get-Content $state -Raw).Trim() -split '\s+'
    Check ($before[$selection] -eq '0') "$($helper.ProcessName) initially acknowledges native/R0 mode"
    foreach($expected in '1','0','1','0') {
        [GuestWindows]::SendMessage($toggle,245,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null
        $deadline=(Get-Date).AddSeconds(6)
        do {Start-Sleep -Milliseconds 150;$actual=(Get-Content $state -Raw).Trim() -split '\s+'} while((Get-Date) -lt $deadline -and ($actual[$selection] -ne $expected -or $actual[$revision] -eq $before[$revision]))
        Check ($actual[$selection] -eq $expected -and $actual[$revision] -ne $before[$revision]) "$($helper.ProcessName) HVM switch=$expected acknowledged by debugger"
        # The backend writes its acknowledgement before the log process polls
        # it. A user cannot click again while the native button is disabled.
        $deadline=(Get-Date).AddSeconds(3)
        while(!( [GuestWindows]::IsWindowEnabled($toggle)) -and (Get-Date) -lt $deadline){Start-Sleep -Milliseconds 100}
        Check ([GuestWindows]::IsWindowEnabled($toggle)) "$($helper.ProcessName) UI applied acknowledgement and re-enabled control"
        $before=$actual
    }
}
'FINAL PASS real PluginHost Tabs and standalone debugger GUIs' | Add-Content $report
} catch { 'FAIL '+($_|Out-String) | Add-Content $report; exit 1 }
