#include "AudioEchoTest.h"
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
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace audioptt {
namespace {
constexpr double pi = 3.14159265358979323846;
constexpr double kFirstHz = 997.0;
constexpr double kSecondHz = 1499.0;
constexpr double kDurationSeconds = 3.0;
constexpr float kOutputLevel = 0.075f;

struct ComScope {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComScope() { if (SUCCEEDED(hr)) CoUninitialize(); }
};
void Require(HRESULT hr, const wchar_t* step) {
    if (FAILED(hr)) {
        wchar_t text[192]{};
        swprintf_s(text, L"%ls failed: HRESULT 0x%08X", step, static_cast<unsigned>(hr));
        // Our operation labels are ASCII; convert explicitly rather than
        // narrowing wchar_t iterators (which triggers MSVC warning C4244).
        std::string ascii;
        for (const wchar_t ch : std::wstring(text))
            ascii.push_back(ch <= 0x7F ? static_cast<char>(ch) : '?');
        throw std::runtime_error(ascii);
    }
}
enum class Sample { Float, Pcm16 };
Sample Format(const WAVEFORMATEX* wave) {
    if (!wave || wave->nChannels == 0 || wave->nSamplesPerSec < 8000 ||
        wave->nSamplesPerSec > 384000)
        throw std::runtime_error("Device returned an invalid sample format");
    if (wave->wBitsPerSample == 32 && wave->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        return Sample::Float;
    if (wave->wBitsPerSample == 16 && wave->wFormatTag == WAVE_FORMAT_PCM)
        return Sample::Pcm16;
    if (wave->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        wave->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wave);
        if (wave->wBitsPerSample == 32 && extended->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
            return Sample::Float;
        if (wave->wBitsPerSample == 16 && extended->SubFormat == KSDATAFORMAT_SUBTYPE_PCM)
            return Sample::Pcm16;
    }
    throw std::runtime_error("Echo test supports only float32/PCM16 WASAPI mix formats");
}
float Decode(const BYTE* data, size_t index, Sample type) {
    if (type == Sample::Float) {
        float value = 0.0f;
        std::memcpy(&value, data + index * sizeof(float), sizeof(value));
        return std::isfinite(value) ? value : 0.0f;
    }
    std::int16_t value = 0;
    std::memcpy(&value, data + index * sizeof(value), sizeof(value));
    return static_cast<float>(value) / 32768.0f;
}
void Encode(BYTE* data, size_t index, Sample type, float value) {
    value = std::clamp(value, -1.0f, 1.0f);
    if (type == Sample::Float) {
        std::memcpy(data + index * sizeof(value), &value, sizeof(value));
    } else {
        const std::int16_t pcm = static_cast<std::int16_t>(std::lround(value * 32767.0f));
        std::memcpy(data + index * sizeof(pcm), &pcm, sizeof(pcm));
    }
}
struct Stream {
    ComPtr<IAudioClient> client;
    WAVEFORMATEX* format = nullptr;
    Sample sample{};
    UINT32 frames = 0;
    bool started = false;
    ~Stream() {
        if (started && client) client->Stop();
        if (format) CoTaskMemFree(format);
    }
    void Open(IMMDevice* device, bool loopback) {
        Require(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                reinterpret_cast<void**>(client.GetAddressOf())), L"Activate audio device");
        Require(client->GetMixFormat(&format), L"Read device mix format");
        sample = Format(format);
        Require(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0,
                300000, 0, format, nullptr), L"Initialize audio stream");
        Require(client->GetBufferSize(&frames), L"Read audio buffer size");
    }
    void Start() {
        Require(client->Start(), L"Start audio stream");
        started = true;
    }
};
double ToneFrequency(double seconds) {
    if (seconds >= 0.0 && seconds < 0.3) return kFirstHz;
    if (seconds >= 0.43 && seconds < 0.73) return kSecondHz;
    if (seconds >= 0.86 && seconds < 1.16) return kFirstHz;
    return 0.0;
}
float ToneAt(double seconds) {
    const double frequency = ToneFrequency(seconds);
    if (frequency == 0.0) return 0.0f;
    const double segmentStart = seconds < 0.3 ? 0.0 : (seconds < 0.73 ? 0.43 : 0.86);
    const double position = seconds - segmentStart;
    const double edge = std::clamp(std::min(position, 0.3 - position) / 0.012, 0.0, 1.0);
    return kOutputLevel * static_cast<float>(edge * std::sin(2.0 * pi * frequency * position));
}
double BandAmplitude(const std::vector<float>& audio, unsigned rate, double start,
                     double seconds, double frequency) {
    const auto from = static_cast<size_t>(std::max(0.0, start) * rate);
    const auto count = static_cast<size_t>(seconds * rate);
    if (!count || from > audio.size() || count > audio.size() - from) return 0.0;
    double cosine = 0.0, sine = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const double angle = (2.0 * pi * frequency / rate) * i;
        const double value = audio[from + i];
        cosine += value * std::cos(angle);
        sine += value * std::sin(angle);
    }
    return 2.0 * std::hypot(cosine, sine) / count;
}
} // namespace

EchoTestResult RunEchoTest(const std::wstring& cableRenderId,
                            const std::wstring& physicalRenderId) {
    EchoTestResult result;
    try {
        if (cableRenderId.empty() || physicalRenderId.empty() ||
            cableRenderId == physicalRenderId)
            throw std::runtime_error("Missing endpoints or identical cable and speaker devices");
        ComScope com;
        Require(com.hr, L"Initialize COM");
        ComPtr<IMMDeviceEnumerator> enumerator;
        Require(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&enumerator)), L"Create audio enumerator");
        ComPtr<IMMDevice> cable, speakers;
        Require(enumerator->GetDevice(cableRenderId.c_str(), &cable), L"Find CABLE Input");
        Require(enumerator->GetDevice(physicalRenderId.c_str(), &speakers), L"Find physical speakers");
        Stream output, input;
        output.Open(cable.Get(), false);
        input.Open(speakers.Get(), true);
        ComPtr<IAudioRenderClient> render;
        ComPtr<IAudioCaptureClient> capture;
        Require(output.client->GetService(IID_PPV_ARGS(&render)), L"Get cable render client");
        Require(input.client->GetService(IID_PPV_ARGS(&capture)), L"Get speaker loopback client");

        const unsigned inputRate = input.format->nSamplesPerSec;
        const unsigned outputRate = output.format->nSamplesPerSec;
        LogInfo(L"Echo test: injecting A-B-A tone into CABLE Input at " +
                std::to_wstring(outputRate) + L" Hz; capturing physical speaker loopback at " +
                std::to_wstring(inputRate) + L" Hz.");
        const size_t outputFrames = static_cast<size_t>(inputRate * kDurationSeconds);
        std::vector<float> heard(outputFrames, 0.0f);
        size_t cursor = 0;
        size_t submitted = 0;

        // Start both streams on the same thread. The speaker loopback collects
        // the actual speaker mix, not AudioPTT's own VB-CABLE render packets.
        LogRenderSessions(L"Echo Test / before tone", physicalRenderId, cableRenderId);
        input.Start();
        output.Start();
        const auto started = std::chrono::steady_clock::now();
        // Session sampling must not block the audio processing loop.
        std::jthread probe([physicalRenderId, cableRenderId]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(850));
            LogRenderSessions(L"Echo Test / while tone active",
                              physicalRenderId, cableRenderId);
        });
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()
               < kDurationSeconds) {
            UINT32 packet = 0;
            Require(capture->GetNextPacketSize(&packet), L"Get speaker loopback packet count");
            while (packet > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                Require(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr),
                        L"Read speaker loopback buffer");
                // The Windows loopback stream may omit silent packets. Use
                // current wall time to fill those gaps instead of compressing
                // the timing of our A-B-A sequence.
                const auto now = std::chrono::steady_clock::now();
                const double elapsed = std::chrono::duration<double>(now - started).count();
                const size_t estimatedEnd = static_cast<size_t>(elapsed * inputRate);
                const size_t estimatedBegin = estimatedEnd > frames ? estimatedEnd - frames : 0;
                const size_t begin = std::max(cursor, estimatedBegin);
                const unsigned channels = input.format->nChannels;
                if (begin < heard.size()) {
                    for (UINT32 f = 0; f < frames && begin + f < heard.size(); ++f) {
                        if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                            const size_t base = static_cast<size_t>(f) * channels;
                            const float left = Decode(data, base, input.sample);
                            const float right = channels > 1
                                ? Decode(data, base + 1, input.sample) : left;
                            heard[begin + f] = (left + right) * 0.5f;
                        }
                    }
                }
                cursor = begin + frames;
                Require(capture->ReleaseBuffer(frames), L"Release speaker loopback buffer");
                Require(capture->GetNextPacketSize(&packet), L"Read next speaker packet count");
            }

            UINT32 padding = 0;
            Require(output.client->GetCurrentPadding(&padding), L"Get cable render padding");
            if (padding > output.frames)
                throw std::runtime_error("VB-CABLE returned invalid render padding");
            const UINT32 freeFrames = output.frames - padding;
            if (freeFrames) {
                BYTE* data = nullptr;
                Require(render->GetBuffer(freeFrames, &data), L"Reserve cable render buffer");
                const unsigned channels = output.format->nChannels;
                for (UINT32 f = 0; f < freeFrames; ++f) {
                    const float tone = ToneAt(static_cast<double>(submitted + f) / outputRate);
                    for (unsigned ch = 0; ch < channels; ++ch)
                        Encode(data, (static_cast<size_t>(f) * channels) + ch,
                               output.sample, tone);
                }
                Require(render->ReleaseBuffer(freeFrames, 0), L"Submit cable render buffer");
                submitted += freeFrames;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        // A-B-A is deliberately tested against the wrong frequency in each
        // slot so ordinary speaker/music content is much less likely to
        // trigger a false positive.
        if (probe.joinable()) probe.join();
        LogRenderSessions(L"Echo Test / after tone", physicalRenderId, cableRenderId);
        size_t nonzeroFrames = 0;
        double sumSquared = 0.0;
        for (const float value : heard) {
            sumSquared += static_cast<double>(value) * value;
            if (std::abs(value) > 0.0001f) ++nonzeroFrames;
        }
        LogInfo(L"Echo test speaker capture: RMS=" +
                std::to_wstring(std::sqrt(sumSquared / heard.size())) +
                L", nonzero frames=" + std::to_wstring(nonzeroFrames) +
                L"/" + std::to_wstring(heard.size()) +
                L", submitted cable frames=" + std::to_wstring(submitted));
        double strongest = 0.0;
        double foundDelay = -1.0;
        for (double lag = 0.0; lag <= 1.35; lag += 0.025) {
            const double a1 = BandAmplitude(heard, inputRate, lag + 0.065,
                                            0.18, kFirstHz);
            const double b = BandAmplitude(heard, inputRate, lag + 0.495,
                                           0.18, kSecondHz);
            const double a2 = BandAmplitude(heard, inputRate, lag + 0.925,
                                            0.18, kFirstHz);
            const double wrongA1 = BandAmplitude(heard, inputRate, lag + 0.065,
                                                 0.18, kSecondHz);
            const double wrongB = BandAmplitude(heard, inputRate, lag + 0.495,
                                                0.18, kFirstHz);
            const double wrongA2 = BandAmplitude(heard, inputRate, lag + 0.925,
                                                 0.18, kSecondHz);
            const double level = std::min({a1, b, a2});
            if (level > strongest) strongest = level;
            if (level > 0.0035 &&
                a1 > wrongA1 * 1.8 && b > wrongB * 1.8 &&
                a2 > wrongA2 * 1.8) {
                result.echoDetected = true;
                result.toneLevel = level;
                foundDelay = lag;
                break;
            }
        }
        result.completed = true;
        if (result.echoDetected)
            result.message = L"ECHO DETECTED: the tone sent to VB-CABLE came back through your "
                             L"physical speaker output. Check Discord Mic Test, Windows Listen, "
                             L"and any other audio-monitoring app.";
        else {
            result.toneLevel = strongest;
            result.message = L"No matching test tone detected in speaker loopback. "
                             L"This does not rule out intermittent echo or another playback device.";
        }
        LogInfo(L"Echo test speaker signature amplitude: " + std::to_wstring(result.toneLevel) +
                L", matched lag=" + (foundDelay < 0.0 ? std::wstring(L"not found") :
                                     std::to_wstring(foundDelay) + L" seconds"));
        LogInfo(L"Echo test process list is observational. Active PIDs do not identify the "
                L"audio source with certainty; compare with Discord fully closed.");
        if (result.echoDetected) LogWarning(result.message);
        else LogInfo(result.message);
    } catch (const std::exception& error) {
        // Exception messages above use ASCII names/hex HRESULT, not device names.
        std::string narrow(error.what());
        result.message = L"Echo test could not run: " +
                         std::wstring(narrow.begin(), narrow.end());
        LogError(result.message);
    }
    return result;
}
} // namespace audioptt
