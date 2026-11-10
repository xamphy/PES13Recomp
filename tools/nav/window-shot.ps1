# Resizes the running pes13 window (client area) and saves what the window shows (PrintWindow),
# i.e. the presented swap chain image including letterbox bars - unlike `shot`, which saves the
# game's own 1280x720 image. Used to test the presenter (letterbox/pillarbox, resizes).
#   tools\nav\window-shot.ps1 -Width 1600 -Height 700 -Out userdata\shots\win_wide.png
param([int]$Width = 0, [int]$Height = 0, [Parameter(Mandatory)][string]$Out, [double]$Settle = 2)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class PesWin {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int h2, uint flags);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern int GetClassNameW(IntPtr h, [MarshalAs(UnmanagedType.LPWStr)] System.Text.StringBuilder s, int n);
    // Top-level window of the process with the game's window class (works for hidden windows too).
    public static IntPtr Find(uint pid) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => {
            uint p; GetWindowThreadProcessId(h, out p);
            if (p != pid || GetWindow(h, 4) != IntPtr.Zero) return true; // skip owned windows (dialogs)
            var sb = new System.Text.StringBuilder(64); GetClassNameW(h, sb, 64);
            if (sb.ToString().StartsWith("PES13") && sb.ToString() != "PES13Prompt") { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
'@
$p = Get-Process pes13 -ErrorAction Stop | Select-Object -First 1
$h = [PesWin]::Find([uint32]$p.Id)
if ($h -eq [IntPtr]::Zero) { throw 'pes13 window not found' }
if (-not [PesWin]::IsWindowVisible($h)) { [void][PesWin]::ShowWindow($h, 4); Start-Sleep 1 }   # SW_SHOWNOACTIVATE (launched hidden)
if ($Width -gt 0 -and $Height -gt 0) {
    $wr = New-Object PesWin+RECT; $cr = New-Object PesWin+RECT
    [void][PesWin]::GetWindowRect($h, [ref]$wr); [void][PesWin]::GetClientRect($h, [ref]$cr)
    $extraW = ($wr.R - $wr.L) - ($cr.R - $cr.L); $extraH = ($wr.B - $wr.T) - ($cr.B - $cr.T)
    # SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE
    [void][PesWin]::SetWindowPos($h, [IntPtr]::Zero, 0, 0, $Width + $extraW, $Height + $extraH, 0x0002 -bor 0x0004 -bor 0x0010)
    Start-Sleep -Milliseconds ([int]($Settle * 1000))
}
$cr = New-Object PesWin+RECT; [void][PesWin]::GetClientRect($h, [ref]$cr)
$wr = New-Object PesWin+RECT; [void][PesWin]::GetWindowRect($h, [ref]$wr)
$bmp = New-Object System.Drawing.Bitmap ($wr.R - $wr.L), ($wr.B - $wr.T)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc(); [void][PesWin]::PrintWindow($h, $hdc, 2); $g.ReleaseHdc($hdc); $g.Dispose()   # 2 = PW_RENDERFULLCONTENT
$dir = Split-Path $Out -Parent; if ($dir) { New-Item -ItemType Directory -Force $dir | Out-Null }
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
"{0} (client {1}x{2})" -f $Out, ($cr.R - $cr.L), ($cr.B - $cr.T)
