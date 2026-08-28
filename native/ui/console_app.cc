#include "console_app.hh"

#include <algorithm>

#include "app_paths.hh"     // app_shell: stateDir()
#include "host.hh"          // app_shell
#include "renderer.hh"      // vk_canvas
#include "msdf.hh"       // vulkan_font_engine: MsdfFont

namespace {
// The face and its baked atlas. One font: this screen is monospaced-in-spirit
// tabular data, and a second weight would buy nothing a colour does not.
constexpr const char* kFontAsset = "fonts/ui.otf";
constexpr const char* kFontCache = "/aoas_font_atlas.bin";

// The one timer: how often the snapshot is re-read. A second is fast enough to
// watch a DAC being plugged in and slow enough that the console is not itself a
// load on the process it is measuring.
constexpr int kPollTimer  = 1;
constexpr int kPollPeriod = 1000;
}  // namespace

ConsoleApp::ConsoleApp(std::unique_ptr<AoasSource> source)
    : source_(std::move(source)) {}

ConsoleApp::~ConsoleApp() = default;

bool ConsoleApp::create(std::unique_ptr<Host> host) {
    host_ = std::move(host);
    if (!host_ || !host_->init(this)) return false;

    renderer_ = std::make_unique<Renderer>(host_->surfaceProvider(), host_->assetReader());

    auto font = std::make_unique<MsdfFont>();
    // The cache path must be WRITABLE, and exeDir() is not on Android — it is
    // where the APK's read-only payload lives. Getting this wrong does not
    // fail; it silently re-bakes the atlas on every single launch, which on a
    // 42,000-kerning-pair face is 34 seconds of msdfgen measured on an S23
    // Ultra, long enough that Android terminates the window before a frame is
    // ever seen. stateDir() is the app's own data directory.
    if (font->generate(host_->assetReader(), kFontAsset,
                       (app_paths::stateDir() + kFontCache).c_str())) {
        renderer_->initMsdf(*font);
        font_ = std::move(font);
    }
    // A missing font is survivable: Canvas falls back to stroked glyphs, which
    // are ugly and readable. Refusing to start would leave the one screen that
    // explains what is wrong unreachable.

    host_->startTimer(kPollTimer, kPollPeriod);
    host_->showWindow();
    return true;
}

void ConsoleApp::run() {
    while (running_) {
        host_->pump(/*haveWork=*/dirty_);
        if (host_->quitRequested()) break;
        if (dirty_ && surfaceOk_) {
            draw();
            dirty_ = false;
        }
    }
}

void ConsoleApp::shutdown() {
    running_ = false;
    if (host_) host_->stopTimer(kPollTimer);
    renderer_.reset();
    font_.reset();
}

void ConsoleApp::onSurfaceLost() {
    // GPU-side state dies with the surface; the snapshot and the scroll
    // position are CPU-side and survive. Getting this split wrong is invisible
    // until the second visit to the app, when every glyph is gone.
    surfaceOk_ = false;
    renderer_.reset();
}

bool ConsoleApp::onSurfaceRecreated() {
    renderer_ = std::make_unique<Renderer>(host_->surfaceProvider(), host_->assetReader());
    if (font_) renderer_->initMsdf(*font_);
    surfaceOk_ = true;
    dirty_     = true;
    return true;
}

void ConsoleApp::onTimer(int timerId) {
    if (timerId != kPollTimer) return;
    // snapshot() is what actually refreshes; the console never caches it.
    source_->snapshot();
    dirty_ = true;
}

void ConsoleApp::onMouseMove(int x, int y) {
    input_.pointerX = static_cast<float>(x);
    input_.pointerY = static_cast<float>(y);
}

void ConsoleApp::onLButtonDown(int x, int y) {
    input_.pointerX = static_cast<float>(x);
    input_.pointerY = static_cast<float>(y);
    input_.pointerDown     = true;
    input_.pointerWentDown = true;
    dirty_ = true;
}

void ConsoleApp::onLButtonUp(int x, int y) {
    input_.pointerX = static_cast<float>(x);
    input_.pointerY = static_cast<float>(y);
    input_.pointerDown   = false;
    input_.pointerWentUp = true;
    dirty_ = true;
    // The action is resolved by the NEXT draw, which hit-tests the buttons
    // where they were actually rendered. Hit-testing here would mean keeping a
    // second copy of the bar's geometry, and two copies of a rectangle is how a
    // button ends up drawn in one place and pressed in another.
    draw();
    dirty_ = false;
}

void ConsoleApp::onMouseWheel(int, int, int delta) {
    state_.scroll -= static_cast<float>(delta);
    dirty_ = true;
}

void ConsoleApp::apply(ConsoleAction a) {
    const AoasSnapshot& s = source_->snapshot();
    switch (a) {
    case ConsoleAction::None: return;
    case ConsoleAction::ForceDisconnect:
        // The one interruption CLAUDE.md permits: a deliberate user action,
        // equivalent to unplugging the DAC.
        source_->forceDisconnect();
        break;
    case ConsoleAction::NextFormat:
        if (!s.outputFormats.empty())
            state_.formatIndex =
                (state_.formatIndex + 1) % static_cast<int>(s.outputFormats.size());
        break;
    case ConsoleAction::OpenStream:
    case ConsoleAction::PlayTone:
    case ConsoleAction::ProbeCapture: {
        if (s.outputFormats.empty()) break;
        const AoasFormat& f =
            s.outputFormats[static_cast<size_t>(state_.formatIndex)];
        if (a == ConsoleAction::OpenStream)
            source_->debugOpenStream(f.sampleRate, f.channels, f.bitDepth);
        else if (a == ConsoleAction::PlayTone)
            source_->debugTone(f.sampleRate, f.channels, f.bitDepth, 2);
        else
            source_->debugCapture(f.sampleRate, f.channels, f.bitDepth);
        break;
    }
    }
    dirty_ = true;
}

void ConsoleApp::draw() {
    if (!renderer_) return;

    const MonitorInfo mon = host_->primaryMonitor();
    const SafeInsets  ins = host_->safeInsets();
    const uint32_t w = static_cast<uint32_t>(mon.bounds.right - mon.bounds.left);
    const uint32_t h = static_cast<uint32_t>(mon.bounds.bottom - mon.bounds.top);
    if (w == 0 || h == 0) return;

    curves_.clear();
    quads_.clear();
    Canvas c(curves_, w, h, nullptr,
             static_cast<float>(ins.top),  static_cast<float>(ins.bottom),
             static_cast<float>(ins.left), static_cast<float>(ins.right));
    if (font_) c.useMsdf(font_.get(), &quads_);

    const ConsoleAction act = draw_console(c, source_->snapshot(), source_->log(),
                                           source_->diagnosticsPermitted(),
                                           state_, input_);

    if (font_) renderer_->initMsdf(*font_);
    renderer_->draw(curves_, /*rotation=*/0, {}, {}, quads_);

    input_.beginFrame();
    if (act != ConsoleAction::None) apply(act);
}
