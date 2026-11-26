// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 out_wasapi contributors. See LICENSE.
#include "audio_notifications.h"
#include <wrl/client.h>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
}
int main() {
    try {
        using wasapi::AudioNotifications;
        Microsoft::WRL::ComPtr<AudioNotifications> selected, following;
        selected.Attach(new AudioNotifications(false));
        following.Attach(new AudioNotifications(true));
        Check(selected->Event() && following->Event(), "notification event creation");
        selected->Target(L"chosen"); following->Target(L"chosen");
        selected->OnDefaultDeviceChanged(eRender, eMultimedia, L"other");
        following->OnDefaultDeviceChanged(eCapture, eMultimedia, L"other");
        following->OnDefaultDeviceChanged(eRender, eCommunications, L"other");
        selected->OnDeviceRemoved(L"other");
        selected->OnPropertyValueChanged(L"other", PKEY_AudioEngine_DeviceFormat);
        selected->OnSimpleVolumeChanged(0.5f, FALSE, nullptr);
        Check(!selected->Take() && !following->Take(), "unrelated device/role/volume triggered recovery");
        following->OnDefaultDeviceChanged(eRender, eMultimedia, nullptr);
        Check(WaitForSingleObject(following->Event(), 0) == WAIT_OBJECT_0 && following->Take() == AudioNotifications::Default,
              "default removal was not signaled");
        selected->OnPropertyValueChanged(L"chosen", PKEY_AudioEngine_DeviceFormat);
        selected->OnPropertyValueChanged(L"chosen", PKEY_AudioEngine_OEMFormat);
        selected->OnDeviceStateChanged(L"chosen", DEVICE_STATE_DISABLED);
        selected->OnSessionDisconnected(DisconnectReasonFormatChanged);
        Check(selected->Take() == (AudioNotifications::Format | AudioNotifications::Device | AudioNotifications::Disconnected),
              "notification coalescing/filtering");
        Check(!selected->Take(), "notification was not consumed");
        selected->Target(L"replacement");
        selected->OnDeviceRemoved(L"chosen");
        Check(!selected->Take(), "old endpoint triggered recovery after routing");
        selected->OnDeviceAdded(L"replacement");
        Check(selected->Take() == AudioNotifications::Device, "selected device return ignored");
        Microsoft::WRL::ComPtr<IAudioSessionEvents> session;
        Check(SUCCEEDED(selected.As(&session)), "session callback QueryInterface");
        selected.Reset(); // Callback reference keeps its event and state alive.
        Check(SUCCEEDED(session->OnSessionDisconnected(DisconnectReasonDeviceRemoval)), "callback lifetime");
        std::cout << "NOTIFICATIONS_PASS: endpoint/role filtering, format/disconnect, coalescing, callback lifetime\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "NOTIFICATIONS_FAIL: " << e.what() << '\n'; return 1; }
}
