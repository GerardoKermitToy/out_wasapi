// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 out_wasapi contributors. See LICENSE.
#include "pcm.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>

namespace wasapi {
bool ValidPcm(int rate, int channels, int bits) noexcept {
    return rate >= 8000 && rate <= 768000 && channels >= 1 && channels <= 8 &&
           (bits == 8 || bits == 16 || bits == 24 || bits == 32);
}
DWORD ChannelMask(int channels) noexcept {
    switch (channels) {
    case 1: return SPEAKER_FRONT_CENTER;
    case 2: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    case 3: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER;
    case 4: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
    case 5: return ChannelMask(4) | SPEAKER_FRONT_CENTER;
    case 6: return ChannelMask(5) | SPEAKER_LOW_FREQUENCY;
    case 7: return ChannelMask(3) | SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_CENTER |
                   SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;
    case 8: return ChannelMask(6) | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;
    default: return 0;
    }
}
WAVEFORMATEXTENSIBLE MakeFormat(int rate, int channels, int bits) noexcept {
    WAVEFORMATEXTENSIBLE f{};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = static_cast<WORD>(channels);
    f.Format.nSamplesPerSec = static_cast<DWORD>(rate);
    f.Format.wBitsPerSample = static_cast<WORD>(bits);
    f.Format.nBlockAlign = static_cast<WORD>(channels * (bits / 8));
    f.Format.nAvgBytesPerSec = f.Format.nSamplesPerSec * f.Format.nBlockAlign;
    f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = static_cast<WORD>(bits);
    f.dwChannelMask = ChannelMask(channels);
    f.SubFormat = kPcmFormat;
    return f;
}
std::wstring DescribeFormat(const WAVEFORMATEX* f) {
    if (!f) return L"Not available";
    const bool ext = f->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                     f->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    const auto* e = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
    const bool floating = f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                          (ext && IsEqualGUID(e->SubFormat, kFloatFormat));
    const bool pcm = f->wFormatTag == WAVE_FORMAT_PCM || (ext && IsEqualGUID(e->SubFormat, kPcmFormat));
    std::wostringstream s;
    s << f->nSamplesPerSec << L" Hz, " << f->nChannels << L" ch, " << f->wBitsPerSample
      << L"-bit " << (floating ? L"float" : pcm ? L"PCM" : L"other");
    if (ext) {
        if (e->Samples.wValidBitsPerSample != f->wBitsPerSample)
            s << L" (" << e->Samples.wValidBitsPerSample << L" valid bits)";
        s << L", mask 0x" << std::hex << e->dwChannelMask;
    }
    return s.str();
}
int FramesToMs(std::uint64_t frames, unsigned rate, int baseMs) noexcept {
    if (!rate) return (std::max)(baseMs, 0);
    const auto limit = static_cast<std::uint64_t>((std::numeric_limits<int>::max)());
    const auto base = static_cast<std::uint64_t>((std::max)(0, baseMs));
    if (frames / rate > limit / 1000) return static_cast<int>(limit);
    const auto ms = frames / rate * 1000 + (frames % rate) * 1000 / rate;
    return static_cast<int>((std::min)(limit, base + ms));
}
void ApplyGain(void* buffer, std::size_t frames, const WAVEFORMATEXTENSIBLE& f,
               int volume, int pan) noexcept {
    volume = std::clamp(volume, 0, 255);
    pan = std::clamp(pan, -128, 128);
    if (!buffer || (volume == 255 && pan == 0)) return;
    if (volume == 0) {
        std::memset(buffer, f.Format.wBitsPerSample == 8 ? 128 : 0, frames * f.Format.nBlockAlign);
        return;
    }
    std::array<double, 8> gains{};
    const double master = volume / 255.0;
    DWORD remaining = f.dwChannelMask;
    for (unsigned ch = 0; ch < f.Format.nChannels && ch < gains.size(); ++ch) {
        const DWORD speaker = remaining & (~remaining + 1); // lowest set bit
        remaining &= ~speaker;
        double balance = 1.0;
        if (f.Format.nChannels > 1) {
            if (pan > 0 && (speaker & (SPEAKER_FRONT_LEFT | SPEAKER_BACK_LEFT | SPEAKER_SIDE_LEFT)))
                balance = 1.0 - pan / 128.0;
            if (pan < 0 && (speaker & (SPEAKER_FRONT_RIGHT | SPEAKER_BACK_RIGHT | SPEAKER_SIDE_RIGHT)))
                balance = 1.0 + pan / 128.0;
        }
        gains[ch] = master * balance;
    }
    auto* p = static_cast<std::uint8_t*>(buffer);
    const unsigned bytes = f.Format.wBitsPerSample / 8;
    for (std::size_t i = 0; i < frames; ++i) {
        for (unsigned ch = 0; ch < f.Format.nChannels; ++ch, p += bytes) {
            const double gain = gains[ch];
            if (gain == 1.0) continue;
            if (bytes == 1) {
                const int sample = static_cast<int>(*p) - 128;
                *p = static_cast<std::uint8_t>(std::lround(sample * gain) + 128);
            } else if (bytes == 2) {
                std::int16_t sample;
                std::memcpy(&sample, p, 2);
                sample = static_cast<std::int16_t>(std::lround(sample * gain));
                std::memcpy(p, &sample, 2);
            } else if (bytes == 3) {
                const std::uint32_t u = p[0] | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16);
                const auto sample = static_cast<std::int32_t>(u & 0x800000 ? u | 0xff000000 : u);
                const auto value = static_cast<std::int32_t>(std::lround(sample * gain));
                p[0] = static_cast<std::uint8_t>(value);
                p[1] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(value) >> 8);
                p[2] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(value) >> 16);
            } else if (bytes == 4) {
                std::int32_t sample;
                std::memcpy(&sample, p, 4);
                const auto value = static_cast<std::int32_t>(std::llround(sample * gain));
                std::memcpy(p, &value, 4);
            }
        }
    }
}
} // namespace wasapi
