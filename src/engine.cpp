// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 out_wasapi contributors. See LICENSE.
#include "engine.h"
#include "audio_stream.h"
#include <avrt.h>
#include <algorithm>
#include <memory>
#include <new>

namespace wasapi {
namespace {
struct MmcssScope {
    HANDLE handle = nullptr;
    ~MmcssScope() { if (handle) AvRevertMmThreadCharacteristics(handle); }
};
}
Engine& AudioEngine() { static Engine engine; return engine; }
Engine::~Engine() { Close(); }
void Engine::RefreshStateLocked() {
    if (status_.recovering) status_.state = L"Recovering / waiting for device";
    else if (FAILED(status_.lastError)) status_.state = L"Error";
    else if (!status_.open) status_.state = L"Closed";
    else if (status_.paused) status_.state = L"Paused";
    else if (status_.draining && queue_.Size()) status_.state = L"Draining";
    else if (status_.started) status_.state = L"Playing";
    else status_.state = L"Buffering / ready";
    // The ring retains raw PCM already submitted but not confirmed played.
    // Only the unsubmitted suffix is reported as the software waiting queue.
    const auto inFlight = status_.submitted - status_.played;
    status_.queueFrames = queue_.Size() - static_cast<std::size_t>(inFlight);
}
void Engine::Fail(const wchar_t* operation, HRESULT hr) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_.lastError = FAILED(hr) ? hr : E_FAIL;
    status_.error = ErrorText(operation, status_.lastError);
    status_.open = status_.started = status_.recovering = false;
    ready_ = true;
    RefreshStateLocked();
    changed_.notify_all();
}
int Engine::Open(int rate, int channels, int bits, const Settings& settings) {
    std::lock_guard<std::mutex> lifecycle(lifecycle_);
    CloseLocked();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int volume = status_.volume, pan = status_.pan;
        status_ = Snapshot{};
        status_.volume = volume;
        status_.pan = pan;
        status_.sessionVolume = settings.sessionVolume && !settings.exclusive;
        status_.autoRecover = settings.autoRecover;
        status_.exclusive = settings.exclusive;
        status_.state = L"Opening";
    }
    if (!ValidPcm(rate, channels, bits)) { Fail(L"Unsupported integer PCM (8/16/24/32-bit, 1–8 ch, 8–768 kHz)", E_INVALIDARG); return -1; }
    if (settings.queueMs < 100 || settings.queueMs > 2000 || settings.prebufferMs < 0 ||
        settings.prebufferMs > settings.queueMs || settings.deviceBufferMs < 10 || settings.deviceBufferMs > 200) {
        Fail(L"Invalid buffer settings", E_INVALIDARG); return -1;
    }
    try {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.input = MakeFormat(rate, channels, bits);
            const auto frames = (static_cast<std::size_t>(rate) * settings.queueMs + 999) / 1000;
            queue_.Reset(frames, status_.input.Format.nBlockAlign);
            status_.queueCapacity = frames;
            status_.prebufferMs = settings.prebufferMs;
            prebufferFrames_ = (static_cast<std::size_t>(rate) * settings.prebufferMs + 999) / 1000;
            stop_ = ready_ = pausePending_ = flushPending_ = false;
            volumePending_ = status_.sessionVolume;
            finished_ = false;
            command_ = acknowledged_ = 0;
#ifdef OUT_WASAPI_TESTING
            testDisconnect_ = false;
            testUnavailable_ = 0;
#endif
        }
        control_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!control_) { Fail(L"Create control event", HRESULT_FROM_WIN32(GetLastError())); return -1; }
        thread_ = std::thread(&Engine::Run, this, settings);
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [&] { return ready_ || finished_; });
        return status_.open ? status_.maximumLatencyMs : -1;
    } catch (const std::bad_alloc&) {
        Fail(L"Allocate audio queue", E_OUTOFMEMORY);
    } catch (...) {
        Fail(L"Start audio thread", E_FAIL);
    }
    if (control_ && !thread_.joinable()) { CloseHandle(control_); control_ = nullptr; }
    return -1;
}
void Engine::CloseLocked() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        if (control_) SetEvent(control_);
    }
    if (thread_.joinable()) thread_.join();
    if (control_) { CloseHandle(control_); control_ = nullptr; }
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.Clear();
    status_.submitted = status_.played;
    status_.open = status_.started = status_.paused = status_.draining = status_.recovering = false;
    status_.padding = 0;
    finished_ = true;
    RefreshStateLocked();
}
void Engine::Close() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_);
    CloseLocked();
}
int Engine::CanWrite() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!status_.open || flushPending_ || stop_) return 0;
    return static_cast<int>(queue_.Free() * status_.input.Format.nBlockAlign);
}
int Engine::Write(const void* data, int bytes) {
    // Only bounded memory copies and scalar state; no COM or driver waits here.
    std::lock_guard<std::mutex> lock(mutex_);
    if (!status_.open || stop_ || flushPending_ || bytes < 0 || (bytes && !data)) return 1;
    const auto align = status_.input.Format.nBlockAlign;
    if (bytes % align) return 1;
    const auto frames = static_cast<std::size_t>(bytes / align);
    if (!queue_.Push(data, frames)) return 1;
    status_.accepted += frames;
    status_.peakQueue = (std::max)(status_.peakQueue, queue_.Size());
    if (frames) status_.draining = false;
    RefreshStateLocked();
    SetEvent(control_);
    return 0;
}
int Engine::IsPlaying() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!status_.open || stop_) return 0;
    status_.draining = true;
    SetEvent(control_);
    RefreshStateLocked();
    // Do not cause a false EOF while the device is temporarily unavailable.
    return (queue_.Size() || status_.recovering) ? 1 : 0;
}
int Engine::Pause(bool pause) {
    std::lock_guard<std::mutex> lifecycle(lifecycle_);
    std::unique_lock<std::mutex> lock(mutex_);
    const int previous = status_.paused ? 1 : 0;
    if (!status_.open || previous == static_cast<int>(pause)) return previous;
    requestedPause_ = pause;
    pausePending_ = true;
    const auto seq = ++command_;
    SetEvent(control_);
    changed_.wait(lock, [&] { return acknowledged_ >= seq || finished_ || !status_.open; });
    return previous;
}
void Engine::Flush(int milliseconds) {
    std::lock_guard<std::mutex> lifecycle(lifecycle_);
    std::unique_lock<std::mutex> lock(mutex_);
    if (!status_.open) return;
    requestedSeek_ = (std::max)(0, milliseconds);
    flushPending_ = true;
    const auto seq = ++command_;
    SetEvent(control_);
    changed_.wait(lock, [&] { return acknowledged_ >= seq || finished_ || !status_.open; });
}
void Engine::SetVolume(int volume) {
    if (volume < 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    status_.volume = std::clamp(volume, 0, 255);
    if (status_.sessionVolume) {
        volumePending_ = true;
        if (status_.open) SetEvent(control_);
    }
}
void Engine::SetPan(int pan) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_.pan = std::clamp(pan, -128, 128);
}
Snapshot Engine::Status() {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}
int Engine::OutputTime() {
    std::lock_guard<std::mutex> lock(mutex_);
    return FramesToMs(status_.played, status_.input.Format.nSamplesPerSec, status_.baseMs);
}
int Engine::WrittenTime() {
    std::lock_guard<std::mutex> lock(mutex_);
    return FramesToMs(status_.accepted, status_.input.Format.nSamplesPerSec, status_.baseMs);
}
#ifdef OUT_WASAPI_TESTING
void Engine::TestDisconnect(unsigned unavailableAttempts) {
    std::lock_guard<std::mutex> lock(mutex_);
    testUnavailable_ = unavailableAttempts;
    testDisconnect_ = true;
    SetEvent(control_);
}
void Engine::TestAllowReconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    testUnavailable_ = 0;
    SetEvent(control_);
}
#endif
void Engine::Run(Settings settings) noexcept {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) {
        Fail(L"Audio thread COM initialization", com);
    } else {
        try {
            // Scope all COM objects before CoUninitialize on this same thread.
            const auto render = [&]() {
                SetThreadDescription(GetCurrentThread(), L"out_wasapi render");
                MmcssScope scheduling;
                DWORD task = 0;
                scheduling.handle = AvSetMmThreadCharacteristicsW(L"Audio", &task);
                Microsoft::WRL::ComPtr<AudioNotifications> notifications;
                notifications.Attach(new AudioNotifications(settings.deviceId.empty()));
                if (!notifications->Event()) throw AudioError{L"Create notification event", HRESULT_FROM_WIN32(GetLastError())};
                std::unique_ptr<AudioStream> stream;
                bool everOpened = false, forceStart = false, emptyEpisode = false;
                unsigned bufferErrors = 0;
                ULONGLONG nextAttempt = 0, nextSessionPoll = 0;
                const auto progress = [&]() {
                    UINT32 padding = 0;
                    CheckAudio(stream->client->GetCurrentPadding(&padding), L"Get current padding");
                    UINT64 position = 0;
                    CheckAudio(stream->clock->GetPosition(&position, nullptr), L"Read audio clock");
                    std::lock_guard<std::mutex> lock(mutex_);
                    const auto outstanding = status_.submitted - status_.played;
                    if (padding > stream->frames || padding > outstanding)
                        throw AudioError{L"Invalid device padding", E_UNEXPECTED};
                    const auto consumed = outstanding - padding;
                    queue_.Consume(static_cast<std::size_t>(consumed));
                    status_.played += consumed;
                    status_.padding = padding;
                    status_.clockPosition = position;
                    RefreshStateLocked();
                };
                const auto commands = [&]() {
                    bool doPause, doFlush, wantPause;
                    int seek;
                    std::uint64_t seq;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        doPause = pausePending_; doFlush = flushPending_;
                        wantPause = requestedPause_; seek = requestedSeek_; seq = command_;
                    }
                    if (!doPause && !doFlush) return;
                    if (stream) {
                        if (stream->started) {
                            CheckAudio(stream->client->Stop(), L"Stop stream for transport");
                            stream->started = false;
                        }
                        if (doFlush) CheckAudio(stream->client->Reset(), L"Reset stream for seek");
                        else progress();
                    }
                    // Transport still works when no device is available. Do not
                    // wait for reconnection to acknowledge pause or discard a seek.
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (doFlush) {
                        queue_.Clear();
                        status_.accepted = status_.submitted = status_.played = 0;
                        status_.padding = 0;
                        status_.baseMs = seek;
                        status_.clockPosition = 0;
                        status_.draining = false;
                        status_.peakQueue = 0;
                        emptyEpisode = forceStart = false;
                    }
                    if (doPause) status_.paused = wantPause;
                    status_.started = false;
                    pausePending_ = flushPending_ = false;
                    acknowledged_ = seq;
                    RefreshStateLocked();
                    changed_.notify_all();
                };
                for (;;) {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        if (stop_) break;
                    }
                    try {
                        commands();
                        const auto changes = notifications->Take();
                        if (stream && changes) {
                            const wchar_t* reason = (changes & AudioNotifications::Format) ? L"Endpoint format changed" :
                                (changes & AudioNotifications::Default) ? L"Default playback device changed" :
                                (changes & AudioNotifications::Disconnected) ? L"Audio session disconnected" : L"Playback device changed";
                            throw AudioError{reason, AUDCLNT_E_DEVICE_INVALIDATED};
                        }
#ifdef OUT_WASAPI_TESTING
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            if (stream && testDisconnect_) {
                                testDisconnect_ = false;
                                throw AudioError{L"Simulated device disconnection", AUDCLNT_E_DEVICE_INVALIDATED};
                            }
                        }
#endif
                        if (!stream) {
                            const auto now = GetTickCount64();
                            if (now < nextAttempt) {
                                HANDLE events[] = {control_, notifications->Event()};
                                const DWORD waited = WaitForMultipleObjects(2, events, FALSE, static_cast<DWORD>(nextAttempt - now));
                                if (waited == WAIT_FAILED) throw AudioError{L"Wait for device recovery", HRESULT_FROM_WIN32(GetLastError())};
                                continue;
                            }
                            {
                                std::lock_guard<std::mutex> lock(mutex_);
                                if (everOpened) ++status_.recoveryAttempts;
#ifdef OUT_WASAPI_TESTING
                                if (everOpened && testUnavailable_) {
                                    --testUnavailable_;
                                    throw AudioError{L"Simulated unavailable device", AUDCLNT_E_DEVICE_INVALIDATED};
                                }
#endif
                            }
                            WAVEFORMATEXTENSIBLE format;
                            { std::lock_guard<std::mutex> lock(mutex_); format = status_.input; }
                            auto opened = std::make_unique<AudioStream>();
                            opened->Initialize(settings, format, settings.autoRecover ? notifications.Get() : nullptr);
                            stream = std::move(opened);
                            {
                                std::lock_guard<std::mutex> lock(mutex_);
                                status_.deviceName = stream->deviceName;
                                status_.deviceId = stream->deviceId;
                                status_.mixFormat = stream->mixFormat;
                                status_.endpointFrames = stream->frames;
                                status_.latency = stream->latency;
                                status_.period = stream->period;
                                status_.clockFrequency = stream->frequency;
                                status_.mmcss = scheduling.handle != nullptr;
                                status_.maximumLatencyMs = FramesToMs(queue_.Capacity() + stream->frames, format.Format.nSamplesPerSec)
                                    + static_cast<int>((stream->latency + 9999) / 10000);
                                if (everOpened) ++status_.recoveries;
                                status_.recovering = false;
                                status_.lastError = S_OK;
                                status_.error.clear();
                                volumePending_ = status_.sessionVolume;
                                status_.sessionLevel = -1.0f;
                                status_.open = ready_ = true;
                                RefreshStateLocked();
                                changed_.notify_all();
                            }
                            forceStart = everOpened;
                            everOpened = true;
                            nextSessionPoll = 0;
                            emptyEpisode = false;
                            bufferErrors = 0;
                        }
                        if (stream->sessionVolume) {
                            int requested = -1;
                            {
                                std::lock_guard<std::mutex> lock(mutex_);
                                if (volumePending_) { requested = status_.volume; volumePending_ = false; }
                            }
                            if (requested >= 0) CheckAudio(stream->sessionVolume->SetMasterVolume(requested / 255.0f, nullptr), L"Set Windows session volume");
                            const auto now = GetTickCount64();
                            if (requested >= 0 || now >= nextSessionPoll) {
                                float level = 0.0f;
                                BOOL muted = FALSE;
                                CheckAudio(stream->sessionVolume->GetMasterVolume(&level), L"Read Windows session volume");
                                CheckAudio(stream->sessionVolume->GetMute(&muted), L"Read Windows session mute");
                                std::lock_guard<std::mutex> lock(mutex_);
                                status_.sessionLevel = level;
                                status_.sessionMuted = muted != FALSE;
                                nextSessionPoll = now + 250;
                            }
                        }
                        progress();
                        UINT32 padding;
                        std::size_t frames = 0;
                        int volume, pan;
                        bool paused;
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            paused = status_.paused;
                            padding = status_.padding;
                            const auto inFlight = static_cast<std::size_t>(status_.submitted - status_.played);
                            const auto waiting = queue_.Size() - inFlight;
                            if (!paused && (stream->started || padding || forceStart || status_.draining || queue_.Size() >= prebufferFrames_)) {
                                frames = (std::min)(waiting, static_cast<std::size_t>(stream->frames - padding));
                                if (frames) queue_.Peek(stream->packet.data(), frames, inFlight);
                            }
                            volume = status_.sessionVolume ? 255 : status_.volume;
                            pan = status_.pan;
                            if (stream->started && !padding && !emptyEpisode) {
                                if (!status_.draining) ++status_.underruns;
                                emptyEpisode = true;
                            }
                            if (padding) emptyEpisode = false;
                        }
                        if (frames) {
                            ApplyGain(stream->packet.data(), frames, stream->format, volume, pan);
                            BYTE* target = nullptr;
                            const HRESULT acquired = stream->output->GetBuffer(static_cast<UINT32>(frames), &target);
                            if (settings.exclusive && acquired == AUDCLNT_E_BUFFER_ERROR && ++bufferErrors < 100) {
                                // A temporarily unavailable exclusive packet must
                                // be retried, not consumed or reported as submitted.
                                WaitForSingleObject(control_, stream->pollMs);
                                continue;
                            }
                            if (acquired == AUDCLNT_E_BUFFER_ERROR)
                                throw AudioError{L"Repeated exclusive buffer errors", AUDCLNT_E_RESOURCES_INVALIDATED};
                            CheckAudio(acquired, L"Acquire render buffer");
                            bufferErrors = 0;
                            std::memcpy(target, stream->packet.data(), frames * stream->format.Format.nBlockAlign);
                            CheckAudio(stream->output->ReleaseBuffer(static_cast<UINT32>(frames), 0), L"Submit render buffer");
                            {
                                std::lock_guard<std::mutex> lock(mutex_);
                                // Retain the raw, pre-gain PCM until padding
                                // confirms it played. Recovery can resubmit it.
                                status_.submitted += frames;
                                ++status_.renderCalls;
                                status_.padding = padding + static_cast<UINT32>(frames);
                                RefreshStateLocked();
                            }
                            padding += static_cast<UINT32>(frames);
                        }
                        if (!paused && !stream->started && padding) {
                            CheckAudio(stream->client->Start(), L"Start stream");
                            stream->started = true;
                        } else if (stream->started && !padding) {
                            CheckAudio(stream->client->Stop(), L"Stop drained stream");
                            stream->started = false;
                        }
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            status_.started = stream->started;
                            RefreshStateLocked();
                        }
                        HANDLE events[] = {control_, notifications->Event(), stream->event.get()};
                        const DWORD count = stream->event ? 3 : 2;
                        const DWORD idle = stream->sessionVolume ? 250 : INFINITE;
                        const DWORD waitMs = stream->started ? (settings.exclusive ? stream->pollMs : 10) : idle;
                        if (WaitForMultipleObjects(count, events, FALSE, waitMs) == WAIT_FAILED)
                            throw AudioError{L"Wait for audio event", HRESULT_FROM_WIN32(GetLastError())};
                    } catch (const AudioError& error) {
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            if (stop_) break;
                        }
                        if (!everOpened || !settings.autoRecover || !RecoverableAudioError(error.code)) {
                            Fail(error.operation, error.code);
                            break;
                        }
                        const bool hadStream = stream != nullptr;
                        if (stream) {
                            // Freeze and sample the old stream if it still works;
                            // otherwise retain the last confirmed playback boundary.
                            if (!stream->started || SUCCEEDED(stream->client->Stop())) {
                                stream->started = false;
                                try { progress(); } catch (const AudioError&) {}
                            }
                            stream.reset();
                            notifications->Take(); // Reopening resolves the latest selection.
                        }
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            status_.replayedFrames += status_.submitted - status_.played;
                            status_.submitted = status_.played;
                            status_.padding = 0;
                            status_.clockPosition = status_.clockFrequency = 0;
                            status_.started = false;
                            status_.recovering = true;
                            status_.lastError = error.code;
                            status_.error = ErrorText(error.operation, error.code);
                            RefreshStateLocked();
                            changed_.notify_all();
                        }
                        // First reopen is immediate; unavailable/busy devices are
                        // retried at 500 ms, with command/Close wakeups in between.
                        nextAttempt = GetTickCount64() + (hadStream ? 0 : 500);
                    }
                }
            };
            render();
        } catch (const AudioError& error) { Fail(error.operation, error.code); }
          catch (const std::bad_alloc&) { Fail(L"Allocate audio resources", E_OUTOFMEMORY); }
          catch (...) { Fail(L"Unexpected audio thread failure", E_FAIL); }
        CoUninitialize();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.open = status_.started = status_.recovering = false;
        ready_ = finished_ = true;
        RefreshStateLocked();
        changed_.notify_all();
    }
}
} // namespace wasapi
