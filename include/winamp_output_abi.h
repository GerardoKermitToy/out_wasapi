// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 out_wasapi contributors. See LICENSE.
#pragma once
// Standalone declaration of the classic Winamp output ABI. Do not pack this
// structure: the host uses native Windows alignment and __cdecl callbacks.
#include <windows.h>
#include <cstddef>
#include <cstdint>

inline constexpr int OUT_VER = 0x10; // Narrow description, supports older hosts.
inline constexpr int OUT_UNSET = 0;
inline constexpr int OUT_SET = 1;
inline constexpr int OUT_PLAYBACK = 0x100;
inline constexpr int OUT_PLUGIN_UNINSTALL_REBOOT = 0;
inline constexpr UINT WM_WA_IPC = WM_USER;
inline constexpr LPARAM IPC_GETINIFILEW = 1334; // Winamp 5.58+

struct Out_Module {
    int version;
    char* description;
    std::intptr_t id; // Host may replace this with its own filename pointer.
    HWND hMainWindow;
    HINSTANCE hDllInstance;
    void (__cdecl* Config)(HWND);
    void (__cdecl* About)(HWND);
    void (__cdecl* Init)();
    void (__cdecl* Quit)();
    int (__cdecl* Open)(int rate, int channels, int bits, int bufferMs, int prebufferMs);
    void (__cdecl* Close)();
    int (__cdecl* Write)(char* data, int bytes); // 0 accepted, 1 busy/error; never partial.
    int (__cdecl* CanWrite)(); // Free bytes, frame-aligned.
    int (__cdecl* IsPlaying)(); // Includes BOTH software queue and endpoint padding.
    int (__cdecl* Pause)(int); // Return previous state.
    void (__cdecl* SetVolume)(int); // 0..255; -666 queries/retains current value.
    void (__cdecl* SetPan)(int); // -128..128.
    void (__cdecl* Flush)(int milliseconds);
    int (__cdecl* GetOutputTime)();
    int (__cdecl* GetWrittenTime)();
};
static_assert(sizeof(void*) == 4, "Classic Winamp is a 32-bit host");
static_assert(offsetof(Out_Module, Config) == 20, "Output ABI alignment");
static_assert(offsetof(Out_Module, Open) == 36, "Output ABI callback order");
static_assert(sizeof(Out_Module) == 80, "Output ABI size");
using GetOutModule = Out_Module* (__cdecl*)();
