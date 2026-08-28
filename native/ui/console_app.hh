#pragma once
#include <memory>
#include <vector>

#include "app_view.hh"      // app_shell
#include "aoas_source.hh"
#include "console_draw.hh"

class Host;
class Renderer;
class MsdfFont;

// ── The AOAS console ─────────────────────────────────────────────────────────
//
// One screen: everything AOAS and the USB bus currently believe, and five
// buttons. It replaces AoasDebugActivity, which built the same screen out of
// LinearLayout and TextView in Java.
//
// It is still not the product. AOAS's real interface is its notification, and
// this exists to answer the questions that can only be answered on real
// hardware. What changed is where it runs: its own process, so that a Vulkan
// frame and the thread feeding the isochronous endpoint never share a
// scheduler slot.
//
// Redraws are event-driven, not free-running. Nothing on this screen moves on
// its own, and a console that renders at 60 fps to show an unchanging format
// list is a console that heats the phone it is diagnosing.
class ConsoleApp : public AppView {
public:
    explicit ConsoleApp(std::unique_ptr<AoasSource> source);
    ~ConsoleApp() override;

    bool create(std::unique_ptr<Host> host);
    void run();

    // ── AppView ─────────────────────────────────────────────────────────────
    void onHostResized() override            { dirty_ = true; }
    void onHostLayoutInvalidated() override  { dirty_ = true; }
    void onHostExposed() override            { dirty_ = true; }
    void shutdown() override;

    void onSurfaceLost() override;
    bool onSurfaceRecreated() override;

    void onLButtonDown(int x, int y) override;
    void onLButtonUp(int x, int y) override;
    void onMouseMove(int x, int y) override;
    void onMouseWheel(int x, int y, int delta) override;
    void onTimer(int timerId) override;

private:
    void draw();
    void apply(ConsoleAction a);

    std::unique_ptr<Host>       host_;
    std::unique_ptr<Renderer>   renderer_;
    std::unique_ptr<MsdfFont>   font_;
    std::unique_ptr<AoasSource> source_;

    std::vector<float> curves_;
    std::vector<float> quads_;

    ConsoleState state_;
    FrameInput   input_;

    bool running_   = true;
    bool dirty_     = true;
    bool surfaceOk_ = true;
};
