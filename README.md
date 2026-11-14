# AudioPTT

A minimal native Windows utility for switching Discord's input between your microphone and desktop audio. **Early framework: audio routing is not implemented yet.**

## Goal

Discord (desktop or browser) stays connected to a single virtual recording endpoint (initially VB-CABLE Output). AudioPTT chooses whether that endpoint receives microphone audio or computer audio. When broadcasting, mic audio must not be forwarded. A Monitor checkbox should determine whether broadcast computer audio is also heard locally; Discord incoming voice must remain audible.

| Broadcast | Monitor | Virtual mic receives | PC playback |
|---|---|---|---|
| Off | Either | Microphone | Normal |
| On | Off | PC audio | Silent locally (except Discord voice) |
| On | On | PC audio | Audible locally |

## Build

Requires Visual Studio 2022 or newer with **Desktop development with C++** and the Windows SDK.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Debug
```

Run `build/Debug/AudioPTT.exe`. Visual Studio can also open this repository as a CMake folder.

## Current status

- Native Win32 UI with Broadcast toggle and Monitor checkbox.
- WASAPI device enumerator for render and capture endpoints; shows device IDs and friendly names.
- Thread-safe state model and an audio engine interface prepared for routing.
- **No audio is transmitted yet.** The UI deliberately labels routing as not implemented.

## Audio architecture notes

A normal Windows application cannot directly inject audio into Discord's microphone API stream. For the first implementation, select **CABLE Output (VB-Audio Virtual Cable)** as Discord's input, and write audio to the matching **CABLE Input** render endpoint. Do not confuse those names: CABLE Input is the playback endpoint, CABLE Output is the recording endpoint.

WASAPI loopback capture captures audio from a render endpoint, but it does not independently reroute applications or safely mute their original output. Fully silent broadcast requires a dedicated virtual playback path (or per-application audio routing), with Discord output excluded to avoid echo. Never implement silent broadcast by muting the Windows default audio device without testing loopback behavior.

## Implementation order

1. Enumerate and select physical microphone, physical speakers, and cable endpoints by persistent endpoint ID.
2. Start microphone capture and write samples to CABLE Input in Off mode, with resampling and format conversion where required.
3. Capture PC audio (WASAPI loopback), write to CABLE Input in On mode, and gate microphone frames completely.
4. Add a safe silent-broadcast path and optional monitor branch while preserving Discord incoming audio.
5. Add global hotkey, tray UI, device-change handling, routing diagnostics, and error recovery.

Never silently claim broadcasting works if a cable endpoint cannot be opened or routing hasn't started. Avoid feedback loops by excluding the cable endpoint from PC audio capture.
