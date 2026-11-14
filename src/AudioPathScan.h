#pragma once
#include <string>

namespace audioptt {
struct AudioPathScanResult {
    bool completed = false;
    bool cableToneDetected = false;       // Verified CABLE Output capture receives injected signal
    bool speakerToneDetected = false;     // Realtek loopback receives injected signal
    bool monitoringExpected = false;
    double micRms = 0.0;
    double cableRms = 0.0;
    double speakerRms = 0.0;
    // First ~300 ms before the known test tone starts. Useful when
    // browser narration is already playing on one endpoint.
    double micBaselineRms = 0.0;
    double cableBaselineRms = 0.0;
    double speakerBaselineRms = 0.0;
    double cableToneAmplitude = 0.0;
    double speakerToneAmplitude = 0.0;
    std::wstring summary;
};
// Diagnostic produces a quiet 997-Hz calibration tone into CABLE Input and
// simultaneously opens read-only WASAPI captures on webcam, CABLE Output,
// and Realtek speaker loopback. Does not change defaults or monitor the mic.
// Call from a worker thread. The tone is also visible to Discord if in a call.
AudioPathScanResult RunAudioPathScan(
    const std::wstring& physicalMicId,
    const std::wstring& cableRenderId,
    const std::wstring& cableCaptureId,
    const std::wstring& physicalSpeakerId,
    bool broadcast,
    bool monitor);
} // namespace audioptt
