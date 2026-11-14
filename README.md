# AudioPTT

A small native C++/Win32 push-button PC-audio broadcaster for Discord. It stays completely idle when Broadcast is OFF: no microphone capture, forwarding, or local monitoring. Requires VB-CABLE for the prototype.

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

**Main** shows Broadcast, Monitor, a compact live snapshot of the Windows default Console/Multimedia/Communications playback devices, physical microphone, and both VB-CABLE endpoints, plus the required Discord input setting. The app cannot read Discord's settings; its Discord-input line is an instruction, not a verified setting. When broadcasting, the Monitor output line uses the saved physical speaker name. Idle status explicitly means AudioPTT is doing nothing, not that your physical microphone is system-muted.

**Logs** provides a timestamped, scrollable event history (last 400 events). It records the detected endpoints and role assignments, reasons an audio setup was blocked, switch/restore operations, WASAPI stream initialization and formats, worker startup errors, and runtime recovery. **Device IDs / diagnostics** on Main appends the full active endpoint names and IDs and opens Logs. You can use **Copy logs** to share an error report or **Clear logs** to start another test. Nothing is automatically uploaded.

A blocked setup should now say *which* Windows default role is still routed through `CABLE Input`. For example, if Console or Multimedia output is already VB-CABLE, switch the Windows default output to real speakers/headphones, then press **Retry audio setup**. Do not set VB-CABLE as Windows' default before AudioPTT starts; AudioPTT changes those roles temporarily while broadcasting.

### Windows audio-session diagnostics

The **Logs** tab now includes an **Audio sessions** button. Clicking it gives a visible completion dialog summarizing active playback sessions, updates a Last scan status line, and appends the full snapshot to Logs. It records the Windows shared-mode playback sessions attached to the saved physical speaker endpoint and VB-CABLE Input, including active/inactive state, process executable name, PID, session label and an audio peak when the COM meter is supported. When Windows denies access to a protected process, the app attempts a process-list fallback to resolve its executable name. Click it while the unwanted delayed voice is audible, then again with Discord completely closed.

The **Echo Test** also records those session inventories **before, during and after** its tone. It now logs the detected tone's approximate return lag, captured-speaker RMS, nonzero frame count, and tone amplitude. These diagnostics may distinguish a Discord/browser audio session from a Windows-level `Listen to this device` monitor. **Session membership alone cannot identify who routed the echo**, and system audio services, drivers, exclusive-mode audio and device listening may not report a distinct process session.

No microphone audio samples, recordings or process paths are saved or uploaded. The log is held in the application's memory until cleared or the application closes. As with the other diagnostic output, copied log text may include local program names, PIDs and device IDs.

### Echo Test (diagnostic only)

With **Broadcast OFF** (fully idle), click **Echo Test** on the Main tab. Echo Test temporarily sends a diagnostic tone into the cable even though normal idle mode does not; this is the only intentional exception. For a meaningful comparison, run it once with Discord open and again after fully closing Discord (including background desktop processes or browser tabs). The app asks for confirmation, then plays a low-level A-B-A tone directly into the VB-CABLE playback endpoint (**CABLE Input**) while simultaneously capturing a few seconds of **WASAPI loopback from the saved physical speakers**. It does not enable microphone monitoring, change Windows default devices, or send data off the PC.

The app compares three frequency-specific tone windows against the physical speaker recording. If the pattern reappears there, it logs an **ECHO DETECTED** warning and opens the Logs tab. That indicates some external monitoring/playback path is bringing VB-CABLE's outgoing signal back to the speakers (for example Discord's mic test or Windows' `Listen to this device`). The test cannot identify which program created the path.

**The tone is real audio sent to VB-CABLE. People in a connected Discord call may hear it.** Run this briefly when appropriate. A "no echo detected" result is not a proof of absence: quiet outputs, disconnected devices, audio format quirks, intermittent monitoring, and headset hardware sidetone may escape the test. This diagnostic is for the first troubleshooting pass and does not run automatically.

### Play Sample Audio (Broadcast-only)

The Main tab includes **Play Sample Audio**, which is **disabled until Broadcast is ON and running**. Clicking it uses the locally installed Windows SAPI voice to say:

> "Hello, this is sample audio. We are playing sample audio."

The voice is synthesized **into memory**, then rendered through a WASAPI client opened on the explicit **CABLE Input** playback endpoint, not the Windows default playback device or physical speakers. It is injected into the same VB-CABLE path that Discord uses as its input while broadcasting. The sample does not capture or mute the physical microphone. Playing does not require audio files or an Internet connection; it requires a working Windows SAPI voice.

- With **Monitor checked**, AudioPTT's normal cable monitor also copies this sample to your physical speakers.
- With **Monitor unchecked**, AudioPTT opens **no physical speaker output** for the sample. Other Discord participants should still hear it, provided Discord is using the virtual microphone (usually **Input=Default** after the Windows default switches).
- If you still hear sample audio locally with Monitor unchecked, investigate Windows **Recording → CABLE Output → Properties → Listen → Listen to this device** or other third-party monitoring. Such an external return path can play the virtual microphone into your speakers independently of AudioPTT's checkbox.

The button disables itself while the sample is playing. To prevent a partially played sample from changing destination, the Broadcast switch and Monitor checkbox are temporarily locked until playback finishes. This is a local routing/synthesis diagnostic; it cannot confirm that remote Discord participants actually received the audio.

## Audio behavior and Discord setup

**Configure Discord microphone input to `Default`, not `CABLE Output`.** Leave Discord output explicitly set to physical speakers/headphones. Before starting AudioPTT, set Windows' default recording devices (including default communications input) to the physical webcam microphone, and Console/Multimedia playback defaults to physical speakers.

| AudioPTT state | Windows default microphone | Windows playback | AudioPTT audio streams |
|---|---|---|---|
| Idle / Broadcast OFF | Your physical webcam microphone | Physical speakers | **None.** Does not capture, mute, or forward the mic |
| Broadcast ON + Monitor unchecked | **CABLE Output** | **CABLE Input** | No local monitor (PC audio reaches cable through Windows defaults) |
| Broadcast ON + Monitor checked (default) | **CABLE Output** | **CABLE Input** | Copies VB-CABLE PC audio to saved physical speakers |

Discord with **Input=Default** should normally use the physical webcam microphone while AudioPTT is idle, then receive PC audio from CABLE Output while Broadcast is active, and return to the webcam after Broadcast stops. The app uses an undocumented Windows default-device policy API to switch all three capture roles (Console/Multimedia/Communications), and Console/Multimedia playback roles. It never opens the webcam microphone itself.

**Caution:** Some versions of Discord and other calling applications retain an already-open input device even after Windows changes the default. The Windows setting itself will switch, but whether an *active Discord call* follows automatically must be tested. In Discord, select Input Device=`Default`, not the explicit physical mic or explicit CABLE Output, and restart/reselect the input if Discord does not follow the new default. AudioPTT cannot guarantee switching Discord's internal device setting.

**The physical microphone is not system-muted by AudioPTT.** Windows continues to allow other applications to use it, and Discord can use it directly while idle. During Broadcast, the Windows default input becomes the virtual microphone, but an app that explicitly selected the physical microphone could still access it. Monitor only controls the optional local copy of *computer* audio; it is checked by default and never monitors the physical microphone.

## Test setup

1. Install VB-CABLE and select the physical webcam microphone as Windows' default input and default communications input. Set physical speakers as default output. Prefer physical speakers as default communications output too.
2. Set **Discord Input=Default** and explicitly set **Discord Output=physical speakers/headphones**.
3. Start AudioPTT. Verify the status reads **Idle - microphone untouched**. Discord should hear your microphone normally through Windows' default input.
4. Press **BROADCAST PC AUDIO**. AudioPTT saves the prior default input/output roles (with crash-recovery data), sets recording defaults to **CABLE Output** and console/multimedia playback to **CABLE Input**. It does not capture your microphone. Discord should now hear PC audio instead of your voice, subject to its response to a default-device change.
5. With Monitor checked, you also hear the PC audio through physical speakers. Turn Broadcast OFF to restore your original Windows microphone and speaker defaults and return AudioPTT to complete idle.
6. If the echo returns, ensure that Windows Recording → CABLE Output → Properties → Listen → **Listen to this device** is **unchecked**; a Windows or third-party monitoring path can still feed the cable into speakers independently of AudioPTT.

## Important limitations

- **Windows default-device switching is currently implemented through an undocumented COM interface (PolicyConfig).** It may not work on all Windows versions. If switching fails, the program should report an error.
- Some applications keep playing to an explicitly selected device or cached audio session even after Windows' default changes. Silent mode therefore applies only to audio actually routed through the virtual cable; it does **not** guarantee a completely silent PC.
- If the app crashes or is force-closed while broadcasting, Windows may continue using VB-CABLE as the default **microphone and playback device**. **Fix manually:** Settings > System > Sound > Input > select your physical microphone; Output > select your physical speakers. Normal restart should offer restoration using the saved route.
- The prototype does not use per-process capture; apps that ignore Windows defaults may not be captured, while Discord audio accidentally routed to VB-CABLE may be rebroadcast.
- The monitor stream handles basic stereo/mono and 16-bit PCM/32-bit float format conversion, with simple linear resampling. Audio glitches are possible and there is no echo cancellation, AGC, or full handling of device removal.
- The hotkey is Ctrl+Alt+Space. A system tray icon, persisted device picker, installer, and bundled virtual audio driver are still not included.

## Code layout

- `src/main.cpp` — Win32 window and controls
- `src/AudioDevices.*` — enumerate Windows audio devices
- `src/AudioRouting.*` — save and switch Windows default microphone and playback devices
- `src/AudioEngine.*` — idle engine, cable loopback monitoring, lifecycle
- `src/AudioSample.*` — offline SAPI speech sample and direct VB-CABLE WASAPI playback
- `CMakeLists.txt` — x64 native C++ build

The output device named **CABLE Input** is the VB-CABLE playback side; Discord records from **CABLE Output**.
