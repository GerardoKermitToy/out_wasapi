// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 out_wasapi contributors. See LICENSE.
#pragma once
#include <windows.h>
namespace wasapi {
void ShowConfig(HWND parent, HINSTANCE instance);
void CloseConfig();
void ShowAbout(HWND parent);
}
