# AudioPTT

A native C++/Win32 push-button microphone / PC-audio switch for Discord. When Broadcast is OFF, it forwards your webcam microphone to VB-CABLE; when on, it forwards PC audio instead. Discord keeps the same virtual microphone input. Requires VB-CABLE for the prototype.

> **Experimental development build.** The app has compiled and the basic microphone/Broadcast device transitions were observed on Windows, but routing, echo recovery, and new diagnostics are not yet fully validated. Broadcast temporarily changes the Windows default playback device.

## Open in Visual Studio

Install the **Desktop development with C++** workload and Windows SDK. Clone the repository, then choose **File > Open > Folder** and select the repository root containing `CMakeLists.txt`. Visual Studio recognizes it as a CMake C++ project automatically; you do **not** need to create a DLL or a new .sln file.

Alternatively, run:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Debug
.\build\Debug\AudioPTT.exe
```

Windows CI is configured in `.github/workflows/windows-build.yml`.

## Main and Logs tabs

The native Win32 window now has **Main** and **Logs** tabs.

**Main** shows Broadcast, Monitor, a compact live snapshot of the Windows default Console/Multimedia/Communications playback devices, physical microphone, and both VB-CABLE endpoints, plus the required Discord input setting. The app cannot read Discord's settings; its Discord-input line is an instruction, not a verified setting. When broadcasting, the Monitor output line uses the saved physical speaker name. The application never deliberately plays the physical microphone through your speakers; its Monitor control is for PC-audio broadcast only.

**Logs** provides a timestamped, scrollable event history (last 400 events). It records the detected endpoints and role assignments, reasons an audio setup was blocked, switch/restore operations, WASAPI stream initialization and formats, worker startup errors, and runtime recovery. **Device IDs / diagnostics** on Main appends the full active endpoint names and IDs and opens Logs. You can use **Copy logs** to share an error report or **Clear logs** to start another test. Nothing is automatically uploaded.

A blocked setup should now say *which* Windows default role is still routed through `CABLE Input`. For example, if Console or Multimedia output is already VB-CABLE, switch the Windows default output to real speakers/headphones, then press **Retry audio setup**. Do not set VB-CABLE as Windows' default before AudioPTT starts; AudioPTT changes those roles temporarily while broadcasting.

### Windows audio-session diagnostics

The **Logs** tab now includes an **Audio sessions** button. Clicking it gives a visible completion dialog summarizing active playback sessions, updates a Last scan status line, and appends the full snapshot to Logs. It records the Windows shared-mode playback sessions attached to the saved physical speaker endpoint and VB-CABLE Input, including active/inactive state, process executable name, PID, session label and an audio peak when the COM meter is supported. When Windows denies access to a protected process, the app attempts a process-list fallback to resolve its executable name. Click it while the unwanted delayed voice is audible, then again with Discord completely closed.

The **Echo Test** also records those session inventories **before, during and after** its tone. It now logs the detected tone's approximate return lag, captured-speaker RMS, nonzero frame count, and tone amplitude. These diagnostics may distinguish a Discord/browser audio session from a Windows-level `Listen to this device` monitor. **Session membership alone cannot identify who routed the echo**, and system audio services, drivers, exclusive-mode audio and device listening may not report a distinct process session.

No microphone audio samples, recordings or process paths are saved or uploaded. The log is held in the application's memory until cleared or the application closes. As with the other diagnostic output, copied log text may include local program names, PIDs and device IDs.

### Echo Test (diagnostic only)

With **Broadcast OFF**, click **Echo Test** on the Main tab. The webcam microphone is normally forwarded to the cable in this mode, but AudioPTT has no intentional speaker-monitoring stream. The test adds a diagnostic tone directly to VB-CABLE and checks for an unexpected return to Realtek. For a meaningful comparison, run it once with Discord open and again after fully closing Discord (including background desktop processes or browser tabs). The app asks for confirmation, then plays a low-level A-B-A tone directly into the VB-CABLE playback endpoint (**CABLE Input**) while simultaneously capturing a few seconds of **WASAPI loopback from the saved physical speakers**. It does not enable microphone monitoring, change Windows default devices, or send data off the PC.

The app compares three frequency-specific tone windows against the physical speaker recording. If the pattern reappears there, it logs an **ECHO DETECTED** warning and opens the Logs tab. That indicates some external monitoring/playback path is bringing VB-CABLE's outgoing signal back to the speakers (for example Discord's mic test or Windows' `Listen to this device`). The test cannot identify which program created the path.

**The tone is real audio sent to VB-CABLE. People in a connected Discord call may hear it.** Run this briefly when appropriate. A "no echo detected" result is not a proof of absence: quiet outputs, disconnected devices, audio format quirks, intermittent monitoring, and headset hardware sidetone may escape the test. This diagnostic is for the first troubleshooting pass and does not run automatically.

### Play Sample Audio (Broadcast-only)

The Main tab includes **Play Sample Audio**, which is **disabled until Broadcast is ON and running**. Clicking it uses the locally installed Windows SAPI voice to say:

> "Hello, this is sample audio. We are playing sample audio."

The voice is synthesized **into memory**, then rendered through a WASAPI client opened on the explicit **CABLE Input** playback endpoint, not the Windows default playback device or physical speakers. It is injected into the same VB-CABLE path that Discord uses as its input while broadcasting. The sample does not capture or mute the physical microphone. Playing does not require audio files or an Internet connection; it requires a working Windows SAPI voice.

- With **Monitor checked**, AudioPTT's normal cable monitor also copies this sample to your physical speakers.
- With **Monitor unchecked**, AudioPTT opens **no physical speaker output** for the sample. Other Discord participants should still hear it, provided Discord has **Input=CABLE Output** explicitly selected.
- If you still hear sample audio locally with Monitor unchecked, investigate Windows **Recording → CABLE Output → Properties → Listen → Listen to this device** or other third-party monitoring. Such an external return path can play the virtual microphone into your speakers independently of AudioPTT's checkbox.

The button disables itself while the sample is playing. To prevent a partially played sample from changing destination, the Broadcast switch and Monitor checkbox are temporarily locked until playback finishes. This is a local routing/synthesis diagnostic; it cannot confirm that remote Discord participants actually received the audio.

## Audio behavior and Discord setup

**Set Discord Input Device explicitly to `CABLE Output (VB-Audio Virtual Cable)` (NOT Default).** Set Discord Output Device explicitly to your physical speakers/headphones (e.g. Realtek). Never select VB-CABLE for Discord's output, or remote voices can be fed back into the cable.

AudioPTT owns the microphone-to-cable feed. This avoids relying on Discord/browser WebRTC to switch an already-open microphone stream when a Windows default changes.

| Mode | What AudioPTT sends into CABLE Input | What you hear locally |
|---|---|---|
| Broadcast OFF | Webcam microphone forwarded into VB-CABLE | Normal physical speaker audio; **no mic monitoring from AudioPTT** |
| Broadcast ON, Monitor unchecked | PC audio from apps following the Windows Console/Multimedia playback default | No speaker monitor stream from AudioPTT; apps explicitly bound to Realtek can still play there |
| Broadcast ON, Monitor checked (default) | Same PC audio | Extra local copy of VB-CABLE mix through saved speakers |

Windows default **microphone/recording devices are not changed** by this build. During Broadcast the webcam forwarding stream is stopped before changing playback defaults, so it never mixes into desktop audio. When Broadcast ends, playback defaults are restored and webcam forwarding resumes. Monitor controls **PC-audio playback only**, not webcam audio.

**Known limitation:** Some running applications, including browser tabs and Discord itself, hold onto a physical playback endpoint or have a per-app output choice. Changing the Windows playback default may not redirect existing Chrome/other browser audio into CABLE Input. Discord itself should remain on physical speakers. If a browser carries both Discord and narration, changing its audio route may also rebroadcast the voices of other callers. Test the direct sample first (which bypasses browser routing), then isolate the narration application's output.

**Known echo:** A Windows `CABLE Output` recording device with `Listen to this device` enabled can independently replay the virtual mic through Realtek even with AudioPTT Monitor OFF. The audio session logs can show such a Windows service session. Disable recording-device Listen and ensure Discord isn't running its microphone test.

## Two-computer test

1. In Discord on the broadcasting computer, set **Input=CABLE Output** explicitly, **Output=Realtek Speakers** explicitly; leave Discord recording processing/noise suppression off initially. Use a second computer in the same voice call to hear the result.
2. Close/restart AudioPTT after updating to this build. With **Broadcast OFF**, speak normally. The remote computer should hear the webcam microphone, which AudioPTT now forwards into CABLE Input. If not, inspect the Windows **Recording → CABLE Output** input level meter while speaking; a moving meter means the cable is receiving audio and Discord/browser permission/processing is the next suspect.
3. With **Broadcast ON**, click **Play Sample Audio**. This directly renders an offline sample voice to CABLE Input, independent of browser playback. The remote computer should hear it regardless of whether Monitor is checked. With Monitor unchecked, AudioPTT sends no audio to the physical speakers.
4. If direct sample works remotely but browser narration doesn't, inspect Windows **Volume Mixer** and the narration browser/tab's *actual* output device. The current build only redirects apps that follow Windows playback defaults. If the CABLE Output meter stays flat during direct sample, investigate the VB-CABLE render/capture pair and driver status before changing Discord.
5. Stop Broadcast and verify the webcam mic automatically resumes at the other computer. Check that ordinary Windows playback is restored.

A successful log saying audio was submitted to VB-CABLE is **not** proof that Discord received it. The Discord/browser and VB-CABLE recording level meter are independent checkpoints.

## Important limitations

- **Windows default-device switching is currently implemented through an undocumented COM interface (PolicyConfig).** It may not work on all Windows versions. If switching fails, the program should report an error.
- Some applications keep playing to an explicitly selected device or cached audio session even after Windows' default changes. Silent mode therefore applies only to audio actually routed through the virtual cable; it does **not** guarantee a completely silent PC.
- If the app crashes or is force-closed while broadcasting, Windows may continue using VB-CABLE as the default **playback device**. **Fix manually:** Settings > System > Sound > Output > select your physical speakers. An older build may also have changed your Windows microphone default; check Input too if transitioning from that build. Normal restart should offer recovery using the saved route.
- The prototype does not use per-process capture; apps that ignore Windows defaults may not be captured, while Discord audio accidentally routed to VB-CABLE may be rebroadcast.
- The monitor stream handles basic stereo/mono and 16-bit PCM/32-bit float format conversion, with simple linear resampling. Audio glitches are possible and there is no echo cancellation, AGC, or full handling of device removal.
- The hotkey is Ctrl+Alt+Space. A system tray icon, persisted device picker, installer, and bundled virtual audio driver are still not included.

## Code layout

- `src/main.cpp` — Win32 window and controls
- `src/AudioDevices.*` — enumerate Windows audio devices
- `src/AudioRouting.*` — save and switch Windows Console/Multimedia playback defaults; legacy input recovery
- `src/AudioEngine.*` — webcam-to-cable forwarding, exclusive desktop broadcast branch, optional local monitor, lifecycle
- `src/AudioSample.*` — offline SAPI speech sample and direct VB-CABLE WASAPI playback
- `CMakeLists.txt` — x64 native C++ build

The output device named **CABLE Input** is the VB-CABLE playback side; Discord records from **CABLE Output**.
