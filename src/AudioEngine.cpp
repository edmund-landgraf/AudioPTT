#include "AudioEngine.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <deque>
#include <future>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace audioptt {
namespace {
using Frame = std::array<float, 2>;
struct ComScope {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComScope() { if (SUCCEEDED(hr)) CoUninitialize(); }
};
void Require(HRESULT hr, const wchar_t* operation) {
    if (FAILED(hr)) {
        wchar_t message[256]{};
        swprintf_s(message, L"%ls failed (HRESULT 0x%08X)", operation,
                   static_cast<unsigned>(hr));
        std::wstring wide(message);
        throw std::runtime_error(std::string(wide.begin(), wide.end()));
    }
}
std::wstring Name(IMMDevice* device) {
    ComPtr<IPropertyStore> properties;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &properties))) return L"";
    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring name;
    if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) &&
        value.vt == VT_LPWSTR && value.pwszVal) name = value.pwszVal;
    PropVariantClear(&value);
    return name;
}
bool IsCable(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), towupper);
    return value.find(L"CABLE") != std::wstring::npos;
}
ComPtr<IMMDevice> FindCableRender(IMMDeviceEnumerator* enumerator) {
    ComPtr<IMMDeviceCollection> list;
    Require(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &list), L"enumerate playback devices");
    UINT count = 0;
    Require(list->GetCount(&count), L"count playback devices");
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (SUCCEEDED(list->Item(i, &device))) {
            const auto name = Name(device);
            // VB-CABLE playback endpoint is named CABLE Input.
            std::wstring upper = name;
            std::transform(upper.begin(), upper.end(), upper.begin(), towupper);
            if (upper.find(L"CABLE INPUT") != std::wstring::npos) return device;
        }
    }
    throw std::runtime_error("VB-CABLE Input playback endpoint was not found");
}
ComPtr<IMMDevice> FindMic(IMMDeviceEnumerator* enumerator) {
    ComPtr<IMMDevice> mic;
    Require(enumerator->GetDefaultAudioEndpoint(eCapture, eCommunications, &mic), L"find default microphone");
    if (IsCable(Name(mic.Get()))) throw std::runtime_error("Default communications microphone is VB-CABLE, not a physical microphone");
    return mic;
}
ComPtr<IMMDevice> FindSpeakers(IMMDeviceEnumerator* enumerator) {
    ComPtr<IMMDevice> speakers;
    Require(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &speakers), L"find default speakers");
    if (IsCable(Name(speakers.Get()))) throw std::runtime_error("Default Windows output is VB-CABLE. Select physical speakers first to avoid a feedback loop");
    return speakers;
}
enum class SampleType { Float32, Pcm16 };
SampleType SampleFormat(const WAVEFORMATEX* format) {
    if (!format || format->nChannels == 0 || format->nSamplesPerSec == 0) 
        throw std::runtime_error("Invalid Windows audio format");
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && format->wBitsPerSample == 32)
        return SampleType::Float32;
    if (format->wFormatTag == WAVE_FORMAT_PCM && format->wBitsPerSample == 16)
        return SampleType::Pcm16;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        auto* ex = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        if (ex->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT && format->wBitsPerSample == 32)
            return SampleType::Float32;
        if (ex->SubFormat == KSDATAFORMAT_SUBTYPE_PCM && format->wBitsPerSample == 16)
            return SampleType::Pcm16;
    }
    throw std::runtime_error("Unsupported audio mix format (expected 32-bit float or 16-bit PCM)");
}
float ReadSample(const BYTE* bytes, size_t offset, SampleType type) {
    if (type == SampleType::Float32) {
        float value;
        std::memcpy(&value, bytes + offset * sizeof(float), sizeof(float));
        return value;
    }
    int16_t value;
    std::memcpy(&value, bytes + offset * sizeof(int16_t), sizeof(int16_t));
    return static_cast<float>(value) / 32768.0f;
}
void WriteSample(BYTE* bytes, size_t offset, SampleType type, float value) {
    value = std::clamp(value, -1.0f, 1.0f);
    if (type == SampleType::Float32) std::memcpy(bytes + offset * sizeof(float), &value, sizeof(float));
    else {
        const auto pcm = static_cast<int16_t>(std::lround(value * 32767.0f));
        std::memcpy(bytes + offset * sizeof(int16_t), &pcm, sizeof(pcm));
    }
}
struct Stream {
    ComPtr<IAudioClient> client;
    WAVEFORMATEX* format = nullptr;
    SampleType type{};
    UINT32 capacity = 0;
    ~Stream() { if (format) CoTaskMemFree(format); }
    Stream() = default;
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;
    void Open(IMMDevice* device, bool loopback) {
        Require(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                 reinterpret_cast<void**>(client.GetAddressOf())), L"activate endpoint");
        Require(client->GetMixFormat(&format), L"get mix format");
        type = SampleFormat(format);
        const DWORD flags = loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
        Require(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 300000, 0, format, nullptr),
                L"initialize audio stream");
        Require(client->GetBufferSize(&capacity), L"get stream buffer size");
    }
};
void Forward(Stream& input, IAudioCaptureClient* capture,
             Stream& output, IAudioRenderClient* render,
             std::atomic<bool>& stop) {
    std::deque<Frame> samples;
    double phase = 0;
    const double step = static_cast<double>(input.format->nSamplesPerSec) / output.format->nSamplesPerSec;
    while (!stop.load()) {
        UINT32 packet = 0;
        Require(capture->GetNextPacketSize(&packet), L"capture packet size");
        while (packet > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            Require(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr), L"capture buffer");
            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            for (UINT32 i = 0; i < frames; ++i) {
                Frame frame{0.0f, 0.0f};
                if (!silent) {
                    const auto channels = input.format->nChannels;
                    frame[0] = ReadSample(data, static_cast<size_t>(i) * channels, input.type);
                    frame[1] = channels > 1
                        ? ReadSample(data, static_cast<size_t>(i) * channels + 1, input.type)
                        : frame[0];
                }
                samples.push_back(frame);
            }
            Require(capture->ReleaseBuffer(frames), L"release capture buffer");
            Require(capture->GetNextPacketSize(&packet), L"next capture packet");
        }
        // Drop old samples rather than permitting unlimited latency after a stall.
        const auto maxFrames = static_cast<size_t>(input.format->nSamplesPerSec / 2);
        if (samples.size() > maxFrames) {
            samples.erase(samples.begin(), samples.end() - static_cast<std::ptrdiff_t>(maxFrames));
            phase = 0;
        }
        UINT32 padding = 0;
        Require(output.client->GetCurrentPadding(&padding), L"render padding");
        UINT32 available = output.capacity - padding;
        if (available) {
            BYTE* data = nullptr;
            Require(render->GetBuffer(available, &data), L"render buffer");
            for (UINT32 i = 0; i < available; ++i) {
                Frame frame{0.0f, 0.0f};
                if (samples.size() >= 2) {
                    const auto& a = samples[0];
                    const auto& b = samples[1];
                    frame[0] = static_cast<float>(a[0] + (b[0] - a[0]) * phase);
                    frame[1] = static_cast<float>(a[1] + (b[1] - a[1]) * phase);
                    phase += step;
                    while (phase >= 1.0 && samples.size() > 1) {
                        samples.pop_front();
                        phase -= 1.0;
                    }
                }
                for (UINT32 channel = 0; channel < output.format->nChannels; ++channel) {
                    const float value = output.format->nChannels == 1
                        ? (frame[0] + frame[1]) * 0.5f : frame[channel % 2];
                    WriteSample(data, static_cast<size_t>(i) * output.format->nChannels + channel,
                                output.type, value);
                }
            }
            Require(render->ReleaseBuffer(available, 0), L"release render buffer");
        }
        Sleep(5);
    }
    input.client->Stop();
    output.client->Stop();
}
} // namespace

AudioEngine::~AudioEngine() { Stop(); }

std::wstring AudioEngine::Status() const {
    std::lock_guard lock(statusMutex_);
    return status_;
}

void AudioEngine::ShutdownWorker() {
    stop_.store(true);
    if (worker_.joinable()) worker_.join();
    running_.store(false);
}

bool AudioEngine::LaunchWorker(bool broadcast, std::wstring& error) {
    ShutdownWorker();
    stop_.store(false);
    std::promise<std::wstring> startup;
    auto future = startup.get_future();
    worker_ = std::thread(&AudioEngine::Worker, this, broadcast, monitor_.load(), std::move(startup));
    error = future.get();
    if (!error.empty()) {
        ShutdownWorker();
        return false;
    }
    return true;
}

bool AudioEngine::Start(std::wstring& error) {
    if (routeCaptured_) { error.clear(); return true; }
    if (!CaptureAudioRoute(route_, error)) {
        std::lock_guard lock(statusMutex_);
        status_ = error;
        return false;
    }
    routeCaptured_ = true;
    if (!LaunchWorker(false, error)) {
        routeCaptured_ = false;
        return false;
    }
    broadcasting_.store(false);
    return true;
}

bool AudioEngine::SetBroadcast(bool enabled, std::wstring& error) {
    if (!routeCaptured_) {
        error = L"AudioPTT is not ready. Confirm the audio devices and restart the app.";
        return false;
    }
    if (enabled == broadcasting_.load() && running_.load()) {
        error.clear();
        return true;
    }
    ShutdownWorker();  // Never mix microphone samples with live desktop audio.

    if (enabled) {
        if (!RouteDesktopToCable(route_, error)) {
            std::wstring ignored;
            LaunchWorker(false, ignored);
            return false;
        }
        if (LaunchWorker(true, error)) {
            broadcasting_.store(true);
            return true;
        }
        std::wstring restoreError;
        RestoreDesktopRoute(route_, restoreError);
        std::wstring ignored;
        LaunchWorker(false, ignored);
        broadcasting_.store(false);
        if (!restoreError.empty()) error += L" Also could not restore default playback: " + restoreError;
        return false;
    }

    if (!RestoreDesktopRoute(route_, error)) {
        std::wstring ignored;
        LaunchWorker(true, ignored);
        // Keep broadcasting=true if routing is still through VB-CABLE.
        return false;
    }
    if (!LaunchWorker(false, error)) {
        broadcasting_.store(false);
        return false;
    }
    broadcasting_.store(false);
    return true;
}

bool AudioEngine::SetMonitor(bool enabled, std::wstring& error) {
    if (enabled == monitor_.load()) { error.clear(); return true; }
    const bool previous = monitor_.load();
    monitor_.store(enabled);
    if (broadcasting_.load() && routeCaptured_) {
        if (!LaunchWorker(true, error)) {
            monitor_.store(previous);
            std::wstring ignored;
            LaunchWorker(true, ignored);
            return false;
        }
    }
    error.clear();
    return true;
}

void AudioEngine::Stop() {
    ShutdownWorker();
    if (routeCaptured_) {
        std::wstring error;
        if (!RestoreDesktopRoute(route_, error) && !error.empty()) {
            std::lock_guard lock(statusMutex_);
            status_ = L"WARNING: Windows playback was not restored: " + error;
        }
        routeCaptured_ = false;
    }
    broadcasting_.store(false);
}

void AudioEngine::Worker(bool broadcast, bool monitor, std::promise<std::wstring> startup) {
    bool signaled = false;
    try {
        ComScope com;
        if (FAILED(com.hr)) throw std::runtime_error("COM initialization failed");

        // Windows itself forwards desktop audio into CABLE Input when broadcasting.
        // Without monitoring, AudioPTT deliberately opens NO local render stream.
        if (broadcast && !monitor) {
            { std::lock_guard lock(statusMutex_);
              status_ = L"PC audio -> Discord (silent locally)"; }
            running_.store(true);
            startup.set_value(L"");
            signaled = true;
            while (!stop_.load()) Sleep(25);
            running_.store(false);
            return;
        }

        ComPtr<IMMDeviceEnumerator> enumerator;
        Require(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&enumerator)), L"create enumerator");
        ComPtr<IMMDevice> cable, speaker, mic;
        Require(enumerator->GetDevice(route_.cableRenderId.c_str(), &cable), L"get VB-CABLE endpoint");

        // OFF: mic capture -> cable playback (virtual mic in Discord).
        // ON + Monitor: cable playback loopback -> physical speaker output.
        Stream input, output;
        if (broadcast) {
            Require(enumerator->GetDevice(route_.physicalOutputId.c_str(), &speaker),
                    L"get physical speaker endpoint");
            input.Open(cable.Get(), true);
            output.Open(speaker.Get(), false);
        } else {
            mic = FindMic(enumerator.Get());
            input.Open(mic.Get(), false);
            output.Open(cable.Get(), false);
        }
        ComPtr<IAudioCaptureClient> capture;
        ComPtr<IAudioRenderClient> render;
        Require(input.client->GetService(IID_PPV_ARGS(&capture)), L"get capture service");
        Require(output.client->GetService(IID_PPV_ARGS(&render)), L"get render service");

        Require(output.client->Start(), L"start render stream");
        Require(input.client->Start(), L"start capture stream");
        { std::lock_guard lock(statusMutex_);
          status_ = broadcast ? L"PC audio -> Discord + speakers" : L"Microphone -> Discord"; }
        running_.store(true);
        startup.set_value(L"");
        signaled = true;
        Forward(input, capture.Get(), output, render.Get(), stop_);
        running_.store(false);
    } catch (const std::exception& ex) {
        std::wstring message = L"Audio engine failed: ";
        const std::string detail = ex.what();
        message.append(detail.begin(), detail.end());
        { std::lock_guard lock(statusMutex_); status_ = message; }
        running_.store(false);
        if (!signaled) startup.set_value(message);
    }
}
} // namespace audioptt
