# CLAUDE.md

Guidance for Claude Code (claude.ai/code) working in AOAS.

## What this is

**AOAS — Android One Audio Server.** A standalone Android background service
that owns the USB permission grant and the live USB connection to one external
audio interface (a Focusrite Clarett, today), so that switching between the
user's own Android apps — Matrix Player, a future audio recorder, a camera
app, anything else — never requires re-granting Android's USB permission
dialog, and never requires the device to physically re-establish its USB
Audio stream.

It is a sibling project to `Matrix_Player`, `audio_engine`, `vk_canvas` and
`app_shell` (all `github.com/minervarr/...`, first-party, same author). AOAS
is not part of Matrix Player and knows nothing about music, tracks, albums,
or playback — it is pure USB-audio plumbing, reused by any Android app the
same way `app_shell` is reused. It links `audio_engine`'s `ae_usb`
(`UsbAudioDriver`) directly, the same library Matrix Player already uses, so
there is exactly one implementation of "talk to a USB Audio Class device" on
Android, shared rather than duplicated.

**This file, and `docs/design.md` alongside it, are a design capture — not
yet code.** They record a design arrived at through a long conversation about
trade-offs (see `docs/design.md` for the full narrative and the paths that
were rejected and why). Read both before writing the first line of AOAS
itself; the "why" behind the load-bearing decisions below is easy to
undo by accident if only the "what" survives.

---

## The problem this solves

Two Android facts compound to make multi-app USB-DAC use painful:

1. **Android's USB device permission is scoped to the requesting app's UID.**
   Two independently-signed apps each need their own grant for the same
   physical device — there is no OS-level "already trusted this device"
   shared across packages (short of `android:sharedUserId`, deprecated since
   API 29 and being removed from the platform — not viable).
2. **Closing and reopening a USB Audio Class isochronous stream makes the DAC
   re-lock its internal clock/PLL.** This is a hardware fact, not a software
   choice, and it is usually audible — a mute, a pop, a few hundred
   milliseconds of silence — every single time the stream is torn down and
   rebuilt.

Every time the user switches from one app to another that also wants the
DAC, both of these fire: a fresh permission dialog, AND an audible reconnect
click. AOAS exists to eliminate both, and the second one is the harder,
more important problem — a re-grantable dialog is an annoyance; an audible
artifact on every app switch is a fidelity problem, and fidelity is the only
thing this project optimizes for. Ease of implementation was explicitly
*not* a factor in this design — see `docs/design.md` for the alternatives
that were rejected specifically because they were easier but worse on this
axis.

---

## The core rule: the USB stream is never closed

**AOAS holds the isochronous USB Audio connection to the DAC open for the
entire lifetime of the service — never closed and reopened on an app
switch.** This is the single decision everything else in this design serves,
and it is why AOAS is a *relay*, not a permission broker:

- A design where AOAS only holds the Android permission grant and hands the
  raw `UsbDeviceConnection` file descriptor to whichever app is currently
  active (call it the broker model) was considered and **rejected**. It
  solves the repeated-permission-dialog problem, but each app still has to
  open its own USB Audio stream when it becomes active and close it when it
  yields — the DAC still re-locks its clock on every switch. It is simpler
  to build and does not solve the problem that motivated this project.
- Instead, AOAS embeds a full USB Audio driver
  (`audio_engine`'s `ae_usb`/`UsbAudioDriver`) and keeps it running
  continuously. Only ONE client app is "active" (owns the DAC) at a time —
  AOAS is not a mixer and never will be; see `docs/design.md`'s "rejected
  approaches" for why multi-client mixing was ruled out by the user
  explicitly, independent of the fidelity argument.
- Client apps write raw PCM into a shared-memory ring buffer; AOAS drains
  whichever ring buffer belongs to the current owner and writes those exact
  bytes to the USB endpoint. Between owners — or when nobody is active — AOAS
  either writes silence or simply stops advancing the buffer; the USB
  interface itself stays open and the DAC's clock lock survives the change.

## The second rule: AOAS never processes audio

**AOAS applies zero DSP, zero EQ, zero resampling, zero gain — pure
passthrough of exactly the bytes a client wrote.** Any processing a client
wants happens in that client, before the samples reach AOAS's ring buffer.
This is deliberate and absolute: the whole point of this system is fidelity,
and a shared audio server is exactly the kind of component that accumulates
"just one small adjustment" over time if the rule isn't stated explicitly and
enforced from the start.

The one thing worth clarifying, because it looks like a contradiction at
first glance: **the shared-memory copy itself does not count as processing.**
A `memcpy` of PCM into a ring buffer and back out is bit-exact — no rounding,
no resampling, no mixing — the same standard `Matrix_Player`'s own
`dsp_null_test` and its `-ffp-contract=off` build flag hold DSP code to (see
that project's `CLAUDE.md` for the precedent: an extra copy is not an
alteration; a changed bit is). AOAS relaying audio through a ring buffer is
consistent with "unaltered chain," not an exception to it.

## The third rule: ownership transfer is cooperative, never a forced audio cut

Exactly one owner at a time, and handing ownership from one app to another
follows a specific order, because forcing a live takeover mid-stream would
itself be an audible interruption — the exact thing this whole design exists
to avoid:

1. The outgoing app stops feeding its ring buffer and releases ownership.
2. AOAS confirms the release.
3. Only then is the incoming app granted ownership and allowed to start
   writing.

The one exception is an **explicit manual disconnect the user triggers from
AOAS's own UI** while a client is still actively streaming — that is treated
as a deliberate user action, equivalent to physically unplugging the DAC
today, and is allowed to interrupt.

**Process death is detected automatically**, via Binder `linkToDeath` on the
owning client's connection — if an owner's process dies without releasing
cleanly, AOAS notices and frees the device without requiring the next app to
wait or the user to intervene. A still-alive client that simply won't release
can also be freed manually from AOAS's UI.

## Why a dedicated server can mean *better* timing, not a compromise

A natural worry about inserting AOAS into the audio path is that an extra
process hop costs quality. The opposite is expected here: a dedicated,
minimal process whose only job is feeding the USB endpoint — no UI thread, no
GPU work, no decoding, no network — should hold more consistent isochronous
timing than each client app managing its own driver thread alongside
everything else it's doing. This is the same reasoning behind JACK, PipeWire,
and macOS's `coreaudiod` existing as separate, dedicated real-time audio
servers rather than being embedded in every app that wants sound.

---

## Rejected approaches (do not re-litigate these without a new reason)

See `docs/design.md` for the full reasoning; summarized here so nobody
re-proposes them from scratch:

- **`android:sharedUserId`** — deprecated since API 29, being removed from
  the platform. Not viable long-term regardless of convenience.
- **Broker model (Model B)** — hub hands out the raw fd, each app runs its
  own driver. Solves the permission dialog, not the DAC re-lock click.
  Rejected specifically because it does not serve the fidelity goal.
- **Multi-client mixing** — out of scope by design. AOAS talks to exactly one
  app at a time; this was the user's own stated reason for the project
  existing at all, independent of any technical argument.

---

## Open questions

Numbers 1, 2, 3 and 5 are now answered; the answers are recorded here, in this
file, as the original note asked. 4 stays deferred.

1. **Wire protocol — RESOLVED.** `aidl/io/nava/aoas/IAoas.aidl` and
   `IAoasClient.aidl` are the contract, generated with AIDL's **Java** backend.
   The NDK backend was the first choice, since it would have kept the whole
   interface in C++, but the NDK ships only libbinder_ndk's C headers — the C++
   ones the generated code needs (`binder_interface_utils.h` and friends) exist
   only in the platform build, and vendoring platform-internal headers into an
   app means betting they stay in step with the device's `.so`. It costs
   nothing that matters: Binder never carries a sample. Audio moves through
   shared memory; Binder carries acquire/release/status only, and the generated
   stub calls into C++ on its first line (`AoasBinder.java`). What the Java
   backend buys is real — every client gets generated, versioned stubs instead
   of hand-rolling the wire format.

   The ring buffer is `native/shm_ring.hh`: single-producer/single-consumer,
   lock-free, atomic read/write indices, laid out over an `ASharedMemory`
   region. Index arithmetic is lifted from `audio_engine`'s `ae::RingBuffer`
   rather than re-derived. The one thing that genuinely differs across a process
   boundary: **the peer's index is untrusted input** and is range-checked before
   every `memcpy`, because a buggy client must not be able to fault the server.

   Note the AIDL shape of `acquire()`: it returns an `int` result code and
   delivers the descriptor through a one-element `out` array, rather than
   returning the descriptor and throwing. `android.os.ServiceSpecificException`
   — the only exception type that carries a numeric code across Binder — is not
   in the public SDK, and the exceptions that are marshallable carry only a
   message. A code the client can switch over beats a prettier signature it has
   to parse strings out of.

2. **Client-side UX for a forced disconnect — RESOLVED.** `IAoasClient` has one
   `oneway` method, `onOwnershipLost(int reason)`, with reasons for the user
   disconnecting by hand, the DAC being detached, and the server stopping. It
   exists because without it a client that lost the device finds out only when
   its ring writes stop being accepted — which is indistinguishable from "the
   buffer is briefly full" — and would show a playing UI over silence.

3. **Sample-rate / bit-depth changes across a switch — RESOLVED: renegotiate,
   never resample.** AOAS runs the DAC at the current owner's native format. An
   incoming owner asking for the same format — the common case — gets a silent
   handover with the isochronous stream untouched. An incoming owner asking for
   a different one causes a real reconfiguration and an audible clock re-lock,
   and that is accepted rather than hidden. Fixing a single rate was considered
   and rejected: it would force somebody to resample everything that did not
   match, and resampling alters *every sample*, where the re-lock is one
   transient between owners. Resampling stays prohibited, like all other DSP.

4. **Multiple simultaneous DACs.** Still deliberately deferred. Today's real use
   case is exactly one Clarett. `app/src/main/res/xml/device_filter.xml` matches
   by USB Audio Class rather than by vendor/product id, so a second device would
   be *recognised*; the service simply takes the first one it finds.

5. **Repo/packaging shape — PARTLY RESOLVED.** AOAS is its own app project with
   its own Gradle build (`app/`, `native/`, `aidl/`). It links `audio_engine`'s
   `ae_usb` target, which is set by `AOAS_AUDIO_ENGINE_DIR` in
   `native/CMakeLists.txt` and currently points at `firstparty/audio_engine`.
   Still to decide: whether that becomes a git submodule under `framework/`
   (matching `app_shell`/`vk_canvas`) and whether AOAS gets its own repo under
   `minervarr`. Record the decision here once made.

---

## What exists so far

The design capture above is backed by code, and as of 2026-08-22 the device path
has been run on real hardware.

```
build.sh               build / install / run / logs / test, one script
aidl/io/nava/aoas/     IAoas.aidl, IAoasClient.aidl -- the client contract
native/                shm_ring.hh, usb_device.*, relay.*, aoas_service.*,
                       jni_bridge.cc, CMakeLists.txt
native/tests/          shm_ring_test.cc -- the bit-exactness gate, runs on desktop
app/src/main/java/     AoasNative, AoasBinder, AoasService, AoasDebugActivity
```

`AoasDebugActivity` is the bring-up console and is **not the product** -- AOAS's
real interface is its notification. It reports Android's own view of the USB bus
(devices, interfaces, endpoints, whether the permission is held), AOAS's state,
and the descriptor capabilities the driver parsed: UAC version, hardware
volume/mute, and the output and capture formats as whole `{rate, channels,
bits}` tuples rather than three independent lists, because the axes are coupled.
Two buttons open a real stream and play a generated 1 kHz sine, and one probes
the capture direction for peak level. Those three are diagnostics, deliberately
absent from `IAoas` -- they reconfigure the stream and generate samples, which is
the opposite of what a client may make AOAS do -- and they refuse while a client
owns the device. The Activity reaches them by casting the in-process binder.

**Toolchain.** `./build.sh` drives a system Gradle 9.7 with AGP 9.2 and JDK 17;
there is no wrapper on purpose (a checked-in jar for a one-developer project).
compileSdk/targetSdk are 36. `./build.sh test` compiles `shm_ring_test` for the
desktop under ASan+UBSan; it passes.

### Verified on hardware (Galaxy S23 Ultra, Android 16, HiBy FC4)

- Android sees the DAC and **AOAS holds the USB permission without a dialog** --
  the manifest `USB_DEVICE_ATTACHED` filter does what it was declared for.
- `UsbAudioDriver` parses the descriptors: UAC2, 30 output alt-settings, 44.1
  through 768 kHz at 16/24/32 bit, no hardware volume, **no capture path** (this
  particular device is output-only; the console says so rather than guessing).
- A stream really opens: 48 kHz / 2 ch / 24 bit in 3-byte subslots, 64 iso
  transfers submitted, feedback endpoint 0x81 live, event thread at nice -19.
- Reconfiguration works, including the extreme: 768 kHz / 2 ch / 32 bit accepted
  exactly 12,288,000 bytes for a 2-second tone, which is the arithmetic to the
  byte.
- Still unverified: the ring-buffer relay end to end with a second app as
  client, ownership handover between two clients, and the silent-handover claim
  itself (that an identical format across a switch produces no audible artifact).
  Those need a second signed client app; the Clarett has not been tried at all.

**One design constraint was discovered here, not designed:** Android 16 refuses
to start a `connectedDevice` foreground service unless the app *already* holds a
permission from the connected-device family -- for AOAS, a granted USB device
permission. So `AoasService` no longer promotes itself in `onCreate()`; it runs
as a plain started service until `openDevice()` succeeds, and goes foreground
there, dropping back out when the device goes away. That ordering is not a
workaround: with no DAC held there is nothing for a foreground service to
protect. Callers must use `startService`, never `startForegroundService`, which
would demand a promotion within five seconds whether or not a DAC is present.

While idle the driver logs a throttled playback underrun (about one per second)
because nothing is feeding the ring. It is noise, not a fault -- the endpoint
keeps running on padded silence, which is what keeps the clock locked.

## Relationship to sibling projects

- **`audio_engine`** (`ae_usb`/`UsbAudioDriver`) — AOAS links this directly
  for the actual USB Audio Class transfer implementation. Not reimplemented;
  reused as-is, the same driver Matrix Player uses for its own direct-USB
  path when AOAS is not involved.
- **`app_shell`** — **not a dependency**, contrary to the original guess above.
  It was examined and set aside for a concrete reason: `AndroidHost::init`
  blocks waiting for `state_->window` and then requires a Vulkan surface and an
  AAssetManager, so it cannot start in a process with no UI, and it contains
  nothing about USB, Binder or `Service` (its only platform services are the IME
  and the clipboard). Its JNI pattern does not transfer either — it resolves
  methods via `GetObjectClass(activity->clazz)`, which does not exist outside a
  NativeActivity.

  It is still worth reading as *precedent* before extending `jni_bridge.cc`:
  `os/activity_bridge.hh` for the up-call/down-call split (blocking calls up,
  a drained slot down), `redirect_stdio_to_logcat()` for a service that wants
  its stdout, and its rule against `NewStringUTF` (Java's modified UTF-8), which
  `jni_bridge.cc` follows.
- **`Matrix_Player`** — the first intended client. Its own Android USB
  permission gap (documented in its `CLAUDE.md`) is being fixed
  independently and directly (each app still needs *some* permission-request
  code even when AOAS exists, since AOAS itself has to request the grant);
  once AOAS exists, Matrix Player becomes a candidate to migrate onto it as a
  client rather than talking to the DAC directly — that migration is not yet
  planned or scheduled.
