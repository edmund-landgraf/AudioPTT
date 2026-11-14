#include "AudioSample.h"
#include "Diagnostics.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <sapi.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace audioptt {
namespace {
constexpr wchar_t kSpeech[] =
    L"Hello, this is sample audio. We are playing sample audio.";
constexpr unsigned kVoiceRate = 22050;

struct ScopedCom {
    const HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ScopedCom() { if (SUCCEEDED(result)) CoUninitialize(); }
};
void Require(HRESULT hr, const char* step) {
    if (FAILED(hr)) {
        char message[256]{};
        sprintf_s(message, "%s failed: HRESULT 0x%08X", step, static_cast<unsigned>(hr));
        throw std::runtime_error(message);
    }
}
unsigned ReadLE16(const BYTE* p) {
    return static_cast<unsigned>(p[0]) | (static_cast<unsigned>(p[1]) << 8);
}
unsigned ReadLE32(const BYTE* p) {
    return static_cast<unsigned>(p[0]) |
           (static_cast<unsigned>(p[1]) << 8) |
           (static_cast<unsigned>(p[2]) << 16) |
           (static_cast<unsigned>(p[3]) << 24);
}
void DecodeWaveIfPresent(std::vector<BYTE>& bytes) {
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
        std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) return;

    size_t at = 12;
    bool validFormat = false;
    size_t pcmStart = 0;
    size_t pcmSize = 0;
    while (at + 8 <= bytes.size()) {
        const unsigned size = ReadLE32(bytes.data() + at + 4);
        const size_t start = at + 8;
        if (size > bytes.size() - start) break;
        if (std::memcmp(bytes.data() + at, "fmt ", 4) == 0 && size >= 16) {
            const BYTE* fmt = bytes.data() + start;
            validFormat = ReadLE16(fmt) == WAVE_FORMAT_PCM &&
                          ReadLE16(fmt + 2) == 1 &&
                          ReadLE32(fmt + 4) == kVoiceRate &&
                          ReadLE16(fmt + 14) == 16;
        } else if (std::memcmp(bytes.data() + at, "data", 4) == 0) {
            pcmStart = start;
            pcmSize = size;
        }
        at = start + size + (size & 1U);
    }
    if (!validFormat || !pcmSize)
        throw std::runtime_error("Speech stream WAV format is not PCM16 mono 22050 Hz");
    std::vector<BYTE> pcm(bytes.begin() + static_cast<std::ptrdiff_t>(pcmStart),
                          bytes.begin() + static_cast<std::ptrdiff_t>(pcmStart + pcmSize));
    bytes.swap(pcm);
}
std::vector<float> GenerateSpeech() {
    ComPtr<IStream> memory;
    Require(CreateStreamOnHGlobal(nullptr, TRUE, &memory), "Create speech memory stream");
    ComPtr<ISpStream> stream;
    Require(CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_ALL,
                             IID_PPV_ARGS(&stream)), "Create SAPI speech stream");

    WAVEFORMATEX pcm{};
    pcm.wFormatTag = WAVE_FORMAT_PCM;
    pcm.nChannels = 1;
    pcm.nSamplesPerSec = kVoiceRate;
    pcm.wBitsPerSample = 16;
    pcm.nBlockAlign = 2;
    pcm.nAvgBytesPerSec = kVoiceRate * 2;
    Require(stream->SetBaseStream(memory.Get(), SPDFID_WaveFormatEx, &pcm),
            "Bind speech output to memory");

    ComPtr<ISpVoice> voice;
    Require(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL,
                             IID_PPV_ARGS(&voice)), "Create Windows SAPI voice");
    // Never allow SAPI to choose the default audio device (currently VB-CABLE
    // in Broadcast mode) or a physical speaker. Synthesize into memory only.
    Require(voice->SetOutput(stream.Get(), FALSE), "Set SAPI output to memory");
    Require(voice->Speak(kSpeech, SPF_DEFAULT, nullptr), "Synthesize sample speech");
    Require(stream->Commit(STGC_DEFAULT), "Flush sample speech");

    STATSTG stat{};
    Require(memory->Stat(&stat, STATFLAG_NONAME), "Read speech byte length");
    if (stat.cbSize.QuadPart <= 0 || stat.cbSize.QuadPart > 8LL * 1024 * 1024)
        throw std::runtime_error("SAPI returned an unexpected speech buffer length");
    std::vector<BYTE> data(static_cast<size_t>(stat.cbSize.QuadPart));
    LARGE_INTEGER zero{};
    Require(memory->Seek(zero, STREAM_SEEK_SET, nullptr), "Rewind speech buffer");
    ULONG received = 0;
    Require(memory->Read(data.data(), static_cast<ULONG>(data.size()), &received),
            "Read speech PCM");
    data.resize(received);
    DecodeWaveIfPresent(data);
    if (data.size() < 4000 || (data.size() % 2) != 0)
        throw std::runtime_error("SAPI generated empty or invalid PCM speech");
    std::vector<float> mono(data.size() / 2);
    for (size_t i = 0; i < mono.size(); ++i) {
        const unsigned raw = ReadLE16(data.data() + i * 2);
        const auto signedSample = static_cast<std::int16_t>(raw);
        mono[i] = static_cast<float>(signedSample) / 32768.0f;
    }
    LogInfo(L"Sample audio: synthesized speech into memory (" +
            std::to_wstring(mono.size()) + L" frames at 22050 Hz).");
    return mono;
}
enum class SampleFormat { Float32, Pcm16 };
SampleFormat GetFormat(const WAVEFORMATEX* wave) {
    if (!wave || wave->nChannels == 0 || wave->nSamplesPerSec == 0)
        throw std::runtime_error("Invalid VB-CABLE mix format");
    if (wave->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && wave->wBitsPerSample == 32)
        return SampleFormat::Float32;
    if (wave->wFormatTag == WAVE_FORMAT_PCM && wave->wBitsPerSample == 16)
        return SampleFormat::Pcm16;
    if (wave->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        wave->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wave);
        if (wave->wBitsPerSample == 32 &&
            extended->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
            return SampleFormat::Float32;
        if (wave->wBitsPerSample == 16 &&
            extended->SubFormat == KSDATAFORMAT_SUBTYPE_PCM)
            return SampleFormat::Pcm16;
    }
    throw std::runtime_error("VB-CABLE mix requires PCM16 or float32 for sample speech");
}
void WriteSample(BYTE* bytes, size_t index, SampleFormat format, float sample) {
    sample = std::clamp(sample, -1.0f, 1.0f);
    if (format == SampleFormat::Float32) {
        std::memcpy(bytes + index * sizeof(float), &sample, sizeof(sample));
    } else {
        const auto pcm = static_cast<std::int16_t>(std::lround(sample * 32767.0f));
        std::memcpy(bytes + index * sizeof(pcm), &pcm, sizeof(pcm));
    }
}
float Interpolate(const std::vector<float>& speech, size_t outputFrame,
                  unsigned outputRate) {
    const double position = static_cast<double>(outputFrame) * kVoiceRate / outputRate;
    const size_t first = static_cast<size_t>(position);
    if (first >= speech.size()) return 0.0f;
    const float a = speech[first];
    const float b = first + 1 < speech.size() ? speech[first + 1] : 0.0f;
    return a + (b - a) * static_cast<float>(position - first);
}
} // namespace

bool PlaySampleAudioToCable(const std::wstring& cableRenderId,
                            std::atomic<bool>& cancel,
                            std::wstring& error) {
    error.clear();
    try {
        if (cableRenderId.empty())
            throw std::runtime_error("No VB-CABLE render endpoint selected");
        ScopedCom com;
        Require(com.result, "Initialize COM on sample thread");

        // SAPI creates the voice in an in-memory stream; it can never use
        // physical speakers or the system's default output for this sample.
        auto speech = GenerateSpeech();
        if (cancel.load()) {
            LogInfo(L"Sample audio cancelled before playback.");
            return true;
        }

        ComPtr<IMMDeviceEnumerator> enumerator;
        Require(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&enumerator)), "Create endpoint enumerator");
        ComPtr<IMMDevice> endpoint;
        Require(enumerator->GetDevice(cableRenderId.c_str(), &endpoint),
                "Find VB-CABLE Input render endpoint");
        ComPtr<IAudioClient> client;
        Require(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                   reinterpret_cast<void**>(client.GetAddressOf())),
                "Activate VB-CABLE render stream");
        WAVEFORMATEX* rawFormat = nullptr;
        Require(client->GetMixFormat(&rawFormat), "Get VB-CABLE mix format");
        const auto freeWave = [](WAVEFORMATEX* p) { if (p) CoTaskMemFree(p); };
        std::unique_ptr<WAVEFORMATEX, decltype(freeWave)> mix(rawFormat, freeWave);
        if (!mix) throw std::runtime_error("VB-CABLE returned no audio mix format");
        const SampleFormat format = GetFormat(mix.get());
        Require(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 300000, 0,
                                   mix.get(), nullptr), "Initialize VB-CABLE render stream");
        UINT32 capacity = 0;
        Require(client->GetBufferSize(&capacity), "Get VB-CABLE render buffer size");
        ComPtr<IAudioRenderClient> render;
        Require(client->GetService(IID_PPV_ARGS(&render)), "Get VB-CABLE render client");
        const unsigned rate = mix->nSamplesPerSec;
        const unsigned channels = mix->nChannels;
        // Leave 150 ms silence at the end so the virtual mic/Discord can
        // finish draining the last spoken syllable.
        const size_t totalFrames = static_cast<size_t>(
            std::ceil(static_cast<double>(speech.size()) * rate / kVoiceRate)) +
            rate * 150 / 1000;
        LogInfo(L"Sample audio: sending spoken sample DIRECTLY to VB-CABLE Input (" +
                std::to_wstring(rate) + L" Hz / " + std::to_wstring(channels) +
                L" channels); no physical-speaker stream opened.");
        size_t submitted = 0;
        bool started = false;
        while (submitted < totalFrames && !cancel.load()) {
            UINT32 padding = 0;
            Require(client->GetCurrentPadding(&padding), "Read render padding");
            if (padding > capacity)
                throw std::runtime_error("Unexpected VB-CABLE buffer padding");
            const UINT32 freeFrames = capacity - padding;
            const UINT32 frames = static_cast<UINT32>(
                std::min(static_cast<size_t>(freeFrames), totalFrames - submitted));
            if (frames > 0) {
                BYTE* data = nullptr;
                Require(render->GetBuffer(frames, &data), "Get VB-CABLE render buffer");
                for (UINT32 i = 0; i < frames; ++i) {
                    const float sample = Interpolate(speech, submitted + i, rate);
                    for (unsigned ch = 0; ch < channels; ++ch)
                        WriteSample(data, (static_cast<size_t>(i) * channels) + ch,
                                    format, sample);
                }
                Require(render->ReleaseBuffer(frames, 0), "Release VB-CABLE render buffer");
                submitted += frames;
                if (!started) {
                    Require(client->Start(), "Start VB-CABLE sample playback");
                    started = true;
                }
            }
            Sleep(5);
        }
        if (started && !cancel.load()) {
            // Wait for the last queued frames to be consumed before reporting
            // completion. This is a capture-device send, not proof of receipt.
            for (int tries = 0; tries < 200 && !cancel.load(); ++tries) {
                UINT32 padding = 0;
                Require(client->GetCurrentPadding(&padding), "Drain VB-CABLE sample");
                if (!padding) break;
                Sleep(10);
            }
        }
        if (started) client->Stop();
        if (cancel.load()) LogInfo(L"Sample audio: playback cancelled.");
        else LogInfo(L"Sample audio: sample delivered to VB-CABLE Input. "
                     L"Discord reception depends on its selected input device.");
        return true;
    } catch (const std::exception& problem) {
        const std::string message(problem.what());
        error.assign(message.begin(), message.end()); // ASCII Win32 labels/error codes
        LogError(L"Sample audio playback failed: " + error);
        return false;
    }
}
} // namespace audioptt
