// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 out_wasapi contributors. See LICENSE.
#include "settings.h"
#include "winamp_output_abi.h"
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <shlobj.h>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace wasapi {
namespace {
std::mutex settingsMutex;
Settings settings;
std::wstring iniPath;
constexpr auto section = L"out_wasapi";
std::wstring ReadString(const wchar_t* key) {
    wchar_t buf[4096]{};
    GetPrivateProfileStringW(section, key, L"", buf, static_cast<DWORD>(std::size(buf)), iniPath.c_str());
    return buf;
}
}
std::wstring ErrorText(const wchar_t* operation, HRESULT hr) {
    wchar_t* message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, static_cast<DWORD>(hr), 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wostringstream s;
    s << operation << L": 0x" << std::hex << std::setw(8) << std::setfill(L'0') << static_cast<unsigned long>(hr);
    if (message) { s << L" — " << message; LocalFree(message); }
    auto result = s.str();
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) result.pop_back();
    return result;
}
void InitializeSettings(HWND host) {
    std::lock_guard<std::mutex> lock(settingsMutex);
    if (!iniPath.empty()) return;
    if (host && IsWindow(host)) {
        const auto result = SendMessageW(host, WM_WA_IPC, 0, IPC_GETINIFILEW);
        if (static_cast<UINT_PTR>(result) > 65535) {
            const auto hostIni = reinterpret_cast<const wchar_t*>(result);
            iniPath = (std::filesystem::path(hostIni).parent_path() / L"out_wasapi.ini").wstring();
        }
    }
    if (iniPath.empty()) {
        wchar_t* local = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &local))) {
            const auto dir = std::filesystem::path(local) / L"out_wasapi";
            CoTaskMemFree(local);
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            iniPath = (dir / L"settings.ini").wstring();
        }
    }
    if (iniPath.empty()) return; // Defaults remain usable; Save reports the failure.
    settings.deviceId = ReadString(L"device");
    settings.queueMs = std::clamp(static_cast<int>(GetPrivateProfileIntW(section, L"queue_ms", 500, iniPath.c_str())), 100, 2000);
    settings.prebufferMs = std::clamp(static_cast<int>(GetPrivateProfileIntW(section, L"prebuffer_ms", 30, iniPath.c_str())), 0, settings.queueMs);
    settings.deviceBufferMs = std::clamp(static_cast<int>(GetPrivateProfileIntW(section, L"device_buffer_ms", 50, iniPath.c_str())), 10, 200);
    settings.sessionVolume = GetPrivateProfileIntW(section, L"session_volume", 1, iniPath.c_str()) != 0;
    settings.autoRecover = GetPrivateProfileIntW(section, L"auto_recover", 1, iniPath.c_str()) != 0;
    settings.exclusive = GetPrivateProfileIntW(section, L"exclusive", 0, iniPath.c_str()) != 0;
}
Settings GetSettings() {
    std::lock_guard<std::mutex> lock(settingsMutex);
    return settings;
}
std::wstring SettingsPath() {
    std::lock_guard<std::mutex> lock(settingsMutex);
    return iniPath;
}
bool SaveSettings(const Settings& s, std::wstring& error) {
    if (s.queueMs < 100 || s.queueMs > 2000 || s.prebufferMs < 0 || s.prebufferMs > s.queueMs ||
        s.deviceBufferMs < 10 || s.deviceBufferMs > 200) {
        error = L"Queue must be 100–2000 ms, prebuffer 0–queue, device buffer 10–200 ms.";
        return false;
    }
    std::lock_guard<std::mutex> lock(settingsMutex);
    if (iniPath.empty()) { error = L"No writable settings location is available."; return false; }
    // Commit all playback settings in one section write.
    std::wstring data = L"device=" + s.deviceId;
    data.push_back(0);
    for (const auto& item : {L"queue_ms=" + std::to_wstring(s.queueMs),
                            L"prebuffer_ms=" + std::to_wstring(s.prebufferMs),
                            L"device_buffer_ms=" + std::to_wstring(s.deviceBufferMs),
                            L"session_volume=" + std::to_wstring(s.sessionVolume ? 1 : 0),
                            L"auto_recover=" + std::to_wstring(s.autoRecover ? 1 : 0),
                            L"exclusive=" + std::to_wstring(s.exclusive ? 1 : 0)}) {
        data += item;
        data.push_back(0);
    }
    data.push_back(0);
    if (!WritePrivateProfileSectionW(section, data.c_str(), iniPath.c_str())) {
        error = ErrorText(L"Save settings", HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }
    settings = s;
    return true;
}
std::vector<DeviceInfo> EnumerateDevices(std::wstring& error) {
    std::vector<DeviceInfo> result;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) { error = ErrorText(L"COM", init); return result; }
    {
        using Microsoft::WRL::ComPtr;
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDeviceCollection> collection;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&enumerator));
        if (SUCCEEDED(hr)) hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
        UINT count = 0;
        if (SUCCEEDED(hr)) hr = collection->GetCount(&count);
        if (FAILED(hr)) error = ErrorText(L"Enumerate playback devices", hr);
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> device;
            ComPtr<IPropertyStore> properties;
            LPWSTR id = nullptr;
            if (FAILED(collection->Item(i, &device))) continue;
            if (FAILED(device->GetId(&id))) continue;
            DeviceInfo info{id, id};
            CoTaskMemFree(id);
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
                PROPVARIANT name;
                PropVariantInit(&name);
                if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR)
                    info.name = name.pwszVal;
                PropVariantClear(&name);
            }
            result.push_back(std::move(info));
        }
    } // Release COM objects before uninitializing this thread.
    if (SUCCEEDED(init)) CoUninitialize();
    return result;
}
} // namespace wasapi
