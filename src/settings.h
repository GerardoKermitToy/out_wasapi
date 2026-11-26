// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 out_wasapi contributors. See LICENSE.
#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace wasapi {
struct Settings {
    std::wstring deviceId; // Empty = follow the default multimedia endpoint.
    int queueMs = 500;
    int prebufferMs = 30;
    int deviceBufferMs = 50;
    bool sessionVolume = true; // Shared mode only; exclusive always uses PCM gain.
    bool autoRecover = true;
    bool exclusive = false;
};
struct DeviceInfo { std::wstring id, name; };
void InitializeSettings(HWND host);
Settings GetSettings();
std::wstring SettingsPath();
bool SaveSettings(const Settings& settings, std::wstring& error);
std::vector<DeviceInfo> EnumerateDevices(std::wstring& error);
std::wstring ErrorText(const wchar_t* operation, HRESULT hr);
} // namespace wasapi
