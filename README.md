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
- No hotkey, tray icon, persisted device picker, installer, or virtual audio driver is included yet.

## Code layout

- `src/main.cpp` — Win32 window and controls
- `src/AudioDevices.*` — enumerate Windows audio devices
- `src/AudioRouting.*` — save and switch Windows default playback device
- `src/AudioEngine.*` — mic forwarding, cable loopback monitoring, lifecycle
- `CMakeLists.txt` — x64 native C++ build

The output device named **CABLE Input** is the VB-CABLE playback side; Discord records from **CABLE Output**.
