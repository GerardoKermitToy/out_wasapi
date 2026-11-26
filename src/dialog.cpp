// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 out_wasapi contributors. See LICENSE.
#include "dialog.h"
#include "engine.h"
#include "settings.h"
#include "resource.h"
#include <commctrl.h>
#include <algorithm>
#include <atomic>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace wasapi {
namespace {
std::atomic<HWND> configWindow{nullptr};
struct DialogState {
    std::vector<DeviceInfo> devices;
    std::wstring report;
};
std::wstring BuildReport(const Snapshot& s) {
    std::wostringstream text;
    text << L"State: " << s.state << L"\r\n"
         << L"Device: " << (s.deviceName.empty() ? L"No active stream" : s.deviceName) << L"\r\n"
         << L"Mode: WASAPI " << (s.exclusive ? L"exclusive, timer-driven, " : L"shared, event-driven, ")
         << (s.mmcss ? L"MMCSS Audio" : L"normal scheduling") << L"\r\n"
         << L"Automatic recovery: " << (s.autoRecover ? L"on" : L"off") << L"; completed: " << s.recoveries
         << L"; reopen attempts: " << s.recoveryAttempts << L"\r\n"
         << L"  Replay boundary: " << s.replayedFrames << L" unconfirmed frames scheduled for replay.\r\n\r\n";
    if (s.input.Format.nSamplesPerSec) {
        text << L"Signal path\r\n"
             << L"  Winamp input:  " << DescribeFormat(&s.input.Format) << L"\r\n"
             << L"  WASAPI client: " << DescribeFormat(&s.input.Format) << L"\r\n"
             << L"  Endpoint mix:  " << (s.mixFormat.empty() ? L"Not available" : s.mixFormat) << L"\r\n"
             << (s.exclusive ? L"  Conversion: none; exact PCM format accepted by the exclusive driver.\r\n"
                              L"  Endpoint mix is the shared-mode setting, not the active exclusive format.\r\n\r\n"
                            : L"  Conversion: Windows audio engine; mix format is not a hardware-format guarantee.\r\n\r\n");
        const auto rate = s.input.Format.nSamplesPerSec;
        const auto align = s.input.Format.nBlockAlign;
        text << L"Buffering\r\n"
             << L"  Software queue: " << s.queueFrames << L" / " << s.queueCapacity << L" frames ("
             << FramesToMs(s.queueFrames, rate) << L" / " << FramesToMs(s.queueCapacity, rate) << L" ms), "
             << s.queueFrames * align << L" bytes\r\n"
             << L"  Device padding: " << s.padding << L" / " << s.endpointFrames << L" frames ("
             << FramesToMs(s.padding, rate) << L" / " << FramesToMs(s.endpointFrames, rate) << L" ms)\r\n"
             << L"  Queued audio: " << FramesToMs(s.queueFrames + s.padding, rate) << L" ms; prebuffer: "
             << s.prebufferMs << L" ms; peak software queue: " << s.peakQueue << L" frames\r\n"
             << std::fixed << std::setprecision(2)
             << L"  Driver stream latency: " << s.latency / 10000.0 << L" ms; default period: "
             << s.period / 10000.0 << L" ms\r\n"
             << L"  Reported maximum latency: " << s.maximumLatencyMs << L" ms (conservative)\r\n\r\n"
             << L"Position and counters (since Open / last seek)\r\n"
             << L"  Played:  " << FramesToMs(s.played, rate, s.baseMs) << L" ms; " << s.played
             << L" frames; " << s.played * align << L" bytes\r\n"
             << L"  Written: " << FramesToMs(s.accepted, rate, s.baseMs) << L" ms; " << s.accepted
             << L" frames; " << s.accepted * align << L" bytes\r\n"
             << L"  Submission frontier: " << s.submitted << L" source frames; render calls: " << s.renderCalls << L"\r\n"
             << L"  Buffer-empty events: " << s.underruns << L" (may include producer starvation / end of stream)\r\n"
             << L"  Audio clock: " << s.clockPosition << L" ticks; frequency: " << s.clockFrequency << L" Hz\r\n"
             << L"  Volume: " << s.volume << L" / 255; balance: " << s.pan << L" (-128 left, +128 right)\r\n";
        if (s.sessionVolume) {
            text << L"  Volume control: Windows mixer (Winamp session); no software volume attenuation.\r\n";
            if (s.sessionLevel >= 0.0f)
                text << L"  Windows session volume: " << s.sessionLevel * 100.0f << L"%; mute: "
                     << (s.sessionMuted ? L"on" : L"off") << L"\r\n";
            text << L"  Balance: software PCM attenuation. Device/master volume is never changed.\r\n\r\n";
        } else {
            if (s.exclusive) text << L"  Exclusive mode bypasses Windows session volume; Winamp volume uses PCM gain.\r\n";
            text << L"  Volume control: software PCM attenuation; Windows mixer volume is independent.\r\n"
                 << L"  Unity volume + centered balance leaves PCM unchanged.\r\n\r\n";
        }
    } else {
        text << L"Start playback to see input format, endpoint mix, buffers and clocks.\r\n\r\n";
    }
    text << L"Last error: " << (s.error.empty() ? L"None" : s.error) << L"\r\n"
         << L"Endpoint ID: " << (s.deviceId.empty() ? L"Not opened" : s.deviceId) << L"\r\n"
         << L"Settings: " << SettingsPath() << L"\r\n"
         << (s.autoRecover ? L"Recovery waits for the chosen device; Stop cancels it. A brief repeat is possible.\r\n"
                           : L"Automatic recovery is off. After a device error, check its connection and restart playback.\r\n");
    return text.str();
}
void UpdateReport(HWND window, DialogState& state) {
    auto report = BuildReport(AudioEngine().Status());
    if (report == state.report) return;
    state.report = std::move(report);
    const auto edit = GetDlgItem(window, IDC_REPORT);
    DWORD start = 0, end = 0;
    SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
    const auto first = SendMessageW(edit, EM_GETFIRSTVISIBLELINE, 0, 0);
    SendMessageW(edit, WM_SETREDRAW, FALSE, 0);
    SetWindowTextW(edit, state.report.c_str());
    SendMessageW(edit, EM_SETSEL, start, end);
    const auto now = SendMessageW(edit, EM_GETFIRSTVISIBLELINE, 0, 0);
    SendMessageW(edit, EM_LINESCROLL, 0, first - now);
    SendMessageW(edit, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(edit, nullptr, TRUE);
}
void LoadDevices(HWND window, DialogState& state, const std::wstring& selected) {
    std::wstring error;
    state.devices = EnumerateDevices(error);
    state.devices.insert(state.devices.begin(), DeviceInfo{L"", L"Default multimedia playback device"});
    if (!selected.empty() && std::none_of(state.devices.begin(), state.devices.end(), [&](const auto& d) { return d.id == selected; }))
        state.devices.push_back({selected, L"Unavailable device: " + selected});
    const auto combo = GetDlgItem(window, IDC_DEVICE);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (std::size_t i = 0; i < state.devices.size(); ++i) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(state.devices[i].name.c_str()));
        if (state.devices[i].id == selected) SendMessageW(combo, CB_SETCURSEL, i, 0);
    }
    if (!error.empty()) SetDlgItemTextW(window, IDC_NOTE, error.c_str());
}
std::wstring SelectedDevice(HWND window, const DialogState& state) {
    const auto index = SendDlgItemMessageW(window, IDC_DEVICE, CB_GETCURSEL, 0, 0);
    if (index < 0 || static_cast<std::size_t>(index) >= state.devices.size()) return {};
    return state.devices[static_cast<std::size_t>(index)].id;
}
void CopyReport(HWND window, const std::wstring& report) {
    const auto bytes = (report.size() + 1) * sizeof(wchar_t);
    HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!data) return;
    void* memory = GlobalLock(data);
    if (!memory) { GlobalFree(data); return; }
    std::memcpy(memory, report.c_str(), bytes);
    GlobalUnlock(data);
    if (!OpenClipboard(window)) { GlobalFree(data); return; }
    if (!EmptyClipboard() || !SetClipboardData(CF_UNICODETEXT, data)) GlobalFree(data);
    CloseClipboard();
}
INT_PTR CALLBACK ConfigProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(window, DWLP_USER));
    try {
        if (message == WM_INITDIALOG) {
            state = reinterpret_cast<DialogState*>(lparam);
            SetWindowLongPtrW(window, DWLP_USER, lparam);
            configWindow = window;
            const auto settings = GetSettings();
            LoadDevices(window, *state, settings.deviceId);
            SetDlgItemInt(window, IDC_QUEUE, settings.queueMs, FALSE);
            SetDlgItemInt(window, IDC_PREBUFFER, settings.prebufferMs, FALSE);
            SetDlgItemInt(window, IDC_DEVICE_BUFFER, settings.deviceBufferMs, FALSE);
            CheckDlgButton(window, IDC_SESSION_VOLUME, settings.sessionVolume ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(window, IDC_AUTO_RECOVER, settings.autoRecover ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(window, IDC_EXCLUSIVE, settings.exclusive ? BST_CHECKED : BST_UNCHECKED);
            EnableWindow(GetDlgItem(window, IDC_SESSION_VOLUME), !settings.exclusive);
            for (int id : {IDC_QUEUE, IDC_PREBUFFER, IDC_DEVICE_BUFFER}) SendDlgItemMessageW(window, id, EM_SETLIMITTEXT, 4, 0);
            SendDlgItemMessageW(window, IDC_REPORT, EM_SETLIMITTEXT, 32767, 0);
            UpdateReport(window, *state);
            SetTimer(window, 1, 250, nullptr);
            return TRUE;
        }
        if (!state) return FALSE;
        switch (message) {
        case WM_TIMER: UpdateReport(window, *state); return TRUE;
        case WM_CLOSE: EndDialog(window, 0); return TRUE;
        case WM_DESTROY: KillTimer(window, 1); configWindow = nullptr; return TRUE;
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
            case IDOK: case IDCANCEL: EndDialog(window, 0); return TRUE;
            case IDC_REFRESH: LoadDevices(window, *state, SelectedDevice(window, *state)); return TRUE;
            case IDC_COPY: CopyReport(window, state->report); return TRUE;
            case IDC_EXCLUSIVE:
                EnableWindow(GetDlgItem(window, IDC_SESSION_VOLUME), IsDlgButtonChecked(window, IDC_EXCLUSIVE) != BST_CHECKED);
                return TRUE;
            case IDC_APPLY: {
                Settings s;
                s.deviceId = SelectedDevice(window, *state);
                BOOL q = FALSE, p = FALSE, d = FALSE;
                s.queueMs = static_cast<int>(GetDlgItemInt(window, IDC_QUEUE, &q, FALSE));
                s.prebufferMs = static_cast<int>(GetDlgItemInt(window, IDC_PREBUFFER, &p, FALSE));
                s.deviceBufferMs = static_cast<int>(GetDlgItemInt(window, IDC_DEVICE_BUFFER, &d, FALSE));
                s.sessionVolume = IsDlgButtonChecked(window, IDC_SESSION_VOLUME) == BST_CHECKED;
                s.autoRecover = IsDlgButtonChecked(window, IDC_AUTO_RECOVER) == BST_CHECKED;
                s.exclusive = IsDlgButtonChecked(window, IDC_EXCLUSIVE) == BST_CHECKED;
                std::wstring error;
                if (!q || !p || !d) error = L"Enter an integer value in each buffer field.";
                if (error.empty() && SaveSettings(s, error))
                    SetDlgItemTextW(window, IDC_NOTE, L"Saved. All playback settings take effect on the next playback start.");
                else MessageBoxW(window, error.c_str(), L"WASAPI settings", MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            }
        }
    } catch (...) {
        // Never unwind through the Windows dialog procedure.
        if (message == WM_INITDIALOG) EndDialog(window, -1);
    }
    return FALSE;
}
}
void ShowConfig(HWND parent, HINSTANCE instance) {
    if (const HWND existing = configWindow.load()) { SetForegroundWindow(existing); return; }
    DialogState state;
    const auto result = DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_CONFIG), parent, ConfigProc,
                                        reinterpret_cast<LPARAM>(&state));
    if (result == -1) MessageBoxW(parent, L"Could not create the settings dialog.", L"WASAPI Output", MB_OK | MB_ICONERROR);
}
void CloseConfig() { if (const HWND window = configWindow.load()) SendMessageW(window, WM_CLOSE, 0, 0); }
void ShowAbout(HWND parent) {
    MessageBoxW(parent, L"Independent WASAPI Output 1.2.0\n\n"
        L"Classic Winamp output plugin, x86.\n"
        L"Event-driven shared-mode playback with live stream diagnostics.\n"
        L"Built using C++ and the Windows SDK only; no Winamp source dependency.\n\n"
        L"Copyright (C) 2026 out_wasapi contributors.\n"
        L"GNU General Public License version 3 only (GPL-3.0-only).\n"
        L"Free software; redistribution is permitted under GPLv3.\n"
        L"ABSOLUTELY NO WARRANTY, including merchantability or fitness.\n"
        L"See the accompanying LICENSE and source README for the full terms.\n\n"
        L"This is an independent project, not a Nullsoft release.", L"About WASAPI Output", MB_OK | MB_ICONINFORMATION);
}
} // namespace wasapi
