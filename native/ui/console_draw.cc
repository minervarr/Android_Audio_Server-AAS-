#include "console_draw.hh"

#include "widgets.hh"

#include <algorithm>
#include <cstdio>
#include <string>

// The palette is deliberately flat and high-contrast. This is an instrument,
// not a product surface: every value on screen is either a fact from the bus or
// a fact from the driver, and a decorative gradient over either one makes a
// wrong reading marginally prettier and no easier to spot.
namespace {

constexpr Color kBg      {0.06f, 0.07f, 0.09f, 1.0f};
constexpr Color kPanel   {0.11f, 0.13f, 0.16f, 1.0f};
constexpr Color kText    {0.88f, 0.90f, 0.93f, 1.0f};
constexpr Color kDim     {0.55f, 0.59f, 0.65f, 1.0f};
constexpr Color kGood    {0.35f, 0.80f, 0.50f, 1.0f};
constexpr Color kBad     {0.92f, 0.40f, 0.38f, 1.0f};
constexpr Color kAccent  {0.36f, 0.62f, 0.95f, 1.0f};
constexpr Color kDisabled{0.22f, 0.24f, 0.28f, 1.0f};

std::string fmtTuple(const AoasFormat& f) {
    if (f.sampleRate == 0) return "-";
    char buf[96];
    // The subslot is a property of the OPEN stream, not of the descriptor, so
    // it is unknown for a format that is merely offered. Printing "0-byte
    // subslot" on every line, as this did, states a falsehood thirty times and
    // buries the one line where the number is real.
    if (f.subslotBytes > 0)
        std::snprintf(buf, sizeof buf, "%d Hz / %d ch / %d bit (%d-byte subslot)",
                      f.sampleRate, f.channels, f.bitDepth, f.subslotBytes);
    else
        std::snprintf(buf, sizeof buf, "%d Hz / %d ch / %d bit",
                      f.sampleRate, f.channels, f.bitDepth);
    return buf;
}

// ── DoP: what a PCM format could CARRY ───────────────────────────────────────
//
// DoP hides a DSD bit stream inside ordinary PCM: each 24-bit frame carries 16
// DSD bits plus an 8-bit 0x05/0xFA marker the DAC watches for. So the PCM rate
// is the DSD rate divided by 16, and the format needs at least 24 bits — a
// 16-bit frame has no room for the marker.
//
// Returns the DSD multiple (64, 128, …) this PCM format could carry, or 0.
//
// This is arithmetic about the CARRIER, not a claim that the device decodes
// DoP. Nothing in USB Audio declares DoP support: a DAC that unwraps it and one
// that plays it as white noise present identical descriptors. The only honest
// label is "could carry", and the section note says so.
int dopLevel(const AoasFormat& f) {
    if (f.bitDepth < 24) return 0;
    const long dsdBits = static_cast<long>(f.sampleRate) * 16;
    if (dsdBits % 2822400 != 0) return 0;
    return static_cast<int>(dsdBits / 2822400) * 64;
}

}  // namespace

ConsoleAction draw_console(Canvas& c,
                           const AoasSnapshot& s,
                           const std::vector<std::string>& log,
                           bool diagnosticsPermitted,
                           ConsoleState& st,
                           const FrameInput& in) {
    ConsoleAction action = ConsoleAction::None;

    c.rect(0, 0, c.w() + c.left() * 2, c.h() + c.top() * 2, kBg);

    // ── Scroll, clamped BEFORE anything is drawn ────────────────────────────
    //
    // This used to clamp at the END of the function, after the frame had
    // already been submitted. The visible result was the reported one: a drag
    // past the end scrolled, was drawn, and then snapped back on the next
    // redraw a second later when the timer fired. The clamp has to happen
    // before the content is placed, against the height the LAST frame measured
    // -- which is the only height available before laying this one out.
    const float barHeight = std::max(13.0f, c.w() * 0.026f) * 1.55f * 2.6f;
    const float viewH     = c.h() - barHeight;
    const float maxScroll = std::max(0.0f, st.contentHeight - viewH);
    st.scroll = std::clamp(st.scroll, 0.0f, maxScroll);

    const float pad  = c.pad();
    const float body = std::max(13.0f, c.w() * 0.026f);
    const float head = body * 1.25f;
    const float line = body * 1.55f;
    const float x0   = c.left() + pad;
    const float x1   = c.right() - pad;

    // ── Why the content is CLIPPED rather than simply drawn first ───────────
    //
    // vk_canvas renders rectangles and MSDF text in two different passes, and
    // the text pass runs last. So a solid bar drawn after the content still
    // ends up UNDER every glyph — which is what the button bar looked like:
    // rows scrolling straight through the buttons, reading as if the bar were
    // translucent. It never was. Painter's order cannot fix this and no amount
    // of alpha would either; the content simply must not be emitted down there.
    //
    // The clip is tile-granular (~16 px per vk_canvas's own note), so the
    // viewport stops a hair short of the bar: at exactly barY a glyph may bleed
    // a few pixels into it, and that bleed is the artifact this is fixing.
    const Canvas::ClipState outerClip = c.saveClip();
    const float clipBottom = c.bottom() - barHeight - line * 0.25f;
    c.setClip(c.left(), c.top(), c.w(), clipBottom - c.top());

    float y = c.top() + pad - st.scroll;

    auto row = [&](const std::string& label, const std::string& value, Color vc) {
        c.text(label, x0, y, body, kDim);
        c.textRight(value, x1, y, body, vc);
        y += line;
    };
    auto section = [&](const char* title) {
        y += line * 0.4f;
        c.text(title, x0, y, head, kAccent);
        y += line * 1.1f;
        c.rect(x0, y - line * 0.55f, x1 - x0, 1.0f, kDisabled);
    };

    // ── The link ────────────────────────────────────────────────────────────
    section("SERVICE");
    row("bound", s.bound ? "yes" : "no", s.bound ? kGood : kBad);
    if (!s.bindError.empty()) row("error", s.bindError, kBad);

    // ── The device ──────────────────────────────────────────────────────────
    section("DEVICE");
    // Two rows, because they are two facts. "attached" is what the format
    // ladder below depends on; "stream open" is whether anything is running.
    row("attached", s.deviceAttached ? "yes" : "no",
        s.deviceAttached ? kGood : kDim);
    row("stream open", s.deviceReady ? "yes" : "no", s.deviceReady ? kGood : kDim);
    row("device", !s.deviceInfo.empty() ? s.deviceInfo
                  : (s.deviceAttached ? "(attached, no stream yet)" : "(none)"),
        s.deviceInfo.empty() ? kDim : kText);
    if (s.uacVersion > 0) {
        row("UAC version", std::to_string(s.uacVersion), kText);
        // Reported as a fact, not as a problem. A DAC without a hardware
        // volume control is not broken; it means AOAS has no volume to offer,
        // and since AOAS applies no gain of its own (rule 2) that is the end
        // of the matter rather than the start of a software-volume feature.
        row("hardware volume", s.hardwareVolume ? "yes" : "no", kText);
        row("hardware mute",   s.hardwareMute   ? "yes" : "no", kText);
    }

    // ── Ownership ───────────────────────────────────────────────────────────
    section("OWNERSHIP");
    row("owner", s.ownerUid < 0 ? "(free)" : s.ownerLabel,
        s.ownerUid < 0 ? kDim : kAccent);
    row("active format", fmtTuple(s.activeFormat), kText);
    row("pending playback", std::to_string(s.pendingPlaybackMs) + " ms", kText);

    // ── Formats ─────────────────────────────────────────────────────────────
    //
    // Whole tuples, one per line. Three independent lists would be shorter and
    // would also claim combinations the device does not accept — the axes are
    // coupled, and a console that implies otherwise is worse than no console.
    section("OUTPUT FORMATS (PCM)");
    if (s.outputFormats.empty()) {
        c.text("(none reported)", x0, y, body, kDim);
        y += line;
    } else {
        for (size_t i = 0; i < s.outputFormats.size(); ++i) {
            const bool sel = static_cast<int>(i) == st.formatIndex;
            // Centred on the text's OPTICAL middle. The trap here is that
            // Canvas::text() takes the TOP-LEFT of the text box, not a
            // baseline — so the row's glyphs run from y down to about
            // y + body, and their middle is near y + body/2. Treating `y` as a
            // baseline (banding upward from it) put the highlight entirely
            // ABOVE the row it marks, hard against the section header, which
            // is what it looked like.
            if (sel) {
                const float mid = y + body * 0.5f;
                c.rect(x0 - pad * 0.3f, mid - line * 0.5f,
                       x1 - x0 + pad * 0.6f, line, kPanel, 4.0f);
            }
            c.text(fmtTuple(s.outputFormats[i]), x0, y, body, sel ? kText : kDim);
            // Marked on the PCM row rather than given a section of its own,
            // because a DoP stream IS this PCM format — there is no separate
            // endpoint to list. Two ways to reach one DSD rate, shown where
            // each actually lives.
            if (const int lvl = dopLevel(s.outputFormats[i])) {
                char d[32];
                std::snprintf(d, sizeof d, "could carry DoP DSD%d", lvl);
                c.textRight(d, x1, y, body * 0.85f, kAccent);
            }
            y += line;
        }
    }

    // ── DSD ─────────────────────────────────────────────────────────────────
    //
    // Its own section, never merged into OUTPUT FORMATS. A native DSD
    // alt-setting reports a container width where PCM reports a sample depth,
    // so listing the two together invites reading a DSD line as a PCM mode and
    // writing PCM into it, which is full-scale noise rather than a wrong tone.
    section(s.dsdFlaggedByDescriptor ? "DSD (native, RAW_DATA alt-setting)"
                                     : "DSD (inferred -- not device-declared)");
    if (s.dsdFormats.empty()) {
        // Deliberately two lines. "No native DSD" and "no DSD at all" are
        // different claims, and only the first one is being made here.
        c.text("(no native DSD alt-setting advertised)", x0, y, body, kDim);
        y += line * 0.9f;
        c.text("DoP rides inside a PCM stream and needs none", x0 + pad, y,
               body * 0.85f, kDim);
        y += line;
    } else {
        // Two genuinely different claims, said differently. When the device
        // set bit 31 this is ITS statement and the console reports it as one;
        // when only the duplicate-alt heuristic fired, the guess is ours and
        // has to be labelled as ours.
        c.text(s.dsdFlaggedByDescriptor
                   ? "device sets bmFormats bit 31 on these alt-settings"
                   : "inferred from duplicate alt-settings; device did not say",
               x0, y, body * 0.85f, kDim);
        y += line * 0.9f;
        // The rate LIST on a DSD alt-setting is routinely copied from the PCM
        // one, so a device can advertise rates its analogue section will refuse
        // -- the descriptor is a claim about the endpoint, not a guarantee
        // about the DAC. Anything above the manufacturer's stated ceiling is
        // therefore expected to fail at configure() time, and the only way to
        // know which is to try. Hence "advertised", never "supported".
        c.text("advertised by the endpoint; not proof the DAC accepts them",
               x0, y, body * 0.85f, kDim);
        y += line * 0.9f;
        // Said here because this is where a reader looks for "and what about
        // DoP?", and the answer is that no descriptor anywhere can tell them.
        c.text("DoP is undeclarable: see the PCM rows above for carriers",
               x0, y, body * 0.85f, kDim);
        y += line * 0.9f;
        for (const auto& f : s.dsdFormats) {
            char b[96];
            std::snprintf(b, sizeof b, "%d Hz / %d ch / %d-byte container",
                          f.sampleRate, f.channels, f.subslotBytes);
            c.text(b, x0, y, body, kText);

            // A native DSD alt-setting carries the bit stream packed into the
            // container, so the DSD rate is frames x container bits -- NOT the
            // frame rate itself. 88200 Hz in a 4-byte container is 2.8224 MHz,
            // which is DSD64. A candidate whose arithmetic lands nowhere near a
            // real DSD multiple is almost certainly a PCM alt-setting that the
            // dongle mislabelled, and showing no label is how it says so.
            const long bits = static_cast<long>(f.sampleRate) * f.subslotBytes * 8;
            const long mult = bits / 2822400;
            if (mult >= 1 && bits % 2822400 == 0) {
                char m[32];
                std::snprintf(m, sizeof m, "DSD%ld", mult * 64);
                c.textRight(m, x1, y, body, kAccent);
            } else {
                c.textRight("not a DSD rate", x1, y, body * 0.85f, kDim);
            }
            y += line;
        }
    }

    section("CAPTURE FORMATS");
    if (s.captureFormats.empty()) {
        // Said plainly rather than left blank: the HiBy FC4 genuinely has no
        // capture path, and "no capture path on this device" and "we did not
        // look" must not render identically.
        c.text("(this device has no capture path)", x0, y, body, kDim);
        y += line;
    } else {
        for (const auto& f : s.captureFormats) {
            c.text(fmtTuple(f), x0, y, body, kDim);
            y += line;
        }
    }

    // ── Android's own view of the bus ───────────────────────────────────────
    section("USB BUS (as Android reports it)");
    if (s.bus.empty()) {
        c.text("(no USB devices)", x0, y, body, kDim);
        y += line;
    } else {
        for (const auto& d : s.bus) {
            c.text(d.name.empty() ? "(unnamed)" : d.name, x0, y, body, kText);
            if (d.vendorId || d.productId) {
                char id[64];
                std::snprintf(id, sizeof id, "%04x:%04x", d.vendorId, d.productId);
                c.textRight(id, x1, y, body, kDim);
            }
            y += line * 0.85f;
            char detail[128];
            if (d.interfaces || d.endpoints)
                std::snprintf(detail, sizeof detail, "%d interfaces, %d endpoints%s",
                              d.interfaces, d.endpoints,
                              d.isAudioClass ? ", audio class" : "");
            else
                std::snprintf(detail, sizeof detail, "%s (counts need UsbManager)",
                              d.isAudioClass ? "audio class" : "unknown class");
            c.text(detail, x0 + pad, y, body * 0.9f, kDim);
            c.textRight(d.permissionHeld ? "permission held" : "no permission",
                        x1, y, body * 0.9f,
                        d.permissionHeld ? kGood : kBad);
            y += line;
        }
    }

    // ── Log ─────────────────────────────────────────────────────────────────
    section("LOG");
    for (const auto& l : log) {
        c.text(l, x0, y, body * 0.9f, kDim);
        y += line * 0.9f;
    }

    // Plus the bar, because the last line of the LOG must be scrollable clear
    // of it. Without this the section header ends up drawn behind the buttons
    // and cannot be reached at any scroll position.
    st.contentHeight = (y + st.scroll) - c.top() + barHeight + pad;

    // Everything below belongs to the bar, which is NOT part of the scrolling
    // content and must not inherit its viewport.
    c.restoreClip(outerClip);

    // ── The button bar ──────────────────────────────────────────────────────
    //
    // Pinned to the bottom rather than scrolled with the content: a forced
    // disconnect is the one thing here somebody may need in a hurry, and
    // hunting for it below a 30-line format list is not that.
    const float barH = barHeight;
    const float barY = c.bottom() - barH;
    // Down to the true bottom of the window, not just barH tall: on a phone the
    // gesture inset sits below c.bottom(), and a bar that stops short of it
    // leaves a strip of page background under the buttons.
    c.rect(c.left(), barY, c.w(), c.bottom() + c.top() - barY, kPanel);
    // A hairline where the scrolling content ends. Without it the bar and the
    // page share a value close enough that the boundary reads as a gap in the
    // list rather than as an edge.
    c.rect(c.left(), barY, c.w(), 1.0f, kDisabled);

    struct Btn { const char* label; ConsoleAction act; bool needsDiag; };
    const Btn btns[] = {
        {"Disconnect",  ConsoleAction::ForceDisconnect, false},
        {"Open stream", ConsoleAction::OpenStream,      true},
        {"1 kHz tone",  ConsoleAction::PlayTone,        true},
        {"Probe in",    ConsoleAction::ProbeCapture,    true},
        {"Format >",    ConsoleAction::NextFormat,      false},
    };
    const int n = static_cast<int>(sizeof btns / sizeof btns[0]);
    const float gap = pad * 0.4f;
    const float bw  = (c.w() - pad * 2 - gap * (n - 1)) / n;

    for (int i = 0; i < n; ++i) {
        Rect r{x0 + (bw + gap) * i, barY + line * 0.55f, bw, line * 1.5f};
        // Greyed rather than hidden, and greyed rather than left live to fail:
        // a diagnostic that reconfigures the DAC must visibly not be available
        // while a real client owns it, since the reason it is unavailable is
        // the whole ownership rule this project exists to enforce.
        const bool enabled = !btns[i].needsDiag || diagnosticsPermitted;
        widgets::drawFitButton(c, r, btns[i].label,
                               enabled ? kAccent : kDisabled,
                               enabled ? kBg : kDim, 6.0f);
        if (enabled && in.pointerWentUp && r.contains(in.pointerX, in.pointerY))
            action = btns[i].act;
    }

    return action;
}
