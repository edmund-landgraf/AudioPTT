# AudioPTT

A native C++/Win32 prototype for feeding either microphone audio or desktop audio into Discord through VB-CABLE, without OBS.

**Development prototype — not yet verified on a Windows machine.** The current engine performs real WASAPI capture and render operations, but is not hardened for production use.

## Build

Install Visual Studio 2022 or newer with **Desktop development with C++** and a Windows SDK, then open this folder in Visual Studio (CMake project), or build:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Debug
.\build\Debug\AudioPTT.exe
```

The GitHub Actions workflow also compiles the application on Windows and uploads an executable build artifact when successful.

## Test setup

1. Install VB-CABLE. In **Windows Sound**, set your physical speakers/headphones as the **default output**.
2. Set a **physical microphone** as your Windows default **communications input** (not VB-CABLE).
3. Set Discord's **Input Device** to **CABLE Output (VB-Audio Virtual Cable)**. Keep Discord's **Output Device** set to your physical speakers/headphones.
4. Close OBS, particularly any audio monitoring routed to VB-CABLE.
5. Start AudioPTT. It automatically attempts microphone → **CABLE Input** on launch.
6. First confirm mic passthrough in Discord's mic test. Then check **Monitor** and press **BROADCAST PC AUDIO**. This switches to physical-speaker loopback → CABLE Input.

**Do not set CABLE Input as the Windows default output for this prototype.** That would produce feedback or duplicate routing. AudioPTT detects this and refuses desktop-loopback capture.

## Current behavior

| Broadcast | Monitor | Discord receives | Local computer audio |
|---|---|---|---|
| Off | Off / On | Physical microphone (forwarded) | Normal playback |
| On | On | Desktop audio from default physical speaker output | Still audible normally |
| On | Off | **Not implemented: button refuses mode** | Unchanged |

The mic is not mixed into desktop audio in Broadcast mode, because the engine stops the microphone stream and opens desktop loopback instead. **The app does not hardware-mute the microphone; it prevents routing it to VB-CABLE.**

The microphone-to-cable and desktop-loopback-to-cable paths use WASAPI shared mode, basic stereo/mono conversion, simple linear sample-rate conversion, and 32-bit floating-point or 16-bit PCM mix formats. They are not yet performance-tuned or thoroughly tested against device resets, sleep/resume, or differing driver formats.

### Limitations to address next

- **Silent desktop broadcast requires different architecture.** WASAPI loopback only copies audio already rendered to a physical device. Disabling local playback separately requires a dedicated virtual playback path and deliberate routing of application audio; do not mute the default device to simulate silence.
- Currently uses Windows *default* devices. Device selection by endpoint ID and automatic reinitialization on device change are still needed.
- A transient audio-driver error stops the worker; the prototype does not automatically reconnect.
- Hotkey, system tray, device settings, visual levels, installer, and proper error telemetry are still pending.
- Discord voice should remain directed to physical speakers; if it is captured through the same loopback, it will be broadcast too. A future version must exclude Discord from system capture.
- No echo cancellation, noise filtering, or AGC is performed by the application.

The Discord input is the **CABLE Output recording endpoint**; AudioPTT writes to the matching **CABLE Input playback endpoint**. A regular user-mode process cannot inject arbitrary PCM samples directly into Discord's internal microphone stream.

## Files

- `src/AudioDevices.*` — Windows capture/render endpoint discovery
- `src/AudioEngine.*` — WASAPI capture, format conversion, sample queue, and cable rendering
- `src/main.cpp` — tiny native Win32 interface
- `CMakeLists.txt` — Visual Studio / x64 build definition
