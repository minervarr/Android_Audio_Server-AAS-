# Android_Audio_Server-AAS-

**AOAS — Android One Audio Server.** A standalone Android background service
that owns the USB permission grant and the live USB connection to one external
audio interface, so switching between the user's Android apps never requires
re-granting Android's USB permission dialog and never re-locks the DAC's
clock. It embeds `audio_engine`'s USB Audio Class driver (`ae_usb`) and
relays raw PCM from the owning client app through a shared-memory ring
buffer — zero DSP, zero resampling, bit-exact passthrough.

Read `CLAUDE.md` and `docs/design.md` before touching anything: the service's
three rules (the USB stream is never closed; AOAS never processes audio;
ownership transfer is cooperative) are load-bearing design, not comments.

## Sibling projects (first-party, same author)

- [`audio_engine`](https://github.com/minervarr/audio_engine) — the USB driver (`firstparty/audio_engine`)
- [`App_shell`](https://github.com/minervarr/App_shell) — read as precedent for JNI patterns (`firstparty/App_shell`)
- [`Vk_Canvas_Lb_LAW`](https://github.com/minervarr/Vk_Canvas_Lb_LAW) — the rendering engine the client apps use (`firstparty/Vk_Canvas_Lb_LAW`)

The three `firstparty/` checkouts are git submodules.

## Build

```bash
./build.sh          # build / install / run / logs / test, one script
./build.sh test     # shm_ring_test on the desktop under ASan+UBSan
```

`./build.sh` drives a system Gradle 9.7 with AGP 9.2 and JDK 17 — there is no
wrapper on purpose (a checked-in jar for a one-developer project). Needs the
Android SDK (platform 36) and NDK 29.0.14206865. ABIs: `armeabi-v7a`,
`arm64-v8a`, `x86_64`.
