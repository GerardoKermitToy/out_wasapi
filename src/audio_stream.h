// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 out_wasapi contributors. See LICENSE.
#pragma once
#include "audio_notifications.h"
#include "pcm.h"
#include "settings.h"
#include <audioclient.h>
#include <wrl/client.h>
#include <memory>
#include <vector>

namespace wasapi {
struct AudioError { const wchar_t* operation; HRESULT code; };
inline void CheckAudio(HRESULT code, const wchar_t* operation) {
    if (FAILED(code)) throw AudioError{operation, code};
}
bool RecoverableAudioError(HRESULT code) noexcept;
struct AudioHandleCloser { void operator()(void* handle) const noexcept { if (handle) CloseHandle(handle); } };
using AudioHandle = std::unique_ptr<void, AudioHandleCloser>;

// Worker-thread-only resources. Initialize through a unique_ptr so partial
// initialization also unregisters callbacks and releases COM on that thread.
struct AudioStream {
    AudioHandle event;
    Microsoft::WRL::ComPtr<AudioNotifications> notifications;
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    Microsoft::WRL::ComPtr<IMMDevice> device;
    Microsoft::WRL::ComPtr<IAudioClient> client;
    Microsoft::WRL::ComPtr<IAudioRenderClient> output;
    Microsoft::WRL::ComPtr<IAudioClock> clock;
    Microsoft::WRL::ComPtr<ISimpleAudioVolume> sessionVolume;
    Microsoft::WRL::ComPtr<IAudioSessionControl> session;
    bool endpointRegistered = false, sessionRegistered = false, started = false;
    UINT32 frames = 0;
    UINT64 frequency = 0;
    REFERENCE_TIME latency = 0, period = 0;
    DWORD pollMs = 5;
    WAVEFORMATEXTENSIBLE format{};
    std::wstring deviceId, deviceName, mixFormat;
    std::vector<std::uint8_t> packet;
    ~AudioStream();
    void Initialize(const Settings& settings, const WAVEFORMATEXTENSIBLE& input, AudioNotifications* listener);
};
} // namespace wasapi
