// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 out_wasapi contributors. See LICENSE.
#include "frame_queue.h"
#include "pcm.h"
#include <array>
#include <cstring>
#include <deque>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>

namespace {
unsigned checks = 0;
void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
void QueueTests() {
    wasapi::FrameQueue q;
    q.Reset(5, sizeof(int));
    int a[] = {1, 2, 3, 4, 5}, out[5]{};
    Check(q.Push(a, 4), "push");
    Check(!q.Push(a, 2) && q.Size() == 4, "reject all, not partial");
    q.Consume(3);
    Check(q.Push(a, 4), "wrap push");
    Check(q.Peek(out, 5) == 5, "peek");
    int expected[] = {4, 1, 2, 3, 4};
    Check(std::memcmp(out, expected, sizeof(out)) == 0, "wrap order");
    Check(q.Peek(out, 4, 2) == 3 && out[0] == 2 && out[1] == 3 && out[2] == 4, "offset peek across wrap");
    Check(q.Peek(out, 1, 5) == 0 && q.Peek(out, 1, 99) == 0, "offset beyond queue");
    // Submitted frames remain in the ring until confirmed played. A failed
    // stream resets only the submission cursor, not the raw PCM or acceptance.
    q.Consume(1);
    Check(q.Peek(out, 4) == 4 && out[0] == 1 && out[3] == 4, "retained recovery suffix");
    Check(q.Peek(out, 2, 2) == 2 && out[0] == 3 && out[1] == 4, "unsubmitted suffix");
    q.Clear();
    Check(q.Free() == 5 && !q.Push(nullptr, 1) && q.Push(nullptr, 0), "empty/null");
    std::mt19937 random(0x51a);
    std::deque<int> reference;
    for (int i = 0; i < 20000; ++i) {
        const unsigned n = random() % 6;
        if (random() & 1) {
            const bool fits = n <= 5 - reference.size();
            Check(q.Push(a, n) == fits, "random push capacity");
            if (fits) for (unsigned j = 0; j < n; ++j) reference.push_back(a[j]);
        } else {
            const auto got = q.Peek(out, n);
            Check(got == (std::min)(static_cast<std::size_t>(n), reference.size()), "random peek count");
            for (std::size_t j = 0; j < got; ++j) Check(out[j] == reference[j], "random sequence");
            q.Consume(got);
            for (std::size_t j = 0; j < got; ++j) reference.pop_front();
        }
        Check(q.Size() == reference.size() && q.Size() + q.Free() == 5, "queue conservation");
    }
}
void PcmTests() {
    Check(wasapi::ValidPcm(44100, 2, 16), "valid format");
    Check(!wasapi::ValidPcm(0, 2, 16) && !wasapi::ValidPcm(48000, 9, 16) &&
          !wasapi::ValidPcm(44100, 2, 20), "invalid formats");
    Check(wasapi::ChannelMask(8) == 0x63f, "7.1 DWORD channel mask");
    for (int bits : {8, 16, 24, 32}) {
        const auto f = wasapi::MakeFormat(48000, 2, bits);
        Check(f.Format.nBlockAlign == bits / 4 && f.Format.nAvgBytesPerSec == 48000u * bits / 4,
              "frame bytes");
        Check(f.Samples.wValidBitsPerSample == bits && f.Format.cbSize == 22, "format extension");
        std::array<std::uint8_t, 64> bytes{};
        for (unsigned i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::uint8_t>(i * 13);
        const auto original = bytes;
        wasapi::ApplyGain(bytes.data(), 64 / f.Format.nBlockAlign, f, 255, 0);
        Check(bytes == original, "unity bit exact");
        wasapi::ApplyGain(bytes.data(), 64 / f.Format.nBlockAlign, f, 0, 0);
        const auto used = (bytes.size() / f.Format.nBlockAlign) * f.Format.nBlockAlign;
        for (std::size_t i = 0; i < used; ++i) Check(bytes[i] == (bits == 8 ? 128 : 0), "silence encoding");
    }
    const auto f = wasapi::MakeFormat(44100, 2, 16);
    std::int16_t stereo[] = {16000, -16000};
    wasapi::ApplyGain(stereo, 1, f, 255, -64);
    Check(stereo[0] == 16000 && stereo[1] == -8000, "negative balance attenuation");
    stereo[0] = stereo[1] = 16000;
    wasapi::ApplyGain(stereo, 1, f, 255, 128);
    Check(stereo[0] == 0 && stereo[1] == 16000, "right balance");
    stereo[0] = stereo[1] = 16000;
    wasapi::ApplyGain(stereo, 1, f, 255, 0);
    Check(stereo[0] == 16000 && stereo[1] == 16000, "center restores both channels");
    std::int32_t signed32[] = {(std::numeric_limits<std::int32_t>::min)(), (std::numeric_limits<std::int32_t>::max)()};
    wasapi::ApplyGain(signed32, 1, wasapi::MakeFormat(48000, 2, 32), 255, -128);
    Check(signed32[0] == (std::numeric_limits<std::int32_t>::min)() && signed32[1] == 0, "32-bit extremes");
    std::uint8_t packed24[] = {0, 0, 0x80, 0xff, 0xff, 0x7f};
    wasapi::ApplyGain(packed24, 1, wasapi::MakeFormat(48000, 2, 24), 255, -64);
    Check(packed24[2] == 0x80 && packed24[5] == 0x40, "packed 24-bit sign and rounding");
    std::int16_t multichannel[] = {1000, 1000, 1000, 1000, 1000, 1000};
    wasapi::ApplyGain(multichannel, 1, wasapi::MakeFormat(48000, 6, 16), 255, -128);
    Check(multichannel[0] == 1000 && multichannel[1] == 0 && multichannel[2] == 1000 &&
          multichannel[3] == 1000 && multichannel[4] == 1000 && multichannel[5] == 0, "5.1 pan, center/LFE intact");
    std::int16_t mono = 12345;
    wasapi::ApplyGain(&mono, 1, wasapi::MakeFormat(48000, 1, 16), 255, 128);
    Check(mono == 12345, "mono pan has no right channel");
}
void TimeTests() {
    Check(wasapi::FramesToMs(0, 44100) == 0, "empty stream has zero written time");
    Check(wasapi::FramesToMs(44100, 44100) == 1000, "one second");
    Check(wasapi::FramesToMs(22050, 44100, 7000) == 7500, "seek base");
    Check(wasapi::FramesToMs(UINT64_MAX, 8000) == INT_MAX, "long duration saturation");
    Check(wasapi::FramesToMs(10, 0, 123) == 123, "unopened format");
}
}
int main() {
    try {
        QueueTests(); PcmTests(); TimeTests();
        std::cout << "CORE_PASS checks=" << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CORE_FAIL: " << error.what() << '\n';
        return 1;
    }
}
