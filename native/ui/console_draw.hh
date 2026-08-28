#pragma once
#include "aoas_source.hh"
#include "canvas.hh"
#include "frame_input.hh"

// ── The console, as a pure function of a snapshot ────────────────────────────
//
// Split out of the app object the same way navaLauncher splits launcher_draw.cc
// out of launcher_app.cc, and for the same payoff: this file includes no Host,
// no Vulkan and no JNI, so its geometry is testable headlessly and it can be
// rendered by vk_canvas's capture tool into a PNG without a phone.
//
// It draws and it hit-tests. It never calls the source — the caller applies
// whatever ConsoleAction comes back. That is what keeps a diagnostic that
// reconfigures the DAC out of a draw function.

enum class ConsoleAction {
    None,
    ForceDisconnect,
    OpenStream,
    PlayTone,
    ProbeCapture,
    NextFormat,
};

struct ConsoleState {
    float scroll = 0.0f;
    // The format the two output diagnostics open the stream at. An index into
    // the snapshot's outputFormats rather than a hardcoded 48 kHz: a DAC that
    // cannot do 48 exists, and a console that opens a stream the device
    // rejected teaches nothing about the device.
    int   formatIndex = 0;
    float contentHeight = 0.0f;   // written by draw(); the caller clamps scroll
};

ConsoleAction draw_console(Canvas& c,
                           const AoasSnapshot& s,
                           const std::vector<std::string>& log,
                           bool diagnosticsPermitted,
                           ConsoleState& st,
                           const FrameInput& in);
