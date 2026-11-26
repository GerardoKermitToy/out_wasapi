# Regression checklist

The earlier read-only investigation of Winamp's original WASAPI 0.3 identified
these eleven groups. This repository implements a new engine rather than
patching or incorporating the original source.

| Original problem | New invariant / coverage |
| --- | --- |
| IsPlaying tests free capacity, truncating the tail | Queue + submitted-minus-played; native short-track and EOF tests; real host EOF test |
| Written time includes free buffer space | 64-bit accepted-frame count; zero-at-open, exact-duration and seek tests |
| Unchecked GetBuffer / ReleaseBuffer | Check every acquisition and submission; retain raw PCM until padding confirms playback; reset only its submission cursor on recovery |
| Write silently discards when full | Whole-block capacity check, return busy without changing accepted count; fill-while-paused test |
| No device / failed enumerator crashes Open | Validate HRESULT and interface before use; missing-endpoint native test |
| Uninitialized padding / clock, ignored transport errors | Initialize outputs, validate results and padding bounds; recover transient failures if enabled, otherwise fail closed with an error report |
| Flush starts playback while paused | Synchronous worker command, Stop + Reset, preserve pause; native and real host paused-seek tests |
| Broken negative/center/pre-open/reopen balance | Store integer pan independently; software attenuation; signed/center/mono/5.1 unit tests and pre-open GUI report |
| Enumerator/device/partial-open leaks | Scoped COM pointers and handles; repeated Open/Close handle-count test |
| Unbalanced or wrong-thread COM teardown | One MTA initialization per worker, paired teardown after all COM releases on that same thread |
| WASAPI service released on a different host callback thread | Host callbacks never own stream COM interfaces; worker does create/use/release |

Original Config was empty. The new native dialog displays source/client/mix
formats, endpoint identity, bounded queue, device padding, clock, times,
volume/balance and last error; the GUI integration test verifies live updates
and settings application without interrupting playback.

Runtime integration checks do not physically unplug devices or prove
leak-freedom for every driver. Version 1.2 adds device-independent notification
filtering/lifetime tests and test-only deterministic disconnection/reopen failure
injection into the same engine code, with real WASAPI clients recreated after
injection. The release DLL contains none of those injection hooks. Hardware
routing and audible-quality certification still require separate testing.
