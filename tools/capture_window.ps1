# Capture the flOw render window (D3D12) to a PNG.
# D3D12 swapchains don't render through GDI PrintWindow, so we force the window
# to the foreground (ALT-key trick bypasses the SetForegroundWindow lock) and
# grab the screen region the client rect occupies.
# Usage: powershell -ExecutionPolicy Bypass -File tools/capture_window.ps1 <out.png>
param([string]$Out = "shot.png")

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win32 {
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
  public struct RECT { public int L, T, R, B; }
  public struct POINT { public int X, Y; }
}
"@
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

$proc = Get-Process flow -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $proc) { Write-Error "flow.exe window not found"; exit 2 }
$h = $proc.MainWindowHandle

# Force foreground: restore, ALT-tap to release the foreground lock, then raise.
[void][Win32]::ShowWindow($h, 9)   # SW_RESTORE
Start-Sleep -Milliseconds 200
$wsh = New-Object -ComObject WScript.Shell
$wsh.SendKeys('%')                 # ALT — unlocks SetForegroundWindow
Start-Sleep -Milliseconds 100
[void][Win32]::BringWindowToTop($h)
[void][Win32]::SetForegroundWindow($h)
Start-Sleep -Milliseconds 700      # let it repaint on top

$r = New-Object Win32+RECT
[void][Win32]::GetClientRect($h, [ref]$r)
$tl = New-Object Win32+POINT; $tl.X = 0; $tl.Y = 0
[void][Win32]::ClientToScreen($h, [ref]$tl)
$w = $r.R - $r.L; $hgt = $r.B - $r.T
if ($w -le 0 -or $hgt -le 0) { Write-Error "bad client rect $w x $hgt"; exit 3 }

$bmp = New-Object System.Drawing.Bitmap $w, $hgt
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($tl.X, $tl.Y, 0, 0, (New-Object System.Drawing.Size $w, $hgt))
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Output "saved $Out ($w x $hgt)"
