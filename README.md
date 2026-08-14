<p align="center">
  <img src="assets/logo512.png" width="200" alt="CatCam mascot: a chubby orange cat waving one paw, wearing a camera-lens collar tag">
</p>

# CatCam

[![Website](https://img.shields.io/badge/catcam.app-F59E4C?logo=googlechrome&logoColor=white)](https://catcam.app)
![Release](https://img.shields.io/github/v/release/igorfyago/CatCam)
![License](https://img.shields.io/badge/license-GPL--2.0-blue)
![Windows](https://img.shields.io/badge/Windows%2011-22H2%2B-0078D6)
![Android](https://img.shields.io/badge/Android-8%2B-3DDC84)
![Cat](https://img.shields.io/badge/cat-approved-brightgreen)

**[catcam.app](https://catcam.app)** — downloads, the three-step setup,
and the pitch, with a cat on it.

Turn a spare Android tablet or phone into a real Windows webcam and a
professional-grade cat surveillance instrument. It also does video calls
with humans, if you are into that sort of thing. Over your Wi-Fi with no
cable at all, or over USB if you like your bits wired.

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
        (foreground service, wake lock)            + a UDP discovery beacon
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

## Transport: Wi-Fi or USB, your call

**Wi-Fi (default, no cable):** the app broadcasts a tiny discovery beacon
on your network; the PC tray hears it and connects directly. No IP
addresses, no pairing codes, no developer options, no adb. If the device
gets a new IP, the tray follows. On a weak network the stream drops
bitrate, then frame rate, instead of freezing: soft beats stuck.

**USB cable:** the stream rides `adb forward tcp:9000`, the lowest-latency
path there is, and the reason cable mode (and only cable mode) needs USB
debugging. The tray re-arms the forward automatically whenever it dies
(cable replug, adb restart). Cats are advised that the cable is not a toy,
advice which will be ignored.

Switch between the two from the app's USB | Wi-Fi control or the tray
menu, whichever is closer, mid-stream. Last touch wins, and the app's
status pill always shows the transport actually carrying the frames.

About the eject tray (USB mode): Windows lists any USB data device under
"Safely Remove Hardware". Setting the device's Default USB configuration
to Charging removes the file-transfer (MTP) entry; the remaining composite
entry can be renamed by setting a FriendlyName on its devnode if the stock
name bothers you. Or use Wi-Fi mode, which has no tray entry because it
has no cable.

## Requirements

- Windows 11 22H2 or newer (the virtual camera API appeared there).
- An Android 8+ device on the same network (or a USB cable; cable mode
  needs USB debugging and `adb`, which the installer can fetch for you).
- For the microphone: VB-Audio Virtual Cable (free donationware, by
  www.vb-cable.com; all participations are welcome). The official
  unmodified pack ships inside the installer and sets up automatically,
  no network needed. Usually zero clicks; VB-Audio's own window only
  appears if the silent install needs help.
- To build from source: Visual Studio 2022 (C++, any edition; the build
  scripts find Community/Professional/Enterprise/BuildTools), Windows 11
  SDK, and for the Android app Gradle + Android SDK.
- A cat. Strictly optional, strongly recommended.

## Install

The short version lives at **[catcam.app](https://catcam.app)**. The
slightly longer version:

1. On the PC: run
   [CatCamSetup.exe](https://github.com/igorfyago/CatCam/releases/latest/download/CatCamSetup.exe).
   One wizard, one UAC prompt: it registers the virtual camera, sets up
   the microphone automatically (VB-Cable, installed silently from the
   bundled official pack), creates the startup task and launches
   the tray. One optional checkbox adds USB cable mode
   (fetches platform-tools). It is not code-signed yet, so SmartScreen
   will ask for courage: "More info", "Run anyway".
2. On the device: install
   [CatCam-android.apk](https://github.com/igorfyago/CatCam/releases/latest/download/CatCam-android.apk)
   and press the big button.
3. Same Wi-Fi: they find each other. Pick "CatCam" as the camera (and
   "CABLE Output (VB-Audio Virtual Cable)" as the microphone) in your
   calling app.
4. Point the device at the cat. Or at yourself. The software does not
   judge, though the cat might.

Uninstall from Settings > Apps like anything else; it removes the task,
the camera registration and every file (VB-Cable has its own
uninstaller). The cat cannot be uninstalled.

Prefer the manual path? The release zip still ships
`installer\install.ps1` for elevated-PowerShell enjoyers, and
`windows\build.bat` builds everything from source.

## The app

A camera app that happens to broadcast: full-bleed preview, a record
shutter (white circle idle, red square live), one-tap camera flip, zoom
buttons, a warm/cool tone nudge, a Day/Night tuning pair (Night is
measured for dark rooms and grainy sensors), a mic level meter, and the
USB | Wi-Fi transport switch. The preview shows exactly the frame your PC
receives, same zoom, same colors, same crop, so what you see is what the
call gets. Each camera remembers its own zoom and tone: the front camera
is for humans, the back camera knows what it is really for.

## The tray

The mascot lives in your system tray; its status dot is the system's
truth: green = frames flowing (cat observable), yellow = host up but the
device stalled (in USB mode the tray re-arms the adb forward
automatically; in Wi-Fi mode it re-follows the beacon), red = host down
(auto-restarts), gray = starting. The menu toggles Wi-Fi mode, drives the
real tablet app over adb when a cable is present (start, stop, flip
camera), opens a live preview that works even when consumer apps cannot,
and toggles the speaker monitor: hear the tablet's room on the PC
speakers, default muted, because hearing your own room narrated back to
you at full volume is a jump scare, not a feature.

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
- Wi-Fi mode not connecting: both devices on the same network? The beacon
  is UDP port 9001 inbound on the PC (the installer opens it); guest
  networks and AP isolation eat broadcasts. `windows\probe.py <device-ip>`
  measures what your network actually delivers, second by second.
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
windows/    host, DLL, tray, build and operations scripts, probe.py
installer/  CatCamSetup.iss (one-click installer) + install.ps1 manual path
site/       catcam.app, the product page
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
