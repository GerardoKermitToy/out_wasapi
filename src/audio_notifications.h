// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 out_wasapi contributors. See LICENSE.
#pragma once
#include <windows.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <atomic>
#include <mutex>
#include <string>

namespace wasapi {
// Callbacks only filter and signal. No stream COM calls, host calls, or pointers
// to Engine. The notification event lives as long as the final callback reference.
class AudioNotifications final : public IMMNotificationClient, public IAudioSessionEvents {
public:
    enum Change : unsigned { Device = 1, Format = 2, Default = 4, Disconnected = 8 };
    explicit AudioNotifications(bool followDefault) : followDefault_(followDefault) {
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    HANDLE Event() const noexcept { return event_; }
    void Target(const std::wstring& id) { std::lock_guard<std::mutex> lock(mutex_); target_ = id; }
    unsigned Take() noexcept { return pending_.exchange(0); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient))
            *object = static_cast<IMMNotificationClient*>(this);
        else if (iid == __uuidof(IAudioSessionEvents)) *object = static_cast<IAudioSessionEvents*>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (followDefault_ && flow == eRender && role == eMultimedia) Signal(Default);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR id) override { return DeviceChange(id); }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) override { return DeviceChange(id); }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD) override { return DeviceChange(id); }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR id, const PROPERTYKEY key) override {
        if ((IsEqualPropertyKey(key, PKEY_AudioEngine_DeviceFormat) ||
             IsEqualPropertyKey(key, PKEY_AudioEngine_OEMFormat)) && Matches(id)) Signal(Format);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnSessionDisconnected(AudioSessionDisconnectReason) override {
        Signal(Disconnected);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDisplayNameChanged(LPCWSTR, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnIconPathChanged(LPCWSTR, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSimpleVolumeChanged(float, BOOL, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnChannelVolumeChanged(DWORD, float[], DWORD, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnGroupingParamChanged(LPCGUID, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnStateChanged(AudioSessionState) override { return S_OK; }
private:
    ~AudioNotifications() { if (event_) CloseHandle(event_); }
    bool Matches(LPCWSTR id) {
        std::lock_guard<std::mutex> lock(mutex_);
        return id && target_ == id;
    }
    void Signal(unsigned change) noexcept { pending_.fetch_or(change); if (event_) SetEvent(event_); }
    HRESULT DeviceChange(LPCWSTR id) { if (Matches(id)) Signal(Device); return S_OK; }
    std::atomic<ULONG> references_{1};
    std::atomic<unsigned> pending_{0};
    HANDLE event_ = nullptr;
    const bool followDefault_;
    std::mutex mutex_;
    std::wstring target_;
};
} // namespace wasapi
