# AudioPTT

A small native C++/Win32 application for switching Discord's input between your microphone and desktop audio without OBS. Requires VB-CABLE for the prototype.

> **Experimental development build.** Routing is implemented in source, but Windows compilation, hardware behavior, and recovery have not yet been verified. In particular, this version changes the Windows default playback device while Broadcast is on.

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

**Main** shows Broadcast, Monitor, a compact live snapshot of the Windows default Console/Multimedia/Communications playback devices, physical microphone, and both VB-CABLE endpoints, plus the required Discord input setting. The app cannot read Discord's settings; its Discord-input line is an instruction, not a verified setting. When broadcasting, the Monitor output line uses the saved physical speaker name.

**Logs** provides a timestamped, scrollable event history (last 400 events). It records the detected endpoints and role assignments, reasons an audio setup was blocked, switch/restore operations, WASAPI stream initialization and formats, worker startup errors, and runtime recovery. **Device IDs / diagnostics** on Main appends the full active endpoint names and IDs and opens Logs. You can use **Copy logs** to share an error report or **Clear logs** to start another test. Nothing is automatically uploaded.

A blocked setup should now say *which* Windows default role is still routed through `CABLE Input`. For example, if Console or Multimedia output is already VB-CABLE, switch the Windows default output to real speakers/headphones, then press **Retry audio setup**. Do not set VB-CABLE as Windows' default before AudioPTT starts; AudioPTT changes those roles temporarily while broadcasting.

### Echo Test (diagnostic only)

With **Broadcast OFF**, click **Echo Test** on the Main tab. The app asks for confirmation, then plays a low-level A-B-A tone directly into the VB-CABLE playback endpoint (**CABLE Input**) while simultaneously capturing a few seconds of **WASAPI loopback from the saved physical speakers**. It does not enable microphone monitoring, change Windows default devices, or send data off the PC.

The app compares three frequency-specific tone windows against the physical speaker recording. If the pattern reappears there, it logs an **ECHO DETECTED** warning and opens the Logs tab. That indicates some external monitoring/playback path is bringing VB-CABLE's outgoing signal back to the speakers (for example Discord's mic test or Windows' `Listen to this device`). The test cannot identify which program created the path.

**The tone is real audio sent to VB-CABLE. People in a connected Discord call may hear it.** Run this briefly when appropriate. A "no echo detected" result is not a proof of absence: quiet outputs, disconnected devices, audio format quirks, intermittent monitoring, and headset hardware sidetone may escape the test. This diagnostic is for the first troubleshooting pass and does not run automatically.

## Audio behavior

| Broadcast | Monitor | Discord input (CABLE Output) | Local playback |
|---|---|---|---|
| Off | Either | Physical microphone via AudioPTT | Normal speakers |
| On | Unchecked | Desktop audio routed to VB-CABLE | Silent for apps following Windows default |
| On | Checked | Desktop audio routed to VB-CABLE | Also copied to physical speakers |

**Monitor controls only local playback. It never disables sending PC audio to Discord.** The microphone is not mixed in during Broadcast. Discord's inbound voice should be sent directly to the physical output, not the cable.

## Test setup

1. Install VB-CABLE, then set physical speakers/headphones as Windows' **default output** before starting.
2. Set a physical microphone as **default communications input**.
3. In Discord, explicitly select **CABLE Output (VB-Audio Virtual Cable)** as the **input** and the actual physical speakers/headphones as the **output**.
4. Close OBS and disable any other VB-CABLE monitoring to avoid feedback.
5. Run AudioPTT. When Broadcast is off, it forwards your physical microphone into **CABLE Input** so Discord hears it.
6. Enable Broadcast. Windows desktop output changes to **CABLE Input**. If Monitor is unchecked, AudioPTT does not reproduce that output locally. If checked, AudioPTT captures a copy from the cable and plays it on the saved physical device.
7. Turn Broadcast off to restore your saved default Windows output and microphone forwarding.

## Important limitations

- **Windows default-device switching is currently implemented through an undocumented COM interface (PolicyConfig).** It may not work on all Windows versions. If switching fails, the program should report an error.
- Some applications keep playing to an explicitly selected device or cached audio session even after Windows' default changes. Silent mode therefore applies only to audio actually routed through the virtual cable; it does **not** guarantee a completely silent PC.
- If the app crashes or is force-closed while broadcasting, Windows may continue using VB-CABLE as the default audio device. **Fix manually:** Settings > System > Sound > Output > select your physical speakers.
- The prototype does not use per-process capture; apps that ignore Windows defaults may not be captured, while Discord audio accidentally routed to VB-CABLE may be rebroadcast.
- Microphone mode handles basic stereo/mono and 16-bit PCM/32-bit float format conversion, with simple linear resampling. Audio glitches are possible and there is no echo cancellation, AGC, or full handling of device removal.
- The hotkey is Ctrl+Alt+Space. A system tray icon, persisted device picker, installer, and bundled virtual audio driver are still not included.

## Code layout

- `src/main.cpp` — Win32 window and controls
- `src/AudioDevices.*` — enumerate Windows audio devices
- `src/AudioRouting.*` — save and switch Windows default playback device
- `src/AudioEngine.*` — mic forwarding, cable loopback monitoring, lifecycle
- `CMakeLists.txt` — x64 native C++ build

The output device named **CABLE Input** is the VB-CABLE playback side; Discord records from **CABLE Output**.
