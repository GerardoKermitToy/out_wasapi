# Design and invariants

This is a new implementation of a Winamp output module, not a fork of the
Winamp source tree. Only the public binary interface is restated locally.
Windows SDK COM/audio declarations come from the Windows SDK, not Winamp.

## Audio path

Winamp integer PCM -> bounded retained frame ring -> WASAPI client -> endpoint.

* A dedicated MTA thread creates, uses and releases all stream COM interfaces.
  Its COM initialization is balanced on that same thread. No loader-lock work.
* Host callbacks touch only bounded memory and synchronized scalar/status state.
  Write never waits for the device and rejects a whole write when full.
* Shared mode is event-driven and explicitly requests Windows' sample-rate and
  channel conversion. Source/client PCM and the endpoint mix are reported apart.
* Exclusive mode is opt-in, uses the exact input PCM format, and bypasses the
  shared audio engine. It uses timer-driven variable-size packets, not exclusive
  event-mode ping-pong packets. This permits exact short tails without synthetic
  silence and uses the documented exclusive GetCurrentPadding semantics.
* Exclusive format support is checked before Initialize. For mono/stereo, an
  equivalent legacy PCM descriptor is tried if the extensible descriptor is
  rejected; samples, rate, channels, bit depth and block alignment do not change.
  Unsupported formats, exclusive permission denial and initial device contention
  fail Open with a diagnostic. There is no implicit shared-mode fallback.
* The requested device buffer is at least the driver's minimum period; the actual
  buffer and latency are read back. Active exclusive streams are serviced at
  half the default period, clamped to 1–5 ms. Shared streams use audio events with
  a 10 ms fallback. Both use MMCSS Audio when available.
* Pause stops the client and preserves PCM and padding. Seek stops/resets it,
  clears retained PCM and counters, preserves pause, and rebases time.
* Exclusive ownership lasts until Close or recovery releases the client,
  including while paused. Changing focus does not release Winamp's device.
  Exclusive playback can prevent other applications/system sounds from playing.

## Retained PCM and time

The frame ring retains **all accepted but not confirmed played raw PCM**,
including the source copy of endpoint padding. Rendering peeks beyond the
in-flight prefix, applies current gain/pan to a reusable packet, and submits it.
Only a successful padding/clock observation retires confirmed frames. Thus:

```
played <= submission_frontier <= accepted
accepted - played = retained_frames = software_waiting_frames + padding
retained_frames <= configured_ring_capacity
```

The configured queue is now a bound on total retained source PCM, not an extra
allocation on top of a replay cache. Queue capacity/backpressure remains bounded
through recovery. Diagnostics show the unsubmitted suffix as Software queue and
the in-flight prefix as Device padding; those numbers are not double-counted.

Written time counts accepted frames. Played time uses confirmed source frames,
not silence or free capacity. Both use 64-bit counters and saturate the public
32-bit millisecond result. IsPlaying drains retained PCM, starts a final queue
below prebuffer, and does not falsely report EOF while recovery is pending.

## Automatic recovery

`auto_recover=1` is the default, including older profiles without the key.
`exclusive=0` is the default. All settings are snapshotted at Open; Apply changes
only the next playback start, not an active stream or an in-progress recovery.

An IMMNotificationClient filters default changes to render/eMultimedia and only
follows them when the configuration selects the default endpoint. Device/state
and AudioEngine format-property notifications are filtered to the active
endpoint. IAudioSessionEvents handles session disconnection. Callbacks coalesce
flags and signal their own lifetime-managed event: they never touch Engine,
rebuild a stream, call the host or release stream COM interfaces.

The render thread unregisters callbacks, releases the old stream, reacquires the
configured endpoint and recreates its client, services, event, clock, format and
latency snapshot. Shared and exclusive access policies are preserved. A fixed
endpoint is never silently replaced by a default endpoint. Where possible the
old stream is stopped and its padding sampled before release. Unconfirmed raw
PCM is retained and its submission cursor rewound, not its accepted/played time.

After a successful initial Open, device/resource invalidation, endpoint creation
failure, service unavailability and device contention trigger recovery. The first
reopen is immediate; transient failures retry at **500 ms**, without a busy loop,
until success or Stop/Close. Initial Open still fails promptly if no device can
be opened. Unsupported PCM, exclusive-mode denial and programming errors fail
closed rather than retry forever. The current error clears on successful reopen;
recovery completions, attempts and replay-boundary frames are reported.

Pause and Flush are acknowledged even when no device is available. Flush
explicitly discards pre-seek PCM and rebases time; new PCM can fill the bounded
ring while recovery waits. Close wakes/cancels the wait. A driver call itself
can still block: no promise is made to interrupt arbitrary driver hangs.

A sudden invalidation can occur between the last successful padding observation
and actual playback. Replaying that unconfirmed prefix can repeat a short piece
of audio; physically gapless/no-repeat hotplug is **not guaranteed**. There is no
arbitrary discard of accepted PCM. Recovery is not source-decoder restart or a
Winamp seek. With recovery disabled, errors fail closed as in version 1.1, and
changes of the default endpoint apply only on the next Open.

## Volume control

`Use Windows volume mixer (Winamp session)` defaults to on, including when an
older INI has no `session_volume` key. Explicit `session_volume=0` selects software
volume. The option is unavailable in the form while exclusive is selected, but
its saved value is preserved for the next shared-mode stream.

In shared session mode, the render thread obtains ISimpleAudioVolume from its
IAudioClient and maps Winamp volume to `volume / 255.0f`. The default per-process
session belongs to Winamp. PCM volume is unity; balance remains software PCM
attenuation. In software mode and in exclusive mode, PCM handles both volume
and balance. Exclusive session-volume controls have no effect, so the plugin
does not use them. No mode changes endpoint/master volume, another session,
or Windows mute; this is not an IAudioEndpointVolume implementation.

Host callbacks coalesce to the latest requested volume and wake the worker even
while paused/empty. They never call COM. Negative sentinels are ignored; repeated
positive values deliberately reapply Winamp's choice after external mixer edits.
Observed shared-session level/mute refresh at 250 ms. External edits persist
until a host volume callback, a new Open or recovery rebuilds the stream. There
is no Windows-to-Winamp slider synchronization. Close does not reset session
volume. In shared software mode, the independent Windows session level is an
additional multiplier; in exclusive mode it does not control the stream.

## Configuration and visual direction

Native Windows dialog; no webview, MFC, Wasabi or custom theme dependency.
Use system colors, respecting user theme/high contrast, and Segoe UI 9pt.
A left-aligned form contains endpoint/buffering, Windows session volume,
automatic recovery and exclusive-access checkboxes. A read-only copyable report
shows active settings separately from the next-start form. No decorative cards,
meters or charts. Keyboard access and 250 ms dialog refresh are retained.

## Build and test

CMake + Visual Studio C++ + Windows SDK only; x86, MultiThreadedDLL (/MD) Release
runtime (/MDd in Debug), classic undecorated exports and full linker PDB.
Device-independent tests cover ring/PCM/time and notification filtering/lifetime.
The native DLL host exercises real WASAPI/GUI/session controls. A separate
recovery_tests executable compiles the same engine with test-only disconnection
and unavailable-reopen injection; those hooks are absent from the release DLL.
It exercises actual client teardown/recreation, not simulated successful COM.
A disposable Winamp copy validates real host loading and playback. The original
Winamp checkout and existing Winamp delivery packages are not modified.
