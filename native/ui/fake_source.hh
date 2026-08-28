#pragma once
#include "aoas_source.hh"

// ── A scripted AOAS, for the desktop build ───────────────────────────────────
//
// Not a mock in the testing sense — nothing asserts against it. It exists so
// the console's layout can be developed against the states that are HARD to
// produce on real hardware and easy to get wrong: a device with thirty output
// formats and no capture path (the HiBy FC4, measured), a client holding
// ownership so every diagnostic is greyed, an unbound service.
//
// The states are modelled on what was actually observed on a Galaxy S23 Ultra
// (see CLAUDE.md, "Verified on hardware") rather than invented, because a fake
// that renders comfortably and a device that does not is a fake that cost time
// instead of saving it.
class FakeSource : public AoasSource {
public:
    enum class Scene { Unbound, NoDevice, Idle, ClientOwns };

    explicit FakeSource(Scene scene = Scene::Idle);

    const AoasSnapshot& snapshot() override { return snap_; }
    void forceDisconnect() override;
    bool debugOpenStream(int sampleRate, int channels, int bitDepth) override;
    long debugTone(int sampleRate, int channels, int bitDepth, int seconds) override;
    DebugCaptureResult debugCapture(int sampleRate, int channels, int bitDepth) override;
    bool diagnosticsPermitted() override { return snap_.bound && snap_.ownerUid < 0; }
    const std::vector<std::string>& log() const override { return log_; }

    void setScene(Scene s);

private:
    void note(const std::string& s);

    AoasSnapshot snap_;
    std::vector<std::string> log_;
};
