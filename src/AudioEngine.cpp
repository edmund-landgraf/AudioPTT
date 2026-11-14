#include "AudioEngine.h"
#include "AudioRouteStore.h"
#include "Diagnostics.h"
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
    // WASAPI initialization is safest on an STA, including its first use.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComScope() { if (SUCCEEDED(hr)) CoUninitialize(); }
};
// Windows APIs use UTF-16; std::runtime_error uses a narrow string.
// Convert explicitly instead of narrowing wchar_t to char (MSVC C4244).
std::string ToUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                        wide.data(), static_cast<int>(wide.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (size <= 0) return "Windows audio error (invalid UTF-16)";
    std::string utf8(static_cast<size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                           wide.data(), static_cast<int>(wide.size()),
                           utf8.data(), size, nullptr, nullptr) != size)
        return "Windows audio error (UTF-8 conversion failed)";
    return utf8;
}
std::wstring FromUtf8(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        utf8.data(), static_cast<int>(utf8.size()),
                                        nullptr, 0);
    if (size <= 0) return L"Error detail unavailable (invalid UTF-8)";
    std::wstring wide(static_cast<size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                            utf8.data(), static_cast<int>(utf8.size()),
                            wide.data(), size) != size)
        return L"Error detail unavailable (UTF-16 conversion failed)";
    return wide;
}
void Require(HRESULT hr, const wchar_t* operation) {
    if (FAILED(hr)) {
        wchar_t message[256]{};
        swprintf_s(message, L"%ls failed (HRESULT 0x%08X)", operation,
                   static_cast<unsigned>(hr));
        throw std::runtime_error(ToUtf8(message));
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
ComPtr<IMMDevice> FindMic(IMMDeviceEnumerator* enumerator) {
    ComPtr<IMMDevice> mic;
    Require(enumerator->GetDefaultAudioEndpoint(eCapture, eCommunications, &mic), L"find default microphone");
    if (IsCable(Name(mic.Get()))) throw std::runtime_error("Default communications microphone is VB-CABLE, not a physical microphone");
    return mic;
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
    if (!std::isfinite(value)) value = 0.0f;
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
             std::atomic<bool>& stop, bool microphoneMode) {
    std::deque<Frame> samples;
    double phase = 0;
    const double step = static_cast<double>(input.format->nSamplesPerSec) / output.format->nSamplesPerSec;
    std::uint64_t capturedFrames = 0, silentFrames = 0, renderedFrames = 0;
    double inSquares = 0.0, outSquares = 0.0;
    double inPeak = 0.0, outPeak = 0.0;
    size_t maxQueuedFrames = 0;
    auto lastReport = std::chrono::steady_clock::now();
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
                const double mono = (static_cast<double>(frame[0]) + frame[1]) * 0.5;
                inSquares += mono * mono;
                inPeak = std::max(inPeak, std::abs(mono));
            }
            capturedFrames += frames;
            if (silent) silentFrames += frames;
            maxQueuedFrames = std::max(maxQueuedFrames, samples.size());
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
                const double mono = (static_cast<double>(frame[0]) + frame[1]) * 0.5;
                outSquares += mono * mono;
                outPeak = std::max(outPeak, std::abs(mono));
            }
            renderedFrames += available;
            Require(render->ReleaseBuffer(available, 0), L"release render buffer");
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - lastReport >= std::chrono::seconds(5)) {
            const std::wstring from = microphoneMode ? L"WEBCAM MIC" : L"CABLE LOOPBACK";
            const std::wstring to = microphoneMode ? L"CABLE INPUT" : L"REALTEK SPEAKERS";
            LogInfo(L"STREAM LEVELS [" + from + L" -> " + to + L"] / 5 s: "
                    L"captured=" + std::to_wstring(capturedFrames) +
                    L" frames, silentFlags=" + std::to_wstring(silentFrames) +
                    L", inputRMS=" + std::to_wstring(capturedFrames ?
                        std::sqrt(inSquares / capturedFrames) : 0.0) +
                    L", inputPeak=" + std::to_wstring(inPeak) +
                    L", submitted=" + std::to_wstring(renderedFrames) +
                    L" frames, renderRMS=" + std::to_wstring(renderedFrames ?
                        std::sqrt(outSquares / renderedFrames) : 0.0) +
                    L", renderPeak=" + std::to_wstring(outPeak) +
                    L", peakQueued=" + std::to_wstring(maxQueuedFrames) + L" frames");
            if (microphoneMode && inPeak < 0.001)
                LogInfo(L"STREAM MIC: very low captured input in this window. "
                        L"This may be silence, muted microphone, or the wrong input endpoint.");
            capturedFrames = silentFrames = renderedFrames = 0;
            inSquares = outSquares = inPeak = outPeak = 0.0;
            maxQueuedFrames = 0;
            lastReport = now;
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
    LogInfo(broadcast ? L"Opening WASAPI desktop monitor worker." :
                        L"Opening WASAPI microphone forwarding worker.");
    stop_.store(false);
    std::promise<std::wstring> startup;
    auto future = startup.get_future();
    worker_ = std::thread(&AudioEngine::Worker, this, broadcast, monitor_.load(), std::move(startup));
    error = future.get();
    if (!error.empty()) {
        LogError(L"WASAPI worker failed to start: " + error);
        ShutdownWorker();
        return false;
    }
    LogInfo(L"WASAPI worker started successfully.");
    return true;
}

bool AudioEngine::Start(std::wstring& error) {
    if (routeCaptured_) {
        if (running_.load()) { error.clear(); return true; }
        LogWarning(L"Audio worker stopped unexpectedly. Attempting restart.");
        return LaunchWorker(broadcasting_.load(), error);
    }
    LogInfo(L"Validating physical microphone, playback defaults, and VB-CABLE endpoints.");
    if (!CaptureAudioRoute(route_, error)) {
        std::lock_guard lock(statusMutex_);
        status_ = error;
        return false;
    }
    routeCaptured_ = true;
    if (!LaunchWorker(false, error)) {
        routeCaptured_ = false;
        LogError(L"Microphone forwarding failed; inspect microphone permissions and device format: " + error);
        return false;
    }
    broadcasting_.store(false);
    healthAlerted_ = false;
    // If a user already restored physical playback after a previous crash,
    // a stale recovery snapshot no longer applies.
    ClearRecoveryRoute();
    return true;
}

bool AudioEngine::SetBroadcast(bool enabled, std::wstring& error) {
    if (!routeCaptured_) {
        error = L"AudioPTT is not ready. Check the devices and restart the app.";
        return false;
    }
    if (enabled == broadcasting_.load() && running_.load()) {
        error.clear();
        return true;
    }

    ShutdownWorker(); // Never mix live microphone forwarding into desktop broadcast.
    if (enabled) {
        LogInfo(L"Broadcast transition: stop microphone forwarding; save physical playback defaults; "
                L"route PC audio into CABLE Input. Discord remains on CABLE Output.");
        // Persist BEFORE changing any system default so a crash can be recovered.
        if (!SaveRecoveryRoute(route_, error)) {
            LogError(L"Broadcast blocked: cannot persist recovery snapshot: " + error);
            std::wstring ignored;
            LaunchWorker(false, ignored);
            return false;
        }
        if (!RouteDesktopToCable(route_, error)) {
            LogError(L"Broadcast blocked: failed to switch Windows playback defaults: " + error);
            // A partially failed playback switch may leave PC output on
            // the cable. Never start microphone forwarding in that state.
            const bool cableActive = IsAudioRoutedToCable(route_);
            broadcasting_.store(cableActive);
            if (!cableActive) {
                ClearRecoveryRoute();
                std::wstring ignored;
                LaunchWorker(false, ignored);
            } else {
                healthAlerted_ = false; // Health check will attempt restoration.
                error += L" Windows playback may still point to VB-CABLE.";
            }
            return false;
        }
        if (LaunchWorker(true, error)) {
            broadcasting_.store(true);
            healthAlerted_ = false;
            LogInfo(monitor_.load() ?
                L"Broadcast active: PC audio to Discord AND local speakers." :
                L"Broadcast active: PC audio to Discord, no local monitor stream.");
            return true;
        }
        std::wstring restoreError;
        const bool restored = RestoreDesktopRoute(route_, restoreError);
        if (restored) {
            ClearRecoveryRoute();
            broadcasting_.store(false);
            std::wstring ignored;
            LaunchWorker(false, ignored);
        } else {
            broadcasting_.store(IsAudioRoutedToCable(route_));
            error += L" CRITICAL: physical playback defaults could not be restored: " + restoreError;
        }
        return false;
    }

    LogInfo(L"Broadcast OFF requested: restore physical playback defaults, then reopen "
            L"webcam capture and forward only to VB-CABLE for Discord.");
    if (!RestoreDesktopRoute(route_, error)) {
        const bool stillCable = IsAudioRoutedToCable(route_);
        broadcasting_.store(stillCable);
        // Leave all AudioPTT streams STOPPED if restoration was partial.
        // In particular, never restart CABLE-to-speaker monitoring while
        // Windows input/output defaults are in an uncertain state.
        if (stillCable) {
            LogError(L"Windows still has a VB-CABLE default after STOP. "
                     L"Local monitor will remain stopped until recovery succeeds.");
            healthAlerted_ = false; // Periodic health check may retry restore.
        } else {
            std::wstring ignored;
            LaunchWorker(false, ignored);
        }
        return false;
    }
    ClearRecoveryRoute();
    broadcasting_.store(false); // Reflect actual output even if mic worker fails.
    healthAlerted_ = false;
    return LaunchWorker(false, error);
}

bool AudioEngine::SetMonitor(bool enabled, std::wstring& error) {
    if (enabled == monitor_.load()) { error.clear(); return true; }
    const bool previous = monitor_.load();
    LogInfo(enabled ? L"Monitor ON: requesting physical-speaker copy." :
                      L"Monitor OFF: stop local-speaker copy; keep Discord audio feed.");
    monitor_.store(enabled);
    if (broadcasting_.load() && routeCaptured_) {
        if (!LaunchWorker(true, error)) {
            LogError(L"Cannot change monitor branch: " + error);
            monitor_.store(previous);
            std::wstring ignored;
            LaunchWorker(true, ignored);
            return false;
        }
    }
    error.clear();
    return true;
}

bool AudioEngine::CheckHealth(std::wstring& alert) {
    alert.clear();
    if (!routeCaptured_ || !broadcasting_.load() || healthAlerted_) return true;
    if (running_.load() && IsDesktopBroadcastRouteActive(route_)) return true;

    healthAlerted_ = true;
    LogError(L"Audio health check detected a stopped worker or incorrect Windows output.");
    ShutdownWorker();
    std::wstring restoreError;
    const bool restored = RestoreDesktopRoute(route_, restoreError);
    if (restored) {
        ClearRecoveryRoute();
        broadcasting_.store(false);
        std::wstring idleError;
        const bool idleReady = LaunchWorker(false, idleError);
        alert = L"Broadcast routing stopped unexpectedly. Windows microphone and speaker defaults restored.";
        if (!idleReady) alert += L" Idle setup also failed: " + idleError;
    } else {
        alert = L"CRITICAL: Desktop broadcast failed and microphone/playback defaults could "
                L"not be restored. Set your physical microphone and speakers in Windows Sound settings. "
                + restoreError;
    }
    { std::lock_guard lock(statusMutex_); status_ = alert; }
    return false;
}

void AudioEngine::Stop() {
    LogInfo(L"Shutting down AudioPTT, stopping microphone forwarding and restoring playback defaults.");
    ShutdownWorker();
    if (routeCaptured_) {
        std::wstring error;
        if (RestoreDesktopRoute(route_, error)) {
            ClearRecoveryRoute();
        } else {
            std::lock_guard lock(statusMutex_);
            status_ = L"WARNING: Windows playback defaults were not restored: " + error;
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
            LogInfo(L"Silent broadcast path armed: Windows apps output directly to CABLE Input; no speaker monitoring.");
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
        Require(enumerator->GetDevice(route_.cableRenderId.c_str(), &cable),
                L"get VB-CABLE endpoint");
        Stream input, output;
        if (broadcast) {
            Require(enumerator->GetDevice(route_.physicalOutputId.c_str(), &speaker),
                    L"get physical speaker endpoint");
            LogInfo(L"Opening CABLE Input loopback capture and physical-speaker monitor. "
                    L"Physical microphone is NOT captured in Broadcast.");
            input.Open(cable.Get(), true);
            output.Open(speaker.Get(), false);
        } else {
            mic = FindMic(enumerator.Get());
            LogInfo(L"Opening webcam microphone capture and VB-CABLE rendering. "
                    L"No physical-speaker stream is opened.");
            input.Open(mic.Get(), false);
            output.Open(cable.Get(), false);
        }
        ComPtr<IAudioCaptureClient> capture;
        ComPtr<IAudioRenderClient> render;
        Require(input.client->GetService(IID_PPV_ARGS(&capture)), L"get capture service");
        Require(output.client->GetService(IID_PPV_ARGS(&render)), L"get render service");

        Require(output.client->Start(), L"start render stream");
        Require(input.client->Start(), L"start capture stream");
        LogInfo(L"WASAPI capture active: " + std::to_wstring(input.format->nSamplesPerSec) +
                L" Hz / " + std::to_wstring(input.format->nChannels) + L" ch; render: " +
                std::to_wstring(output.format->nSamplesPerSec) + L" Hz / " +
                std::to_wstring(output.format->nChannels) + L" ch.");
        { std::lock_guard lock(statusMutex_);
          status_ = broadcast ? L"PC audio -> Discord + speakers" :
                                L"Webcam microphone -> VB-CABLE -> Discord"; }
        running_.store(true);
        startup.set_value(L"");
        signaled = true;
        Forward(input, capture.Get(), output, render.Get(), stop_, !broadcast);
        running_.store(false);
    } catch (const std::exception& ex) {
        std::wstring message = L"Audio engine failed: ";
        message += FromUtf8(ex.what());
        { std::lock_guard lock(statusMutex_); status_ = message; }
        LogError(message);
        running_.store(false);
        if (!signaled) startup.set_value(message);
    }
}
} // namespace audioptt
