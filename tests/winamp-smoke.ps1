# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 out_wasapi contributors. See LICENSE.
param(
    [Parameter(Mandatory=$true)][string]$WinampDirectory,
    [Parameter(Mandatory=$true)][string]$PluginPath,
    [Parameter(Mandatory=$true)][string]$WorkDirectory,
    [string]$MixerProbePath,
    [switch]$SoftwareVolume,
    [switch]$Exclusive,
    [switch]$NoAutoRecover,
    [ValidateRange(8000,768000)][int]$SampleRate = 44100
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
if ([IntPtr]::Size -ne 4) { throw 'Run with SysWOW64\WindowsPowerShell\v1.0\powershell.exe (x86) to inspect loaded x86 modules.' }
$source = (Resolve-Path -LiteralPath $WinampDirectory).Path
$plugin = (Resolve-Path -LiteralPath $PluginPath).Path
if (-not $MixerProbePath) { $MixerProbePath = Join-Path (Split-Path -Parent $plugin) 'mixer_probe.exe' }
$mixerProbe = (Resolve-Path -LiteralPath $MixerProbePath).Path
if (-not (Test-Path -LiteralPath (Join-Path $source 'winamp.exe'))) { throw 'winamp.exe is missing.' }
$root = [IO.Path]::GetFullPath($WorkDirectory)
if (Test-Path -LiteralPath $root) { throw 'WorkDirectory must not exist. Use a fresh disposable directory.' }
$null = New-Item -ItemType Directory -Path $root
Copy-Item -Path (Join-Path $source '*') -Destination $root -Recurse
Copy-Item -LiteralPath $plugin -Destination (Join-Path $root 'Plugins\out_wasapi.dll') -Force
# Change only the disposable copy. Never use the installed profile or associations.
$env:APPDATA = Join-Path $root 'TestAppData\Roaming'
$env:LOCALAPPDATA = Join-Path $root 'TestAppData\Local'
$null = New-Item -ItemType Directory -Force -Path $env:APPDATA,$env:LOCALAPPDATA,(Join-Path $root 'WasapiProfile')
@'
[Winamp]
inidir=%WINAMP_PROGRAM_DIR%\WasapiProfile
'@ | Set-Content -LiteralPath (Join-Path $root 'paths.ini') -Encoding ascii
@'
[Winamp]
newverchk=0
newverchk2=0
newverchk3=0
newverchk_rc=0
check_ft_startup=0
no_registry=1
user_consent_join_channels=0
volume=0
outname=out_wasapi.dll
skin=
[WinampReg]
NeedReg=0
'@ | Set-Content -LiteralPath (Join-Path $root 'WasapiProfile\winamp.ini') -Encoding ascii
@'
[out_wasapi]
queue_ms=500
prebuffer_ms=30
device_buffer_ms=50
'@ | Set-Content -LiteralPath (Join-Path $root 'WasapiProfile\out_wasapi.ini') -Encoding ascii
# Omit session_volume in the normal run to verify migration/default-on behavior.
if ($SoftwareVolume) { Add-Content -LiteralPath (Join-Path $root 'WasapiProfile\out_wasapi.ini') -Value 'session_volume=0' -Encoding ascii }
if ($Exclusive) { Add-Content -LiteralPath (Join-Path $root 'WasapiProfile\out_wasapi.ini') -Value 'exclusive=1' -Encoding ascii }
if ($NoAutoRecover) { Add-Content -LiteralPath (Join-Path $root 'WasapiProfile\out_wasapi.ini') -Value 'auto_recover=0' -Encoding ascii }
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class WasapiSmoke {
    public delegate bool EnumCallback(IntPtr w, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumCallback c, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr w, out uint p);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr w, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr w, uint m, IntPtr p, IntPtr l);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr w, uint m, IntPtr p, IntPtr l);
    [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll")] public static extern bool ReadProcessMemory(IntPtr p, IntPtr address, byte[] bytes, int count, out IntPtr read);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr w, out Rect r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr w, IntPtr dc, uint flags);
}
'@
function Find-MainWindow([int]$ProcessId) {
    $script:found = [IntPtr]::Zero
    $script:ownerId = $ProcessId
    $callback = [WasapiSmoke+EnumCallback]{ param($window,$unused)
        [uint32]$owner = 0
        $null = [WasapiSmoke]::GetWindowThreadProcessId($window,[ref]$owner)
        if ($owner -eq $script:ownerId) {
            $name = New-Object Text.StringBuilder 256
            $null = [WasapiSmoke]::GetClassName($window,$name,256)
            if ($name.ToString() -eq 'Winamp v1.x') { $script:found = $window }
        }
        return $true
    }
    $null = [WasapiSmoke]::EnumWindows($callback,[IntPtr]::Zero)
    return $script:found
}
function Ipc([int]$Code,[int]$Value=0) { return [WasapiSmoke]::SendMessage($script:main,1024,[IntPtr]$Value,[IntPtr]$Code).ToInt64() }
function Read-Mixer([int]$ProcessId) {
    $json = & $mixerProbe --read $ProcessId
    if ($LASTEXITCODE -ne 0) { throw 'Windows mixer observation failed.' }
    return ($json | ConvertFrom-Json)
}
# A 4-second silent file exercises natural EOF and catches IsPlaying truncation.
$track = Join-Path $root 'silence.wav'
$pcm = New-Object byte[] ($SampleRate*4*4)
$writer = New-Object IO.BinaryWriter ([IO.File]::Create($track))
try {
    $writer.Write([Text.Encoding]::ASCII.GetBytes('RIFF')); $writer.Write([int](36+$pcm.Length))
    $writer.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt ')); $writer.Write([int]16)
    $writer.Write([int16]1); $writer.Write([int16]2); $writer.Write([int]$SampleRate)
    $writer.Write([int]($SampleRate*4)); $writer.Write([int16]4); $writer.Write([int16]16)
    $writer.Write([Text.Encoding]::ASCII.GetBytes('data')); $writer.Write([int]$pcm.Length); $writer.Write($pcm)
} finally { $writer.Dispose() }
$process = Start-Process -FilePath (Join-Path $root 'winamp.exe') -ArgumentList '/NEW','/NOREG',('"'+$track+'"') -WorkingDirectory $root -PassThru
$null = $process.Handle
$forced = $false
try {
    $deadline = [DateTime]::UtcNow.AddSeconds(90)
    $script:main = [IntPtr]::Zero
    while ([DateTime]::UtcNow -lt $deadline -and $script:main -eq [IntPtr]::Zero -and -not $process.HasExited) {
        Start-Sleep -Milliseconds 100
        $script:main = Find-MainWindow $process.Id
    }
    if ($script:main -eq [IntPtr]::Zero) { throw 'Winamp did not create its main window.' }
    # Rewind/restart once the GUI exists, so startup time cannot consume the fixture.
    $null = [WasapiSmoke]::SendMessage($script:main,273,[IntPtr]40045,[IntPtr]::Zero)
    Start-Sleep -Milliseconds 700
    if ((Ipc 104) -ne 1) { throw 'Winamp did not start playback.' }
    $position = Ipc 105
    if ($position -lt 100 -or $position -gt 2500) { throw "Playback clock is not progressing: $position" }
    $process.Refresh()
    $loaded = @($process.Modules | Where-Object { $_.ModuleName -ieq 'out_wasapi.dll' })
    if ($loaded.Count -ne 1 -or $loaded[0].FileName -ine (Join-Path $root 'Plugins\out_wasapi.dll')) { throw 'New WASAPI DLL was not loaded from the disposable package.' }
    if ((Get-FileHash -LiteralPath $loaded[0].FileName -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $plugin -Algorithm SHA256).Hash) { throw 'Loaded DLL differs from built DLL.' }
    Write-Output ('LOADED ' + $loaded[0].FileName)
    # Output DLLs are all loaded during startup; additionally verify selection.
    $remoteName = [WasapiSmoke]::SendMessage($script:main,1024,[IntPtr]::Zero,[IntPtr]625)
    $readHandle = [WasapiSmoke]::OpenProcess(0x1010,$false,$process.Id)
    if ($readHandle -eq [IntPtr]::Zero) { throw 'Cannot read active output plugin name.' }
    try {
        $nameBytes = New-Object byte[] 260
        $readCount = [IntPtr]::Zero
        if (-not [WasapiSmoke]::ReadProcessMemory($readHandle,$remoteName,$nameBytes,260,[ref]$readCount)) { throw 'Cannot read IPC_GETOUTPUTPLUGIN result.' }
        $activeOutput = [Text.Encoding]::ASCII.GetString($nameBytes).Split([char]0)[0]
        if ($activeOutput -ine 'out_wasapi.dll') { throw "Wrong active output: $activeOutput" }
        Write-Output ('ACTIVE_OUTPUT ' + $activeOutput)
    } finally { $null = [WasapiSmoke]::CloseHandle($readHandle) }
    Write-Output ('PLAYING PositionMs=' + $position)
    $beforeMixer = Read-Mixer $process.Id
    foreach ($volume in @(64,128,255,0)) {
        $null = Ipc 122 $volume
        Start-Sleep -Milliseconds 80
        $mixer = Read-Mixer $process.Id
        $expected = if ($SoftwareVolume -or $Exclusive) { $beforeMixer.Volume } else { $volume / 255.0 }
        if ([Math]::Abs($mixer.Volume - $expected) -gt 0.001) { throw "Windows mixer volume differs from Winamp: requested=$volume actual=$($mixer.Volume) expected=$expected" }
        if ([Math]::Abs($mixer.EndpointVolume - $beforeMixer.EndpointVolume) -gt 0.001 -or $mixer.EndpointMuted -ne $beforeMixer.EndpointMuted) { throw 'Endpoint master volume/mute changed.' }
        if ($mixer.Muted -ne $beforeMixer.Muted) { throw 'Session mute changed.' }
        Write-Output ('MIXER_VALUE Winamp={0} Session={1} Master={2}' -f $volume,$mixer.Volume,$mixer.EndpointVolume)
    }
    # Rewind before transport checks; independent probe startup may consume time.
    $null = Ipc 106 500
    $null = [WasapiSmoke]::SendMessage($script:main,273,[IntPtr]40046,[IntPtr]::Zero)
    Start-Sleep -Milliseconds 120
    $paused = Ipc 105
    Start-Sleep -Milliseconds 250
    if ((Ipc 104) -ne 3 -or [Math]::Abs((Ipc 105)-$paused) -gt 30) { throw 'Pause did not preserve position.' }
    $null = Ipc 122 100
    Start-Sleep -Milliseconds 100
    $pausedMixer = Read-Mixer $process.Id
    $expectedPaused = if ($SoftwareVolume -or $Exclusive) { $beforeMixer.Volume } else { 100 / 255.0 }
    if ([Math]::Abs($pausedMixer.Volume - $expectedPaused) -gt 0.001 -or (Ipc 104) -ne 3) { throw 'Mixer update while paused failed or resumed playback.' }
    $null = Ipc 122 0
    Write-Output ('WINAMP_MIXER_PASS Mode={0} PausedSession={1} MasterUnchanged=True' -f $(if ($Exclusive) { 'exclusive-pcm' } elseif ($SoftwareVolume) { 'software' } else { 'session-default' }),$pausedMixer.Volume)
    # IPC_JUMPTOTIME, while paused: engine must not resume itself.
    $null = Ipc 106 2000
    Start-Sleep -Milliseconds 150
    if ((Ipc 104) -ne 3) { throw 'Seek resumed paused playback.' }
    $null = [WasapiSmoke]::SendMessage($script:main,273,[IntPtr]40046,[IntPtr]::Zero)
    Start-Sleep -Milliseconds 250
    if ((Ipc 104) -ne 1 -or (Ipc 105) -lt 2000) { throw 'Resume after seek failed.' }
    Add-Type -AssemblyName System.Drawing
    $r = New-Object WasapiSmoke+Rect
    $null = [WasapiSmoke]::GetWindowRect($script:main,[ref]$r)
    $bitmap = New-Object Drawing.Bitmap ($r.Right-$r.Left),($r.Bottom-$r.Top)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $dc = $graphics.GetHdc()
    try { $null = [WasapiSmoke]::PrintWindow($script:main,$dc,2) } finally { $graphics.ReleaseHdc($dc) }
    $bitmap.Save((Join-Path $root 'winamp-playing.png'),[Drawing.Imaging.ImageFormat]::Png)
    $graphics.Dispose(); $bitmap.Dispose()
    $null = [WasapiSmoke]::SendMessage($script:main,273,[IntPtr]40047,[IntPtr]::Zero)
    if ((Ipc 104) -ne 0) { throw 'Stop failed.' }
    $null = [WasapiSmoke]::SendMessage($script:main,273,[IntPtr]40045,[IntPtr]::Zero)
    $elapsed = [Diagnostics.Stopwatch]::StartNew()
    $lastPosition = 0
    do {
        Start-Sleep -Milliseconds 20
        $playing = Ipc 104
        if ($playing -eq 1) {
            $samplePosition = Ipc 105
            # IPC_ISPLAYING and IPC_GETOUTPUTTIME are separate messages. The
            # track can stop between them, making the latter return -1.
            if ($samplePosition -ge 0) { $lastPosition = [Math]::Max($lastPosition,$samplePosition) }
        }
    } while ($playing -eq 1 -and $elapsed.ElapsedMilliseconds -lt 7000)
    if ($playing -ne 0 -or $elapsed.ElapsedMilliseconds -lt 3500 -or $lastPosition -lt 3600) {
        throw ('Natural EOF truncated or hung: elapsed={0} position={1} state={2}' -f $elapsed.ElapsedMilliseconds,$lastPosition,$playing)
    }
    Write-Output ('EOF_PASS ElapsedMs={0} LastPositionMs={1}' -f $elapsed.ElapsedMilliseconds,$lastPosition)
} finally {
    if (-not $process.HasExited) {
        $null = $process.CloseMainWindow()
        if (-not $process.WaitForExit(10000)) { $forced=$true; $process.Kill(); $process.WaitForExit() }
    }
    Write-Output ('SHUTDOWN Code={0} Forced={1}' -f $process.ExitCode,$forced)
}
if ($forced -or $process.ExitCode -ne 0) { throw 'Normal shutdown required.' }
Write-Output 'WINAMP_SMOKE_PASS: real DLL load, playback, pause, seek, stop, natural EOF, normal shutdown.'
