# out_wasapi

An independent **32-bit WASAPI output plugin for classic Winamp**.
The output binary is **`out_wasapi.dll`**. This is a new C++17 implementation;
building it does not require Winamp sources, the Winamp SDK, Wasabi, MFC, ATL,
vcpkg, or any third-party libraries.

## Features

- WASAPI **shared mode by default**, with Windows' high-quality sample-rate and channel
  conversion explicitly enabled. Integer PCM: 8/16/24/32 bits and 1–8 channels;
  the selected endpoint's Windows audio engine must accept the combination.
- Event-driven shared rendering and timer-driven exclusive rendering on a
  dedicated MTA thread, using MMCSS `Audio` when available. All stream COM interfaces are created, used, and released there.
- A bounded, frame-aligned ring buffer. `Write` accepts the whole block or
  returns `1` without pretending that dropped data was accepted. Host write
  callbacks do not wait for free space or call the audio driver.
- Correct tail draining, including tracks shorter than the prebuffer threshold.
  Written time counts accepted frames; played time counts submitted frames minus
  WASAPI padding, rather than unused buffer capacity. Raw unplayed PCM is retained
  within the same bounded ring for recovery, including the copy of device padding.
- Pause/resume and seeking that preserves the paused state. Frame counters are
  64-bit; the classic host's 32-bit millisecond values saturate at `INT_MAX`.
- Volume and balance persist before opening a device and across reopenings.
  **Windows mixer session volume is enabled by default:** Winamp's volume controls
  its application session in Windows, without changing endpoint/master volume or
  another application's session. Optional software PCM volume leaves the Windows
  mixer independent. Balance is software PCM attenuation in either mode.
  With no software attenuation, the plugin leaves PCM bits unchanged; Windows'
  shared mixer may still transform the audio.
- Select a specific playback endpoint or the default multimedia endpoint.
  **Automatic recovery is on by default:** device/format changes recreate the
  stream while preserving pause, retained PCM and time accounting. An explicitly
  selected device is never silently replaced by another endpoint.
- Optional **exclusive access**, off by default. Uses the exact source PCM format
  without Windows shared-engine conversion; unsupported/busy/disabled-exclusive
  endpoints fail visibly, without a silent shared fallback.

## Settings and live diagnostics

Open **Preferences → Plug-ins → Output → Independent WASAPI Output → Configure**.
The native dialog refreshes its technical report every 250 ms while open:

- State, device name/ID, active shared/exclusive mode, servicing mode, MMCSS,
  recovery completions/attempts, and the unconfirmed-frame replay boundary.
- **Winamp input PCM**, **WASAPI client format**, and **endpoint mix format**.
- Software queue and endpoint padding/capacity in frames, bytes, and milliseconds.
- Prebuffer, peak queue occupancy, driver stream latency, and default device period.
- Played/written positions, 64-bit frame/byte counters, and render-call count.
- Buffer-empty events, audio clock, requested volume/balance, active volume
  mode, observed Windows session level/mute, and the last error.

[Settings during playback](docs/settings-playing.png)

**Copy report** copies the complete Unicode report. Information remains available
while paused, after Stop, and after a failed open. **Apply** saves changes for the
**next playback start** without interrupting the current stream. **Close** does
not save unapplied changes.

### Volume control

**Use Windows volume mixer (Winamp session)** is **on by default**, also for
existing profiles that do not yet have the setting. With it enabled, changing
Winamp's volume changes its session level in the Windows volume mixer in **shared
mode**, including
while paused. Winamp 0–255 maps linearly to the session's 0–100% level. The plugin
does not also attenuate PCM volume, so the level is not applied twice. Balance
remains software-based. The plugin never changes endpoint/master volume, another
application's session, or the Windows mute state.

To restore independent software volume, clear the checkbox, choose **Apply**,
then **Stop → Play**. This saves `session_volume=0` in `out_wasapi.ini`; an explicit
choice remains saved. A missing key defaults to `1`. The report identifies the
active stream's mode separately from the checkbox saved for the next start.

External Windows mixer changes stay effective until Winamp sends another volume
callback or playback reopens. The report shows the actual observed session level
and mute in session mode. This is not two-way slider synchronization: moving the
Windows slider does not move Winamp's own slider. Session volume is not reset on
Stop; Windows may persist it. In software mode, Windows mixer volume is a separate
multiplier, so a previously reduced or muted session still affects playback.

### Device/format recovery

**Automatically recover after device or format changes** is **on by default**.
A missing `auto_recover` key means `1`; clearing it and applying saves
`auto_recover=0`. Like the other settings, the choice applies on the next Open.

When the default endpoint is selected, the active stream follows Windows'
**default multimedia playback device**. A specifically selected endpoint stays
selected even if the Windows default changes. Device state/removal, endpoint
format properties, session disconnection, and relevant invalid-device/resource
errors can trigger rebuilding the stream on its owning render thread. The live
report rereads the endpoint mix, actual buffer, latency and clock on each reopen.

After a successful initial Open, a missing/busy device or unavailable audio
service is retried every **500 ms** until it returns or playback is stopped.
`Recovering / waiting for device` is not EOF. The ring remains bounded; Winamp
can fill its remaining space and then receives normal backpressure. Pause and
seek work while waiting; a seek explicitly discards old-position PCM. Stop/Close
cancels recovery. Initial Open still fails promptly if no endpoint can be opened.
Unsupported exclusive PCM and exclusive permission denial remain visible errors,
not endless retries or a switch to shared mode.

Accepted PCM is retained until padding confirms it played. On abrupt device
loss, the last unconfirmed prefix is replayed: a short repeat or gap is possible,
so this is **not a guarantee of seamless hotplug**. Played time does not regress
or advance while waiting. Recovery is not a restart/seek of the input decoder.
When recovery is off, invalidation stops output with a diagnostic and requires
Stop → Play; default-device changes apply on the next Open only.

### Exclusive access

**Exclusive access to playback device** defaults to **off** (`exclusive=0`,
also when the key is absent). Enable it, Apply, then Stop → Play to use exclusive
WASAPI. Windows must allow applications to take exclusive control of that
endpoint, and no other exclusive client may own it. Other applications and system
sounds can be blocked while Winamp owns the device, including while paused.
Stop releases ownership. Shared mode remains the recommended general default.

There is **no resampler or bit-depth/channel conversion** in exclusive mode.
The driver must accept the track's decoded PCM format. Switching tracks to an
unsupported format fails with an actionable error; clear exclusive and restart
to use shared conversion. An equivalent legacy mono/stereo PCM descriptor can
be used for driver compatibility without changing the samples or their format.

Windows session volume does not affect exclusive streams. The Windows mixer
checkbox is disabled in the form while exclusive is selected; its saved shared-
mode preference is retained. In exclusive mode, Winamp volume and balance use
PCM gain, without touching endpoint/master volume or mute. Unity volume and
centered balance preserve source PCM; this is not certification of DAC bit purity.
The Endpoint mix report remains the **shared** engine setting, not the active
exclusive format. Exclusive uses timer-driven variable-size packets (1–5 ms
service checks), allowing exact short tails without padding the source with
silence; shared mode remains event-driven.

The output ABI receives decoded PCM, not source codec/container metadata. The
plugin therefore does not invent MP3/AAC bitrates or codec names. Endpoint mix
format is **not a guarantee of the physical DAC format**. Driver latency and
queue occupancy are not measurements of end-to-end speaker latency. Buffer-empty
events may include the end of a stream and do not independently prove an audible
glitch.

## Build without Winamp sources

Requirements: Windows 10/11; Visual Studio 2022+ or Build Tools with **Desktop
development with C++**, x86 tools, and a Windows 10/11 SDK; CMake 3.21+ on `PATH`
(the build wrapper also checks CMake's standard Program Files location).
The plugin uses **MultiThreadedDLL (`/MD`)** for Release and
MultiThreadedDebugDLL (`/MDd`) for Debug. Running a Release DLL requires the
**Microsoft Visual C++ Redistributable x86 (14.x)** compatible with the selected
MSVC toolset, in addition to Windows system DLLs. The x64 redistributable alone
is not sufficient. The binary delivery does not bundle the runtime installer.

From the repository directory:

```powershell
.\build.cmd
```

Outputs: `build\Release\out_wasapi.dll` and `build\Release\out_wasapi.pdb`.
The wrapper also runs core, notification and ABI tests. CMake chooses an installed Visual
Studio generator and produces a solution that can be opened in Visual Studio.

Equivalent manual configuration for VS2022:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
```

```powershell
cmake --build build --config Release --parallel
```

```powershell
ctest --test-dir build -C Release --output-on-failure
```

Use a different build directory when changing generators. x64 configuration is
intentionally rejected: classic Winamp loads x86 DLLs. Building neither accesses
sibling source trees nor downloads anything from the network.

## Install

1. Close Winamp. Back up the existing `Plugins\out_wasapi.dll`, if present.
2. Copy the new `out_wasapi.dll` into your Winamp installation's `Plugins` folder.
3. Start Winamp and select **Independent WASAPI Output v1.2.0** under Output.
4. Open Configure, adjust the device/buffers if necessary, and select Apply.
   Restart playback to use the new settings.

Tested host: Winamp 5.9.2 x86. The classic narrow-description ABI (`OUT_VER = 0x10`)
is declared locally in [include/winamp_output_abi.h](include/winamp_output_abi.h).
The profile location is obtained through `IPC_GETINIFILEW` (Winamp 5.58+).
Settings are stored in **`out_wasapi.ini`**, beside the host's `winamp.ini`.
Without that IPC, the fallback is `%LOCALAPPDATA%\out_wasapi\settings.ini`.
The plugin itself does not edit `winamp.ini`, file associations, system volume,
or an unrelated installed Winamp profile.

Defaults: automatic recovery on, exclusive off, Windows mixer session volume on,
500 ms retained PCM queue, 30 ms prebuffer, 50 ms requested device buffer.
Allowed ranges: queue 100–2000 ms, prebuffer 0–queue, device buffer 10–200 ms.
WASAPI selects the actual device buffer size, which diagnostics report.
Like many classic outputs, this plugin uses its own buffer settings instead of
`Open`'s buffer/prebuffer arguments. The `-666` video-sync sentinel is never
interpreted as a buffer duration; no silence trimming is performed.

## Tests

Core tests do not require an audio device. `plugin_host.exe` additionally loads
the **actual DLL** through its public ABI and exercises a real WASAPI endpoint.
Test audio is silence; the tests do not change global device settings. Use a
new evidence directory for each run:

```powershell
.\build\Release\plugin_host.exe --audio .\build\Release\out_wasapi.dll .\test-results\audio-01
```

```powershell
.\build\Release\plugin_host.exe --ui .\build\Release\out_wasapi.dll .\test-results\ui-01
```

```powershell
.\build\Release\plugin_host.exe --missing-device .\build\Release\out_wasapi.dll .\test-results\missing-01
```

Session-volume regression checks (the first command deliberately omits the
INI key to test the default):

```powershell
.\build\Release\plugin_host.exe --session-volume .\build\Release\out_wasapi.dll .\test-results\session-01
```

```powershell
.\build\Release\plugin_host.exe --software-volume .\build\Release\out_wasapi.dll .\test-results\software-01
```

`--audio-software` and `--ui-software` repeat the audio/UI checks with the checkbox
off. The GUI test saves `settings-playing.png`, a live text report, and a report
after a seek while paused. It checks volume-mode persistence, recovery-on/exclusive-off defaults,
checkbox persistence/interaction, and verifies Apply does not switch active modes. The real-host smoke test takes a **ready-to-run Winamp
installation**, not its sources, and creates a disposable full copy:

```powershell
& "$env:WINDIR\SysWOW64\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File .\tests\winamp-smoke.ps1 -WinampDirectory "C:\path\to\Winamp" -PluginPath .\build\Release\out_wasapi.dll -WorkDirectory .\test-results\winamp-01
```

The script refuses an existing WorkDirectory, uses a separate profile, verifies
the loaded DLL's SHA-256 and the active output selection, then checks playback,
pause, seeking, Stop, natural EOF, and normal shutdown of its own instance.
A read-only `mixer_probe.exe` independently observes Winamp's actual Windows
session at requested levels 0/64/128/255 and while paused. It must be beside the
built plugin, or supplied with `-MixerProbePath`. Add `-SoftwareVolume` for a
second run that verifies Winamp volume leaves the Windows session level unchanged.
The probe also checks that endpoint/master volume and mute do not change.
The script does not close other Winamp processes.

Recovery checks use the production engine sources compiled into a separate test
executable with test-only disconnect/reopen-failure hooks. They tear down and
recreate **real WASAPI clients** without changing global Windows settings:

```powershell
.\build\Release\recovery_tests.exe
```

```powershell
.\build\Release\recovery_tests.exe --exclusive
```

The exclusive command briefly reserves the default output device. Exit code 77
means none of its tested stereo PCM formats could be opened, not a passing
exclusive playback test. It verifies both enabled and disabled recovery, PCM/time
conservation, pause/seek while unavailable, cancellation and repeated lifetimes.
The release DLL has no injection hooks or extra exports.

To test the actual release DLL's exclusive settings, transport and GUI:

```powershell
.\build\Release\plugin_host.exe --exclusive .\build\Release\out_wasapi.dll .\test-results\exclusive-01
```

Add `-Exclusive` to the Winamp smoke command for an exclusive run. Its WAV is
stereo 16-bit PCM at 44100 Hz by default; `-SampleRate 48000` selects another
source rate if the device requires it. `-NoAutoRecover` selects the opt-out path.
These are local test options, not hidden settings in the release plugin.

Recorded local results: [docs/TESTING.md](docs/TESTING.md).
Architecture and invariants: [docs/DESIGN.md](docs/DESIGN.md).
Original defect coverage: [docs/REGRESSIONS.md](docs/REGRESSIONS.md).

## Scope of version 1.2

This is shared or opt-in exclusive WASAPI output, not ASIO, encoded passthrough,
or a guarantee of gapless handoff across separate `Open`/`Close` calls. Crossfade,
DSP and visualizations are not implemented here. Recovery is supported but does
not guarantee physically seamless/no-repeat hotplug. Unit/IPC checks do not replace listening
or hardware testing with different HDMI/USB/Bluetooth endpoints and drivers.

## License

Copyright (C) 2026 out_wasapi contributors.

This program is free software: you can redistribute it and/or modify it under
the terms of the **GNU General Public License, version 3 only**
(`SPDX-License-Identifier: GPL-3.0-only`). See [LICENSE](LICENSE) for the full text.
It comes **without any warranty**, including merchantability or fitness for a
particular purpose. Distributing binaries requires complying with GPLv3,
including its Corresponding Source obligations.

This is an independent project, not an official Nullsoft/Winamp release. The
Winamp implementation was used only as a behavioral reference and to identify
the public ABI. Its audio-engine implementation and Wasabi are not included.
This license covers this repository's original code; it does not relicense
Winamp, Windows SDK/system components, or a host installation. Compatibility and
redistribution of a combined host/plugin product must be assessed separately.
