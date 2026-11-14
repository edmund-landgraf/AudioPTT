#include "AudioPathScan.h"
#include "Diagnostics.h"
#include "AudioSessionDiagnostics.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

using Microsoft::WRL::ComPtr;
namespace audioptt {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kToneHz = 997.0;
constexpr float kToneLevel = 0.06f;
constexpr double kToneStart = 0.35;
constexpr double kToneEnd = 2.50;
constexpr double kTotalSeconds = 3.45;
constexpr double kProbeThreshold = 0.004;

struct ScopedCom {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ScopedCom() { if (SUCCEEDED(hr)) CoUninitialize(); }
};
void Require(HRESULT hr, const char* operation) {
    if (FAILED(hr)) {
        char message[256]{};
        sprintf_s(message, "%s failed (HRESULT 0x%08X)", operation,
                  static_cast<unsigned>(hr));
        throw std::runtime_error(message);
    }
}
std::wstring Wide(const std::string& ascii) {
    return std::wstring(ascii.begin(), ascii.end());
}
enum class Format { Float32, Pcm16 };
Format SampleType(const WAVEFORMATEX* wave) {
    if (!wave || !wave->nChannels || wave->nSamplesPerSec < 8000 ||
        wave->nSamplesPerSec > 384000)
        throw std::runtime_error("Invalid WASAPI mix format");
    if (wave->wBitsPerSample == 32 && wave->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        return Format::Float32;
    if (wave->wBitsPerSample == 16 && wave->wFormatTag == WAVE_FORMAT_PCM)
        return Format::Pcm16;
    if (wave->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        wave->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* ex = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wave);
        if (wave->wBitsPerSample == 32 &&
            ex->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) return Format::Float32;
        if (wave->wBitsPerSample == 16 &&
            ex->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) return Format::Pcm16;
    }
    throw std::runtime_error("Unsupported mix format; expected float32 or PCM16");
}
float Decode(const BYTE* data, size_t sample, Format format) {
    if (format == Format::Float32) {
        float value = 0.0f;
        std::memcpy(&value, data + sample * sizeof(value), sizeof(value));
        return std::isfinite(value) ? value : 0.0f;
    }
    std::int16_t value = 0;
    std::memcpy(&value, data + sample * sizeof(value), sizeof(value));
    return static_cast<float>(value) / 32768.0f;
}
void Encode(BYTE* data, size_t sample, Format format, float value) {
    value = std::clamp(value, -1.0f, 1.0f);
    if (format == Format::Float32) {
        std::memcpy(data + sample * sizeof(value), &value, sizeof(value));
    } else {
        const auto signedValue = static_cast<std::int16_t>(std::lround(value * 32767.0f));
        std::memcpy(data + sample * sizeof(signedValue), &signedValue, sizeof(signedValue));
    }
}
struct ToneMeter {
    double largest = 0.0;
    double cosSum = 0.0;
    double sinSum = 0.0;
    unsigned count = 0;
    unsigned windowLength = 0;
    double phase = 0.0;
    double phaseStep = 0.0;
    explicit ToneMeter(unsigned rate) {
        windowLength = std::max(1U, rate / 5); // 200-ms coherent windows
        phaseStep = 2.0 * kPi * kToneHz / rate;
    }
    void Add(float value) {
        cosSum += value * std::cos(phase);
        sinSum += value * std::sin(phase);
        phase += phaseStep;
        if (phase > 2.0 * kPi) phase -= 2.0 * kPi;
        if (++count == windowLength) {
            largest = std::max(largest, 2.0 * std::hypot(cosSum, sinSum) / count);
            cosSum = sinSum = 0.0;
            count = 0;
        }
    }
};
struct CaptureProbe {
    const wchar_t* label;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    WAVEFORMATEX* mix = nullptr;
    Format format{};
    std::unique_ptr<ToneMeter> tone;
    double sumSquares = 0.0;
    double peak = 0.0;
    double baselineSquares = 0.0;
    std::uint64_t baselineFrames = 0;
    std::uint64_t frames = 0;
    std::uint64_t silentFrames = 0;
    bool started = false;
    bool available = false;

    explicit CaptureProbe(const wchar_t* title) : label(title) {}
    ~CaptureProbe() {
        if (started && client) client->Stop();
        if (mix) CoTaskMemFree(mix);
    }
    bool Open(IMMDevice* endpoint, bool loopback) {
        try {
            Require(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                    reinterpret_cast<void**>(client.GetAddressOf())), "Activate capture endpoint");
            Require(client->GetMixFormat(&mix), "Get capture mix format");
            format = SampleType(mix);
            Require(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                    loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0,
                    300000, 0, mix, nullptr), "Initialize read-only capture");
            Require(client->GetService(IID_PPV_ARGS(&capture)), "Get capture client");
            tone = std::make_unique<ToneMeter>(mix->nSamplesPerSec);
            LogInfo(std::wstring(L"PATH SCAN ") + label + L" opened: " +
                    std::to_wstring(mix->nSamplesPerSec) + L" Hz, " +
                    std::to_wstring(mix->nChannels) + L" ch, " +
                    (loopback ? L"render-loopback" : L"capture endpoint"));
            Require(client->Start(), "Start capture endpoint");
            started = available = true;
            return true;
        } catch (const std::exception& e) {
            LogWarning(std::wstring(L"PATH SCAN ") + label +
                       L" unavailable: " + Wide(e.what()));
            return false;
        }
    }
    void Poll(bool toneWindow, bool baselineWindow) {
        if (!available) return;
        UINT32 pending = 0;
        Require(capture->GetNextPacketSize(&pending), "Read capture packet size");
        while (pending > 0) {
            BYTE* data = nullptr;
            UINT32 count = 0;
            DWORD flags = 0;
            Require(capture->GetBuffer(&data, &count, &flags, nullptr, nullptr),
                    "Read capture buffer");
            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            if (silent) silentFrames += count;
            for (UINT32 i = 0; i < count; ++i) {
                float value = 0.0f;
                if (!silent) {
                    const size_t offset = static_cast<size_t>(i) * mix->nChannels;
                    value = Decode(data, offset, format);
                    if (mix->nChannels > 1)
                        value = (value + Decode(data, offset + 1, format)) * 0.5f;
                }
                peak = std::max(peak, static_cast<double>(std::abs(value)));
                sumSquares += static_cast<double>(value) * value;
                ++frames;
                if (baselineWindow) {
                    baselineSquares += static_cast<double>(value) * value;
                    ++baselineFrames;
                }
                if (toneWindow) tone->Add(value);
            }
            Require(capture->ReleaseBuffer(count), "Release capture packet");
            Require(capture->GetNextPacketSize(&pending), "Read next capture packet size");
        }
    }
    double Rms() const { return frames ? std::sqrt(sumSquares / frames) : 0.0; }
    double BaselineRms() const {
        return baselineFrames ? std::sqrt(baselineSquares / baselineFrames) : 0.0;
    }
    double Tone() const { return tone ? tone->largest : 0.0; }
    void Log() const {
        const auto name = std::wstring(label);
        if (!available) {
            LogWarning(L"PATH SCAN " + name + L": NOT OPENED; no measurement available");
            return;
        }
        LogInfo(L"PATH SCAN " + name + L": frames=" + std::to_wstring(frames) +
                L", silentFrames=" + std::to_wstring(silentFrames) +
                L", RMS=" + std::to_wstring(Rms()) +
                L", baselineRMS(before tone)=" + std::to_wstring(BaselineRms()) +
                L", peak=" + std::to_wstring(peak) +
                L", 997Hz amplitude=" + std::to_wstring(Tone()));
    }
};
struct RenderProbe {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    WAVEFORMATEX* mix = nullptr;
    Format format{};
    UINT32 capacity = 0;
    std::uint64_t frames = 0;
    bool started = false;
    ~RenderProbe() {
        if (started && client) client->Stop();
        if (mix) CoTaskMemFree(mix);
    }
    void Open(IMMDevice* endpoint) {
        Require(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                reinterpret_cast<void**>(client.GetAddressOf())), "Activate CABLE Input render");
        Require(client->GetMixFormat(&mix), "Get CABLE Input render format");
        format = SampleType(mix);
        Require(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 300000, 0,
                                   mix, nullptr), "Initialize CABLE Input render");
        Require(client->GetBufferSize(&capacity), "Get cable render capacity");
        Require(client->GetService(IID_PPV_ARGS(&render)), "Get cable render client");
        LogInfo(L"PATH SCAN CABLE Input render: " +
                std::to_wstring(mix->nSamplesPerSec) + L" Hz, " +
                std::to_wstring(mix->nChannels) + L" ch, buffer=" +
                std::to_wstring(capacity) + L" frames");
    }
    void Pump() {
        UINT32 padding = 0;
        Require(client->GetCurrentPadding(&padding), "Get cable render padding");
        if (padding > capacity) throw std::runtime_error("Invalid cable render padding");
        const unsigned count = capacity - padding;
        if (count == 0) return;
        BYTE* bytes = nullptr;
        Require(render->GetBuffer(count, &bytes), "Reserve cable render frames");
        const unsigned rate = mix->nSamplesPerSec;
        for (unsigned i = 0; i < count; ++i) {
            const double t = static_cast<double>(frames + i) / rate;
            const double edge = std::min(t - kToneStart, kToneEnd - t);
            const float envelope = (edge > 0) ?
                static_cast<float>(std::clamp(edge / 0.02, 0.0, 1.0)) : 0.0f;
            const float tone = envelope * kToneLevel *
                static_cast<float>(std::sin(2.0 * kPi * kToneHz * t));
            for (unsigned c = 0; c < mix->nChannels; ++c)
                Encode(bytes, (static_cast<size_t>(i) * mix->nChannels) + c,
                       format, tone);
        }
        Require(render->ReleaseBuffer(count, 0), "Submit cable render frames");
        frames += count;
        if (!started) {
            Require(client->Start(), "Start cable render signal");
            started = true;
        }
    }
};
ComPtr<IMMDevice> OpenDevice(IMMDeviceEnumerator* enumerator, const std::wstring& id,
                             const char* operation) {
    ComPtr<IMMDevice> endpoint;
    Require(enumerator->GetDevice(id.c_str(), &endpoint), operation);
    return endpoint;
}
} // namespace

AudioPathScanResult RunAudioPathScan(
    const std::wstring& physicalMicId,
    const std::wstring& cableRenderId,
    const std::wstring& cableCaptureId,
    const std::wstring& physicalSpeakerId,
    bool broadcast, bool monitor) {

    AudioPathScanResult result;
    result.monitoringExpected = broadcast && monitor;
    LogInfo(L"==== FULL PATH SCAN START (Broadcast " +
            std::wstring(broadcast ? L"ON" : L"OFF") + L", Monitor " +
            std::wstring(monitor ? L"ON" : L"OFF") + L") ====");
    try {
        if (cableRenderId.empty() || cableCaptureId.empty() ||
            physicalMicId.empty() || physicalSpeakerId.empty())
            throw std::runtime_error("One or more required endpoint IDs missing");
        if (cableRenderId == physicalSpeakerId || cableCaptureId == physicalMicId)
            throw std::runtime_error("Cable endpoint unexpectedly matches a physical endpoint");
        ScopedCom com;
        Require(com.hr, "Initialize scan COM");
        ComPtr<IMMDeviceEnumerator> enumerator;
        Require(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&enumerator)), "Create device enumerator");
        auto physicalMic = OpenDevice(enumerator.Get(), physicalMicId, "Find physical microphone");
        auto cableInput = OpenDevice(enumerator.Get(), cableRenderId, "Find CABLE Input");
        auto cableOutput = OpenDevice(enumerator.Get(), cableCaptureId, "Find CABLE Output");
        auto speakers = OpenDevice(enumerator.Get(), physicalSpeakerId, "Find physical speaker");
        LogRenderSessions(L"Full path scan / before test signal", physicalSpeakerId,
                          cableRenderId);
        LogCaptureSessions(L"Full path scan / before test signal", physicalMicId,
                           cableCaptureId);

        CaptureProbe mic(L"WEBCAM MICROPHONE");
        CaptureProbe cable(L"CABLE Output (Discord input)");
        CaptureProbe speaker(L"REALTEK SPEAKER LOOPBACK");
        mic.Open(physicalMic.Get(), false);
        cable.Open(cableOutput.Get(), false);
        speaker.Open(speakers.Get(), true);
        if (!cable.available)
            throw std::runtime_error("Cannot open CABLE Output for independent WASAPI capture");

        RenderProbe output;
        output.Open(cableInput.Get());
        LogInfo(L"PATH SCAN injecting a quiet 997 Hz test tone into explicit CABLE Input. "
                L"Recording simultaneous webcam/CABLE Output/Realtek levels; no microphone "
                L"or device-default changes. Tone may be heard by Discord participants.");

        const auto begin = std::chrono::steady_clock::now();
        while (std::chrono::duration<double>(
                   std::chrono::steady_clock::now() - begin).count() < kTotalSeconds) {
            const double seconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - begin).count();
            // Allow for WASAPI/driver buffering by sampling several 200-ms
            // windows during a long continuous injected tone.
            const bool toneWindow = seconds >= 0.60 && seconds < 2.95;
            const bool baselineWindow = seconds < 0.28;
            mic.Poll(false, baselineWindow);
            cable.Poll(toneWindow, baselineWindow);
            speaker.Poll(toneWindow, baselineWindow);
            output.Pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        mic.Poll(false, false);
        cable.Poll(false, false);
        speaker.Poll(false, false);
        LogInfo(L"PATH SCAN render frames submitted=" + std::to_wstring(output.frames));
        mic.Log();
        cable.Log();
        speaker.Log();
        LogRenderSessions(L"Full path scan / after test signal", physicalSpeakerId,
                          cableRenderId);
        LogCaptureSessions(L"Full path scan / after test signal", physicalMicId,
                           cableCaptureId);

        result.micRms = mic.Rms();
        result.cableRms = cable.Rms();
        result.speakerRms = speaker.Rms();
        result.micBaselineRms = mic.BaselineRms();
        result.cableBaselineRms = cable.BaselineRms();
        result.speakerBaselineRms = speaker.BaselineRms();
        result.cableToneAmplitude = cable.Tone();
        result.speakerToneAmplitude = speaker.Tone();
        result.cableToneDetected = cable.Tone() >= kProbeThreshold;
        result.speakerToneDetected = speaker.available &&
                                     speaker.Tone() >= kProbeThreshold;
        result.completed = true;

        LogInfo(L"PATH SCAN RESULT: injected 997Hz " +
                std::wstring(result.cableToneDetected ?
                    L"DETECTED on CABLE Output (cable path passes)." :
                    L"NOT DETECTED on CABLE Output (cable path failed or signal suppressed)."));
        if (result.speakerToneDetected && !result.monitoringExpected)
            LogWarning(L"PATH SCAN RESULT: injected tone also reached REALTEK with "
                       L"AudioPTT monitor OFF. External Listen/routing is suspected.");
        else if (result.speakerToneDetected)
            LogInfo(L"PATH SCAN RESULT: speaker tone is expected with Monitor ON.");
        else
            LogInfo(L"PATH SCAN RESULT: no injected tone detected in Realtek loopback.");
        if (broadcast && cable.available && speaker.available &&
            result.cableBaselineRms < 0.001 && result.speakerBaselineRms > 0.005)
            LogWarning(L"PATH SCAN: before injecting the test tone, Realtek had audio "
                       L"but CABLE Output was almost silent. Existing browser audio may "
                       L"still be pinned to Realtek. Play narration during scan to verify.");

        result.summary =
            L"VB-CABLE test: " +
            std::wstring(result.cableToneDetected ? L"PASS - signal captured on CABLE Output" :
                                                     L"FAIL - test tone not captured at CABLE Output") +
            L"\nCABLE Output tone amplitude: " + std::to_wstring(result.cableToneAmplitude) +
            L"\nRealtek tone: " +
            std::wstring(result.speakerToneDetected ?
                        (result.monitoringExpected ? L"present (Monitor ON)" :
                                                    L"UNEXPECTED (Monitor OFF)") :
                        L"not detected") +
            L"\nRealtek tone amplitude: " + std::to_wstring(result.speakerToneAmplitude) +
            L"\nWebcam RMS: " + std::to_wstring(result.micRms) +
            L"\nCABLE Output RMS: " + std::to_wstring(result.cableRms) +
            L"\nPre-test cable RMS: " + std::to_wstring(result.cableBaselineRms) +
            L"\nPre-test Realtek RMS: " + std::to_wstring(result.speakerBaselineRms) +
            L"\n\nThe scan cannot verify what Discord sends to remote listeners. "
            L"Full measurements and process sessions are in Logs.";
        LogInfo(L"==== FULL PATH SCAN COMPLETE ====");
    } catch (const std::exception& e) {
        result.summary = L"Full path scan incomplete: " + Wide(e.what());
        LogError(result.summary);
    }
    return result;
}
} // namespace audioptt
