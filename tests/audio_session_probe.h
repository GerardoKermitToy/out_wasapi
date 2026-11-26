// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 out_wasapi contributors. See LICENSE.
#pragma once
#include <windows.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <sstream>
#include <stdexcept>

// Test-only observer. The plugin never uses endpoint/master volume APIs.
namespace test_audio {
inline void Require(HRESULT hr, const char* operation) {
    if (SUCCEEDED(hr)) return;
    std::ostringstream message;
    message << operation << ": 0x" << std::hex << static_cast<unsigned long>(hr);
    throw std::runtime_error(message.str());
}
class Apartment {
public:
    Apartment() { Require(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "Test COM initialization"); }
    ~Apartment() { CoUninitialize(); }
    Apartment(const Apartment&) = delete;
    Apartment& operator=(const Apartment&) = delete;
};
class SessionProbe {
    Apartment apartment_; // Outlives all COM interfaces below.
public:
    Microsoft::WRL::ComPtr<IAudioSessionManager2> manager;
    Microsoft::WRL::ComPtr<ISimpleAudioVolume> volume;
    Microsoft::WRL::ComPtr<IAudioEndpointVolume> endpoint;
    explicit SessionProbe(DWORD processId) {
        using Microsoft::WRL::ComPtr;
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> device;
        Require(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&enumerator)), "Create test endpoint enumerator");
        Require(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device), "Get test default endpoint");
        Require(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr,
                                 reinterpret_cast<void**>(endpoint.GetAddressOf())), "Observe endpoint volume");
        Require(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_INPROC_SERVER, nullptr,
                                 reinterpret_cast<void**>(manager.GetAddressOf())), "Observe audio sessions");
        ComPtr<IAudioSessionEnumerator> sessions;
        Require(manager->GetSessionEnumerator(&sessions), "Enumerate audio sessions");
        int count = 0;
        Require(sessions->GetCount(&count), "Count audio sessions");
        for (int i = 0; i < count; ++i) {
            ComPtr<IAudioSessionControl> control;
            ComPtr<IAudioSessionControl2> identity;
            Require(sessions->GetSession(i, &control), "Read audio session");
            Require(control.As(&identity), "Read session identity");
            DWORD owner = 0;
            const HRESULT result = identity->GetProcessId(&owner);
            if (result != S_OK || owner != processId) continue;
            AudioSessionState state = AudioSessionStateExpired;
            Require(control->GetState(&state), "Read audio session state");
            if (state == AudioSessionStateExpired) continue;
            if (volume) throw std::runtime_error("Ambiguous probe: process has multiple audio sessions");
            Require(control.As(&volume), "Observe process session volume");
        }
        if (!volume) throw std::runtime_error("No audio session found for the requested process on the default endpoint");
    }
    float Level() const {
        float value = 0.0f;
        Require(volume->GetMasterVolume(&value), "Read session volume");
        return value;
    }
    bool Muted() const {
        BOOL value = FALSE;
        Require(volume->GetMute(&value), "Read session mute");
        return value != FALSE;
    }
    float EndpointLevel() const {
        float value = 0.0f;
        Require(endpoint->GetMasterVolumeLevelScalar(&value), "Read endpoint master volume");
        return value;
    }
    bool EndpointMuted() const {
        BOOL value = FALSE;
        Require(endpoint->GetMute(&value), "Read endpoint master mute");
        return value != FALSE;
    }
};
} // namespace test_audio
