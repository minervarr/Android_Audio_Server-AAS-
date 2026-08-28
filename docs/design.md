# AOAS design narrative

Written after a long design conversation, captured here so a fresh session
has the full reasoning, not just the conclusion. See the project's
`CLAUDE.md` for the condensed, load-bearing rules; this document is the
"how we got there" and the "how it should work" sketch.

---

## 1. The problem, precisely

The user connects one external audio interface (a Focusrite Clarett) to an
Android device and wants to use it, at full fidelity, from several of their
own apps: a music player (Matrix Player), a future audio recorder, a camera
app that wants an external audio input, and possibly others later. Today,
switching which app is "using" the DAC means:

- Android re-prompts the USB permission dialog for the newly-active app,
  because the grant is scoped to that app's UID and does not transfer.
- Whichever app was using the DAC has to close its USB Audio Class
  isochronous stream, and the newly-active app has to open a fresh one. USB
  Audio Class devices generally derive their internal clock from the active
  isochronous stream; tearing it down and rebuilding it forces the DAC to
  re-lock that clock, which is very commonly audible as a mute, pop, or a
  brief dropout.

The second problem is the one that matters more. The permission dialog is a
UI annoyance, solvable in principle by simpler means. The clock re-lock is a
hardware-level fidelity cost that repeats on every single app switch, and the
user's standing instruction for this whole line of work is: **quality and
fidelity are the only currency; ease of implementation is not a factor.**

## 2. Approaches considered, and why each was set aside

### 2.1 `android:sharedUserId`

Would let two apps share a UID and therefore a permission grant. Rejected
outright — Google deprecated this in API 29 and is actively removing it from
the platform in later versions. Building on it would be building on
something already scheduled to stop working.

### 2.2 Broker model ("Model B")

A hub app that:
- Requests and holds the Android USB permission once.
- On each app switch, hands the raw `UsbDeviceConnection` file descriptor to
  whichever client app is now active.
- Each client app runs its own instance of the USB Audio driver
  (`UsbAudioDriver`/`ae_usb`), opening the device fresh each time it becomes
  active and closing it when it yields.

This is considerably simpler to build — no shared-memory audio relay, no
continuous background stream, no ring buffer protocol. It fully solves the
permission-dialog problem. **It was rejected anyway**, because it does
nothing about the clock re-lock: every hand-off is still a close-then-reopen
of the isochronous stream at the hardware level, so the audible pop persists
on every switch. Given fidelity is the only criterion, "simpler but still
audibly clicks on every switch" lost to "harder but silent on every switch."

### 2.3 Multi-client mixing server

A hub that could serve more than one app at once, mixing their streams. This
was never seriously in the running: the user was explicit that the intended
usage is strictly one app "talking" to the interface at a time — not a
software mixer. Ruled out by the user's own stated design intent, separate
from any technical argument about fidelity.

### 2.4 Chosen: continuous-stream relay ("Model A")

A dedicated background service that:
- Requests the Android USB permission itself, once.
- Opens the USB Audio Class isochronous stream to the DAC and **keeps it
  open continuously**, for as long as the service runs — independent of
  which client app, if any, is currently active.
- Lets exactly one client "own" the device at a time.
- Receives raw PCM from the current owner via a shared-memory ring buffer and
  writes those exact bytes to the USB endpoint — no processing.
- When no owner is active, or between owners, either writes silence or
  simply stops advancing — but never closes the USB interface.

This is more work: it requires embedding a full USB Audio driver in the
service (reusing `audio_engine`'s `ae_usb`/`UsbAudioDriver` rather than
writing a second one), and a real inter-process audio transport (a
shared-memory ring buffer, not just a fd handoff). It was chosen anyway,
because it is the only option among those considered that actually removes
the clock re-lock artifact — the DAC never sees its isochronous stream stop,
so it never has to re-acquire its clock, regardless of how many times the
"logical" owner changes above it.

A secondary argument in its favor, beyond just avoiding the re-lock: a
dedicated process whose only responsibility is feeding the USB endpoint —
with no UI thread, no GPU work, no decode work competing for CPU time or
scheduling — is plausibly *more* stable in its isochronous timing than each
client app managing its own driver thread in the middle of everything else
that app is doing. This mirrors why JACK, PipeWire, and macOS's `coreaudiod`
exist as dedicated audio server processes rather than being embedded per-app.
This is a plausibility argument, not yet measured — worth validating once
something runs on real hardware.

## 3. Non-negotiable design rules that follow from the choice

1. **The isochronous USB stream to the DAC is opened once and stays open for
   the service's lifetime.** No code path may close and reopen it as part of
   ownership transfer. If the DAC is physically unplugged, Android will
   revoke the permission and force a real close — that is an unavoidable OS
   behavior and not something to route around; document it as an expected
   edge case, not a design failure.
2. **AOAS performs zero audio processing.** No EQ, no resampling, no gain
   changes, no dithering — pure passthrough of whatever bytes the current
   owner wrote. Any processing belongs entirely to the client, before the
   samples enter the ring buffer. A shared-memory copy is not "processing" in
   this sense — it introduces no rounding or bit change, only an extra,
   deterministic memory copy — but the rule exists to make sure nothing else
   ever gets added to that copy path later "just this once."
3. **Only one owner at a time; no mixing.** Not a mixing server, ever, by the
   user's explicit design intent.
4. **Ownership transfer is cooperative by default.** Outgoing owner releases
   first, AOAS confirms, incoming owner is granted next — never a forced
   takeover of a live stream. The one allowed exception is a manual
   disconnect the user triggers from AOAS's own UI, treated the same as
   physically unplugging the interface: an intentional interruption, not a
   bug.
5. **Process death is handled automatically.** An owning client whose process
   dies without releasing (crash, force-stop, OOM kill) must not permanently
   strand the device. Binder's `linkToDeath` mechanism is the natural fit —
   it fires when the underlying Binder connection dies, which happens
   whenever the owning process dies, without needing a heartbeat or polling.

## 4. Sketch of the mechanism (not yet a spec — a starting point)

**Process model**: AOAS runs as a foreground Android service (needs a
persistent notification and, likely, exemption from background/battery
restrictions to avoid being killed mid-session — Android is aggressive about
reclaiming background processes, and a killed AOAS mid-playback is exactly
the failure this design exists to prevent).

**Permission and device layer**: AOAS is the only app that ever calls
`UsbManager.requestPermission()` for the DAC. It owns the `UsbDeviceConnection`
and the `UsbAudioDriver`/`ae_usb` instance built on top of it, matching how
Matrix Player already talks to the same driver directly today.

**Client connection**: a client app binds to AOAS via a Binder service
(likely `signature`-level custom permission, restricting binding to apps
signed with a shared key — this is an operational commitment across all AOAS
client apps and should be made deliberately, not accidentally, when it's
implemented). Binding registers a `linkToDeath` recipient so AOAS learns
immediately if the client's process dies.

**Ownership protocol** (control channel, exact shape still open):
- `requestOwnership()` — client asks to become the active owner. Granted
  immediately if nobody else owns the device; otherwise the request either
  waits or fails, to be decided (does the previous owner get a chance to
  finish, or is this always synchronous with the outgoing owner already
  having released? — leaning toward the latter, matching rule 4 above).
- `releaseOwnership()` — current owner gives it up cleanly.
- On `linkToDeath` firing for the current owner, AOAS treats it exactly as an
  implicit `releaseOwnership()`.
- A `setFormat(sampleRate, bitDepth, channels)` call (or equivalent) for the
  case where the incoming owner needs the DAC in a different configuration
  than it's currently running — this is the one case where AOAS may need to
  reconfigure the actual USB Audio interface, which the design accepts as
  unavoidable (see open question 3 in CLAUDE.md) but should minimize.

**Audio data channel**: a shared-memory ring buffer per active session
(`android.os.SharedMemory` or equivalent), single-producer (the client)
single-consumer (AOAS), lock-free via atomic read/write indices — the same
kind of structure used internally by low-latency audio libraries generally.
AOAS's consumption loop is the same real-time-priority loop pattern
`UsbAudioDriver` already uses for its isochronous transfers in
`audio_engine`; only the *source* of the PCM changes (ring buffer instead of
a direct decode-loop call).

**Silence/idle behavior**: when no owner is active, or the current owner's
ring buffer is empty (isn't feeding fast enough), AOAS should feed silence
to the USB endpoint rather than stall it — stalling risks an underrun in the
transfer itself, which is its own kind of audible glitch, distinct from and
just as bad as the clock re-lock this design avoids.

## 5. What is deliberately NOT designed yet

- The exact AIDL interface and its versioning strategy across independently
  updated client apps.
- The client-side callback surface for "you lost the DAC" (forced disconnect,
  device unplugged, permission revoked).
- Handling more than one DAC at once.
- Whether AOAS becomes its own git repository (matching the
  `audio_engine`/`vk_canvas`/`app_shell` pattern under `minervarr`) or stays
  a single standalone app project.
- Any UI design for AOAS's own control surface (the "who owns the DAC right
  now, disconnect" screen).

These are intentionally left as open questions in `CLAUDE.md` rather than
guessed at here, so the next working session resolves them with real
requirements instead of inheriting placeholder answers.
