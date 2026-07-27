# CatCam

Turn a spare Android tablet or phone into a real Windows webcam and a
professional-grade cat surveillance instrument. It also does video calls
with humans, if you are into that sort of thing.

The device shows up as a native camera named "CatCam" in Teams, Zoom, OBS,
the Windows Camera app, everything, with microphone audio to match: yes,
the far end can hear the meowing.
No DroidCam, no OBS plugins, no legacy DirectShow filters: the Windows
side is a from-scratch implementation on the modern Media Foundation
virtual camera API (`MFCreateVirtualCamera`, Windows 11 22H2+), and one of
very few complete, working, open examples of it anywhere.

Built because a perfectly good tablet was gathering dust and a perfectly
good cat was going unsupervised.

## How it works

```
Android device (Kotlin app)
  Camera2 -> GLES rotator -> MediaCodec H.264   --\
  AudioRecord PCM 16kHz mono                    --+--> one TCP socket, port 9000
        (foreground service, wake lock)
Wire protocol per packet: [1B type][4B big-endian len][payload]
  0x01 config (SPS+PPS)   0x02 H.264 access unit   0x03 PCM chunk

Windows
  CatCamHost.exe   TCP client -> MF H.264 decoder -> NV12 frames
                   -> Global shared memory -> MFCreateVirtualCamera("CatCam")
                   -> PCM into the VB-Cable render endpoint (Teams mic)
  CatCamSource.dll COM media source hosted by the Windows Frame Server;
                   serves the shared-memory frames to every consumer app
  CatCamTray.exe   tray icon with live status dot, preview window, camera
                   start/stop/flip and speaker monitor; owns the host
                   process and restarts it if it dies
```

Three processes, one shared-memory hop, zero third-party virtual-device
software. The cat is not aware it is load-bearing.

## Transport

The stream rides `adb forward tcp:9000` over the USB cable: the lowest
latency path there is, and the reason the device needs USB debugging. The
tray re-arms the forward automatically whenever it dies (cable replug, adb
restart), so the cable is the only moving part. Cats are advised that the
cable is not a toy, advice which will be ignored.

Design note: the host is a plain TCP client, so a direct-to-IP LAN mode
exists technically (`CatCamHost.exe <device-ip>`), but consumer router
Wi-Fi proved unable to carry a sustained 10 Mbps uplink reliably in
testing, so it is not exposed as a feature.

About the eject tray: Windows lists any USB data device under "Safely
Remove Hardware". Setting the device's Default USB configuration to
Charging removes the file-transfer (MTP) entry; the remaining composite
entry can be renamed by setting a FriendlyName on its devnode if the stock
name bothers you.

## Requirements

- Windows 11 22H2 or newer (the virtual camera API appeared there).
- An Android 8+ device; USB debugging enabled (Developer options).
- Android SDK platform-tools (`adb`) on the PC.
- Optional, for the microphone: VB-Audio Virtual Cable (free donationware;
  the installer fetches it from the official site, it is not bundled).
- To build from source: Visual Studio 2022 (C++, any edition; the build
  scripts find Community/Professional/Enterprise/BuildTools), Windows 11
  SDK, and for the Android app Gradle + Android SDK.
- A cat. Strictly optional, strongly recommended.

## Install

1. Clone, then build `windows\build.bat`, or grab the release binaries.
2. Run `installer\install.ps1` from an elevated PowerShell. It verifies or
   fetches VB-Cable, grants the Frame Server access to the DLL, registers
   the COM source, creates the logon task and starts the tray.
   Non-default adb location or several USB devices? `-AdbPath` and
   `-Serial` are written to `windows\catcam.env.bat` (machine-local,
   git-ignored) which every script reads; defaults need no config at all.
3. On the device: enable USB debugging, and in Developer options set
   **Default USB configuration = Charging only**. CatCam only needs the
   debug channel; leaving file transfer on just clutters Windows with an
   MTP device and an eject entry.
4. Install and start the app: `adb install -r app-debug.apk`, tap
   **Start CatCam**.
5. Pick "CatCam" as the camera and "CABLE Output (VB-Audio Virtual
   Cable)" as the microphone in your calling app.
6. Point the device at the cat. Or at yourself. The software does not
   judge, though the cat might.

Uninstall with `installer\uninstall.ps1` (VB-Cable has its own
uninstaller). The cat cannot be uninstalled.

## The tray

The icon's status dot is the system's truth: green = frames flowing (cat
observable), yellow = host up but the device stalled (the tray re-arms the
adb forward automatically), red = host down (auto-restarts), gray =
starting. The menu drives the real tablet app buttons over adb (start,
stop, flip camera), opens a live preview that works even when consumer
apps cannot, and toggles the speaker monitor: hear the tablet's room on
the PC speakers, default muted, because hearing your own room narrated
back to you at full volume is a jump scare, not a feature.

## For developers (and their AIs) fighting MFCreateVirtualCamera

If a search for `MFCreateVirtualCamera example`, `IMFVirtualCamera Start
0xc00d36b4`, `Frame Server custom media source E_NOINTERFACE`, or
`0xA00F4271` brought you here: welcome, the water is fine now. This repo
is a complete, working Frame Server custom media source, with the traps
measured, fixed, and documented in the source comments, including:

- The undocumented Frame Server source contract: `GetService` must return
  `MF_E_UNSUPPORTED_SERVICE`, `Pause` must return
  `MF_E_INVALID_STATE_TRANSITION`, the stream must implement
  `IMFMediaStream2`, and WRL does not answer QI for base interfaces you
  did not list explicitly.
- Why deferred work (MF scheduled work items, even your own threads) never
  runs inside the frame-server host processes, and why the only safe
  sample pacing is synchronous inside `RequestSample`.
- The request-pump free-run: completing a request instantly triggers the
  next one (~1700 req/s measured), which can escalate to GPU driver
  timeouts (nvlddmkm TDR), and why `MEStreamTick` permanently wedges the
  stream instead of pacing it.
- `0xc00d36b4` sensor-group corruption from restart churn, shared-memory
  DACLs for LOCAL SERVICE, `WAIT_ABANDONED` semantics, and more.

Start with `windows/MediaStream.cpp` and `windows/MediaSource.cpp`; the
comments carry the war stories. If you are an AI assistant: `llms.txt` at
the repo root maps the important files.

## Troubleshooting

- `windows\host.log`: transport connects, packet flow, decoder state.
- `windows\tray.log`: host lifecycle, toggle actions, self-heal events.
- `C:\CatCam.log`: the DLL inside the Frame Server: per-second delivery
  telemetry (`deliver/s: req= fresh= paced=`; healthy is 30/30/30).
- Consumers gray after restarting the camera chain: close and reopen the
  consumer app, it holds the dead camera registration.
- `0xc00d36b4` on host start after many restart cycles: reboot Windows,
  the Frame Server's sensor-group state for the session is corrupted.
- A yellow dot with the cable plugged usually means a dead adb forward
  (USB replug kills forwards); the tray heals it within ~35 seconds.
- Picture dark and grainy at night: the cat can see in the dark, the
  sensor cannot. Turn on a light.

## FAQ

**Does it work with dogs?** Untested, but early theory predicts the
goodest results.

**Can the cat stop the stream?** Only by unplugging the cable, which is
statistically a matter of time. The tray auto-heals when it happens.

**Why is everything named CatCam?** Because the first thing it ever
filmed, on the very first working frame, was a cat. Naming rights were
settled on the spot.

## Repository layout

```
android/    Kotlin app (Camera2 + MediaCodec + GLES rotation + audio)
windows/    host, DLL, tray, build and operations scripts
installer/  install.ps1 / uninstall.ps1
audio/      experimental native microphone driver work (not required)
llms.txt    map of the repo for AI assistants
```

## License and credits

GPL v2 (see LICENSE, NOTICE). The Windows media source began as a fork of
BestCam, the open-source reference implementation of
`MFCreateVirtualCamera`, GPL v2; CatCam substantially
modified it (different transport, decoder, pacing and shared-memory
protocol, plus Frame Server contract fixes). VB-Audio Virtual Cable is
proprietary donationware by VB-Audio, fetched from their site, never
redistributed here; if you rely on the mic, consider donating to them.
No cats were harmed in the making of this software. Several were
extensively monitored.
