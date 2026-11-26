// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 out_wasapi contributors. See LICENSE.
#include "winamp_output_abi.h"
#include "resource.h"
#include "audio_session_probe.h"
#include <cmath>
#include <windows.h>
#include <objidl.h>
#include <algorithm>
// Windows SDK 19041 GDI+ headers use unqualified min/max with NOMINMAX.
namespace Gdiplus { using std::min; using std::max; }
#pragma warning(push)
#pragma warning(disable:4458) // Older SDK GDI+ headers shadow their own members.
#include <gdiplus.h>
#pragma warning(pop)
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
Out_Module* output = nullptr;
std::wstring hostIni;
std::filesystem::path evidence;
HMODULE library = nullptr;
HWND host = nullptr;
void Check(bool success, const char* message) {
    if (!success) throw std::runtime_error(message);
}
void WriteUtf8(const std::filesystem::path& path, const std::wstring& text) {
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(count, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), count, nullptr, nullptr);
    std::ofstream stream(path, std::ios::binary);
    stream << utf8;
}
LRESULT CALLBACK HostProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_WA_IPC && l == IPC_GETINIFILEW) return reinterpret_cast<LRESULT>(hostIni.c_str());
    return DefWindowProcW(window, message, w, l);
}
std::vector<char> Silence(int rate, int channels, int bits, int ms) {
    const auto bytes = static_cast<std::size_t>(rate) * ms / 1000 * channels * bits / 8;
    return std::vector<char>(bytes, bits == 8 ? static_cast<char>(128) : 0);
}
void Feed(const std::vector<char>& samples, int align) {
    const auto deadline = GetTickCount64() + 8000;
    std::size_t position = 0;
    while (position < samples.size()) {
        Check(GetTickCount64() < deadline, "feed timeout");
        auto count = (std::min)({samples.size() - position, static_cast<std::size_t>(8192 - 8192 % align),
                                 static_cast<std::size_t>((std::max)(0, output->CanWrite()))});
        count -= count % align;
        if (!count) { Sleep(1); continue; }
        Check(output->Write(const_cast<char*>(samples.data()) + position, static_cast<int>(count)) == 0, "accepted capacity must be writable");
        position += count;
    }
}
void Drain(int expectedMs) {
    const auto deadline = GetTickCount64() + 6000;
    int previous = output->GetOutputTime();
    while (output->IsPlaying()) {
        Check(GetTickCount64() < deadline, "drain timeout / IsPlaying stuck");
        const int played = output->GetOutputTime();
        Check(played >= previous, "played time regressed");
        Check(played <= output->GetWrittenTime(), "played beyond written audio");
        previous = played;
        Sleep(2);
    }
    Check(output->GetOutputTime() == expectedMs, "tail was truncated / played time incorrect");
    Check(output->GetWrittenTime() == expectedMs, "accepted frame counter incorrect");
}
void AudioTests() {
    Check(output->Open(0, 2, 16, 0, 0) < 0, "invalid input accepted");
    Check(output->CanWrite() == 0 && output->IsPlaying() == 0, "failed Open reports playing");
    output->Close(); output->Close();
    output->SetVolume(0); output->SetPan(-64);
    Check(output->Open(44100, 2, 16, -666, 0) >= 0, "WASAPI Open failed (an active playback device is required)");
    Check(output->GetWrittenTime() == 0 && output->GetOutputTime() == 0, "free capacity counted as audio");
    Check(output->Pause(1) == 0, "initial pause state");
    const int capacity = output->CanWrite();
    Check(capacity >= 8192 && capacity % 4 == 0, "bad initial capacity");
    auto full = std::vector<char>(capacity, 0);
    Feed(full, 4);
    Check(output->CanWrite() == 0, "paused software queue should be full");
    const int written = output->GetWrittenTime();
    char frame[4]{};
    Check(output->Write(frame, 4) == 1 && output->GetWrittenTime() == written, "full Write falsely succeeded");
    Check(output->Write(frame, 3) == 1 && output->Write(nullptr, 4) == 1, "invalid write accepted");
    Check(output->IsPlaying() != 0, "paused queued data should still be playing");
    output->Flush(7000);
    Check(output->Pause(1) == 1, "seek lost pause state");
    Check(output->GetWrittenTime() == 7000 && output->GetOutputTime() == 7000, "flush time base");
    Check(output->IsPlaying() == 0, "flush did not clear buffers");
    Feed(Silence(44100, 2, 16, 40), 4); // shorter than configured prebuffer (200ms)
    Check(output->IsPlaying() != 0, "short stream not queued");
    Sleep(120);
    Check(output->GetOutputTime() == 7000, "Flush resumed paused playback");
    Check(output->Pause(0) == 1, "resume return value");
    Drain(7040);
    output->Flush(11000);
    Check(output->GetOutputTime() == 11000 && output->GetWrittenTime() == 11000, "running seek base");
    Feed(Silence(44100, 2, 16, 250), 4);
    Drain(11250);
    output->Close();
    Check(output->CanWrite() == 0 && output->IsPlaying() == 0, "closed device reports audio");
    for (const auto format : {std::vector<int>{22050, 1, 8}, {44100, 2, 16}, {48000, 2, 24},
                              {96000, 2, 32}, {48000, 6, 16}}) {
        const int rate = format[0], channels = format[1], bits = format[2];
        Check(output->Open(rate, channels, bits, 0, 0) >= 0, "shared format conversion failed");
        const int available = output->CanWrite();
        Sleep(20);
        Check(output->CanWrite() >= available, "CanWrite decreased without Write");
        Feed(Silence(rate, channels, bits, 100), channels * bits / 8);
        Drain(100);
        output->Close();
        std::cout << "FORMAT_PASS " << rate << " Hz " << channels << " ch " << bits << " bit\n";
    }
    DWORD before = 0, after = 0;
    GetProcessHandleCount(GetCurrentProcess(), &before);
    for (int i = 0; i < 20; ++i) {
        Check(output->Open(48000, 2, 16, 0, 0) >= 0, "reopen failed");
        Feed(Silence(48000, 2, 16, 20), 4);
        Drain(20);
        output->Close();
    }
    GetProcessHandleCount(GetCurrentProcess(), &after);
    std::cout << "HANDLES before=" << before << " after=" << after << '\n';
    Check(after <= before + 4, "handle growth after repeated Open/Close");
    std::cout << "AUDIO_PASS: queue/backpressure, formats, short tail, timing, pause, seek, reopen\n";
}
void ExpectLevel(const test_audio::SessionProbe& probe, float expected) {
    const auto deadline = GetTickCount64() + 2000;
    float actual = probe.Level();
    while (std::fabs(actual - expected) > 0.001f && GetTickCount64() < deadline) {
        Sleep(5);
        actual = probe.Level();
    }
    if (std::fabs(actual - expected) > 0.001f) {
        std::cerr << "Expected session volume " << expected << ", observed " << actual << '\n';
        Check(false, "Windows session volume differs from expected level");
    }
}
void VolumeTests(bool sessionMode) {
    output->SetVolume(64); // Must survive Open, not just subsequent callbacks.
    Check(output->Open(44100, 2, 16, 0, 0) >= 0, "volume test Open failed");
    test_audio::SessionProbe probe(GetCurrentProcessId());
    const float master = probe.EndpointLevel();
    const bool masterMuted = probe.EndpointMuted();
    struct RestoreSession {
        test_audio::SessionProbe& probe;
        float level;
        bool muted;
        ~RestoreSession() {
            probe.volume->SetMasterVolume(level, nullptr);
            probe.volume->SetMute(muted, nullptr);
        }
    } restore{probe, probe.Level(), probe.Muted()};
    if (sessionMode) ExpectLevel(probe, 64 / 255.0f);
    else test_audio::Require(probe.volume->SetMasterVolume(0.73f, nullptr), "Set independent test mixer level");
    const float softwareLevel = probe.Level();
    // A second, isolated session must not be changed by plugin callbacks.
    GUID isolated{};
    test_audio::Require(CoCreateGuid(&isolated), "Create isolated session ID");
    Microsoft::WRL::ComPtr<ISimpleAudioVolume> other;
    test_audio::Require(probe.manager->GetSimpleAudioVolume(&isolated, 0, &other), "Create isolated test session");
    test_audio::Require(other->SetMasterVolume(0.83f, nullptr), "Set isolated session volume");
    test_audio::Require(probe.volume->SetMute(TRUE, nullptr), "Mute test session externally");
    output->Pause(1);
    int requested = 64;
    for (int value : {0, 1, 64, 128, 255, 999, -1, -666}) {
        output->SetVolume(value);
        if (value >= 0) requested = (std::min)(value, 255);
        ExpectLevel(probe, sessionMode ? requested / 255.0f : softwareLevel);
        Check(probe.Muted(), "SetVolume cleared Windows mute");
        Check(output->Pause(1) == 1, "volume change resumed paused playback");
    }
    if (sessionMode) {
        test_audio::Require(probe.volume->SetMasterVolume(0.37f, nullptr), "Simulate external mixer edit");
        Sleep(450);
        ExpectLevel(probe, 0.37f); // Do not fight the Windows mixer every render tick.
        output->SetVolume(255); // Same host value still explicitly reapplies volume.
        ExpectLevel(probe, 1.0f);
        for (int i = 0; i < 1000; ++i) output->SetVolume(i % 256);
        output->SetVolume(99);
        ExpectLevel(probe, 99 / 255.0f);
    }
    output->SetPan(-128);
    Sleep(30);
    float isolatedLevel = 0.0f;
    test_audio::Require(other->GetMasterVolume(&isolatedLevel), "Observe isolated test session");
    Check(std::fabs(isolatedLevel - 0.83f) < 0.001f, "plugin changed another audio session");
    Check(std::fabs(probe.EndpointLevel() - master) < 0.001f && probe.EndpointMuted() == masterMuted,
          "plugin changed endpoint/master volume or mute");
    output->SetVolume(128);
    ExpectLevel(probe, sessionMode ? 128 / 255.0f : softwareLevel);
    test_audio::Require(probe.volume->SetMute(FALSE, nullptr), "Unmute test fixture");
    output->SetPan(0);
    Feed(Silence(44100, 2, 16, 40), 4);
    output->Pause(0);
    Drain(40);
    output->Close();
    Check(output->Open(44100, 2, 16, 0, 0) >= 0, "volume test reopen failed");
    ExpectLevel(probe, sessionMode ? 128 / 255.0f : softwareLevel);
    output->Close();
    std::cout << (sessionMode ? "SESSION_VOLUME_PASS" : "SOFTWARE_VOLUME_PASS")
              << ": pre-open/reopen, paused updates, scalar mapping, mute preservation, session isolation, master unchanged\n";
}
struct DialogSearch { HWND result = nullptr; DWORD pid = GetCurrentProcessId(); };
BOOL CALLBACK FindDialog(HWND window, LPARAM data) {
    auto& search = *reinterpret_cast<DialogSearch*>(data);
    DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
    if (pid == search.pid && GetDlgItem(window, IDC_REPORT)) { search.result = window; return FALSE; }
    return TRUE;
}
HWND WaitDialog() {
    for (int i = 0; i < 500; ++i) {
        DialogSearch search;
        EnumWindows(FindDialog, reinterpret_cast<LPARAM>(&search));
        if (search.result) return search.result;
        Sleep(10);
    }
    return nullptr;
}
std::wstring Text(HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring result(length + 1, 0);
    GetWindowTextW(window, result.data(), length + 1);
    result.resize(length);
    return result;
}
void Screenshot(HWND window) {
    RECT rect{}; GetWindowRect(window, &rect);
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    HDC screen = GetDC(window), memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    auto previous = SelectObject(memory, bitmap);
    Check(PrintWindow(window, memory, 2) != 0, "PrintWindow failed");
    SelectObject(memory, previous);
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    Check(Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok, "GDI+ startup");
    {
        Gdiplus::Bitmap image(bitmap, nullptr);
        const CLSID png{0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0, 0, 0xf8, 0x1e, 0xf3, 0x2e}};
        Check(image.Save((evidence / L"settings-playing.png").c_str(), &png) == Gdiplus::Ok, "screenshot save");
    }
    Gdiplus::GdiplusShutdown(token);
    DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(window, screen);
}
void UiTest(bool sessionMode) {
    output->SetVolume(0); output->SetPan(-64);
    Check(output->Open(44100, 2, 16, 0, 0) >= 0, "UI playback Open failed");
    Feed(Silence(44100, 2, 16, 1800), 4);
    std::exception_ptr failure;
    const auto activeMode = sessionMode ? L"Volume control: Windows mixer" : L"Volume control: software PCM";
    std::thread inspector([&] {
        HWND dialog = nullptr;
        try {
            dialog = WaitDialog();
            Check(dialog != nullptr, "settings dialog missing");
            Sleep(150);
            const auto report = Text(GetDlgItem(dialog, IDC_REPORT));
            WriteUtf8(evidence / L"settings-playing.txt", report);
            Check(report.find(L"44100 Hz, 2 ch, 16-bit PCM") != std::wstring::npos, "input format missing");
            Check(report.find(L"Endpoint mix:") != std::wstring::npos && report.find(L"Device padding:") != std::wstring::npos,
                  "live technical fields missing");
            Check(report.find(L"Last error: None") != std::wstring::npos, "unexpected stream error");
            Check(report.find(L"balance: -64") != std::wstring::npos, "pan set before Open was lost");
            Check(IsDlgButtonChecked(dialog, IDC_SESSION_VOLUME) == (sessionMode ? UINT{BST_CHECKED} : UINT{BST_UNCHECKED}),
                  "volume-mode checkbox does not match saved/default settings");
            Check(IsDlgButtonChecked(dialog, IDC_AUTO_RECOVER) == BST_CHECKED && IsDlgButtonChecked(dialog, IDC_EXCLUSIVE) == BST_UNCHECKED,
                  "recovery/exclusive defaults incorrect");
            Check(report.find(L"Automatic recovery: on") != std::wstring::npos && report.find(L"WASAPI shared, event-driven") != std::wstring::npos,
                  "active recovery/access mode missing");
            Check(report.find(activeMode) != std::wstring::npos, "active volume mode missing from report");
            if (sessionMode) Check(report.find(L"Windows session volume: 0.00%") != std::wstring::npos,
                                   "observed mixer volume missing from report");
            Screenshot(dialog);
            const int played = output->GetOutputTime();
            SetDlgItemInt(dialog, IDC_QUEUE, 1000, FALSE);
            CheckDlgButton(dialog, IDC_SESSION_VOLUME, sessionMode ? BST_UNCHECKED : BST_CHECKED);
            CheckDlgButton(dialog, IDC_AUTO_RECOVER, BST_UNCHECKED);
            CheckDlgButton(dialog, IDC_EXCLUSIVE, BST_CHECKED);
            SendMessageW(dialog, WM_COMMAND, IDC_EXCLUSIVE, 0);
            Check(!IsWindowEnabled(GetDlgItem(dialog, IDC_SESSION_VOLUME)), "exclusive did not disable shared-session control");
            SendMessageW(dialog, WM_COMMAND, IDC_APPLY, 0);
            Check(Text(GetDlgItem(dialog, IDC_NOTE)).find(L"Saved.") != std::wstring::npos, "Apply did not save");
            Check(output->GetOutputTime() >= played && output->IsPlaying(), "Apply interrupted playback");
            output->Pause(1);
            Sleep(300);
            Check(Text(GetDlgItem(dialog, IDC_REPORT)).find(L"State: Paused") != std::wstring::npos, "live pause status missing");
            output->Flush(23000);
            Sleep(300);
            auto paused = Text(GetDlgItem(dialog, IDC_REPORT));
            Check(paused.find(L"23000 ms") != std::wstring::npos, "seek status missing");
            Check(paused.find(activeMode) != std::wstring::npos && paused.find(L"Automatic recovery: on") != std::wstring::npos &&
                  paused.find(L"WASAPI shared, event-driven") != std::wstring::npos, "Apply switched active playback modes");
            WriteUtf8(evidence / L"settings-paused-seek.txt", paused);
        } catch (...) { failure = std::current_exception(); }
        if (!dialog) dialog = WaitDialog();
        if (dialog) PostMessageW(dialog, WM_CLOSE, 0, 0);
    });
    output->Config(host);
    inspector.join();
    if (failure) std::rethrow_exception(failure);
    output->Close();
    const auto ini = (evidence / L"out_wasapi.ini").wstring();
    Check(GetPrivateProfileIntW(L"out_wasapi", L"session_volume", -1, ini.c_str()) == (sessionMode ? 0u : 1u), "volume mode not saved");
    Check(GetPrivateProfileIntW(L"out_wasapi", L"auto_recover", -1, ini.c_str()) == 0 &&
          GetPrivateProfileIntW(L"out_wasapi", L"exclusive", -1, ini.c_str()) == 1, "recovery/exclusive settings not saved");
    // Verify exclusive persistence without requiring exclusive-capable hardware
    // in the ordinary shared-mode GUI test. Then restore shared before Open.
    std::thread verifySaved([&] {
        HWND dialog = nullptr;
        try {
            dialog = WaitDialog();
            Check(dialog != nullptr, "saved settings dialog missing");
            Check(IsDlgButtonChecked(dialog, IDC_AUTO_RECOVER) == BST_UNCHECKED && IsDlgButtonChecked(dialog, IDC_EXCLUSIVE) == BST_CHECKED,
                  "saved recovery/exclusive checkboxes not restored");
            Check(!IsWindowEnabled(GetDlgItem(dialog, IDC_SESSION_VOLUME)), "saved exclusive session control not disabled");
            CheckDlgButton(dialog, IDC_EXCLUSIVE, BST_UNCHECKED);
            SendMessageW(dialog, WM_COMMAND, IDC_EXCLUSIVE, 0);
            Check(IsWindowEnabled(GetDlgItem(dialog, IDC_SESSION_VOLUME)) != 0, "shared session control not restored");
            SendMessageW(dialog, WM_COMMAND, IDC_APPLY, 0);
        } catch (...) { failure = std::current_exception(); }
        if (dialog) PostMessageW(dialog, WM_CLOSE, 0, 0);
    });
    output->Config(host); verifySaved.join();
    if (failure) std::rethrow_exception(failure);
    Check(output->Open(44100, 2, 16, 0, 0) >= 0, "reopen after settings failed");
    Check(output->CanWrite() == 44100 * 4, "saved queue setting was not applied on reopen");
    std::thread verifyMode([&] {
        HWND dialog = nullptr;
        try {
            dialog = WaitDialog();
            Check(dialog != nullptr, "reopened stream settings dialog missing");
            const auto expected = sessionMode ? L"Volume control: software PCM" : L"Volume control: Windows mixer";
            const auto report = Text(GetDlgItem(dialog, IDC_REPORT));
            Check(report.find(expected) != std::wstring::npos && report.find(L"Automatic recovery: off") != std::wstring::npos,
                  "saved volume/recovery mode not applied on reopen");
            Check(IsDlgButtonChecked(dialog, IDC_SESSION_VOLUME) == (sessionMode ? UINT{BST_UNCHECKED} : UINT{BST_CHECKED}),
                  "saved volume-mode checkbox not restored");
        } catch (...) { failure = std::current_exception(); }
        if (dialog) PostMessageW(dialog, WM_CLOSE, 0, 0);
    });
    output->Config(host); verifyMode.join();
    if (failure) std::rethrow_exception(failure);
    output->Close();
    std::cout << "UI_PASS: live report, pause/seek, Apply isolation, volume/recovery/exclusive defaults and persistence, screenshot\n";
}
void ExclusiveTest() {
    output->SetVolume(0);
    bool opened = false;
    int rate = 44100, bits = 16;
    for (int candidateRate : {44100, 48000, 96000}) {
        for (int candidateBits : {16, 24, 32}) {
            if (output->Open(candidateRate, 2, candidateBits, 0, 0) >= 0) {
                rate = candidateRate; bits = candidateBits; opened = true; break;
            }
            output->Close();
        }
        if (opened) break;
    }
    Check(opened, "no tested exclusive PCM format could be opened");
    Check(output->Pause(1) == 0, "exclusive initial pause");
    Feed(Silence(rate, 2, bits, 40), 2 * bits / 8);
    Check(output->IsPlaying() != 0, "exclusive short tail missing");
    std::exception_ptr failure;
    std::thread inspect([&] {
        HWND dialog = nullptr;
        try {
            dialog = WaitDialog();
            Check(dialog != nullptr, "exclusive settings dialog missing");
            Sleep(200); // Allow the first WM_PAINT before PrintWindow.
            const auto report = Text(GetDlgItem(dialog, IDC_REPORT));
            Check(report.find(L"WASAPI exclusive, timer-driven") != std::wstring::npos &&
                  report.find(L"Volume control: software PCM") != std::wstring::npos &&
                  report.find(L"Automatic recovery: on") != std::wstring::npos,
                  "exclusive access/volume/recovery report");
            Check(IsDlgButtonChecked(dialog, IDC_EXCLUSIVE) == BST_CHECKED &&
                  !IsWindowEnabled(GetDlgItem(dialog, IDC_SESSION_VOLUME)), "exclusive checkbox interaction");
            WriteUtf8(evidence / L"exclusive-paused.txt", report);
            Screenshot(dialog);
        } catch (...) { failure = std::current_exception(); }
        if (dialog) PostMessageW(dialog, WM_CLOSE, 0, 0);
    });
    output->Config(host); inspect.join();
    if (failure) std::rethrow_exception(failure);
    output->Pause(0); Drain(40);
    output->Pause(1); output->Flush(7000);
    Feed(Silence(rate, 2, bits, 80), 2 * bits / 8);
    Sleep(100);
    Check(output->GetOutputTime() == 7000 && output->Pause(1) == 1, "exclusive paused seek");
    output->SetVolume(128); output->SetPan(64);
    output->Pause(0); Drain(7080); output->Close();
    Check(output->Open(12345, 8, 8, 0, 0) < 0 && !output->CanWrite() && !output->IsPlaying(),
          "unsupported exclusive format silently fell back / reported audio");
    output->Close();
    std::cout << "EXCLUSIVE_DLL_PASS: actual DLL/INI mode, supported PCM, software volume, GUI, short tail, paused seek, unsupported-format rejection\n";
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        Check(argc >= 3, "Usage: plugin_host --abi|--audio|--audio-software|--ui|--ui-software|--session-volume|--software-volume|--exclusive|--missing-device DLL [new-evidence-directory]");
        const std::wstring mode = argv[1];
        const bool softwareMode = mode == L"--software-volume" || mode == L"--audio-software" || mode == L"--ui-software";
        const bool uiMode = mode == L"--ui" || mode == L"--ui-software";
        evidence = argc >= 4 ? std::filesystem::absolute(argv[3]) : std::filesystem::temp_directory_path() / L"out_wasapi-abi";
        std::filesystem::create_directories(evidence);
        hostIni = (evidence / L"winamp.ini").wstring();
        const auto pluginIni = (evidence / L"out_wasapi.ini").wstring();
        if (mode != L"--abi") {
            Check(!std::filesystem::exists(pluginIni), "evidence directory already contains plugin settings; use a new directory");
            WritePrivateProfileStringW(L"out_wasapi", L"queue_ms", uiMode ? L"2000" : L"500", pluginIni.c_str());
            WritePrivateProfileStringW(L"out_wasapi", L"prebuffer_ms", L"200", pluginIni.c_str());
            // Omit the key in normal runs to verify session volume defaults to on.
            if (softwareMode) WritePrivateProfileStringW(L"out_wasapi", L"session_volume", L"0", pluginIni.c_str());
            if (mode == L"--exclusive") WritePrivateProfileStringW(L"out_wasapi", L"exclusive", L"1", pluginIni.c_str());
            if (mode == L"--missing-device") WritePrivateProfileStringW(L"out_wasapi", L"device", L"{missing-test-endpoint}", pluginIni.c_str());
        }
        WNDCLASSW wc{}; wc.lpfnWndProc = HostProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"out_wasapi_test_host";
        Check(RegisterClassW(&wc) != 0, "host class registration");
        host = CreateWindowW(wc.lpszClassName, L"WASAPI test host", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
        Check(host != nullptr, "host window");
        library = LoadLibraryW(argv[2]);
        Check(library != nullptr, "LoadLibrary failed");
        const auto get = reinterpret_cast<GetOutModule>(GetProcAddress(library, "winampGetOutModule"));
        Check(get != nullptr, "undecorated winampGetOutModule missing");
        Check(GetProcAddress(library, "winampGetOutModeChange") && GetProcAddress(library, "winampUninstallPlugin"), "auxiliary exports missing");
        output = get();
        Check(output && output->version == OUT_VER && output->id >= 65536, "bad output ABI");
        Check(output->Config && output->About && output->Init && output->Quit && output->Open && output->Close &&
              output->Write && output->CanWrite && output->IsPlaying && output->Pause && output->SetVolume && output->SetPan &&
              output->Flush && output->GetOutputTime && output->GetWrittenTime, "null callback");
        output->hMainWindow = host; output->hDllInstance = library;
        output->Init();
        if (mode == L"--abi") std::cout << "ABI_PASS: x86 module, structure and undecorated exports\n";
        else if (mode == L"--audio" || mode == L"--audio-software") AudioTests();
        else if (uiMode) UiTest(!softwareMode);
        else if (mode == L"--session-volume" || mode == L"--software-volume") VolumeTests(!softwareMode);
        else if (mode == L"--exclusive") ExclusiveTest();
        else if (mode == L"--missing-device") {
            Check(output->Open(44100, 2, 16, 0, 0) < 0, "missing device silently fell back");
            Check(output->CanWrite() == 0 && output->IsPlaying() == 0, "failed device reports capacity/audio");
            output->Close();
            std::cout << "MISSING_DEVICE_PASS: no crash, no capacity, no false drain\n";
        } else Check(false, "unknown test mode");
        output->Quit(); output = nullptr;
        FreeLibrary(library); library = nullptr;
        DestroyWindow(host); host = nullptr;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "TEST_FAIL: " << e.what() << '\n';
        if (output) output->Quit();
        if (library) FreeLibrary(library);
        if (host) DestroyWindow(host);
        return 1;
    }
}
