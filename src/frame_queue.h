// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 out_wasapi contributors. See LICENSE.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace wasapi {
// No allocations after Reset. Callers serialize access; units are whole frames.
class FrameQueue {
public:
    void Reset(std::size_t frames, std::size_t frameBytes) {
        storage_.assign(frames * frameBytes, 0);
        frameBytes_ = frameBytes;
        capacity_ = frames;
        Clear();
    }
    void Clear() noexcept { head_ = used_ = 0; }
    std::size_t Size() const noexcept { return used_; }
    std::size_t Capacity() const noexcept { return capacity_; }
    std::size_t Free() const noexcept { return capacity_ - used_; }
    bool Push(const void* source, std::size_t frames) noexcept {
        if (frames > Free() || (frames && !source)) return false;
        if (!frames) return true;
        const auto tail = (head_ + used_) % capacity_;
        const auto first = (std::min)(frames, capacity_ - tail);
        std::memcpy(storage_.data() + tail * frameBytes_, source, first * frameBytes_);
        std::memcpy(storage_.data(), static_cast<const std::uint8_t*>(source) + first * frameBytes_,
                    (frames - first) * frameBytes_);
        used_ += frames;
        return true;
    }
    std::size_t Peek(void* destination, std::size_t frames, std::size_t offset = 0) const noexcept {
        if (offset >= used_) return 0;
        frames = (std::min)(frames, used_ - offset);
        if (!frames) return 0;
        const auto start = (head_ + offset) % capacity_;
        const auto first = (std::min)(frames, capacity_ - start);
        std::memcpy(destination, storage_.data() + start * frameBytes_, first * frameBytes_);
        std::memcpy(static_cast<std::uint8_t*>(destination) + first * frameBytes_, storage_.data(),
                    (frames - first) * frameBytes_);
        return frames;
    }
    void Consume(std::size_t frames) noexcept {
        frames = (std::min)(frames, used_);
        if (capacity_) head_ = (head_ + frames) % capacity_;
        used_ -= frames;
    }
private:
    std::vector<std::uint8_t> storage_;
    std::size_t frameBytes_ = 0, capacity_ = 0, head_ = 0, used_ = 0;
};
} // namespace wasapi
