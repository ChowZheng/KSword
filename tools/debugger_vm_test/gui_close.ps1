# Run in the VM console session; closes only the two test log surfaces.
param([string]$Root='C:\ksword\debugger-tests\20260930')
$ErrorActionPreference='Stop'
Add-Type -TypeDefinition @'
using System;using System.Text;using System.Collections.Generic;using System.Runtime.InteropServices;
public static class CloseTestWindows {
 public delegate bool EnumProc(IntPtr h,IntPtr p);
 [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f,IntPtr p);
 [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h,EnumProc f,IntPtr p);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint p);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 public static IntPtr[] Surfaces(uint pid) {var result=new List<IntPtr>();EnumWindows((r,p)=>{uint owner;GetWindowThreadProcessId(r,out owner);if(owner==pid)EnumChildWindows(r,(h,q)=>{var s=new StringBuilder(128);GetClassName(h,s,128);if(s.ToString()=="KSwordCheatEngineLogSurface"||s.ToString()=="KSwordX96dbgLogSurface")result.Add(h);return true;},IntPtr.Zero);return true;},IntPtr.Zero);return result.ToArray();}
}
'@
$clients=@(Get-Process | Where-Object { $_.ProcessName -in @('cheatengine-x86_64','x64dbg') -and $_.Path.StartsWith("$Root\plugin\",[StringComparison]::OrdinalIgnoreCase) })
$gui=Get-Process -Id ([int](Get-Content "$Root\gui.pid"))
$surfaces=[CloseTestWindows]::Surfaces($gui.Id)
if($surfaces.Count -ne 2){throw 'Expected exactly two owned log surfaces'}
foreach($surface in $surfaces){[CloseTestWindows]::PostMessage($surface,16,[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null}
Start-Sleep -Seconds 2
if(Get-Process KswordCheatEngineLauncher,KswordX96dbgLauncher -ErrorAction SilentlyContinue){throw 'Log surfaces did not retire'}
foreach($client in $clients) {
    if($client.HasExited){throw "Closing log surface killed $($client.ProcessName)"}
    "PASS closing log surface leaves $($client.ProcessName) PID $($client.Id) alive" | Add-Content "$Root\gui-close.log"
}
'PASS owned log surfaces retired' | Add-Content "$Root\gui-close.log"
$clients | Stop-Process -Force
$gui.CloseMainWindow() | Out-Null
$gui.WaitForExit(15000) | Out-Null
if(!$gui.HasExited){$gui|Stop-Process -Force}
