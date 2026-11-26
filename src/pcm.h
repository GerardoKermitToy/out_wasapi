// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 out_wasapi contributors. See LICENSE.
#pragma once
#include <windows.h>
#include <mmreg.h>
#include <cstdint>
#include <string>

namespace wasapi {
inline constexpr GUID kPcmFormat{1, 0, 0x0010, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
inline constexpr GUID kFloatFormat{3, 0, 0x0010, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
bool ValidPcm(int rate, int channels, int bits) noexcept;
DWORD ChannelMask(int channels) noexcept;
WAVEFORMATEXTENSIBLE MakeFormat(int rate, int channels, int bits) noexcept;
std::wstring DescribeFormat(const WAVEFORMATEX* format);
// In-place integer PCM attenuation. Unity is an exact no-op, including 32-bit.
void ApplyGain(void* buffer, std::size_t frames, const WAVEFORMATEXTENSIBLE& format,
               int volume, int pan) noexcept;
int FramesToMs(std::uint64_t frames, unsigned rate, int baseMs = 0) noexcept;
} // namespace wasapi
