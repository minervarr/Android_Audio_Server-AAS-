// The desktop build of the console.
//
// It talks to a FakeSource, so it cannot see a real DAC — and that is not what
// it is for. Every layout question this screen has (does a 30-format ladder
// scroll cleanly, does "no capture path" read as a fact rather than a failure,
// are the diagnostics visibly disabled while a client owns the device) is
// answerable here in a second and costs an install cycle and a plugged-in
// interface on the phone.
//
// Same pattern, same reason, as navaLauncher's app/desktop_main.cc.
#include <cstring>
#include <memory>

#include "app_main.hh"   // app_shell: app_shell_main()
#include "console_app.hh"
#include "fake_source.hh"
#include "host.hh"

int app_shell_main(int argc, char** argv) {
    FakeSource::Scene scene = FakeSource::Scene::Idle;
    if (argc > 1) {
        if (!std::strcmp(argv[1], "unbound"))   scene = FakeSource::Scene::Unbound;
        else if (!std::strcmp(argv[1], "nodevice")) scene = FakeSource::Scene::NoDevice;
        else if (!std::strcmp(argv[1], "owned"))    scene = FakeSource::Scene::ClientOwns;
    }

    ConsoleApp app(std::make_unique<FakeSource>(scene));
    if (!app.create(make_host())) return 1;
    app.run();
    return 0;
}
