# Local verification report

Date: 2026-11-26. Version: 1.2.0. Platform: Windows 11, x86 plugin in a
32-bit Winamp 5.9.2 host. Documentation and test commands are in English.

## Build and runtime

The delivered DLL uses Visual Studio 2022 Build Tools, MSVC 19.44.35228,
Windows SDK 10.0.19041.0, CMake 4.4.2, optimized Release code and full linker PDB.
Release is **MultiThreadedDLL (/MD)**; Debug is MultiThreadedDebugDLL (/MDd).
Generated projects were checked for all seven C++ targets, including the new
notification/recovery test executables. The standalone build.cmd wrapper also
passed with Visual Studio 2026, MSVC 19.51.36256 and Windows SDK 10.0.28000.0;
its three CTest groups passed. That second-toolchain build is not the delivered
VS2022 DLL.

The delivery's PE/import/symbol check found one x86 image, one matching PDB and
no unresolved direct imports on this machine. The DLL imports MSVCP140.dll,
VCRUNTIME140.dll and UCRT API sets. It requires a compatible **Microsoft Visual
C++ Redistributable x86 (14.x)**; runtime DLLs/installers are not bundled. These
checks do not prove that an arbitrary clean PC already has the runtime.

The source build needs only C++17, CMake and the Windows SDK. No Winamp source,
Wasabi, MFC, ATL, Qt, vcpkg or downloaded library is required. The original Winamp
checkout and existing host delivery packages were not modified.

## Verified groups

The following groups used the final VS2022 release DLL or, for injected recovery,
the same engine sources in the dedicated test executable.

| Group | Result and evidence |
| --- | --- |
| Core tests | PASS: 52,842 checks; ring wraparound/offset peeking, retained recovery suffix, randomized queue conservation, all-or-nothing writes, PCM layout/gain/pan and time saturation |
| Notification tests | PASS: selected endpoint and render/eMultimedia filtering, null default removal, format/device/session changes, ignored unrelated roles/devices/volume, coalescing, callback lifetime after owner release |
| DLL/ABI | PASS: x86 load, callback/layout checks and the three classic undecorated exports |
| Shared playback | PASS in session and software volume modes: 22050 Hz mono 8-bit, 44100 Hz stereo 16-bit, 48000 Hz stereo packed 24-bit, 96000 Hz stereo 32-bit and 48000 Hz 5.1 16-bit |
| Backpressure | PASS: full paused queue rejects a complete write without changing accepted time; null and partial-frame writes rejected |
| Time/tail/transport | PASS: zero-at-open, monotonic played time bounded by accepted time, exact final duration, short tail below prebuffer, Pause return values, paused seek, Flush queue/counter/base reset |
| Repeated lifetime | PASS: 20 Open/drain/Close cycles per shared volume mode; handles 227 before/after in each run |
| Missing selected endpoint | PASS: initial Open fails promptly, no default fallback, zero capacity and IsPlaying |
| Windows session volume | PASS: missing key defaults on, pre-open/reopen volume, paused updates, 0/1/64/128/255, upper clamping, ignored negative sentinels, rapid callbacks, external edits and explicit reapply |
| Volume isolation | PASS: shared Windows mute, endpoint/master volume/mute and an independent comparison session are preserved |
| Software-volume option | PASS: explicit session_volume=0 preserves an independent Windows session level through Winamp volume callbacks/reopen |
| Settings GUI | PASS in both initial volume modes: recovery on/exclusive off defaults, saved values, exclusive disables session-volume control without erasing its preference, Apply does not switch active modes, saved settings applied on reopen |
| Exclusive release DLL | PASS: actual exclusive=1 INI load; supported stereo PCM, active exclusive/timer-driven report, software volume, disabled shared-session control, 40 ms tail, paused seek and unsupported-format rejection without shared fallback |
| Shared recovery | PASS: playing/paused real-client rebuild after injected loss, unavailable retries, accepted PCM conservation, monotonic/frozen time, offline pause/seek, Close cancellation, disabled recovery, clean manual reopen and 12 repeated recoveries |
| Exclusive recovery | PASS: same recovery/transport/conservation checks in exclusive; exact supported format remains exclusive, software volume remains active; unsupported PCM produces 0x88890008 |
| Real Winamp | PASS in shared/session, shared/software and exclusive/PCM modes: loaded DLL hash, output selection, playback/position, independent mixer observations, pause, paused seek, resume, Stop, natural EOF and normal exit 0 without forced termination |

## Recovery evidence and boundaries

recovery_tests compiles the **same production engine sources** with
OUT_WASAPI_TESTING defined for that executable only. A deterministic injected
AUDCLNT_E_DEVICE_INVALIDATED and failed reopen attempts exercise the state
machine while actual WASAPI clients are stopped, unregistered, destroyed and
recreated. No successful audio API result is faked. Hooks are absent from the
release DLL and do not add plugin exports.

Both modes checked the following:

- Accepted PCM stays bounded and conserved as played + waiting + padding.
- A paused queue survives recovery; reconnection does not resume it.
- Playback time stays frozen during the missing-device interval and does not
  regress or exceed written time; the final accepted duration is exact.
- Pause/resume and Flush respond while waiting. A seek discards pre-seek PCM and
  preserves pause and its new time base.
- Close wakes and cancels the retry wait; the test requires completion below
  500 ms in this non-hung-driver scenario.
- autoRecover=false produces an error, zero capacity/not-playing and no retries.
- Recovery keeps the access/volume policy and clears the current error on success.
- Twelve additional paused recovery/drain/Close cycles showed no handle growth:
  shared 214 -> 214; exclusive 212 -> 194. Counts are individual observations,
  not universal leak-freedom guarantees.

The shared playing-recovery run retained 2400 unconfirmed frames for replay;
the exclusive run retained 3072. These are cumulative replay-candidate counts
for those runs, not a measured audible repetition length or discarded frames.
Exclusive PCM succeeded at 44100 Hz, stereo, 16-bit on the tested endpoint.
Other exclusive sample rates/bit depths are hardware-dependent and are not
advertised as automatically converted. The unsupported 12345 Hz, 8-channel,
8-bit test was rejected; no shared fallback occurred.

## Actual Winamp mixer and EOF observations

A read-only separate Core Audio probe observed the disposable Winamp process:

| Requested Winamp volume | Shared/session level | Shared/software level | Exclusive session level (not applied to its audio) |
| --- | --- | --- | --- |
| 64 / 255 | 25.098% | 100% | 100% |
| 128 / 255 | 50.1961% | 100% | 100% |
| 255 / 255 | 100% | 100% | 100% |
| 0 / 255 | 0% | 100% | 100% |
| 100 / 255 while paused | 39.2157% | 100% | 100% |

Endpoint/master level stayed at 52%; its mute and session mute were not changed
by Winamp callbacks. In exclusive mode PCM applies volume instead; a stationary
Windows session slider is intentional. No endpoint-volume API is used by the
plugin in any mode.

The four-second silent track reached natural EOF in 4037 ms in shared/session,
4059 ms in shared/software and 4039 ms in exclusive. Last sampled positions were
3992, 3982 and 4000 ms respectively. Individual wall-clock measurements are not
fixed acceptance constants; EOF can occur between separate IPC queries.

The [default shared settings screenshot](settings-playing.png) and
[exclusive settings screenshot](settings-exclusive.png) were visually inspected.
The former shows recovery checked and exclusive unchecked. The latter shows
exclusive checked, the session-volume option disabled, and active exclusive
format/buffering diagnostics. An initial exclusive capture ran before WM_PAINT
and was black; the harness was corrected to wait for painting and the successful
replacement inspected. Actual Winamp playback screenshots were also inspected.

## Reproduction and local evidence

See [README.md](../README.md) for commands. build.cmd runs core, notification and
ABI tests only. Audio, volume, recovery, GUI and real Winamp tests are explicit
opt-in checks requiring an active render endpoint. Exclusive tests temporarily
reserve it and can block other sounds. Tests use silence, not music fixtures.

Raw logs/screenshots remain local under the Git-ignored test-results directory:

- build-recovery-01.log: initial PROPERTYKEY linker failure; corrected with the
  SDK GUID-definition translation unit before successful builds.
- build-recovery-02.log through build-recovery-04.log: successful VS2022 builds.
- recovery-01-shared.log, recovery-01-exclusive.log: injected real-client recovery.
- recovery-01-audio.log, recovery-01-audio-software.log, recovery-01-missing-device.log.
- recovery-01-session-volume.log, recovery-01-software-volume.log.
- recovery-01-ui and recovery-01-ui-software: shared GUI reports/screenshots.
- recovery-02-exclusive-dll.log and its directory: release DLL exclusive GUI and
  transport checks after the paint-wait correction.
- winamp-recovery-01.log, winamp-recovery-software-01.log and
  winamp-exclusive-01.log: final real-host runs.
- symbols-recovery-stage.json: final x86 imports, SHA-256 and PDB GUID/age match.

Earlier 1.0/1.1 and /MT logs remain historical evidence only. Failed shell
invocations and corrected intermediate failures are not counted as passing tests.

## Limits of the evidence

Only one physical/default endpoint was exercised. No Windows default device or
format was actually changed during playback, and no physical hotplug, USB/HDMI
removal, Bluetooth reconnection, audio-service restart, arbitrary driver hang,
long-duration soak or captured-output/listening test was performed. Notification
filtering and deterministic loss/retry injection are **not a substitute** for
those hardware tests. Real Winamp runs verify normal transport in each mode;
they do not inject recovery into Winamp's release DLL.

Sudden invalidation can replay a short unconfirmed fragment and introduce a gap;
seamless/no-repeat hotplug is not guaranteed. There is no ASIO, encoded
passthrough or cross-track gapless guarantee. Shared endpoint mix is not proof
of physical DAC format; exclusive format acceptance is not an audio-quality or
DAC bit-purity certification. No Windows-to-Winamp slider synchronization is
implemented. Winamp and its source/license are not part of this plugin delivery.
