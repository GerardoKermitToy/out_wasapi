// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 out_wasapi contributors. See LICENSE.
#pragma once
#include "frame_queue.h"
#include "pcm.h"
#include "settings.h"
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace wasapi {
struct Snapshot {
    bool open = false, paused = false, started = false, draining = false, mmcss = false;
    bool sessionVolume = false, sessionMuted = false;
    bool autoRecover = true, exclusive = false, recovering = false;
    std::uint64_t recoveries = 0, recoveryAttempts = 0, replayedFrames = 0;
    float sessionLevel = -1.0f; // Last observed Windows mixer level, or unavailable.
    WAVEFORMATEXTENSIBLE input{};
    std::wstring state = L"Closed", deviceName, deviceId, mixFormat;
    std::wstring error;
    HRESULT lastError = S_OK;
    std::uint64_t accepted = 0, submitted = 0, played = 0, renderCalls = 0, underruns = 0;
    std::uint64_t clockPosition = 0, clockFrequency = 0;
    std::size_t queueFrames = 0, queueCapacity = 0, peakQueue = 0;
    UINT32 endpointFrames = 0, padding = 0;
    std::int64_t latency = 0, period = 0;
    int baseMs = 0, volume = 255, pan = 0, prebufferMs = 0, maximumLatencyMs = 0;
};
class Engine {
public:
    ~Engine();
    int Open(int rate, int channels, int bits, const Settings& settings);
    void Close();
    int Write(const void* data, int bytes);
    int CanWrite();
    int IsPlaying();
    int Pause(bool pause);
    void Flush(int milliseconds);
    void SetVolume(int volume);
    void SetPan(int pan);
    Snapshot Status();
    int OutputTime();
    int WrittenTime();
#ifdef OUT_WASAPI_TESTING
    // Test executable only; not compiled into or exported by the release DLL.
    void TestDisconnect(unsigned unavailableAttempts);
    void TestAllowReconnect();
#endif
private:
    void CloseLocked();
    void Run(Settings settings) noexcept;
    void Fail(const wchar_t* operation, HRESULT hr);
    void RefreshStateLocked();
    std::mutex lifecycle_, mutex_;
    std::condition_variable changed_;
    std::thread thread_;
    HANDLE control_ = nullptr;
    bool stop_ = false, ready_ = false, finished_ = true;
    bool pausePending_ = false, requestedPause_ = false, flushPending_ = false, volumePending_ = false;
    int requestedSeek_ = 0;
    std::uint64_t command_ = 0, acknowledged_ = 0;
    std::size_t prebufferFrames_ = 0;
    FrameQueue queue_;
    Snapshot status_;
#ifdef OUT_WASAPI_TESTING
    bool testDisconnect_ = false;
    unsigned testUnavailable_ = 0;
#endif
};
Engine& AudioEngine();
} // namespace wasapi
