// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 out_wasapi contributors. See LICENSE.
#include "audio_stream.h"
#include <functiondiscoverykeys_devpkey.h>
#include <algorithm>

namespace wasapi {
namespace {
struct TaskFree { void operator()(WAVEFORMATEX* p) const noexcept { CoTaskMemFree(p); } };
}
bool RecoverableAudioError(HRESULT code) noexcept {
    return code == AUDCLNT_E_DEVICE_INVALIDATED || code == AUDCLNT_E_RESOURCES_INVALIDATED ||
           code == AUDCLNT_E_SERVICE_NOT_RUNNING || code == AUDCLNT_E_DEVICE_IN_USE ||
           code == AUDCLNT_E_ENDPOINT_CREATE_FAILED || code == RPC_E_DISCONNECTED ||
           code == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE) || code == HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}
AudioStream::~AudioStream() {
    // Never unregister from inside a notification callback.
    if (sessionRegistered) session->UnregisterAudioSessionNotification(notifications.Get());
    if (endpointRegistered) enumerator->UnregisterEndpointNotificationCallback(notifications.Get());
    if (started && client) client->Stop();
    // COM fields are released before the WASAPI event handle (declaration order).
}
void AudioStream::Initialize(const Settings& settings, const WAVEFORMATEXTENSIBLE& input, AudioNotifications* listener) {
    CheckAudio(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&enumerator)), L"Create endpoint enumerator");
    if (listener) {
        notifications = listener;
        listener->Target(settings.deviceId);
        CheckAudio(enumerator->RegisterEndpointNotificationCallback(listener), L"Register device notifications");
        endpointRegistered = true;
    }
    CheckAudio(settings.deviceId.empty()
        ? enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device)
        : enumerator->GetDevice(settings.deviceId.c_str(), &device),
        L"Open playback device (check connection / choose another device)");
    DWORD state = 0;
    CheckAudio(device->GetState(&state), L"Read device state");
    if (!(state & DEVICE_STATE_ACTIVE)) throw AudioError{L"Selected device is not active", AUDCLNT_E_DEVICE_INVALIDATED};
    LPWSTR id = nullptr;
    CheckAudio(device->GetId(&id), L"Read endpoint ID");
    struct FreeId { LPWSTR value; ~FreeId() { CoTaskMemFree(value); } } freeId{id};
    deviceId = id;
    deviceName = L"Playback device";
    if (listener) listener->Target(deviceId);
    Microsoft::WRL::ComPtr<IPropertyStore> properties;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR)
            deviceName = value.pwszVal;
        PropVariantClear(&value);
    }
    CheckAudio(device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
                                reinterpret_cast<void**>(client.GetAddressOf())), L"Activate WASAPI");
    WAVEFORMATEX* rawMix = nullptr;
    const HRESULT mixResult = client->GetMixFormat(&rawMix);
    std::unique_ptr<WAVEFORMATEX, TaskFree> mix(rawMix);
    CheckAudio(mixResult, L"Get endpoint mix format");
    mixFormat = DescribeFormat(mix.get());
    format = input;
    REFERENCE_TIME minimum = 0;
    CheckAudio(client->GetDevicePeriod(&period, &minimum), L"Get device period");
    if (settings.exclusive) {
        HRESULT supported = client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &format.Format, nullptr);
        // Some stereo drivers accept only the equivalent legacy PCM descriptor.
        // This changes no sample, rate, channel, bit depth, or block alignment.
        if (supported == AUDCLNT_E_UNSUPPORTED_FORMAT && format.Format.nChannels <= 2) {
            format.Format.wFormatTag = WAVE_FORMAT_PCM;
            format.Format.cbSize = 0;
            supported = client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &format.Format, nullptr);
        }
        CheckAudio(supported == S_FALSE ? AUDCLNT_E_UNSUPPORTED_FORMAT : supported,
                   L"Exclusive PCM format unsupported (choose shared mode or a supported input format)");
    }
    const auto duration = (std::max)(static_cast<REFERENCE_TIME>(settings.deviceBufferMs) * 10000, minimum);
    // Exclusive timer-driven buffering permits exact partial packets and short
    // tails without adding/counting artificial silence. GetCurrentPadding is
    // defined for this mode. Shared mode retains its event-driven render path.
    const DWORD flags = settings.exclusive ? 0 : AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    CheckAudio(client->Initialize(settings.exclusive ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED,
        flags, duration, 0, &format.Format, nullptr), settings.exclusive
        ? L"Initialize exclusive stream (device busy, exclusive disabled, or unsupported format; no shared fallback)"
        : L"Initialize shared stream");
    if (!settings.exclusive) {
        event.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (!event) throw AudioError{L"Create audio event", HRESULT_FROM_WIN32(GetLastError())};
        CheckAudio(client->SetEventHandle(event.get()), L"Set WASAPI event");
    }
    CheckAudio(client->GetBufferSize(&frames), L"Get endpoint buffer size");
    if (!frames) throw AudioError{L"Empty endpoint buffer", E_UNEXPECTED};
    CheckAudio(client->GetService(IID_PPV_ARGS(&output)), L"Get render interface");
    CheckAudio(client->GetService(IID_PPV_ARGS(&clock)), L"Get audio clock");
    if (settings.sessionVolume && !settings.exclusive)
        CheckAudio(client->GetService(IID_PPV_ARGS(&sessionVolume)), L"Get Windows session volume");
    if (listener) {
        CheckAudio(client->GetService(IID_PPV_ARGS(&session)), L"Get audio session notifications");
        CheckAudio(session->RegisterAudioSessionNotification(listener), L"Register session notifications");
        sessionRegistered = true;
    }
    CheckAudio(clock->GetFrequency(&frequency), L"Get clock frequency");
    if (!frequency) throw AudioError{L"Zero clock frequency", E_UNEXPECTED};
    CheckAudio(client->GetStreamLatency(&latency), L"Get stream latency");
    pollMs = static_cast<DWORD>(std::clamp<REFERENCE_TIME>(period / 20000, 1, 5));
    packet.resize(static_cast<std::size_t>(frames) * input.Format.nBlockAlign);
}
} // namespace wasapi
