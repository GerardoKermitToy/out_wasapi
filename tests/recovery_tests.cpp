// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 out_wasapi contributors. See LICENSE.
#include "engine.h"
#include <windows.h>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Predicate> void Wait(Predicate predicate, const char* message, DWORD timeout = 6000) {
    const auto deadline = GetTickCount64() + timeout;
    while (!predicate()) { Check(GetTickCount64() < deadline, message); Sleep(2); }
}
void Conserved(wasapi::Engine& engine) {
    const auto s = engine.Status();
    Check(s.played <= s.submitted && s.submitted <= s.accepted, "position ordering");
    Check(s.accepted - s.played == s.queueFrames + s.padding, "accepted PCM conservation");
    Check(s.queueFrames + s.padding <= s.queueCapacity, "bounded retained memory");
}
void Feed(wasapi::Engine& engine, int rate, int bits, int ms) {
    const int bytes = rate * ms / 1000 * 2 * bits / 8;
    std::vector<char> silence(bytes, bits == 8 ? static_cast<char>(128) : 0);
    Check(engine.CanWrite() >= bytes && engine.Write(silence.data(), bytes) == 0, "fixture write");
}
void Drain(wasapi::Engine& engine, int expected) {
    int previous = engine.OutputTime();
    Wait([&] {
        Conserved(engine);
        const int now = engine.OutputTime();
        Check(now >= previous && now <= engine.WrittenTime(), "time regressed or exceeded accepted audio");
        previous = now;
        return engine.IsPlaying() == 0;
    }, "recovery drain timeout");
    Check(engine.OutputTime() == expected && engine.WrittenTime() == expected, "recovery tail/frame count");
}
void Exercise(wasapi::Engine& engine, wasapi::Settings settings, int rate, int bits) {
    engine.Pause(true);
    Feed(engine, rate, bits, 320);
    engine.TestDisconnect(100);
    Wait([&] { return engine.Status().recovering && engine.Status().recoveryAttempts >= 1; }, "did not enter recovery");
    const auto waiting = engine.Status();
    Check(waiting.open && waiting.paused && !waiting.started && waiting.played == 0, "recovery lost paused state");
    Check(waiting.accepted == static_cast<std::uint64_t>(rate) * 320 / 1000 && engine.IsPlaying(), "recovery discarded accepted PCM / false EOF");
    Conserved(engine);
    auto began = GetTickCount64();
    Check(engine.Pause(false) == 1 && engine.Pause(true) == 0, "offline pause commands");
    engine.Flush(9000);
    Check(GetTickCount64() - began < 500, "transport waited for a missing device");
    Check(engine.Status().paused && engine.OutputTime() == 9000 && engine.WrittenTime() == 9000, "offline seek lost base/pause");
    Feed(engine, rate, bits, 80);
    engine.SetVolume(0); engine.SetPan(-64);
    engine.TestAllowReconnect();
    Wait([&] { return !engine.Status().recovering && engine.Status().recoveries >= 1; }, "did not reopen");
    Check(engine.Status().paused && !engine.Status().started && engine.OutputTime() == 9000, "reopen resumed paused playback");
    Check(engine.Status().exclusive == settings.exclusive && engine.Status().sessionVolume == (settings.sessionVolume && !settings.exclusive),
          "recovery changed access/volume mode");
    engine.Pause(false);
    Drain(engine, 9080);
    engine.Flush(0);
    Feed(engine, rate, bits, 400);
    Wait([&] { return engine.OutputTime() >= 20; }, "playback did not start");
    const int previous = engine.OutputTime();
    engine.TestDisconnect(100);
    Wait([&] { return engine.Status().recovering && engine.Status().recoveryAttempts >= 3; }, "playing recovery not entered");
    Check(engine.OutputTime() >= previous && engine.WrittenTime() == 400 && engine.IsPlaying(), "playing recovery lost time / false EOF");
    const int frozen = engine.OutputTime();
    Sleep(50);
    Check(engine.OutputTime() == frozen, "missing device advanced playback position");
    Conserved(engine);
    engine.TestAllowReconnect();
    Drain(engine, 400);
    Check(engine.Status().recoveries >= 2 && engine.Status().lastError == S_OK, "successful recovery did not clear current error");
    std::cout << "PLAYING_RECOVERY_PASS replay_boundary_frames=" << engine.Status().replayedFrames << '\n';
    engine.Pause(true);
    Feed(engine, rate, bits, 100);
    engine.TestDisconnect(100);
    Wait([&] { return engine.Status().recovering; }, "close fixture recovery");
    began = GetTickCount64();
    engine.Close();
    Check(GetTickCount64() - began < 500 && !engine.IsPlaying() && !engine.CanWrite(), "Close failed to cancel recovery");

    settings.autoRecover = false;
    Check(engine.Open(rate, 2, bits, settings) >= 0, "disabled recovery Open");
    engine.Pause(true); Feed(engine, rate, bits, 100);
    engine.TestDisconnect(0);
    Wait([&] { return !engine.Status().open; }, "disabled recovery did not fail closed");
    Check(!engine.Status().recovering && engine.Status().recoveryAttempts == 0 && !engine.CanWrite() && !engine.IsPlaying(),
          "disabled recovery retried / reported audio");
    engine.Close();
    settings.autoRecover = true;
    Check(engine.Open(rate, 2, bits, settings) >= 0, "clean reopen after disabled recovery");
    Feed(engine, rate, bits, 40); Drain(engine, 40); engine.Close();
    // Repeated notification registrations/releases must not retain old clients.
    DWORD before = 0, after = 0;
    GetProcessHandleCount(GetCurrentProcess(), &before);
    for (int i = 0; i < 12; ++i) {
        Check(engine.Open(rate, 2, bits, settings) >= 0, "repeat Open");
        engine.Pause(true); Feed(engine, rate, bits, 20);
        engine.TestDisconnect(0);
        Wait([&] { return engine.Status().recoveries == 1; }, "repeat recovery");
        Check(engine.Status().paused && engine.WrittenTime() == 20, "repeat recovery lost queue/pause");
        engine.Pause(false); Drain(engine, 20); engine.Close();
    }
    GetProcessHandleCount(GetCurrentProcess(), &after);
    std::cout << "RECOVERY_HANDLES before=" << before << " after=" << after << '\n';
    Check(after <= before + 6, "handle growth after recovery cycles");
    std::cout << (settings.exclusive ? "EXCLUSIVE_RECOVERY_PASS" : "SHARED_RECOVERY_PASS")
        << ": paused/playing reopen, PCM conservation, monotonic time, offline seek/pause, cancellation, opt-out, repeated lifetime\n";
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        const bool exclusive = argc > 1 && std::wstring(argv[1]) == L"--exclusive";
        wasapi::Settings settings;
        settings.exclusive = exclusive;
        wasapi::Engine engine;
        engine.SetVolume(0);
        int rate = 48000, bits = 16;
        if (exclusive) {
            bool opened = false;
            for (int candidateRate : {48000, 44100, 96000}) {
                for (int candidateBits : {16, 24, 32}) {
                    if (engine.Open(candidateRate, 2, candidateBits, settings) >= 0) {
                        rate = candidateRate; bits = candidateBits; opened = true; break;
                    }
                    engine.Close();
                }
                if (opened) break;
            }
            if (!opened) {
                std::wcerr << L"EXCLUSIVE_UNAVAILABLE: " << engine.Status().error << L'\n';
                return 77;
            }
            Check(!engine.Status().sessionVolume && engine.Status().exclusive, "exclusive mode used session volume");
            std::cout << "EXCLUSIVE_FORMAT " << rate << " Hz stereo " << bits << " bit\n";
        } else Check(engine.Open(rate, 2, bits, settings) >= 0, "shared Open");
        Exercise(engine, settings, rate, bits);
        if (exclusive) {
            Check(engine.Open(12345, 8, 8, settings) < 0, "unsupported exclusive format fell back to shared");
            Check(engine.Status().exclusive && !engine.Status().open && engine.CanWrite() == 0, "exclusive rejection status");
            std::wcout << L"EXCLUSIVE_REJECT_PASS: " << engine.Status().error << L'\n';
            engine.Close();
        }
        return 0;
    } catch (const std::exception& e) { std::cerr << "RECOVERY_FAIL: " << e.what() << '\n'; return 1; }
}
