// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 out_wasapi contributors. See LICENSE.
#include "winamp_output_abi.h"
#include "engine.h"
#include "dialog.h"
#include "settings.h"

namespace {
extern Out_Module module;
char description[] = "Independent WASAPI Output v1.2.0";
void __cdecl Config(HWND parent) {
    try { wasapi::InitializeSettings(module.hMainWindow); wasapi::ShowConfig(parent, module.hDllInstance); }
    catch (...) { MessageBoxW(parent, L"Cannot open WASAPI settings.", L"WASAPI Output", MB_OK | MB_ICONERROR); }
}
void __cdecl About(HWND parent) { wasapi::ShowAbout(parent); }
void __cdecl Init() { try { wasapi::InitializeSettings(module.hMainWindow); } catch (...) {} }
void __cdecl Quit() { wasapi::CloseConfig(); wasapi::AudioEngine().Close(); }
int __cdecl Open(int rate, int channels, int bits, int, int) {
    // As with most classic outputs, configuration owns buffering. In particular
    // the -666 video-sync sentinel is not a duration and must never size a queue.
    try { return wasapi::AudioEngine().Open(rate, channels, bits, wasapi::GetSettings()); }
    catch (...) { return -1; }
}
void __cdecl Close() { wasapi::AudioEngine().Close(); }
int __cdecl Write(char* data, int bytes) { return wasapi::AudioEngine().Write(data, bytes); }
int __cdecl CanWrite() { return wasapi::AudioEngine().CanWrite(); }
int __cdecl IsPlaying() { return wasapi::AudioEngine().IsPlaying(); }
int __cdecl Pause(int value) { return wasapi::AudioEngine().Pause(value != 0); }
void __cdecl SetVolume(int value) { wasapi::AudioEngine().SetVolume(value); }
void __cdecl SetPan(int value) { wasapi::AudioEngine().SetPan(value); }
void __cdecl Flush(int value) { wasapi::AudioEngine().Flush(value); }
int __cdecl OutputTime() { return wasapi::AudioEngine().OutputTime(); }
int __cdecl WrittenTime() { return wasapi::AudioEngine().WrittenTime(); }
Out_Module module = {OUT_VER, description, 0x57534150, nullptr, nullptr, Config, About,
    Init, Quit, Open, Close, Write, CanWrite, IsPlaying, Pause, SetVolume, SetPan,
    Flush, OutputTime, WrittenTime};
}
extern "C" Out_Module* __cdecl winampGetOutModule() { return &module; }
extern "C" int __cdecl winampUninstallPlugin(HINSTANCE, HWND, int) { return OUT_PLUGIN_UNINSTALL_REBOOT; }
extern "C" void __cdecl winampGetOutModeChange(int mode) {
    if ((mode & OUT_SET) == OUT_UNSET && !(mode & OUT_PLAYBACK)) wasapi::AudioEngine().Close();
}
