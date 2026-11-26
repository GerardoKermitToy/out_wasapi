// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 out_wasapi contributors. See LICENSE.
#include "audio_session_probe.h"
#include <iostream>
#include <string>

// Read-only mixer observer for the disposable real-Winamp smoke test.
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 3 || std::wstring(argv[1]) != L"--read")
            throw std::runtime_error("Usage: mixer_probe --read process-id (default multimedia endpoint)");
        const DWORD pid = std::stoul(argv[2]);
        if (!pid) throw std::runtime_error("A nonzero process ID is required");
        test_audio::SessionProbe probe(pid);
        std::cout << "{\"ProcessId\":" << pid << ",\"Volume\":" << probe.Level()
                  << ",\"Muted\":" << (probe.Muted() ? "true" : "false")
                  << ",\"EndpointVolume\":" << probe.EndpointLevel()
                  << ",\"EndpointMuted\":" << (probe.EndpointMuted() ? "true" : "false") << "}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MIXER_PROBE_FAIL: " << error.what() << '\n';
        return 1;
    }
}
