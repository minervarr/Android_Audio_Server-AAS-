#pragma once
#include "aoas_source.hh"

// ── The real AoasSource: the server, in this process ─────────────────────────
//
// The console is a NativeActivity in AOAS's own process, so this reads
// aoas::processServer() directly. That is the same access the Java console had
// — AoasDebugActivity reached the diagnostics by casting the in-process binder
// — expressed in C++ instead of through a cast.
//
// It is NOT a client of IAoas and must never become one. The three diagnostics
// reconfigure the stream and generate samples, which is precisely what a client
// may not make AOAS do (CLAUDE.md, rule 2); they stay off the AIDL contract by
// staying on this side of it.
class ServiceSource : public AoasSource {
public:
    const AoasSnapshot& snapshot() override;
    void forceDisconnect() override;
    bool debugOpenStream(int sampleRate, int channels, int bitDepth) override;
    long debugTone(int sampleRate, int channels, int bitDepth, int seconds) override;
    DebugCaptureResult debugCapture(int sampleRate, int channels, int bitDepth) override;
    bool diagnosticsPermitted() override;
    const std::vector<std::string>& log() const override { return log_; }

private:
    void note(const std::string& s);

    AoasSnapshot snap_;
    std::vector<std::string> log_;
};
