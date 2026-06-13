#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <windowsx.h>
#include <objbase.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <gdiplus.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfplay.h>
#include <mfreadwrite.h>
#include <mmdeviceapi.h>
#include <mmsystem.h>
#include <propvarutil.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "resource.h"

using namespace Gdiplus;
namespace fs = std::filesystem;

constexpr UINT_PTR kUiTimer = 1;
constexpr int kTopHeight = 132;
constexpr int kBrowserHeight = 360;
constexpr int kBottomHeight = 86;
constexpr int kLeftBrowserWidth = 500;
constexpr int kWaveMargin = 14;
constexpr int kSplitterSize = 8;
constexpr int kStartupClientWidth = 1280;
constexpr int kStartupClientHeight = 820;
constexpr double kRmsFloorDb = -60.0;
constexpr double kSmoothScrollFrameMs = 4.0;
constexpr ULONGLONG kPlaybackUiIntervalMs = 33;
constexpr ULONGLONG kMediaPositionQueryIntervalMs = 250;
constexpr ULONGLONG kSeekFadeOutMs = 18;
constexpr ULONGLONG kSeekFadeInMs = 45;
constexpr DWORD kAudioStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
constexpr int kAudioOutputComboId = 1004;
constexpr int kWaveformPeakTarget = 3000;
constexpr int kWaveformRmsTarget = 12000;
constexpr wchar_t kSettingsRegistryPath[] = L"Software\\AudioWavePlayer";
constexpr wchar_t kAudioOutputValueName[] = L"AudioOutputEndpointId";
constexpr wchar_t kWindowClassName[] = L"AudioWavePlayer.Native";
constexpr wchar_t kSingleInstanceMutexName[] = L"Local\\AudioWavePlayer.SingleInstance";
constexpr ULONG_PTR kCopyDataOpenFile = 1;
std::atomic<int> gWaveformWorkers = 0;

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

enum class PlaybackOrder { Sequential, RepeatOne, Shuffle };
enum class DragMode { None, Seek, Volume, HorizontalSplitter, VerticalSplitter };
enum class SeekFadePhase { None, FadingOut, FadingIn };

template <class T>
void SafeRelease(T** value)
{
    if (*value)
    {
        (*value)->Release();
        *value = nullptr;
    }
}

std::wstring LowerExt(const std::wstring& path)
{
    auto ext = fs::path(path).extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    return ext;
}

bool IsSupportedAudio(const std::wstring& path)
{
    const auto ext = LowerExt(path);
    return ext == L".wav" || ext == L".mp3" || ext == L".m4a" || ext == L".mp4" ||
           ext == L".aac" || ext == L".wma" || ext == L".flac";
}

bool IsCommandSwitch(const std::wstring& value)
{
    return value.size() > 1 && (value[0] == L'-' || value[0] == L'/');
}

std::wstring FindStartupAudioFile(PWSTR* argv, int argc)
{
    if (!argv || argc <= 1)
        return {};

    std::wstring firstSupported;
    for (int i = 1; i < argc; ++i)
    {
        std::wstring value = argv[i] ? argv[i] : L"";
        if (value.empty() || IsCommandSwitch(value))
            continue;

        std::wstring candidate;
        for (int j = i; j < argc; ++j)
        {
            if (argv[j] && *argv[j])
            {
                if (!candidate.empty())
                    candidate += L' ';
                candidate += argv[j];
            }

            if (!IsSupportedAudio(candidate))
                continue;

            std::error_code ec;
            if (fs::exists(candidate, ec))
                return candidate;
            if (firstSupported.empty())
                firstSupported = candidate;
        }
    }

    return firstSupported;
}

HWND FindExistingPlayerWindow()
{
    for (int i = 0; i < 30; ++i)
    {
        HWND hwnd = FindWindowW(kWindowClassName, nullptr);
        if (hwnd)
            return hwnd;
        Sleep(100);
    }
    return nullptr;
}

void ActivateExistingPlayer(HWND hwnd)
{
    if (!hwnd)
        return;
    if (IsIconic(hwnd))
        ShowWindow(hwnd, SW_RESTORE);
    else
        ShowWindow(hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);
}

bool SendFileToExistingPlayer(const std::wstring& path)
{
    HWND hwnd = FindExistingPlayerWindow();
    if (!hwnd)
        return false;

    if (!path.empty())
    {
        COPYDATASTRUCT copyData{};
        copyData.dwData = kCopyDataOpenFile;
        copyData.cbData = (DWORD)((path.size() + 1) * sizeof(wchar_t));
        copyData.lpData = const_cast<wchar_t*>(path.c_str());
        SendMessage(hwnd, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&copyData));
    }

    ActivateExistingPlayer(hwnd);
    return true;
}

std::wstring FileNameOf(const std::wstring& path)
{
    return fs::path(path).filename().wstring();
}

bool SameText(const std::wstring& a, const std::wstring& b)
{
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

std::wstring LoadSavedAudioOutputId()
{
    DWORD bytes = 0;
    const LSTATUS sizeStatus = RegGetValueW(
        HKEY_CURRENT_USER,
        kSettingsRegistryPath,
        kAudioOutputValueName,
        RRF_RT_REG_SZ,
        nullptr,
        nullptr,
        &bytes);
    if (sizeStatus != ERROR_SUCCESS || bytes <= sizeof(wchar_t))
        return L"";

    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    const LSTATUS readStatus = RegGetValueW(
        HKEY_CURRENT_USER,
        kSettingsRegistryPath,
        kAudioOutputValueName,
        RRF_RT_REG_SZ,
        nullptr,
        value.data(),
        &bytes);
    if (readStatus != ERROR_SUCCESS)
        return L"";
    if (!value.empty() && value.back() == L'\0')
        value.pop_back();
    return value;
}

void SaveAudioOutputId(const std::wstring& endpointId)
{
    RegSetKeyValueW(
        HKEY_CURRENT_USER,
        kSettingsRegistryPath,
        kAudioOutputValueName,
        REG_SZ,
        endpointId.c_str(),
        (DWORD)((endpointId.size() + 1) * sizeof(wchar_t)));
}

std::wstring FormatTime(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0)
        seconds = 0;
    const int totalMs = (int)std::llround(seconds * 1000.0);
    const int ms = totalMs % 1000;
    const int totalSec = totalMs / 1000;
    return std::format(L"{}:{:02}.{:03}", totalSec / 60, totalSec % 60, ms);
}

std::wstring FormatTrackLength(double seconds)
{
    if (!std::isfinite(seconds) || seconds <= 0)
        return L"";

    const int totalSec = (int)std::llround(seconds);
    const int hours = totalSec / 3600;
    const int minutes = (totalSec / 60) % 60;
    const int sec = totalSec % 60;
    return hours > 0
        ? std::format(L"{}:{:02}:{:02}", hours, minutes, sec)
        : std::format(L"{}:{:02}", minutes, sec);
}

std::wstring FormatSize(uintmax_t bytes)
{
    const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB" };
    double value = (double)bytes;
    int unit = 0;
    while (value >= 1024.0 && unit < 3)
    {
        value /= 1024.0;
        ++unit;
    }
    return unit == 0 ? std::format(L"{} {}", (uintmax_t)value, units[unit])
                     : std::format(L"{:.1f} {}", value, units[unit]);
}

std::wstring FormatHResult(HRESULT hr)
{
    return std::format(L"0x{:08X}", (unsigned long)hr);
}

std::wstring FormatFileTime(const fs::file_time_type& value)
{
    try
    {
        const auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            value - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        const std::time_t time = std::chrono::system_clock::to_time_t(systemTime);
        std::tm local{};
        localtime_s(&local, &time);
        return std::format(L"{:02}.{:02}.{} {:02}:{:02}",
            local.tm_mday,
            local.tm_mon + 1,
            local.tm_year + 1900,
            local.tm_hour,
            local.tm_min);
    }
    catch (...)
    {
        return L"";
    }
}

std::wstring FormatDb(double db)
{
    return db <= kRmsFloorDb ? L"-inf" : std::format(L"{:.1f}dB", db);
}

bool IsHiddenOrSystemPath(const fs::path& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        ((attributes & FILE_ATTRIBUTE_HIDDEN) || (attributes & FILE_ATTRIBUTE_SYSTEM));
}

enum class PreferredAppMode
{
    Default,
    AllowDark,
    ForceDark,
    ForceLight,
    Max
};

using SetPreferredAppModeFn = PreferredAppMode(WINAPI*)(PreferredAppMode);
using AllowDarkModeForWindowFn = BOOL(WINAPI*)(HWND, BOOL);

void EnableImmersiveControlDarkMode(HWND hwnd, bool enabled)
{
    static HMODULE uxtheme = LoadLibraryW(L"uxtheme.dll");
    static auto setPreferredAppMode = uxtheme
        ? reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)))
        : nullptr;
    static auto allowDarkModeForWindow = uxtheme
        ? reinterpret_cast<AllowDarkModeForWindowFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(133)))
        : nullptr;
    if (setPreferredAppMode)
        setPreferredAppMode(enabled ? PreferredAppMode::ForceDark : PreferredAppMode::ForceLight);
    if (allowDarkModeForWindow && hwnd)
        allowDarkModeForWindow(hwnd, enabled);
}

double RmsToDb(double rms)
{
    return rms <= 0.000001 ? kRmsFloorDb : 20.0 * std::log10(rms);
}

double DbToMeter(double db)
{
    return std::clamp((db - kRmsFloorDb) / -kRmsFloorDb, 0.0, 1.0);
}

struct Button
{
    RECT rect{};
    std::wstring text;
    int id{};
    bool icon = false;
};

struct AudioFileItem
{
    std::wstring name;
    std::wstring path;
    std::wstring length;
    std::wstring format;
    std::wstring size;
    std::wstring modified;
    double durationSeconds = 0;
    uintmax_t sizeBytes = 0;
    fs::file_time_type modifiedTime{};
};

struct AudioOutputDevice
{
    std::wstring id;
    std::wstring name;
    bool isDefault = false;
};

struct WaveformData
{
    std::vector<float> peaks;
    std::vector<float> rms;
    int channels = 1;
    double durationSeconds = 0;
    int sampleRate = 0;
    int bitsPerSample = 0;
    int bitrateKbps = 0;
};

struct WaveformResult
{
    int token = 0;
    bool ok = false;
    std::wstring path;
    WaveformData data;
};

std::vector<AudioOutputDevice> EnumerateAudioOutputDevices()
{
    std::vector<AudioOutputDevice> devices;
    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
        return devices;

    std::wstring defaultId;
    IMMDevice* defaultDevice = nullptr;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defaultDevice)))
    {
        PWSTR id = nullptr;
        if (SUCCEEDED(defaultDevice->GetId(&id)) && id)
        {
            defaultId = id;
            CoTaskMemFree(id);
        }
        defaultDevice->Release();
    }

    IMMDeviceCollection* collection = nullptr;
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection)))
    {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i)
        {
            IMMDevice* device = nullptr;
            if (FAILED(collection->Item(i, &device)) || !device)
                continue;

            AudioOutputDevice output;
            PWSTR id = nullptr;
            if (SUCCEEDED(device->GetId(&id)) && id)
            {
                output.id = id;
                output.isDefault = _wcsicmp(output.id.c_str(), defaultId.c_str()) == 0;
                CoTaskMemFree(id);
            }

            IPropertyStore* properties = nullptr;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties)))
            {
                PROPVARIANT name;
                PropVariantInit(&name);
                if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR && name.pwszVal)
                    output.name = name.pwszVal;
                PropVariantClear(&name);
                properties->Release();
            }

            if (output.name.empty())
                output.name = L"Audio output";
            if (!output.id.empty())
                devices.push_back(std::move(output));
            device->Release();
        }
        collection->Release();
    }

    enumerator->Release();
    return devices;
}

class MediaPlayerHost
{
public:
    explicit MediaPlayerHost(HWND hwnd) : hwnd_(hwnd) {}
    ~MediaPlayerHost() { Close(); }

    bool Open(const std::wstring& path, const std::wstring& endpointId, HRESULT* failure = nullptr)
    {
        if (failure)
            *failure = S_OK;
        Close();
        IMFMediaSession* session = nullptr;
        IMFMediaSource* source = nullptr;
        IMFPresentationDescriptor* presentation = nullptr;
        IMFTopology* topology = nullptr;

        HRESULT hr = MFCreateMediaSession(nullptr, &session);
        if (SUCCEEDED(hr))
            hr = CreateMediaSource(path, &source);
        if (SUCCEEDED(hr))
            hr = source->CreatePresentationDescriptor(&presentation);
        if (SUCCEEDED(hr))
            hr = BuildTopology(source, presentation, endpointId, &topology);
        if (SUCCEEDED(hr))
            hr = session->SetTopology(0, topology);

        SafeRelease(&topology);
        SafeRelease(&presentation);
        if (FAILED(hr))
        {
            if (failure)
                *failure = hr;
            if (session)
            {
                session->Close();
                session->Shutdown();
                session->Release();
            }
            if (source)
            {
                source->Shutdown();
                source->Release();
            }
            return false;
        }
        session_ = session;
        source_ = source;
        position_ = 0;
        fadeScale_ = 1.0f;
        endpointId_ = endpointId;
        ready_ = true;
        SetVolume(volume_);
        return true;
    }

    void Close()
    {
        ready_ = false;
        playing_ = false;
        position_ = 0;
        fadeScale_ = 1.0f;
        if (session_)
        {
            session_->Stop();
            session_->Close();
            session_->Shutdown();
            session_->Release();
            session_ = nullptr;
        }
        if (source_)
        {
            source_->Shutdown();
            source_->Release();
            source_ = nullptr;
        }
    }

    void Play()
    {
        if (session_ && ready_ && SUCCEEDED(StartAt(position_)))
        {
            ApplyVolume();
            playing_ = true;
        }
    }

    void Pause()
    {
        if (!session_)
            return;
        position_ = Position();
        if (SUCCEEDED(session_->Pause()))
            playing_ = false;
    }

    void Stop()
    {
        if (!session_)
            return;
        session_->Stop();
        playing_ = false;
        position_ = 0;
    }

    void Seek(double seconds)
    {
        if (!session_)
            return;
        position_ = std::max(0.0, seconds);
        if (playing_ && SUCCEEDED(StartAt(position_)))
            ApplyVolume();
    }

    double Position() const
    {
        if (!session_ || !playing_)
            return position_;
        IMFClock* clock = nullptr;
        double seconds = position_;
        if (SUCCEEDED(session_->GetClock(&clock)) && clock)
        {
            LONGLONG clockTime = 0;
            MFTIME systemTime = 0;
            if (SUCCEEDED(clock->GetCorrelatedTime(0, &clockTime, &systemTime)))
                seconds = std::max(0.0, (double)clockTime / 10000000.0);
            clock->Release();
        }
        return seconds;
    }

    void SetVolume(float volume)
    {
        volume_ = std::clamp(volume, 0.0f, 1.0f);
        ApplyVolume();
    }

    void SetFadeScale(float fadeScale)
    {
        fadeScale_ = std::clamp(fadeScale, 0.0f, 1.0f);
        ApplyVolume();
    }

    bool IsPlaying() const { return playing_; }

    void PumpEvents()
    {
        if (!session_)
            return;
        while (true)
        {
            IMFMediaEvent* event = nullptr;
            HRESULT hr = session_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
            if (hr == MF_E_NO_EVENTS_AVAILABLE)
                break;
            if (FAILED(hr))
                break;

            MediaEventType type = MEUnknown;
            event->GetType(&type);
            if (type == MESessionEnded)
            {
                playing_ = false;
                position_ = 0;
                PostMessage(hwnd_, WM_APP + 1, 0, 0);
            }
            else if (type == MESessionTopologyStatus)
            {
                ApplyVolume();
            }
            event->Release();
        }
    }

private:
    static HRESULT CreateMediaSource(const std::wstring& path, IMFMediaSource** source)
    {
        if (!source)
            return E_POINTER;
        *source = nullptr;
        IMFByteStream* stream = nullptr;
        IMFSourceResolver* resolver = nullptr;
        IUnknown* object = nullptr;
        MF_OBJECT_TYPE objectType = MF_OBJECT_INVALID;
        HRESULT hr = MFCreateFile(
            MF_ACCESSMODE_READ,
            MF_OPENMODE_FAIL_IF_NOT_EXIST,
            MF_FILEFLAGS_NONE,
            path.c_str(),
            &stream);
        if (SUCCEEDED(hr))
            hr = MFCreateSourceResolver(&resolver);
        if (SUCCEEDED(hr))
            hr = resolver->CreateObjectFromByteStream(
                stream,
                path.c_str(),
                MF_RESOLUTION_MEDIASOURCE,
                nullptr,
                &objectType,
                &object);
        if (SUCCEEDED(hr))
            hr = object->QueryInterface(IID_PPV_ARGS(source));
        SafeRelease(&object);
        SafeRelease(&resolver);
        SafeRelease(&stream);
        return hr;
    }

    static HRESULT BuildTopology(
        IMFMediaSource* source,
        IMFPresentationDescriptor* presentation,
        const std::wstring& endpointId,
        IMFTopology** topology)
    {
        if (!source || !presentation || !topology)
            return E_POINTER;
        *topology = nullptr;
        IMFTopology* nextTopology = nullptr;
        HRESULT hr = MFCreateTopology(&nextTopology);
        DWORD streamCount = 0;
        if (SUCCEEDED(hr))
            hr = presentation->GetStreamDescriptorCount(&streamCount);

        bool addedAudio = false;
        for (DWORD i = 0; SUCCEEDED(hr) && i < streamCount; ++i)
        {
            BOOL selected = FALSE;
            IMFStreamDescriptor* stream = nullptr;
            IMFMediaTypeHandler* handler = nullptr;
            GUID majorType = GUID_NULL;
            hr = presentation->GetStreamDescriptorByIndex(i, &selected, &stream);
            if (SUCCEEDED(hr))
                hr = stream->GetMediaTypeHandler(&handler);
            if (SUCCEEDED(hr))
                hr = handler->GetMajorType(&majorType);

            if (SUCCEEDED(hr) && majorType == MFMediaType_Audio)
            {
                presentation->SelectStream(i);
                hr = AddAudioBranch(nextTopology, source, presentation, stream, endpointId);
                addedAudio = SUCCEEDED(hr);
            }
            else if (SUCCEEDED(hr))
            {
                presentation->DeselectStream(i);
            }
            SafeRelease(&handler);
            SafeRelease(&stream);
        }

        if (SUCCEEDED(hr) && !addedAudio)
            hr = MF_E_INVALIDMEDIATYPE;
        if (SUCCEEDED(hr))
        {
            *topology = nextTopology;
            nextTopology = nullptr;
        }
        SafeRelease(&nextTopology);
        return hr;
    }

    static HRESULT AddAudioBranch(
        IMFTopology* topology,
        IMFMediaSource* source,
        IMFPresentationDescriptor* presentation,
        IMFStreamDescriptor* stream,
        const std::wstring& endpointId)
    {
        IMFTopologyNode* sourceNode = nullptr;
        IMFTopologyNode* outputNode = nullptr;
        IMFActivate* renderer = nullptr;
        HRESULT hr = MFCreateTopologyNode(MF_TOPOLOGY_SOURCESTREAM_NODE, &sourceNode);
        if (SUCCEEDED(hr))
            hr = sourceNode->SetUnknown(MF_TOPONODE_SOURCE, source);
        if (SUCCEEDED(hr))
            hr = sourceNode->SetUnknown(MF_TOPONODE_PRESENTATION_DESCRIPTOR, presentation);
        if (SUCCEEDED(hr))
            hr = sourceNode->SetUnknown(MF_TOPONODE_STREAM_DESCRIPTOR, stream);
        if (SUCCEEDED(hr))
            hr = topology->AddNode(sourceNode);

        if (SUCCEEDED(hr))
            hr = MFCreateAudioRendererActivate(&renderer);
        if (SUCCEEDED(hr) && !endpointId.empty())
            hr = renderer->SetString(MF_AUDIO_RENDERER_ATTRIBUTE_ENDPOINT_ID, endpointId.c_str());
        if (SUCCEEDED(hr))
            hr = MFCreateTopologyNode(MF_TOPOLOGY_OUTPUT_NODE, &outputNode);
        if (SUCCEEDED(hr))
            hr = outputNode->SetObject(renderer);
        if (SUCCEEDED(hr))
            hr = outputNode->SetUINT32(MF_TOPONODE_STREAMID, 0);
        if (SUCCEEDED(hr))
            hr = outputNode->SetUINT32(MF_TOPONODE_NOSHUTDOWN_ON_REMOVE, FALSE);
        if (SUCCEEDED(hr))
            hr = topology->AddNode(outputNode);
        if (SUCCEEDED(hr))
            hr = sourceNode->ConnectOutput(0, outputNode, 0);

        SafeRelease(&renderer);
        SafeRelease(&outputNode);
        SafeRelease(&sourceNode);
        return hr;
    }

    HRESULT StartAt(double seconds)
    {
        PROPVARIANT start;
        PropVariantInit(&start);
        start.vt = VT_I8;
        start.hVal.QuadPart = (LONGLONG)(seconds * 10000000.0);
        HRESULT hr = session_->Start(&GUID_NULL, &start);
        PropVariantClear(&start);
        return hr;
    }

    void ApplyVolume() const
    {
        if (!session_)
            return;
        IMFSimpleAudioVolume* volume = nullptr;
        if (SUCCEEDED(MFGetService(session_, MR_POLICY_VOLUME_SERVICE, IID_PPV_ARGS(&volume))) && volume)
        {
            volume->SetMasterVolume(std::clamp(volume_ * fadeScale_, 0.0f, 1.0f));
            volume->Release();
        }
    }

    HWND hwnd_{};
    IMFMediaSession* session_{};
    IMFMediaSource* source_{};
    std::wstring endpointId_;
    bool ready_ = false;
    bool playing_ = false;
    float volume_ = 0.8f;
    float fadeScale_ = 1.0f;
    double position_ = 0;
};

struct ComInit
{
    ComInit() { CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); }
    ~ComInit() { CoUninitialize(); }
};

class AudioWaveApp
{
public:
    int Run(HINSTANCE instance, int showCommand, const std::wstring& startupFile)
    {
        instance_ = instance;
        EnableImmersiveControlDarkMode(nullptr, true);
        INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES };
        InitCommonControlsEx(&controls);
        GdiplusStartupInput gdiplusInput;
        GdiplusStartup(&gdiplusToken_, &gdiplusInput, nullptr);
        if (FAILED(MFStartup(MF_VERSION)))
            return 1;
        RegisterClass();
        const UINT startupDpi = GetDpiForSystem();
        RECT workArea{};
        SystemParametersInfo(SPI_GETWORKAREA, 0, &workArea, 0);
        const int desiredClientWidth = MulDiv(kStartupClientWidth, (int)startupDpi, 96);
        const int desiredClientHeight = MulDiv(kStartupClientHeight, (int)startupDpi, 96);
        RECT startupRect{ 0, 0,
            std::min(desiredClientWidth, (int)((workArea.right - workArea.left) * 0.90)),
            std::min(desiredClientHeight, (int)((workArea.bottom - workArea.top) * 0.90)) };
        AdjustWindowRectExForDpi(&startupRect, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, FALSE, WS_EX_ACCEPTFILES, startupDpi);
        const int startupWidth = startupRect.right - startupRect.left;
        const int startupHeight = startupRect.bottom - startupRect.top;
        const int startupX = workArea.left + std::max<LONG>(0, (workArea.right - workArea.left - startupWidth) / 2);
        const int startupY = workArea.top + std::max<LONG>(0, (workArea.bottom - workArea.top - startupHeight) / 2);
        hwnd_ = CreateWindowEx(
            WS_EX_ACCEPTFILES,
            kWindowClassName,
            L"AudioWave Player",
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            startupX,
            startupY,
            startupWidth,
            startupHeight,
            nullptr,
            nullptr,
            instance_,
            this);
        if (!hwnd_)
            return 1;
        player_ = std::make_unique<MediaPlayerHost>(hwnd_);
        const int initialShowCommand = (!startupFile.empty() && showCommand == SW_HIDE) ? SW_SHOWNORMAL : showCommand;
        ShowWindow(hwnd_, initialShowCommand);
        UpdateWindow(hwnd_);
        timeBeginPeriod(1);
        timerResolutionRaised_ = true;
        SetTimer(hwnd_, kUiTimer, 4, nullptr);
        if (!startupFile.empty() && IsSupportedAudio(startupFile))
            LoadFile(startupFile, true);

        MSG msg{};
        while (GetMessage(&msg, nullptr, 0, 0))
        {
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_SPACE)
            {
                TogglePlay();
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        player_.reset();
        while (gWaveformWorkers.load() > 0)
            Sleep(10);
        MFShutdown();
        if (timerResolutionRaised_)
            timeEndPeriod(1);
        GdiplusShutdown(gdiplusToken_);
        return (int)msg.wParam;
    }

private:
    enum ButtonId
    {
        OpenId = 1,
        PrevId,
        PlayId,
        StopId,
        NextId,
        RestartId,
        OrderId,
        ThemeId,
        MuteId
    };

    enum class FileSortColumn { Name = 0, Length = 1, Format = 2, Size = 3, Modified = 4 };

    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* app = reinterpret_cast<AudioWaveApp*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            auto* create = reinterpret_cast<CREATESTRUCT*>(lParam);
            app = reinterpret_cast<AudioWaveApp*>(create->lpCreateParams);
            SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            app->hwnd_ = hwnd;
        }
        return app ? app->WndProc(message, wParam, lParam) : DefWindowProc(hwnd, message, wParam, lParam);
    }

    static LRESULT CALLBACK FileHeaderSubclassProc(
        HWND hwnd,
        UINT message,
        WPARAM wParam,
        LPARAM lParam,
        UINT_PTR subclassId,
        DWORD_PTR refData)
    {
        UNREFERENCED_PARAMETER(subclassId);
        auto* app = reinterpret_cast<AudioWaveApp*>(refData);
        return app ? app->HandleFileHeaderMessage(hwnd, message, wParam, lParam)
                   : DefSubclassProc(hwnd, message, wParam, lParam);
    }

    static LRESULT CALLBACK BrowserControlSubclassProc(
        HWND hwnd,
        UINT message,
        WPARAM wParam,
        LPARAM lParam,
        UINT_PTR subclassId,
        DWORD_PTR refData)
    {
        UNREFERENCED_PARAMETER(subclassId);
        auto* app = reinterpret_cast<AudioWaveApp*>(refData);
        return app ? app->HandleBrowserControlMessage(hwnd, message, wParam, lParam)
                   : DefSubclassProc(hwnd, message, wParam, lParam);
    }

    void RegisterClass()
    {
        WNDCLASSEX wc{ sizeof(wc) };
        wc.lpfnWndProc = StaticWndProc;
        wc.hInstance = instance_;
        wc.lpszClassName = kWindowClassName;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = LoadIcon(instance_, MAKEINTRESOURCE(IDI_APPICON));
        wc.hIconSm = LoadIcon(instance_, MAKEINTRESOURCE(IDI_APPICON));
        wc.hbrBackground = nullptr;
        RegisterClassEx(&wc);

        WNDCLASSEX treeSurface{ sizeof(treeSurface) };
        treeSurface.style = CS_DBLCLKS;
        treeSurface.lpfnWndProc = DefWindowProc;
        treeSurface.hInstance = instance_;
        treeSurface.lpszClassName = L"AudioWavePlayer.TreeSurface";
        treeSurface.hCursor = LoadCursor(nullptr, IDC_ARROW);
        treeSurface.hbrBackground = nullptr;
        RegisterClassEx(&treeSurface);
    }

    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_CREATE:
            OnCreate();
            return 0;
        case WM_SIZE:
            Layout();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_ENTERSIZEMOVE:
            inWindowResize_ = true;
            return 0;
        case WM_EXITSIZEMOVE:
            inWindowResize_ = false;
            ClearWaveCache();
            Layout();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_GETMINMAXINFO:
            OnGetMinMaxInfo(reinterpret_cast<MINMAXINFO*>(lParam));
            return 0;
        case WM_DPICHANGED:
            OnDpiChanged(HIWORD(wParam), reinterpret_cast<RECT*>(lParam));
            return 0;
        case WM_PAINT:
            Paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_TIMER:
            OnTimer();
            return 0;
        case WM_LBUTTONDOWN:
            OnMouseDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_LBUTTONUP:
            FinishDrag();
            return 0;
        case WM_MOUSEMOVE:
            OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_MOUSELEAVE:
            OnMouseLeave();
            return 0;
        case WM_MOUSEWHEEL:
            OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
            return 0;
        case WM_COMMAND:
            OnCommand(LOWORD(wParam), HIWORD(wParam));
            return 0;
        case WM_NOTIFY:
            return OnNotify(reinterpret_cast<NMHDR*>(lParam));
        case WM_CTLCOLORLISTBOX:
            SetTextColor(reinterpret_cast<HDC>(wParam), darkTheme_ ? RGB(238, 242, 246) : RGB(30, 36, 44));
            SetBkColor(reinterpret_cast<HDC>(wParam), darkTheme_ ? RGB(18, 20, 24) : RGB(255, 255, 255));
            return reinterpret_cast<LRESULT>(darkTheme_ ? darkListBrush_ : lightListBrush_);
        case WM_CTLCOLORSTATIC:
            SetTextColor(reinterpret_cast<HDC>(wParam), darkTheme_ ? RGB(238, 242, 246) : RGB(30, 36, 44));
            SetBkColor(reinterpret_cast<HDC>(wParam), darkTheme_ ? RGB(29, 32, 38) : RGB(255, 255, 255));
            return reinterpret_cast<LRESULT>(darkTheme_ ? darkListBrush_ : lightListBrush_);
        case WM_DROPFILES:
            OnDrop(reinterpret_cast<HDROP>(wParam));
            return 0;
        case WM_COPYDATA:
            return OnCopyData(reinterpret_cast<COPYDATASTRUCT*>(lParam));
        case WM_KEYDOWN:
            OnKeyDown(wParam);
            return 0;
        case WM_APP + 1:
            OnPlaybackEnded();
            return 0;
        case WM_APP + 3:
            OnWaveformMessage(reinterpret_cast<WaveformResult*>(lParam));
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd_, kUiTimer);
            if (font_)
            {
                DeleteObject(font_);
                font_ = nullptr;
            }
            if (darkListBrush_)
                DeleteObject(darkListBrush_);
            if (lightListBrush_)
                DeleteObject(lightListBrush_);
            ClearWaveCache();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProc(hwnd_, message, wParam, lParam);
        }
    }

    void OnCreate()
    {
        dpi_ = GetDpiForWindow(hwnd_);
        treeModel_ = CreateWindowEx(
            0,
            WC_TREEVIEW,
            nullptr,
            WS_CHILD | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT,
            0,
            0,
            0,
            0,
            hwnd_,
            (HMENU)1003,
            instance_,
            nullptr);
        treeView_ = CreateWindowEx(
            WS_EX_CLIENTEDGE,
            L"AudioWavePlayer.TreeSurface",
            nullptr,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL,
            0,
            0,
            0,
            0,
            hwnd_,
            (HMENU)1002,
            instance_,
            nullptr);
        fileList_ = CreateWindowEx(
            WS_EX_CLIENTEDGE,
            WC_LISTVIEW,
            nullptr,
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            0,
            0,
            0,
            0,
            hwnd_,
            (HMENU)1001,
            instance_,
            nullptr);
        outputCombo_ = CreateWindowEx(
            0,
            WC_COMBOBOX,
            nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
            0,
            0,
            0,
            0,
            hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAudioOutputComboId)),
            instance_,
            nullptr);
        RecreateUiFont();
        SendMessage(treeView_, WM_SETFONT, (WPARAM)font_, TRUE);
        SendMessage(treeModel_, WM_SETFONT, (WPARAM)font_, TRUE);
        SendMessage(fileList_, WM_SETFONT, (WPARAM)font_, TRUE);
        SendMessage(outputCombo_, WM_SETFONT, (WPARAM)font_, TRUE);
        SetWindowSubclass(treeView_, BrowserControlSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
        SetWindowSubclass(fileList_, BrowserControlSubclassProc, 2, reinterpret_cast<DWORD_PTR>(this));
        darkListBrush_ = CreateSolidBrush(RGB(18, 20, 24));
        lightListBrush_ = CreateSolidBrush(RGB(255, 255, 255));
        ListView_SetExtendedListViewStyle(fileList_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        TreeView_SetExtendedStyle(treeModel_, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
        ApplyTreeImageList();
        selectedOutputId_ = LoadSavedAudioOutputId();
        RefreshAudioOutputs();
        ApplyNativeControlTheme();
        AddFileColumn(0, L"Name", 620);
        AddFileColumn(1, L"Length", 100);
        AddFileColumn(2, L"Format", 120);
        AddFileColumn(3, L"Size", 140);
        AddFileColumn(4, L"Modified", 220);
        EnableHeaderFullDrag();
        DragAcceptFiles(hwnd_, TRUE);
        ApplyCaptionTheme();
        PopulateTreeRoots();
        RedrawWindow(treeView_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE);
        SetCurrentFolder(fs::current_path().wstring(), L"");
    }

    int S(int value) const
    {
        return MulDiv(value, (int)dpi_, 96);
    }

    void RecreateUiFont()
    {
        if (font_)
            DeleteObject(font_);
        font_ = CreateFont(-S(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    }

    void OnDpiChanged(UINT dpi, const RECT* suggestedRect)
    {
        dpi_ = dpi;
        RecreateUiFont();
        SendMessage(treeView_, WM_SETFONT, (WPARAM)font_, TRUE);
        SendMessage(treeModel_, WM_SETFONT, (WPARAM)font_, TRUE);
        SendMessage(fileList_, WM_SETFONT, (WPARAM)font_, TRUE);
        SendMessage(outputCombo_, WM_SETFONT, (WPARAM)font_, TRUE);
        ClearWaveCache();
        if (suggestedRect)
        {
            SetWindowPos(hwnd_, nullptr,
                suggestedRect->left,
                suggestedRect->top,
                suggestedRect->right - suggestedRect->left,
                suggestedRect->bottom - suggestedRect->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        Layout();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void OnGetMinMaxInfo(MINMAXINFO* info)
    {
        if (!info)
            return;
        info->ptMinTrackSize.x = S(1100);
        info->ptMinTrackSize.y = S(720);
    }

    void Layout()
    {
        GetClientRect(hwnd_, &client_);
        bottomRect_ = { 0, client_.bottom - S(kBottomHeight), client_.right, client_.bottom };
        const LONG waveTop = S(kTopHeight) + S(kWaveMargin);
        const LONG maxWaveHeight = std::max<LONG>(S(160), bottomRect_.top - waveTop - S(kSplitterSize) - S(180));
        const LONG targetWaveHeight = std::clamp<LONG>((LONG)std::round(client_.bottom * waveHeightRatio_), S(160), maxWaveHeight);
        browserHeight_ = std::max<LONG>(S(180), bottomRect_.top - S(8) - (waveTop + targetWaveHeight + S(kSplitterSize)));
        browserHeight_ = std::clamp<LONG>(browserHeight_, S(180), std::max<LONG>(S(180), bottomRect_.top - S(kTopHeight) - S(120)));
        treeWidth_ = std::clamp<LONG>((LONG)std::round((client_.right - S(kWaveMargin) * 2) * treeWidthRatio_), S(220), std::max<LONG>(S(220), client_.right - S(520)));
        browserRect_ = { S(kWaveMargin), std::max<LONG>(S(kTopHeight) + S(160), bottomRect_.top - browserHeight_),
            client_.right - S(kWaveMargin), bottomRect_.top - S(8) };
        waveRect_ = { S(kWaveMargin), S(kTopHeight) + S(kWaveMargin),
            client_.right - S(kWaveMargin), browserRect_.top - S(kSplitterSize) };
        horizontalSplitterRect_ = { waveRect_.left, waveRect_.bottom, waveRect_.right, browserRect_.top };
        folderRect_ = { browserRect_.left, browserRect_.top, std::min<LONG>(browserRect_.left + treeWidth_, browserRect_.right - S(420)), browserRect_.bottom };
        verticalSplitterRect_ = { folderRect_.right, browserRect_.top, folderRect_.right + S(kSplitterSize + 2), browserRect_.bottom };
        fileRect_ = { verticalSplitterRect_.right, browserRect_.top, browserRect_.right, browserRect_.bottom };
        const BOOL repaintChildren = dragMode_ == DragMode::None && !inWindowResize_;
        ::MoveWindow(treeView_, folderRect_.left, folderRect_.top,
            folderRect_.right - folderRect_.left, folderRect_.bottom - folderRect_.top, repaintChildren);
        ::MoveWindow(fileList_, fileRect_.left, fileRect_.top,
            fileRect_.right - fileRect_.left, fileRect_.bottom - fileRect_.top, repaintChildren);
        LayoutFileColumns();
        UpdateTreeScrollInfo();
        UpdateFileListScrollInfo();
        LayoutButtons();
        if (outputCombo_)
        {
            ::MoveWindow(outputCombo_,
                outputComboRect_.left,
                outputComboRect_.top,
                outputComboRect_.right - outputComboRect_.left,
                S(260),
                repaintChildren);
        }
    }

    void AddFileColumn(int index, const wchar_t* text, int width)
    {
        LVCOLUMN column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = const_cast<wchar_t*>(text);
        column.cx = width;
        column.iSubItem = index;
        ListView_InsertColumn(fileList_, index, &column);
    }

    void LayoutFileColumns()
    {
        if (!fileList_)
            return;
        const int width = std::max<LONG>(0, fileRect_.right - fileRect_.left - GetSystemMetrics(SM_CXVSCROLL) - S(8));
        const int lengthWidth = S(90);
        const int formatWidth = S(100);
        const int sizeWidth = S(130);
        const int modifiedWidth = S(210);
        const int nameWidth = std::max(S(260), width - lengthWidth - formatWidth - sizeWidth - modifiedWidth);
        ListView_SetColumnWidth(fileList_, 0, nameWidth);
        ListView_SetColumnWidth(fileList_, 1, lengthWidth);
        ListView_SetColumnWidth(fileList_, 2, formatWidth);
        ListView_SetColumnWidth(fileList_, 3, sizeWidth);
        ListView_SetColumnWidth(fileList_, 4, modifiedWidth);
    }

    void EnableHeaderFullDrag()
    {
        HWND header = ListView_GetHeader(fileList_);
        if (!header)
            return;
        LONG_PTR style = GetWindowLongPtr(header, GWL_STYLE);
        SetWindowLongPtr(header, GWL_STYLE, style | HDS_FULLDRAG);
        SetWindowPos(header, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        SetWindowSubclass(header, FileHeaderSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
    }

    std::wstring SelectedFilePath() const
    {
        const int index = ListView_GetNextItem(fileList_, -1, LVNI_SELECTED);
        if (index >= 0 && index < (int)files_.size())
            return files_[index].path;
        return currentFile_;
    }

    int CompareFileItems(const AudioFileItem& a, const AudioFileItem& b) const
    {
        switch (fileSortColumn_)
        {
        case FileSortColumn::Length:
            if (a.durationSeconds != b.durationSeconds)
                return a.durationSeconds < b.durationSeconds ? -1 : 1;
            break;
        case FileSortColumn::Format:
        {
            const int result = _wcsicmp(a.format.c_str(), b.format.c_str());
            if (result != 0)
                return result;
            break;
        }
        case FileSortColumn::Size:
            if (a.sizeBytes != b.sizeBytes)
                return a.sizeBytes < b.sizeBytes ? -1 : 1;
            break;
        case FileSortColumn::Modified:
            if (a.modifiedTime != b.modifiedTime)
                return a.modifiedTime < b.modifiedTime ? -1 : 1;
            break;
        case FileSortColumn::Name:
        default:
            break;
        }

        const int nameResult = _wcsicmp(a.name.c_str(), b.name.c_str());
        if (nameResult != 0)
            return nameResult;
        return _wcsicmp(a.path.c_str(), b.path.c_str());
    }

    void SortFileItems()
    {
        std::sort(files_.begin(), files_.end(), [&](const auto& a, const auto& b) {
            const int result = CompareFileItems(a, b);
            return fileSortAscending_ ? result < 0 : result > 0;
        });
    }

    void RebuildFileListItems(const std::wstring& selected)
    {
        ListView_DeleteAllItems(fileList_);
        int selectedIndex = -1;
        for (int i = 0; i < (int)files_.size(); ++i)
        {
            LVITEM item{};
            item.mask = LVIF_TEXT;
            item.iItem = i;
            item.pszText = files_[i].name.data();
            ListView_InsertItem(fileList_, &item);
            ListView_SetItemText(fileList_, i, 1, files_[i].length.data());
            ListView_SetItemText(fileList_, i, 2, files_[i].format.data());
            ListView_SetItemText(fileList_, i, 3, files_[i].size.data());
            ListView_SetItemText(fileList_, i, 4, files_[i].modified.data());
            if (!selected.empty() && _wcsicmp(files_[i].path.c_str(), selected.c_str()) == 0)
                selectedIndex = i;
        }
        if (selectedIndex >= 0)
        {
            ListView_SetItemState(fileList_, selectedIndex, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(fileList_, selectedIndex, FALSE);
        }
    }

    void SortFileListByColumn(int column)
    {
        const auto nextColumn = static_cast<FileSortColumn>(column);
        if (fileSortColumn_ == nextColumn)
            fileSortAscending_ = !fileSortAscending_;
        else
        {
            fileSortColumn_ = nextColumn;
            fileSortAscending_ = true;
        }

        const std::wstring selected = SelectedFilePath();
        SortFileItems();
        RebuildFileListItems(selected);
        UpdateFileListScrollInfo();
        RedrawWindow(fileList_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
        HWND header = ListView_GetHeader(fileList_);
        if (header)
            RedrawWindow(header, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    }

    void ApplyTreeImageList()
    {
        SHFILEINFO info{};
        HIMAGELIST images = reinterpret_cast<HIMAGELIST>(SHGetFileInfoW(
            L"C:\\",
            FILE_ATTRIBUTE_DIRECTORY,
            &info,
            sizeof(info),
            SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES));
        if (images)
        {
            ImageList_SetBkColor(images, CLR_NONE);
            TreeView_SetImageList(treeModel_, images, TVSIL_NORMAL);
        }
    }

    int ShellIconIndex(const std::wstring& path, DWORD attributes) const
    {
        SHFILEINFO info{};
        SHGetFileInfoW(
            path.c_str(),
            attributes,
            &info,
            sizeof(info),
            SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
        return info.iIcon;
    }

    LRESULT HandleFileHeaderMessage(HWND header, UINT message, WPARAM wParam, LPARAM lParam)
    {
        UNREFERENCED_PARAMETER(wParam);
        switch (message)
        {
        case WM_ERASEBKGND:
            FillControlBackground(reinterpret_cast<HDC>(wParam), header);
            return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(header, &ps);
            DrawFileHeaderControl(header, hdc);
            EndPaint(header, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN:
        {
            HDHITTESTINFO hit{};
            hit.pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            SendMessage(header, HDM_HITTEST, 0, reinterpret_cast<LPARAM>(&hit));
            if (hit.iItem >= 0 && (hit.flags & (HHT_ONDIVIDER | HHT_ONDIVOPEN)))
            {
                headerResizeColumn_ = hit.iItem;
                headerResizeStartX_ = hit.pt.x;
                headerResizeStartWidth_ = ListView_GetColumnWidth(fileList_, headerResizeColumn_);
                SetCapture(header);
                SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
                return 0;
            }
            if (hit.iItem >= 0 && hit.iItem < 5)
            {
                SortFileListByColumn(hit.iItem);
                return 0;
            }
            break;
        }
        case WM_MOUSEMOVE:
            if (headerResizeColumn_ >= 0)
            {
                const int x = GET_X_LPARAM(lParam);
                const int width = std::max(S(48), headerResizeStartWidth_ + x - headerResizeStartX_);
                ListView_SetColumnWidth(fileList_, headerResizeColumn_, width);
                RedrawWindow(header, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
                RedrawWindow(fileList_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
                SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
                return 0;
            }
            break;
        case WM_LBUTTONUP:
            if (headerResizeColumn_ >= 0)
            {
                headerResizeColumn_ = -1;
                if (GetCapture() == header)
                    ReleaseCapture();
                RedrawWindow(fileList_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
                return 0;
            }
            break;
        case WM_CAPTURECHANGED:
            headerResizeColumn_ = -1;
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(header, FileHeaderSubclassProc, 1);
            break;
        default:
            break;
        }
        return DefSubclassProc(header, message, wParam, lParam);
    }

    LRESULT HandleBrowserControlMessage(HWND control, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_LBUTTONDOWN:
            if (control == treeView_)
                return HandleTreeClick(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), false);
            if (control == fileList_)
                return HandleFileListClick(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            break;
        case WM_LBUTTONDBLCLK:
            if (control == treeView_)
                return HandleTreeClick(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), true);
            break;
        case WM_MOUSEWHEEL:
            if (control == treeView_)
            {
                AddTreeScrollImpulse(-GET_WHEEL_DELTA_WPARAM(wParam));
                return 0;
            }
            if (control == fileList_)
            {
                AddFileListScrollImpulse(-GET_WHEEL_DELTA_WPARAM(wParam));
                return 0;
            }
            break;
        case WM_VSCROLL:
            if (control == treeView_)
                return HandleTreeVScroll(LOWORD(wParam), HIWORD(wParam));
            if (control == fileList_)
                return HandleFileListVScroll(LOWORD(wParam), HIWORD(wParam));
            break;
        case WM_ERASEBKGND:
            UNREFERENCED_PARAMETER(wParam);
            return 1;
        case WM_PAINT:
            if (control == fileList_)
            {
                PAINTSTRUCT ps{};
                HDC hdc = BeginPaint(control, &ps);
                RECT contentRect{};
                GetClientRect(control, &contentRect);
                contentRect.top = std::max<LONG>(contentRect.top, FileListHeaderBottom());
                RECT paintRect{};
                if (IntersectRect(&paintRect, &ps.rcPaint, &contentRect))
                    PaintControlBuffered(control, hdc, paintRect, [&](HDC buffer) { DrawFileListControl(buffer); });
                EndPaint(control, &ps);
                return 0;
            }
            if (control == treeView_)
            {
                PAINTSTRUCT ps{};
                HDC hdc = BeginPaint(control, &ps);
                RECT clientRect{};
                GetClientRect(control, &clientRect);
                PaintControlBuffered(control, hdc, clientRect, [&](HDC buffer) { DrawTreeControl(buffer); });
                EndPaint(control, &ps);
                return 0;
            }
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(control, BrowserControlSubclassProc, control == treeView_ ? 1 : 2);
            break;
        default:
            break;
        }
        return DefSubclassProc(control, message, wParam, lParam);
    }

    RECT HeaderClientRectInFileList() const
    {
        RECT headerRect{};
        HWND header = ListView_GetHeader(fileList_);
        if (!header)
            return headerRect;
        GetWindowRect(header, &headerRect);
        MapWindowPoints(nullptr, fileList_, reinterpret_cast<POINT*>(&headerRect), 2);
        return headerRect;
    }

    void FillFileListEmptyArea(HDC hdc)
    {
        RECT client{};
        GetClientRect(fileList_, &client);
        const RECT headerRect = HeaderClientRectInFileList();
        RECT fillRect = client;
        fillRect.top = std::max<LONG>(fillRect.top, headerRect.bottom);
        const int count = ListView_GetItemCount(fileList_);
        if (count > 0)
        {
            RECT itemRect{};
            if (ListView_GetItemRect(fileList_, count - 1, &itemRect, LVIR_BOUNDS))
                fillRect.top = std::max<LONG>(fillRect.top, itemRect.bottom);
        }
        if (fillRect.top < fillRect.bottom)
        {
            HBRUSH brush = CreateSolidBrush(darkTheme_ ? RGB(18, 20, 24) : RGB(255, 255, 255));
            FillRect(hdc, &fillRect, brush);
            DeleteObject(brush);
        }
    }

    int FileListHeaderBottom() const
    {
        const RECT headerRect = HeaderClientRectInFileList();
        return std::max<LONG>(0, headerRect.bottom);
    }

    void UpdateFileListScrollInfo(bool redraw = true)
    {
        if (!fileList_)
            return;
        RECT client{};
        GetClientRect(fileList_, &client);
        const int headerBottom = FileListHeaderBottom();
        const int page = std::max<int>(1, client.bottom - headerBottom);
        const int content = std::max(page, (int)files_.size() * FileListRowHeight());
        const int maxOffset = std::max(0, content - page);
        fileListScrollOffset_ = std::clamp(fileListScrollOffset_, 0, maxOffset);
        SCROLLINFO info{ sizeof(info) };
        info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        info.nMin = 0;
        info.nMax = content - 1;
        info.nPage = page;
        info.nPos = fileListScrollOffset_;
        SetScrollInfo(fileList_, SB_VERT, &info, redraw);
    }

    void SetFileListScrollOffset(int offset)
    {
        RECT client{};
        GetClientRect(fileList_, &client);
        const int page = std::max<int>(1, client.bottom - FileListHeaderBottom());
        const int maxOffset = std::max<int>(0, (int)files_.size() * FileListRowHeight() - page);
        const int next = std::clamp(offset, 0, maxOffset);
        if (next == fileListScrollOffset_)
            return;
        fileListScrollOffset_ = next;
        UpdateFileListScrollInfo();
        RedrawFileListNow();
    }

    void DrawFileListControl(HDC hdc)
    {
        RECT client{};
        GetClientRect(fileList_, &client);
        FillControlBackground(hdc, fileList_);
        const int headerBottom = FileListHeaderBottom();
        const int rowHeight = FileListRowHeight();
        const COLORREF text = darkTheme_ ? RGB(238, 242, 246) : RGB(30, 36, 44);
        const COLORREF selectedBg = darkTheme_ ? RGB(50, 60, 72) : RGB(216, 232, 252);
        const COLORREF bg = darkTheme_ ? RGB(18, 20, 24) : RGB(255, 255, 255);
        const COLORREF line = darkTheme_ ? RGB(45, 52, 62) : RGB(224, 229, 235);
        HBRUSH bgBrush = CreateSolidBrush(bg);
        HBRUSH selectedBrush = CreateSolidBrush(selectedBg);
        HPEN linePen = CreatePen(PS_SOLID, 1, line);
        HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(hdc, font_));
        HGDIOBJ oldPen = SelectObject(hdc, linePen);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, text);

        const int first = std::max(0, fileListScrollOffset_ / rowHeight);
        const int yOffset = fileListScrollOffset_ % rowHeight;
        std::vector<int> columnEdges;
        columnEdges.reserve(5);
        int x = client.left;
        for (int col = 0; col < 5; ++col)
        {
            x += ListView_GetColumnWidth(fileList_, col);
            columnEdges.push_back(x);
        }

        for (int i = first; i < (int)files_.size(); ++i)
        {
            const int y = headerBottom + (i - first) * rowHeight - yOffset;
            if (y >= client.bottom)
                break;
            RECT rowRect{ client.left, y, client.right, y + rowHeight };
            const bool selected = (ListView_GetItemState(fileList_, i, LVIS_SELECTED) & LVIS_SELECTED) != 0;
            FillRect(hdc, &rowRect, selected ? selectedBrush : bgBrush);

            x = client.left;
            const std::wstring values[] = { files_[i].name, files_[i].length, files_[i].format, files_[i].size, files_[i].modified };
            for (int col = 0; col < 5; ++col)
            {
                const int cellRight = columnEdges[col];
                RECT cell{ x, y, cellRight, y + rowHeight };
                RECT textRect{ cell.left + S(col == 0 ? 12 : 10), cell.top, cell.right - S(8), cell.bottom };
                DrawTextW(hdc, values[col].c_str(), -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                x = cellRight;
            }
            MoveToEx(hdc, client.left, rowRect.bottom - 1, nullptr);
            LineTo(hdc, client.right, rowRect.bottom - 1);
        }
        for (int edge : columnEdges)
        {
            MoveToEx(hdc, edge - 1, headerBottom, nullptr);
            LineTo(hdc, edge - 1, client.bottom);
        }
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldFont);
        DeleteObject(linePen);
        DeleteObject(selectedBrush);
        DeleteObject(bgBrush);
    }

    void RedrawFileListNow()
    {
        RECT paintRect{};
        GetClientRect(fileList_, &paintRect);
        paintRect.top = std::max<LONG>(paintRect.top, FileListHeaderBottom());
        if (paintRect.right <= paintRect.left || paintRect.bottom <= paintRect.top)
            return;

        HDC hdc = GetDC(fileList_);
        if (!hdc)
            return;
        PaintControlBuffered(fileList_, hdc, paintRect, [&](HDC buffer) { DrawFileListControl(buffer); });
        ReleaseDC(fileList_, hdc);
    }

    int TreeItemLevel(HTREEITEM item) const
    {
        int level = 0;
        while ((item = TreeView_GetParent(treeModel_, item)) != nullptr)
            ++level;
        return level;
    }

    int TreeRowHeight() const
    {
        return std::max(S(24), (int)SendMessage(treeModel_, TVM_GETITEMHEIGHT, 0, 0));
    }

    std::vector<HTREEITEM> VisibleTreeItems() const
    {
        std::vector<HTREEITEM> items;
        for (HTREEITEM item = TreeView_GetRoot(treeModel_); item; item = TreeView_GetNextVisible(treeModel_, item))
            items.push_back(item);
        return items;
    }

    int TreeContentHeight() const
    {
        return (int)VisibleTreeItems().size() * TreeRowHeight();
    }

    void UpdateTreeScrollInfo(bool redraw = true)
    {
        if (!treeView_)
            return;
        RECT client{};
        GetClientRect(treeView_, &client);
        const int page = std::max(1L, client.bottom - client.top);
        const int content = std::max(page, TreeContentHeight());
        const int maxOffset = std::max(0, content - page);
        treeScrollOffset_ = std::clamp(treeScrollOffset_, 0, maxOffset);
        SCROLLINFO info{ sizeof(info) };
        info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        info.nMin = 0;
        info.nMax = content - 1;
        info.nPage = page;
        info.nPos = treeScrollOffset_;
        SetScrollInfo(treeView_, SB_VERT, &info, redraw);
    }

    void SetTreeScrollOffset(int offset)
    {
        RECT client{};
        GetClientRect(treeView_, &client);
        const int page = std::max<int>(1, client.bottom - client.top);
        const int maxOffset = std::max<int>(0, TreeContentHeight() - page);
        const int next = std::clamp(offset, 0, maxOffset);
        if (next == treeScrollOffset_)
            return;
        treeScrollOffset_ = next;
        UpdateTreeScrollInfo();
        RedrawWindow(treeView_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE);
    }

    void ScrollTreeByPixels(int dy)
    {
        SetTreeScrollOffset(treeScrollOffset_ + dy);
    }

    void AddTreeScrollImpulse(int wheelDelta)
    {
        treeScrollVelocity_ += (double)wheelDelta * (double)(TreeRowHeight() * 3) * 0.12 / (double)WHEEL_DELTA;
    }

    void AddFileListScrollImpulse(int wheelDelta)
    {
        fileListScrollVelocity_ += (double)wheelDelta * (double)(FileListRowHeight() * 3) * 0.12 / (double)WHEEL_DELTA;
    }

    bool StepSmoothScroll()
    {
        const ULONGLONG now = GetTickCount64();
        if (lastSmoothScrollTick_ == 0)
            lastSmoothScrollTick_ = now;
        const ULONGLONG elapsedMs = std::max<ULONGLONG>(1, now - lastSmoothScrollTick_);
        lastSmoothScrollTick_ = now;
        const double frameScale = std::clamp((double)elapsedMs / kSmoothScrollFrameMs, 0.25, 8.0);
        bool changed = false;
        auto step = [&](double& velocity, int currentOffset, auto setter) {
            if (std::abs(velocity) < 0.08)
            {
                velocity = 0;
                return;
            }
            const int delta = (int)std::round(velocity * frameScale);
            if (delta != 0)
            {
                setter(currentOffset + delta);
                changed = true;
            }
            velocity *= std::pow(0.88, frameScale);
        };
        step(treeScrollVelocity_, treeScrollOffset_, [&](int value) { SetTreeScrollOffset(value); });
        step(fileListScrollVelocity_, fileListScrollOffset_, [&](int value) { SetFileListScrollOffset(value); });
        return changed;
    }

    bool IsBrowserSmoothScrolling() const
    {
        return std::abs(treeScrollVelocity_) >= 0.08 || std::abs(fileListScrollVelocity_) >= 0.08;
    }

    LRESULT HandleTreeVScroll(WORD request, WORD thumb)
    {
        SCROLLINFO info{ sizeof(info) };
        info.fMask = SIF_ALL;
        GetScrollInfo(treeView_, SB_VERT, &info);
        int target = treeScrollOffset_;
        switch (request)
        {
        case SB_LINEUP: target -= S(24); break;
        case SB_LINEDOWN: target += S(24); break;
        case SB_PAGEUP: target -= (int)info.nPage; break;
        case SB_PAGEDOWN: target += (int)info.nPage; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION:
            target = (int)info.nTrackPos;
            if (target == 0 && thumb != 0)
                target = thumb;
            break;
        case SB_TOP: target = 0; break;
        case SB_BOTTOM: target = info.nMax; break;
        default:
            return 0;
        }
        SetTreeScrollOffset(target);
        return 0;
    }

    void ScrollFileListByPixels(int dy)
    {
        if (!fileList_ || dy == 0)
            return;
        SetFileListScrollOffset(fileListScrollOffset_ + dy);
        HWND header = ListView_GetHeader(fileList_);
        if (header)
            InvalidateRect(header, nullptr, FALSE);
    }

    int FileListRowHeight() const
    {
        RECT rect{};
        if (ListView_GetItemRect(fileList_, 0, &rect, LVIR_BOUNDS))
            return std::max(1L, rect.bottom - rect.top);
        return S(24);
    }

    LRESULT HandleFileListVScroll(WORD request, WORD thumb)
    {
        SCROLLINFO info{ sizeof(info) };
        info.fMask = SIF_ALL;
        GetScrollInfo(fileList_, SB_VERT, &info);
        const int rowHeight = FileListRowHeight();
        int target = fileListScrollOffset_;
        switch (request)
        {
        case SB_LINEUP: target -= S(24); break;
        case SB_LINEDOWN: target += S(24); break;
        case SB_PAGEUP: target -= (int)info.nPage; break;
        case SB_PAGEDOWN: target += (int)info.nPage; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION:
            target = (int)info.nTrackPos;
            if (target == 0 && thumb != 0)
                target = thumb;
            break;
        case SB_TOP: target = 0; break;
        case SB_BOTTOM: target = info.nMax; break;
        default:
            return 0;
        }
        UNREFERENCED_PARAMETER(rowHeight);
        SetFileListScrollOffset(target);
        return 0;
    }

    void DrawTreeControl(HDC hdc)
    {
        RECT client{};
        GetClientRect(treeView_, &client);
        FillControlBackground(hdc, treeView_);

        const COLORREF textColor = darkTheme_ ? RGB(238, 242, 246) : RGB(30, 36, 44);
        const COLORREF mutedColor = darkTheme_ ? RGB(142, 157, 176) : RGB(88, 98, 112);
        const COLORREF lineColor = darkTheme_ ? RGB(55, 63, 75) : RGB(198, 205, 214);
        const COLORREF selectedBg = darkTheme_ ? RGB(50, 60, 72) : RGB(216, 232, 252);

        SetBkMode(hdc, TRANSPARENT);
        HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(hdc, font_));
        const int indent = (int)SendMessage(treeModel_, TVM_GETINDENT, 0, 0);
        const int rowPad = S(3);
        const int expanderSize = S(11);
        const int iconSize = S(16);

        const auto items = VisibleTreeItems();
        const int rowHeight = TreeRowHeight();
        for (int row = 0; row < (int)items.size(); ++row)
        {
            HTREEITEM item = items[row];
            RECT itemRect{ client.left, row * rowHeight - treeScrollOffset_, client.right, (row + 1) * rowHeight - treeScrollOffset_ };
            if (itemRect.bottom < client.top || itemRect.top > client.bottom)
                continue;

            const bool selected = item == selectedTreeItem_;
            RECT rowRect{ client.left, itemRect.top, client.right, itemRect.bottom };
            HBRUSH rowBrush = CreateSolidBrush(selected ? selectedBg : (darkTheme_ ? RGB(18, 20, 24) : RGB(255, 255, 255)));
            FillRect(hdc, &rowRect, rowBrush);
            DeleteObject(rowBrush);

            wchar_t text[MAX_PATH]{};
            TVITEM tvItem{};
            tvItem.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_CHILDREN | TVIF_STATE;
            tvItem.hItem = item;
            tvItem.pszText = text;
            tvItem.cchTextMax = _countof(text);
            tvItem.stateMask = TVIS_EXPANDED;
            TreeView_GetItem(treeModel_, &tvItem);

            const int level = TreeItemLevel(item);
            const int left = S(8) + level * indent;
            const int centerY = itemRect.top + (itemRect.bottom - itemRect.top) / 2;
            const bool hasChildren = TreeView_GetChild(treeModel_, item) != nullptr;
            if (hasChildren)
            {
                RECT box{ left, centerY - expanderSize / 2, left + expanderSize, centerY + expanderSize / 2 + 1 };
                HPEN pen = CreatePen(PS_SOLID, 1, selected ? RGB(185, 197, 210) : lineColor);
                HGDIOBJ oldPen = SelectObject(hdc, pen);
                HBRUSH oldBrush = reinterpret_cast<HBRUSH>(SelectObject(hdc, GetStockObject(NULL_BRUSH)));
                Rectangle(hdc, box.left, box.top, box.right, box.bottom);
                MoveToEx(hdc, box.left + S(3), centerY, nullptr);
                LineTo(hdc, box.right - S(3), centerY);
                if ((tvItem.state & TVIS_EXPANDED) == 0)
                {
                    MoveToEx(hdc, box.left + expanderSize / 2, box.top + S(3), nullptr);
                    LineTo(hdc, box.left + expanderSize / 2, box.bottom - S(3));
                }
                SelectObject(hdc, oldBrush);
                SelectObject(hdc, oldPen);
                DeleteObject(pen);
            }

            const int iconX = left + S(18);
            const int iconY = centerY - iconSize / 2;
            DrawTreeFolderIcon(hdc, iconX, iconY, iconSize, IsDrivePath(treePaths_[item]), selected);

            RECT textRect{ iconX + S(24), itemRect.top - rowPad, client.right - S(8), itemRect.bottom + rowPad };
            SetTextColor(hdc, treePaths_[item].empty() ? mutedColor : textColor);
            DrawTextW(hdc, text, -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        }

        SelectObject(hdc, oldFont);
    }

    bool IsDrivePath(const std::wstring& path) const
    {
        return path.size() == 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
    }

    void DrawTreeFolderIcon(HDC hdc, int x, int y, int size, bool drive, bool selected)
    {
        const COLORREF fill = selected
            ? (darkTheme_ ? RGB(178, 188, 200) : RGB(68, 78, 92))
            : (darkTheme_ ? RGB(128, 140, 154) : RGB(104, 114, 128));
        const COLORREF stroke = selected
            ? (darkTheme_ ? RGB(212, 220, 228) : RGB(42, 50, 62))
            : (darkTheme_ ? RGB(92, 102, 116) : RGB(78, 88, 102));
        HBRUSH brush = CreateSolidBrush(fill);
        HPEN pen = CreatePen(PS_SOLID, 1, stroke);
        HGDIOBJ oldBrush = SelectObject(hdc, brush);
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        if (drive)
        {
            RoundRect(hdc, x, y + size / 5, x + size, y + size - size / 8, S(3), S(3));
            MoveToEx(hdc, x + S(3), y + size - S(5), nullptr);
            LineTo(hdc, x + size - S(3), y + size - S(5));
        }
        else
        {
            POINT points[] = {
                { x, y + size / 3 },
                { x + size / 3, y + size / 3 },
                { x + size / 2, y + size / 5 },
                { x + size - S(1), y + size / 5 },
                { x + size - S(1), y + size - S(2) },
                { x, y + size - S(2) }
            };
            Polygon(hdc, points, _countof(points));
        }
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBrush);
        DeleteObject(pen);
        DeleteObject(brush);
    }

    void FillControlBackground(HDC hdc, HWND control)
    {
        RECT rect{};
        GetClientRect(control, &rect);
        HBRUSH brush = CreateSolidBrush(darkTheme_ ? RGB(18, 20, 24) : RGB(255, 255, 255));
        FillRect(hdc, &rect, brush);
        DeleteObject(brush);
    }

    template <typename DrawFn>
    void PaintControlBuffered(HWND control, HDC target, const RECT& paintRect, DrawFn draw)
    {
        UNREFERENCED_PARAMETER(control);
        const int width = paintRect.right - paintRect.left;
        const int height = paintRect.bottom - paintRect.top;
        if (width <= 0 || height <= 0)
            return;

        HDC buffer = CreateCompatibleDC(target);
        if (!buffer)
        {
            draw(target);
            return;
        }
        HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
        if (!bitmap)
        {
            DeleteDC(buffer);
            draw(target);
            return;
        }
        HGDIOBJ oldBitmap = SelectObject(buffer, bitmap);
        if (!oldBitmap)
        {
            DeleteObject(bitmap);
            DeleteDC(buffer);
            draw(target);
            return;
        }
        POINT oldOrigin{};
        SetViewportOrgEx(buffer, -paintRect.left, -paintRect.top, &oldOrigin);
        draw(buffer);
        SetViewportOrgEx(buffer, oldOrigin.x, oldOrigin.y, nullptr);
        BitBlt(target, paintRect.left, paintRect.top, width, height, buffer, 0, 0, SRCCOPY);
        SelectObject(buffer, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(buffer);
    }

    void DrawFileHeaderControl(HWND header, HDC hdc)
    {
        RECT client{};
        GetClientRect(header, &client);
        FillControlBackground(hdc, header);

        const COLORREF text = darkTheme_ ? RGB(180, 195, 214) : RGB(45, 52, 62);
        const COLORREF line = darkTheme_ ? RGB(49, 57, 68) : RGB(198, 205, 214);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, text);
        HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(hdc, font_));

        const int count = Header_GetItemCount(header);
        for (int i = 0; i < count; ++i)
        {
            RECT itemRect{};
            SendMessage(header, HDM_GETITEMRECT, i, reinterpret_cast<LPARAM>(&itemRect));
            wchar_t buffer[128]{};
            HDITEM item{};
            item.mask = HDI_TEXT;
            item.pszText = buffer;
            item.cchTextMax = _countof(buffer);
            SendMessage(header, HDM_GETITEMW, i, reinterpret_cast<LPARAM>(&item));

            RECT textRect = itemRect;
            textRect.left += S(12);
            textRect.right -= S(8);
            if ((int)fileSortColumn_ == i)
                textRect.right -= S(20);
            DrawTextW(hdc, buffer, -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

            if ((int)fileSortColumn_ == i)
            {
                const int cx = itemRect.right - S(14);
                const int cy = itemRect.top + (itemRect.bottom - itemRect.top) / 2;
                POINT points[3]{};
                if (fileSortAscending_)
                {
                    points[0] = { cx, cy - S(4) };
                    points[1] = { cx - S(5), cy + S(3) };
                    points[2] = { cx + S(5), cy + S(3) };
                }
                else
                {
                    points[0] = { cx, cy + S(4) };
                    points[1] = { cx - S(5), cy - S(3) };
                    points[2] = { cx + S(5), cy - S(3) };
                }
                HBRUSH brush = CreateSolidBrush(text);
                HGDIOBJ oldBrush = SelectObject(hdc, brush);
                Polygon(hdc, points, 3);
                SelectObject(hdc, oldBrush);
                DeleteObject(brush);
            }

            HPEN pen = CreatePen(PS_SOLID, 1, line);
            HGDIOBJ oldPen = SelectObject(hdc, pen);
            MoveToEx(hdc, itemRect.right - 1, itemRect.top, nullptr);
            LineTo(hdc, itemRect.right - 1, itemRect.bottom);
            MoveToEx(hdc, itemRect.left, itemRect.bottom - 1, nullptr);
            LineTo(hdc, itemRect.right, itemRect.bottom - 1);
            SelectObject(hdc, oldPen);
            DeleteObject(pen);
        }
        SelectObject(hdc, oldFont);
    }

    void LayoutButtons()
    {
        buttons_.clear();
        const int topY = S(46);
        AddButton(OpenId, L"Open", std::max<LONG>(S(760), client_.right - S(290)), topY, S(188), S(56), false);
        AddButton(ThemeId, darkTheme_ ? L"\xE708" : L"\xE706", client_.right - S(76), topY + S(8), S(48), S(40), true);

        int x = S(32);
        const int by = bottomRect_.top + S(20);
        AddButton(RestartId, L"\xE72C", x, by, S(70), S(56), true); x += S(84);
        AddButton(PrevId, L"\xE100", x, by, S(70), S(56), true); x += S(84);
        AddButton(PlayId, player_ && player_->IsPlaying() ? L"\xE769" : L"\xE768", x, by, S(70), S(56), true); x += S(84);
        AddButton(NextId, L"\xE101", x, by, S(70), S(56), true); x += S(84);
        AddButton(StopId, L"\xE71A", x, by, S(70), S(56), true); x += S(84);
        AddButton(OrderId, L"\xE8EE", x, by, S(70), S(56), true); x += S(110);

        volumeRect_ = { client_.right - S(255), by + S(8), client_.right - S(74), by + S(56) };
        const LONG outputRight = volumeRect_.left - S(18);
        const LONG outputLeft = std::min<LONG>(outputRight - S(150), std::max<LONG>(S(650), volumeRect_.left - S(305)));
        outputComboRect_ = { outputLeft, by + S(25), outputRight, by + S(51) };
    }

    void AddButton(int id, const std::wstring& text, int x, int y, int w, int h, bool icon)
    {
        buttons_.push_back(Button{ RECT{ x, y, x + w, y + h }, text, id, icon });
    }

    std::wstring OrderText() const
    {
        switch (order_)
        {
        case PlaybackOrder::RepeatOne: return L"Repeat 1";
        case PlaybackOrder::Shuffle: return L"Shuffle";
        default: return L"Sequential";
        }
    }

    void Paint()
    {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd_, &ps);
        RECT dirty = ps.rcPaint;
        if (dirty.right <= dirty.left || dirty.bottom <= dirty.top)
            dirty = client_;
        const int dirtyWidth = std::max<LONG>(1, dirty.right - dirty.left);
        const int dirtyHeight = std::max<LONG>(1, dirty.bottom - dirty.top);
        HDC memoryDc = CreateCompatibleDC(hdc);
        HBITMAP bitmap = CreateCompatibleBitmap(hdc, dirtyWidth, dirtyHeight);
        HGDIOBJ oldBitmap = SelectObject(memoryDc, bitmap);
        POINT oldOrigin{};
        SetViewportOrgEx(memoryDc, -dirty.left, -dirty.top, &oldOrigin);
        Graphics g(memoryDc);
        g.SetSmoothingMode(SmoothingModeHighQuality);
        DrawBackground(g);
        RECT topRect{ 0, 0, client_.right, S(kTopHeight) };
        RECT browserPaintRect = browserRect_;
        UnionRect(&browserPaintRect, &browserPaintRect, &horizontalSplitterRect_);
        UnionRect(&browserPaintRect, &browserPaintRect, &verticalSplitterRect_);
        if (RectsIntersect(dirty, topRect))
            DrawTop(g);
        if (RectsIntersect(dirty, browserPaintRect))
            DrawBrowser(g);
        if (RectsIntersect(dirty, waveRect_))
            DrawWaveform(g);
        if (RectsIntersect(dirty, bottomRect_))
            DrawBottom(g);
        SetViewportOrgEx(memoryDc, oldOrigin.x, oldOrigin.y, nullptr);
        BitBlt(hdc, dirty.left, dirty.top, dirtyWidth, dirtyHeight, memoryDc, 0, 0, SRCCOPY);
        SelectObject(memoryDc, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(memoryDc);
        EndPaint(hwnd_, &ps);
    }

    bool RectsIntersect(const RECT& a, const RECT& b) const
    {
        RECT intersection{};
        return IntersectRect(&intersection, &a, &b) != FALSE;
    }

    Color Bg() const { return darkTheme_ ? Color(255, 18, 20, 24) : Color(255, 246, 247, 249); }
    Color Panel() const { return darkTheme_ ? Color(255, 29, 32, 38) : Color(255, 255, 255, 255); }
    Color Text() const { return darkTheme_ ? Color(255, 238, 242, 246) : Color(255, 30, 36, 44); }
    Color Muted() const { return darkTheme_ ? Color(255, 146, 156, 168) : Color(255, 91, 101, 112); }
    Color Line() const { return darkTheme_ ? Color(255, 53, 58, 68) : Color(255, 221, 225, 231); }
    Color Accent() const { return Color(255, 255, 122, 61); }
    Color HoverBlue() const { return Color(255, 80, 190, 255); }

    void DrawBackground(Graphics& g)
    {
        SolidBrush brush(Bg());
        g.FillRectangle(&brush, 0, 0, client_.right, client_.bottom);
    }

    void DrawTop(Graphics& g)
    {
        SolidBrush panel(Panel());
        g.FillRectangle(&panel, 0, 0, client_.right, S(kTopHeight));
        const int titleTop = S(24);
        const int titleHeight = S(52);
        DrawTextInRect(g, player_ && player_->IsPlaying() ? L"Played" : L"Paused", Rect(S(32), titleTop, S(120), titleHeight), 28, Accent(), FontStyleBold, L"Segoe UI", StringAlignmentNear);
        DrawTextInRect(g, currentFile_.empty() ? L"Open an audio file" : FileNameOf(currentFile_), Rect(S(160), titleTop, std::max<LONG>(S(300), client_.right - S(760)), titleHeight), 30, Text(), FontStyleBold, L"Segoe UI", StringAlignmentNear);
        DrawTextInRect(g, FormatTime(Duration()), Rect(std::min<LONG>(client_.right - S(540), S(610)), titleTop, S(180), titleHeight), 30, Muted(), FontStyleBold, L"Segoe UI", StringAlignmentNear);
        DrawTextLine(g, formatLine_, S(32), S(86), 20, Muted(), FontStyleRegular, S(900));
        for (const auto& button : buttons_)
            if (button.id == OpenId || button.id == ThemeId)
                DrawButton(g, button);
    }

    void DrawBrowser(Graphics& g)
    {
        SolidBrush panel(Panel());
        Pen line(Line(), 1);
        g.FillRectangle(&panel, (INT)browserRect_.left, (INT)browserRect_.top, (INT)(browserRect_.right - browserRect_.left), (INT)(browserRect_.bottom - browserRect_.top));
        g.DrawRectangle(&line, (INT)browserRect_.left, (INT)browserRect_.top, (INT)(browserRect_.right - browserRect_.left), (INT)(browserRect_.bottom - browserRect_.top));
        SolidBrush splitter(darkTheme_ ? Color(255, 37, 42, 50) : Color(255, 225, 229, 236));
        g.FillRectangle(&splitter, (INT)horizontalSplitterRect_.left, (INT)horizontalSplitterRect_.top,
            (INT)(horizontalSplitterRect_.right - horizontalSplitterRect_.left), (INT)(horizontalSplitterRect_.bottom - horizontalSplitterRect_.top));
        g.FillRectangle(&splitter, (INT)verticalSplitterRect_.left, (INT)verticalSplitterRect_.top,
            (INT)(verticalSplitterRect_.right - verticalSplitterRect_.left), (INT)(verticalSplitterRect_.bottom - verticalSplitterRect_.top));
    }

    void DrawWaveform(Graphics& g)
    {
        const int width = waveRect_.right - waveRect_.left;
        const int height = waveRect_.bottom - waveRect_.top;
        if (width <= 0 || height <= 0)
            return;

        if ((dragMode_ == DragMode::HorizontalSplitter || inWindowResize_) && waveBaseBitmap_ && wavePlayedBitmap_)
        {
            DrawStretchedWaveCache(g, width, height);
            return;
        }

        EnsureWaveCache(width, height);
        HDC target = g.GetHDC();
        HDC source = CreateCompatibleDC(target);
        if (waveBaseBitmap_)
        {
            HGDIOBJ old = SelectObject(source, waveBaseBitmap_);
            BitBlt(target, waveRect_.left, waveRect_.top, width, height, source, 0, 0, SRCCOPY);
            SelectObject(source, old);
        }

        const double progress = Duration() > 0 ? std::clamp(CurrentPosition() / Duration(), 0.0, 1.0) : 0.0;
        const int playedWidth = (int)std::round(width * progress);
        if (wavePlayedBitmap_ && playedWidth > 0)
        {
            HGDIOBJ old = SelectObject(source, wavePlayedBitmap_);
            BitBlt(target, waveRect_.left, waveRect_.top, playedWidth, height, source, 0, 0, SRCCOPY);
            SelectObject(source, old);
        }
        DeleteDC(source);
        g.ReleaseHDC(target);

        Pen accent(Accent(), 2);
        const int seekX = waveRect_.left + playedWidth;
        g.DrawLine(&accent, seekX, waveRect_.top, seekX, waveRect_.bottom);
        DrawWavePositionLabel(g, seekX, FormatTime(CurrentPosition()), Accent(), waveRect_.bottom - S(46));
        DrawWaveHover(g);
    }

    void DrawStretchedWaveCache(Graphics& g, int width, int height)
    {
        HDC target = g.GetHDC();
        HDC source = CreateCompatibleDC(target);
        HGDIOBJ old = SelectObject(source, waveBaseBitmap_);
        StretchBlt(target, waveRect_.left, waveRect_.top, width, height, source, 0, 0, waveCacheWidth_, waveCacheHeight_, SRCCOPY);
        const double progress = Duration() > 0 ? std::clamp(CurrentPosition() / Duration(), 0.0, 1.0) : 0.0;
        const int playedWidth = (int)std::round(width * progress);
        if (playedWidth > 0)
        {
            SelectObject(source, wavePlayedBitmap_);
            const int sourcePlayedWidth = std::max(1, (int)std::round(waveCacheWidth_ * progress));
            StretchBlt(target, waveRect_.left, waveRect_.top, playedWidth, height, source, 0, 0, sourcePlayedWidth, waveCacheHeight_, SRCCOPY);
        }
        SelectObject(source, old);
        DeleteDC(source);
        g.ReleaseHDC(target);

        Pen accent(Accent(), 2);
        const int seekX = waveRect_.left + playedWidth;
        g.DrawLine(&accent, seekX, waveRect_.top, seekX, waveRect_.bottom);
        DrawWavePositionLabel(g, seekX, FormatTime(CurrentPosition()), Accent(), waveRect_.bottom - S(46));
        DrawWaveHover(g);
    }

    void DrawWavePositionLabel(Graphics& g, int markerX, const std::wstring& text, Color bg, int y)
    {
        const int labelWidth = S(78);
        const int labelHeight = S(25);
        int x = markerX + S(8);
        if (x + labelWidth > waveRect_.right - S(4))
            x = markerX - labelWidth - S(8);
        x = std::clamp(x, (int)waveRect_.left + S(4), (int)waveRect_.right - labelWidth - S(4));
        y = std::clamp(y, (int)waveRect_.top + S(6), (int)waveRect_.bottom - labelHeight - S(6));

        SolidBrush brush(bg);
        g.FillRectangle(&brush, x, y, labelWidth, labelHeight);
        DrawTextInRect(g, text, Rect(x, y, labelWidth, labelHeight), 13, Color(255, 255, 255, 255), FontStyleBold, L"Consolas", StringAlignmentCenter);
    }

    void DrawWaveHover(Graphics& g)
    {
        if (!waveHover_ || Duration() <= 0)
            return;
        const int x = std::clamp<int>(waveHoverPoint_.x, (int)waveRect_.left, (int)waveRect_.right);
        const double progress = (double)(x - waveRect_.left) / std::max<LONG>(1, waveRect_.right - waveRect_.left);
        const double seconds = std::clamp(progress, 0.0, 1.0) * Duration();
        Pen hoverPen(HoverBlue(), 2);
        g.DrawLine(&hoverPen, x, waveRect_.top, x, waveRect_.bottom);
        DrawWavePositionLabel(g, x, FormatTime(seconds), HoverBlue(), waveRect_.top + S(8));
    }

    void EnsureWaveCache(int width, int height)
    {
        if (waveBaseBitmap_ &&
            wavePlayedBitmap_ &&
            waveCacheWidth_ == width &&
            waveCacheHeight_ == height &&
            waveCacheVersion_ == waveformVersion_ &&
            waveCacheDarkTheme_ == darkTheme_)
        {
            return;
        }

        ClearWaveCache();
        waveCacheWidth_ = width;
        waveCacheHeight_ = height;
        waveCacheVersion_ = waveformVersion_;
        waveCacheDarkTheme_ = darkTheme_;

        HDC screen = GetDC(hwnd_);
        waveBaseBitmap_ = CreateCompatibleBitmap(screen, width, height);
        wavePlayedBitmap_ = CreateCompatibleBitmap(screen, width, height);
        HDC memory = CreateCompatibleDC(screen);
        RenderWaveBitmap(memory, waveBaseBitmap_, width, height, darkTheme_ ? Color(255, 124, 138, 152) : Color(255, 95, 109, 125));
        RenderWaveBitmap(memory, wavePlayedBitmap_, width, height, darkTheme_ ? Color(255, 204, 216, 224) : Color(255, 60, 76, 94));
        DeleteDC(memory);
        ReleaseDC(hwnd_, screen);
    }

    void RenderWaveBitmap(HDC memory, HBITMAP bitmap, int width, int height, Color waveColor)
    {
        HGDIOBJ old = SelectObject(memory, bitmap);
        Graphics g(memory);
        g.SetSmoothingMode(SmoothingModeNone);
        SolidBrush waveBg(Bg());
        g.FillRectangle(&waveBg, 0, 0, width, height);
        Pen grid(Line(), 1);
        for (int i = 1; i < 8; ++i)
        {
            const int x = width * i / 8;
            g.DrawLine(&grid, x, 0, x, height);
        }
        for (int i = 1; i < 4; ++i)
        {
            const int y = height * i / 4;
            g.DrawLine(&grid, 0, y, width, y);
        }

        if (waveform_.peaks.size() < 2)
        {
            SelectObject(memory, old);
            return;
        }

        const int channels = std::max(1, waveform_.channels);
        const int valuesPerPoint = std::max(1, waveform_.channels) * 2;
        const int points = (int)waveform_.peaks.size() / valuesPerPoint;
        if (points <= 0)
        {
            SelectObject(memory, old);
            return;
        }
        SolidBrush waveBrush(waveColor);
        std::vector<float> minColumns(width * channels);
        std::vector<float> maxColumns(width * channels);

        for (int x = 0; x < width; ++x)
        {
            const int start = std::clamp((int)std::floor((double)x * points / width), 0, points - 1);
            const int end = std::clamp((int)std::ceil((double)(x + 1) * points / width), start + 1, points);
            for (int ch = 0; ch < channels; ++ch)
            {
                float mn = 0, mx = 0;
                for (int i = start; i < end; ++i)
                {
                    const int offset = i * valuesPerPoint + ch * 2;
                    mn = std::min(mn, waveform_.peaks[offset]);
                    mx = std::max(mx, waveform_.peaks[offset + 1]);
                }
                minColumns[x * channels + ch] = mn;
                maxColumns[x * channels + ch] = mx;
            }
        }

        auto smoothColumn = [&](const std::vector<float>& values, int x, int ch) {
            constexpr int weights[] = { 1, 2, 4, 2, 1 };
            float sum = 0;
            int weightSum = 0;
            for (int k = -2; k <= 2; ++k)
            {
                const int sx = std::clamp(x + k, 0, width - 1);
                const int weight = weights[k + 2];
                sum += values[sx * channels + ch] * weight;
                weightSum += weight;
            }
            return sum / (float)weightSum;
        };

        for (int x = 0; x < width; ++x)
        {
            for (int ch = 0; ch < channels; ++ch)
            {
                const float mn = smoothColumn(minColumns, x, ch);
                const float mx = smoothColumn(maxColumns, x, ch);
                const int channelTop = height * ch / channels;
                const int channelBottom = height * (ch + 1) / channels;
                const int channelHeight = channelBottom - channelTop;
                const int center = channelTop + channelHeight / 2;
                const double amp = channelHeight * 0.44;
                const int top = (int)std::round(center - mx * amp);
                const int bottom = (int)std::round(center - mn * amp);
                g.FillRectangle(&waveBrush, x, top, 1, std::max(1, bottom - top));
            }
        }
        SelectObject(memory, old);
    }

    void ClearWaveCache()
    {
        if (waveBaseBitmap_)
        {
            DeleteObject(waveBaseBitmap_);
            waveBaseBitmap_ = nullptr;
        }
        if (wavePlayedBitmap_)
        {
            DeleteObject(wavePlayedBitmap_);
            wavePlayedBitmap_ = nullptr;
        }
        waveCacheWidth_ = 0;
        waveCacheHeight_ = 0;
    }

    void DrawBottom(Graphics& g)
    {
        SolidBrush panel(Panel());
        Pen line(Line(), 1);
        g.FillRectangle(&panel, (INT)bottomRect_.left, (INT)bottomRect_.top, (INT)(bottomRect_.right - bottomRect_.left), (INT)(bottomRect_.bottom - bottomRect_.top));
        g.DrawLine(&line, 0, bottomRect_.top, bottomRect_.right, bottomRect_.top);

        LayoutButtons();
        for (const auto& button : buttons_)
            if (button.id != OpenId && button.id != ThemeId)
                DrawButton(g, button);

        DrawVolume(g);
        DrawTextLine(g, L"Output", outputComboRect_.left, bottomRect_.top + S(7), 13, Muted(), FontStyleBold, outputComboRect_.right - outputComboRect_.left);
        DrawTextLine(g, FormatTime(CurrentPosition()), S(560), bottomRect_.top + S(27), 28, Text(), FontStyleRegular, S(200));
    }

    void DrawVolume(Graphics& g)
    {
        SolidBrush back(Line());
        SolidBrush fill(Accent());
        DrawTextLine(g, L"Vol", volumeRect_.left - S(54), volumeRect_.top + S(10), 16, Accent(), FontStyleBold, S(42), StringAlignmentCenter);
        g.FillRectangle(&back, (INT)volumeRect_.left, (INT)volumeRect_.top, (INT)(volumeRect_.right - volumeRect_.left), (INT)(volumeRect_.bottom - volumeRect_.top));
        const int fillWidth = (int)((volumeRect_.right - volumeRect_.left) * volume_);
        g.FillRectangle(&fill, volumeRect_.left, volumeRect_.top, fillWidth, volumeRect_.bottom - volumeRect_.top);
        DrawTextLine(g, std::format(L"{}", (int)std::round(volume_ * 100)), volumeRect_.left, volumeRect_.top + S(12), 17, Color(255, 255, 255, 255), FontStyleBold, volumeRect_.right - volumeRect_.left, StringAlignmentCenter);
    }

    void DrawMeters(Graphics& g, int x, int y)
    {
        DrawTextLine(g, L"L", x, y, 13, Muted(), FontStyleBold);
        DrawMeter(g, x + 20, y + 4, leftDb_);
        DrawTextLine(g, FormatDb(leftDb_), x + 112, y, 13, Muted(), FontStyleRegular);
        DrawTextLine(g, L"R", x, y + 28, 13, Muted(), FontStyleBold);
        DrawMeter(g, x + 20, y + 32, rightDb_);
        DrawTextLine(g, FormatDb(rightDb_), x + 112, y + 28, 13, Muted(), FontStyleRegular);
    }

    void DrawMeter(Graphics& g, int x, int y, double db)
    {
        SolidBrush back(Line());
        SolidBrush fill(Accent());
        g.FillRectangle(&back, x, y, 84, 10);
        g.FillRectangle(&fill, x, y, (int)(84 * DbToMeter(db)), 10);
    }

    void DrawButton(Graphics& g, const Button& button)
    {
        SolidBrush brush(darkTheme_ ? Color(255, 39, 43, 51) : Color(255, 238, 241, 245));
        Pen border(Line(), 1);
        Rect rect(button.rect.left, button.rect.top, button.rect.right - button.rect.left, button.rect.bottom - button.rect.top);
        g.FillRectangle(&brush, rect);
        g.DrawRectangle(&border, rect);
        DrawTextInRect(g, button.text, rect, button.icon ? 22 : 13, Text(), FontStyleBold, button.icon ? L"Segoe MDL2 Assets" : L"Segoe UI");
    }

    void DrawTextInRect(
        Graphics& g,
        const std::wstring& text,
        const Rect& rect,
        int size,
        Color color,
        INT style,
        const wchar_t* familyName,
        StringAlignment align = StringAlignmentCenter)
    {
        FontFamily family(familyName);
        Font font(&family, (REAL)S(size), style, UnitPixel);
        SolidBrush brush(color);
        StringFormat format;
        format.SetTrimming(StringTrimmingEllipsisCharacter);
        format.SetFormatFlags(StringFormatFlagsNoWrap);
        format.SetAlignment(align);
        format.SetLineAlignment(StringAlignmentCenter);
        RectF textRect((REAL)rect.X, (REAL)rect.Y, (REAL)rect.Width, (REAL)rect.Height);
        g.DrawString(text.c_str(), -1, &font, textRect, &format, &brush);
    }

    void DrawTextLine(
        Graphics& g,
        const std::wstring& text,
        int x,
        int y,
        int size,
        Color color,
        INT style,
        int width = 900,
        StringAlignment align = StringAlignmentNear,
        std::optional<Color> background = std::nullopt)
    {
        FontFamily family(L"Segoe UI");
        Font font(&family, (REAL)S(size), style, UnitPixel);
        SolidBrush brush(color);
        StringFormat format;
        format.SetTrimming(StringTrimmingEllipsisCharacter);
        format.SetFormatFlags(StringFormatFlagsNoWrap);
        format.SetAlignment(align);
        RectF rect((REAL)x, (REAL)y, (REAL)width, (REAL)S(size + 8));
        if (background)
        {
            SolidBrush bg(*background);
            g.FillRectangle(&bg, rect);
        }
        g.DrawString(text.c_str(), -1, &font, rect, &format, &brush);
    }

    void OnMouseDown(int x, int y)
    {
        POINT pt{ x, y };
        if (PtInRect(&horizontalSplitterRect_, pt))
        {
            dragMode_ = DragMode::HorizontalSplitter;
            lastDragPoint_ = pt;
            SetCapture(hwnd_);
            return;
        }
        if (PtInRect(&verticalSplitterRect_, pt))
        {
            dragMode_ = DragMode::VerticalSplitter;
            lastDragPoint_ = pt;
            SetCapture(hwnd_);
            return;
        }
        for (const auto& button : buttons_)
        {
            if (PtInRect(&button.rect, pt))
            {
                ExecuteButton(button.id);
                return;
            }
        }
        if (PtInRect(&waveRect_, pt) && Duration() > 0)
        {
            dragMode_ = DragMode::Seek;
            SetCapture(hwnd_);
            SeekFromX(x);
            if (player_ && !player_->IsPlaying() && !currentFile_.empty())
                StartPlayback(true);
            return;
        }
        if (PtInRect(&volumeRect_, pt))
        {
            dragMode_ = DragMode::Volume;
            SetCapture(hwnd_);
            SetVolumeFromX(x);
        }
    }

    void OnMouseMove(int x, int y)
    {
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd_, &pt);
        UNREFERENCED_PARAMETER(x);
        UNREFERENCED_PARAMETER(y);
        lastDragPoint_ = pt;
        const bool wasWaveHover = waveHover_;
        if (PtInRect(&waveRect_, pt))
        {
            waveHover_ = true;
            waveHoverPoint_ = pt;
            if (!trackingMouseLeave_)
            {
                TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd_, 0 };
                trackingMouseLeave_ = TrackMouseEvent(&track) != FALSE;
            }
        }
        else
        {
            waveHover_ = false;
        }

        if (PtInRect(&horizontalSplitterRect_, pt))
            SetCursor(LoadCursor(nullptr, IDC_SIZENS));
        else if (PtInRect(&verticalSplitterRect_, pt))
            SetCursor(LoadCursor(nullptr, IDC_SIZEWE));

        if (dragMode_ == DragMode::Seek)
            SeekFromX(x);
        else if (dragMode_ == DragMode::Volume)
            SetVolumeFromX(x);

        if (wasWaveHover != waveHover_ ||
            waveHover_)
        {
            RequestWaveRedraw();
        }
    }

    void OnMouseLeave()
    {
        trackingMouseLeave_ = false;
        if (waveHover_)
        {
            waveHover_ = false;
            RequestWaveRedraw();
        }
    }

    bool CursorInsideChild(HWND child) const
    {
        if (!child)
            return false;
        POINT pt{};
        GetCursorPos(&pt);
        RECT rect{};
        GetWindowRect(child, &rect);
        return PtInRect(&rect, pt) != FALSE;
    }

    void OnMouseWheel(int delta)
    {
        if (CursorInsideChild(treeView_))
        {
            AddTreeScrollImpulse(-delta);
            return;
        }
        if (CursorInsideChild(fileList_))
        {
            AddFileListScrollImpulse(-delta);
            return;
        }
        SetVolume(volume_ + (delta > 0 ? 0.05f : -0.05f));
    }

    LRESULT HandleTreeClick(int x, int y, bool toggleExpansion)
    {
        POINT pt{ x, y };
        HTREEITEM item = TreeItemFromFullRowPoint(pt);
        if (!item)
            return 0;

        const std::wstring path = GetTreePath(item);
        const bool onExpander = IsTreeExpanderPoint(item, pt);
        selectedTreeItem_ = item;

        if (onExpander)
        {
            if (!toggleExpansion)
                ToggleTreeItemExpansion(item);
        }
        else if (toggleExpansion)
        {
            ToggleTreeItemExpansion(item);
        }

        if (!path.empty())
            SetCurrentFolder(path, L"");

        RedrawWindow(treeView_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE);
        return 0;
    }

    HTREEITEM TreeItemFromFullRowPoint(POINT pt) const
    {
        RECT client{};
        GetClientRect(treeView_, &client);
        if (!PtInRect(&client, pt))
            return nullptr;
        const auto items = VisibleTreeItems();
        const int index = (pt.y + treeScrollOffset_) / TreeRowHeight();
        return index >= 0 && index < (int)items.size() ? items[index] : nullptr;
    }

    bool IsTreeExpanderPoint(HTREEITEM item, POINT pt) const
    {
        if (!item || !TreeView_GetChild(treeModel_, item))
            return false;

        const int index = TreeVisibleIndex(item);
        if (index < 0)
            return false;

        const int rowHeight = TreeRowHeight();
        const int rowTop = index * rowHeight - treeScrollOffset_;
        const int rowBottom = rowTop + rowHeight;
        if (pt.y < rowTop || pt.y >= rowBottom)
            return false;

        const int indent = (int)SendMessage(treeModel_, TVM_GETINDENT, 0, 0);
        const int left = S(8) + TreeItemLevel(item) * indent;
        RECT expanderRect{ left - S(6), rowTop, left + S(20), rowBottom };
        return PtInRect(&expanderRect, pt) != FALSE;
    }

    void ToggleTreeItemExpansion(HTREEITEM item)
    {
        if (!item || !TreeView_GetChild(treeModel_, item))
            return;
        const bool expanded = (TreeView_GetItemState(treeModel_, item, TVIS_EXPANDED) & TVIS_EXPANDED) != 0;
        if (!expanded)
            EnsureTreeChildrenLoaded(item);
        TreeView_Expand(treeModel_, item, expanded ? TVE_COLLAPSE : TVE_EXPAND);
        UpdateTreeScrollInfo();
        RedrawWindow(treeView_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE);
    }

    LRESULT HandleFileListClick(int x, int y)
    {
        const int rowHeight = FileListRowHeight();
        const int headerBottom = FileListHeaderBottom();
        if (y < headerBottom)
            return DefSubclassProc(fileList_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
        const int index = (y - headerBottom + fileListScrollOffset_) / rowHeight;
        if (index < 0 || index >= (int)files_.size())
            return 0;
        ListView_SetItemState(fileList_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(fileList_, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        RedrawWindow(fileList_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
        LoadFile(files_[index].path, true);
        return 0;
    }

    void RedrawWaveNow()
    {
        const int width = waveRect_.right - waveRect_.left;
        const int height = waveRect_.bottom - waveRect_.top;
        if (width <= 0 || height <= 0)
            return;

        HDC screen = GetDC(hwnd_);
        if (!screen)
            return;
        HDC memory = CreateCompatibleDC(screen);
        HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
        HGDIOBJ oldBitmap = SelectObject(memory, bitmap);
        POINT oldOrigin{};
        SetViewportOrgEx(memory, -waveRect_.left, -waveRect_.top, &oldOrigin);
        {
            Graphics g(memory);
            g.SetSmoothingMode(SmoothingModeHighQuality);
            DrawWaveform(g);
        }
        SetViewportOrgEx(memory, oldOrigin.x, oldOrigin.y, nullptr);
        BitBlt(screen, waveRect_.left, waveRect_.top, width, height, memory, 0, 0, SRCCOPY);
        SelectObject(memory, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(memory);
        ReleaseDC(hwnd_, screen);
    }

    void RequestWaveRedraw()
    {
        const ULONGLONG now = GetTickCount64();
        if (now - lastWaveRedrawRequestTick_ < kPlaybackUiIntervalMs)
            return;
        lastWaveRedrawRequestTick_ = now;
        InvalidateRect(hwnd_, &waveRect_, FALSE);
    }

    void UpdateWaveHoverFromCursor()
    {
        if (!waveHover_ && dragMode_ != DragMode::Seek)
            return;
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd_, &pt);
        const bool nextHover = PtInRect(&waveRect_, pt);
        if (nextHover)
        {
            if (!waveHover_ || pt.x != waveHoverPoint_.x || pt.y != waveHoverPoint_.y)
            {
                waveHover_ = true;
                waveHoverPoint_ = pt;
                RequestWaveRedraw();
            }
        }
        else if (waveHover_)
        {
            waveHover_ = false;
            RequestWaveRedraw();
        }
    }

    void RedrawLayoutDuringResize()
    {
        RedrawWindow(treeView_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE);
        RedrawWindow(fileList_, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE);
        RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }

    void UpdateSplitterDragFromCursor()
    {
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd_, &pt);
        lastDragPoint_ = pt;

        if (dragMode_ == DragMode::HorizontalSplitter)
        {
            const LONG waveTop = S(kTopHeight) + S(kWaveMargin);
            const LONG waveHeight = std::clamp<LONG>(pt.y - waveTop, S(160), std::max<LONG>(S(160), bottomRect_.top - waveTop - S(kSplitterSize) - S(180)));
            const double nextRatio = std::clamp((double)waveHeight / std::max<LONG>(1, client_.bottom), 0.20, 0.70);
            if (std::abs(nextRatio - waveHeightRatio_) > 0.001)
            {
                waveHeightRatio_ = nextRatio;
                Layout();
                RedrawLayoutDuringResize();
            }
        }
        else if (dragMode_ == DragMode::VerticalSplitter)
        {
            const LONG browserWidth = std::max<LONG>(1, browserRect_.right - browserRect_.left);
            const double nextRatio = std::clamp((double)(pt.x - browserRect_.left) / browserWidth, 0.18, 0.72);
            if (std::abs(nextRatio - treeWidthRatio_) > 0.001)
            {
                treeWidthRatio_ = nextRatio;
                Layout();
                RedrawLayoutDuringResize();
            }
        }
    }

    void FinishDrag()
    {
        const DragMode endedDrag = dragMode_;
        dragMode_ = DragMode::None;
        ReleaseCapture();
        if (endedDrag == DragMode::HorizontalSplitter)
        {
            ClearWaveCache();
        }
        RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_ERASE);
    }

    void OnCommand(WORD id, WORD notify)
    {
        if (id == kAudioOutputComboId && notify == CBN_SELCHANGE && !suppressOutputChange_)
            OnAudioOutputChanged();
    }

    void RefreshAudioOutputs()
    {
        const std::wstring previousId = selectedOutputId_;
        outputDevices_.clear();
        outputDevices_.push_back(AudioOutputDevice{ L"", L"System default", false });
        auto devices = EnumerateAudioOutputDevices();
        outputDevices_.insert(outputDevices_.end(), std::make_move_iterator(devices.begin()), std::make_move_iterator(devices.end()));

        bool keepPrevious = previousId.empty();
        for (const auto& device : outputDevices_)
        {
            if (!previousId.empty() && SameText(device.id, previousId))
            {
                keepPrevious = true;
                break;
            }
        }
        selectedOutputId_ = keepPrevious ? previousId : L"";
        PopulateOutputCombo();
    }

    void PopulateOutputCombo()
    {
        if (!outputCombo_)
            return;
        suppressOutputChange_ = true;
        SendMessage(outputCombo_, CB_RESETCONTENT, 0, 0);
        int selectedIndex = 0;
        for (int i = 0; i < (int)outputDevices_.size(); ++i)
        {
            std::wstring label = outputDevices_[i].name;
            if (outputDevices_[i].isDefault)
                label += L" (default)";
            const LRESULT index = SendMessage(outputCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            if (index >= 0)
                SendMessage(outputCombo_, CB_SETITEMDATA, (WPARAM)index, (LPARAM)i);
            if (SameText(outputDevices_[i].id, selectedOutputId_))
                selectedIndex = i;
        }
        SendMessage(outputCombo_, CB_SETCURSEL, selectedIndex, 0);
        suppressOutputChange_ = false;
    }

    void OnAudioOutputChanged()
    {
        const LRESULT selected = SendMessage(outputCombo_, CB_GETCURSEL, 0, 0);
        if (selected == CB_ERR)
            return;
        const LRESULT itemData = SendMessage(outputCombo_, CB_GETITEMDATA, (WPARAM)selected, 0);
        if (itemData == CB_ERR || itemData < 0 || itemData >= (LRESULT)outputDevices_.size())
            return;
        const std::wstring nextOutputId = outputDevices_[(size_t)itemData].id;
        if (SameText(selectedOutputId_, nextOutputId))
            return;
        selectedOutputId_ = nextOutputId;
        SaveAudioOutputId(selectedOutputId_);
        ReopenCurrentFileOnSelectedOutput();
    }

    void ReopenCurrentFileOnSelectedOutput()
    {
        if (!player_ || currentFile_.empty())
            return;
        EndSeekCrossfade();
        const bool wasPlaying = player_->IsPlaying();
        double position = wasPlaying ? player_->Position() : playbackPosition_;
        position = std::clamp(position, 0.0, Duration());
        if (!player_->Open(currentFile_, selectedOutputId_))
        {
            MessageBox(hwnd_, L"Could not switch to this audio output.", L"Playback error", MB_ICONERROR);
            return;
        }
        player_->SetVolume(volume_);
        playbackPosition_ = position;
        player_->Seek(playbackPosition_);
        const ULONGLONG now = GetTickCount64();
        lastPlaybackClockTick_ = wasPlaying ? now : 0;
        lastMediaPositionQueryTick_ = now;
        if (wasPlaying)
            player_->Play();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    LRESULT OnNotify(NMHDR* header)
    {
        if (!header)
            return 0;
        if (header->hwndFrom == ListView_GetHeader(fileList_))
        {
            if (header->code == NM_CUSTOMDRAW)
                return OnFileHeaderCustomDraw(reinterpret_cast<NMCUSTOMDRAW*>(header));
            return 0;
        }
        if (header->idFrom == 1001 && header->code == NM_CUSTOMDRAW)
            return OnFileListCustomDraw(reinterpret_cast<NMLVCUSTOMDRAW*>(header));
        if (header->idFrom == 1001 && (header->code == NM_CLICK || header->code == NM_DBLCLK))
        {
            const int index = ListView_GetNextItem(fileList_, -1, LVNI_SELECTED);
            if (index >= 0 && index < (int)files_.size())
                LoadFile(files_[index].path, true);
        }
        else if (header->idFrom == 1003 && header->code == NM_CUSTOMDRAW)
        {
            return OnTreeCustomDraw(reinterpret_cast<NMTVCUSTOMDRAW*>(header));
        }
        else if (header->idFrom == 1003 && header->code == TVN_SELCHANGED)
        {
            if (suppressTreeSelection_)
                return 0;
            auto* changed = reinterpret_cast<NMTREEVIEW*>(header);
            const auto path = GetTreePath(changed->itemNew.hItem);
            if (!path.empty())
                SetCurrentFolder(path, L"");
        }
        else if (header->idFrom == 1003 && header->code == TVN_ITEMEXPANDING)
        {
            auto* expanding = reinterpret_cast<NMTREEVIEW*>(header);
            if (expanding->action == TVE_EXPAND)
                EnsureTreeChildrenLoaded(expanding->itemNew.hItem);
        }
        return 0;
    }

    LRESULT OnTreeCustomDraw(NMTVCUSTOMDRAW* draw)
    {
        if (!draw)
            return CDRF_DODEFAULT;
        if (draw->nmcd.dwDrawStage == CDDS_PREPAINT)
        {
            FillControlBackground(draw->nmcd.hdc, treeModel_);
            return CDRF_SKIPDEFAULT;
        }
        return CDRF_SKIPDEFAULT;
    }

    LRESULT OnFileHeaderCustomDraw(NMCUSTOMDRAW* draw)
    {
        if (!draw)
            return CDRF_DODEFAULT;
        if (draw->dwDrawStage == CDDS_PREPAINT)
            return CDRF_NOTIFYITEMDRAW;
        if (draw->dwDrawStage != CDDS_ITEMPREPAINT)
            return CDRF_DODEFAULT;

        const bool dark = darkTheme_;
        const COLORREF bg = dark ? RGB(24, 28, 34) : RGB(245, 247, 250);
        const COLORREF text = dark ? RGB(180, 195, 214) : RGB(45, 52, 62);
        const COLORREF line = dark ? RGB(49, 57, 68) : RGB(198, 205, 214);
        HBRUSH brush = CreateSolidBrush(bg);
        FillRect(draw->hdc, &draw->rc, brush);
        DeleteObject(brush);

        wchar_t buffer[128]{};
        HDITEM item{};
        item.mask = HDI_TEXT;
        item.pszText = buffer;
        item.cchTextMax = _countof(buffer);
        SendMessage(draw->hdr.hwndFrom, HDM_GETITEMW, draw->dwItemSpec, reinterpret_cast<LPARAM>(&item));

        RECT textRect = draw->rc;
        textRect.left += S(12);
        textRect.right -= S(8);
        SetBkMode(draw->hdc, TRANSPARENT);
        SetTextColor(draw->hdc, text);
        HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(draw->hdc, font_));
        DrawTextW(draw->hdc, buffer, -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        SelectObject(draw->hdc, oldFont);

        HPEN pen = CreatePen(PS_SOLID, 1, line);
        HGDIOBJ oldPen = SelectObject(draw->hdc, pen);
        MoveToEx(draw->hdc, draw->rc.right - 1, draw->rc.top, nullptr);
        LineTo(draw->hdc, draw->rc.right - 1, draw->rc.bottom);
        MoveToEx(draw->hdc, draw->rc.left, draw->rc.bottom - 1, nullptr);
        LineTo(draw->hdc, draw->rc.right, draw->rc.bottom - 1);
        SelectObject(draw->hdc, oldPen);
        DeleteObject(pen);
        return CDRF_SKIPDEFAULT;
    }

    LRESULT OnFileListCustomDraw(NMLVCUSTOMDRAW* draw)
    {
        if (!draw)
            return CDRF_DODEFAULT;
        switch (draw->nmcd.dwDrawStage)
        {
        case CDDS_PREPAINT:
            return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT:
            return CDRF_NOTIFYSUBITEMDRAW;
        case CDDS_ITEMPREPAINT | CDDS_SUBITEM:
            DrawFileListSubItem(draw);
            return CDRF_SKIPDEFAULT;
        default:
            return CDRF_DODEFAULT;
        }
    }

    void DrawFileListSubItem(NMLVCUSTOMDRAW* draw)
    {
        const int itemIndex = (int)draw->nmcd.dwItemSpec;
        const int subItem = draw->iSubItem;
        RECT rect{};
        ListView_GetSubItemRect(fileList_, itemIndex, subItem, LVIR_BOUNDS, &rect);
        const bool selected = (ListView_GetItemState(fileList_, itemIndex, LVIS_SELECTED) & LVIS_SELECTED) != 0;
        const COLORREF bg = selected
            ? (darkTheme_ ? RGB(50, 60, 72) : RGB(216, 232, 252))
            : (darkTheme_ ? RGB(18, 20, 24) : RGB(255, 255, 255));
        const COLORREF text = darkTheme_ ? RGB(238, 242, 246) : RGB(30, 36, 44);
        const COLORREF line = darkTheme_ ? RGB(45, 52, 62) : RGB(224, 229, 235);
        HBRUSH brush = CreateSolidBrush(bg);
        FillRect(draw->nmcd.hdc, &rect, brush);
        DeleteObject(brush);

        wchar_t buffer[MAX_PATH]{};
        ListView_GetItemText(fileList_, itemIndex, subItem, buffer, _countof(buffer));
        RECT textRect = rect;
        textRect.left += S(subItem == 0 ? 12 : 10);
        textRect.right -= S(8);
        SetBkMode(draw->nmcd.hdc, TRANSPARENT);
        SetTextColor(draw->nmcd.hdc, text);
        HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(draw->nmcd.hdc, font_));
        DrawTextW(draw->nmcd.hdc, buffer, -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        SelectObject(draw->nmcd.hdc, oldFont);

        HPEN pen = CreatePen(PS_SOLID, 1, line);
        HGDIOBJ oldPen = SelectObject(draw->nmcd.hdc, pen);
        MoveToEx(draw->nmcd.hdc, rect.left, rect.bottom - 1, nullptr);
        LineTo(draw->nmcd.hdc, rect.right, rect.bottom - 1);
        MoveToEx(draw->nmcd.hdc, rect.right - 1, rect.top, nullptr);
        LineTo(draw->nmcd.hdc, rect.right - 1, rect.bottom);
        SelectObject(draw->nmcd.hdc, oldPen);
        DeleteObject(pen);
    }

    void ExecuteButton(int id)
    {
        switch (id)
        {
        case OpenId: OpenDialog(); break;
        case PrevId: PlayAdjacent(-1); break;
        case PlayId: TogglePlay(); break;
        case StopId:
            if (player_)
            {
                EndSeekCrossfade();
                player_->Stop();
                playbackPosition_ = 0;
                lastPlaybackClockTick_ = 0;
                lastMediaPositionQueryTick_ = 0;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            break;
        case NextId: PlayAdjacent(1); break;
        case RestartId: Seek(0); break;
        case OrderId: ToggleOrder(); break;
        case ThemeId: darkTheme_ = !darkTheme_; ClearWaveCache(); ApplyCaptionTheme(); ApplyNativeControlTheme(); InvalidateRect(hwnd_, nullptr, TRUE); break;
        case MuteId: ToggleMute(); break;
        default: break;
        }
    }

    void OpenDialog()
    {
        IFileOpenDialog* dialog = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
            return;
        COMDLG_FILTERSPEC filters[] = {
            { L"Audio files", L"*.wav;*.mp3;*.m4a;*.mp4;*.aac;*.wma;*.flac" },
            { L"All files", L"*.*" }
        };
        dialog->SetFileTypes(2, filters);
        dialog->SetTitle(L"Open audio file");
        if (SUCCEEDED(dialog->Show(hwnd_)))
        {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item)))
            {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                {
                    LoadFile(path, true);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }

    void OnDrop(HDROP drop)
    {
        wchar_t path[MAX_PATH]{};
        const UINT count = DragQueryFile(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i)
        {
            if (DragQueryFile(drop, i, path, MAX_PATH) && IsSupportedAudio(path))
            {
                LoadFile(path, true);
                break;
            }
        }
        DragFinish(drop);
    }

    LRESULT OnCopyData(const COPYDATASTRUCT* copyData)
    {
        if (!copyData || copyData->dwData != kCopyDataOpenFile || !copyData->lpData)
            return FALSE;
        if (copyData->cbData < sizeof(wchar_t) || copyData->cbData % sizeof(wchar_t) != 0)
            return FALSE;

        const auto* text = static_cast<const wchar_t*>(copyData->lpData);
        const size_t charCount = copyData->cbData / sizeof(wchar_t);
        std::wstring path(text, text + charCount);
        if (!path.empty() && path.back() == L'\0')
            path.pop_back();
        if (path.empty())
            return FALSE;

        LoadFile(path, true);
        ActivateExistingPlayer(hwnd_);
        return TRUE;
    }

    void OnKeyDown(WPARAM key)
    {
        switch (key)
        {
        case VK_SPACE: TogglePlay(); break;
        case VK_LEFT: Seek(CurrentPosition() - 5); break;
        case VK_RIGHT: Seek(CurrentPosition() + 5); break;
        case VK_UP: SetVolume(volume_ + 0.05f); break;
        case VK_DOWN: SetVolume(volume_ - 0.05f); break;
        case 'O': if (GetKeyState(VK_CONTROL) < 0) OpenDialog(); break;
        default: break;
        }
    }

    void TogglePlay()
    {
        if (!player_ || currentFile_.empty())
            return;
        if (player_->IsPlaying())
        {
            playbackPosition_ = player_->Position();
            lastPlaybackClockTick_ = 0;
            lastMediaPositionQueryTick_ = GetTickCount64();
            player_->Pause();
            EndSeekCrossfade();
        }
        else
        {
            StartPlayback();
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void StartPlayback(bool fadeIn = false)
    {
        if (!player_ || currentFile_.empty())
            return;
        const ULONGLONG now = GetTickCount64();
        if (fadeIn)
            BeginSeekCrossfade(now);
        else
            EndSeekCrossfade();
        player_->Play();
        if (!player_->IsPlaying())
        {
            EndSeekCrossfade();
            return;
        }
        lastPlaybackClockTick_ = now;
        lastMediaPositionQueryTick_ = now;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void BeginSeekCrossfade(ULONGLONG now)
    {
        if (!player_)
            return;
        seekFadePhase_ = SeekFadePhase::FadingIn;
        seekCrossfadeStartTick_ = now;
        SetPlayerFadeScale(0.0f);
    }

    void BeginSeekFadeOut(ULONGLONG now, double targetPosition)
    {
        if (!player_)
            return;
        pendingSeekPosition_ = std::clamp(targetPosition, 0.0, Duration());
        pendingSeekActive_ = true;
        seekFadeOutStartScale_ = currentSeekFadeScale_;
        seekFadePhase_ = SeekFadePhase::FadingOut;
        seekCrossfadeStartTick_ = now;
    }

    void EndSeekCrossfade()
    {
        seekFadePhase_ = SeekFadePhase::None;
        pendingSeekActive_ = false;
        seekCrossfadeStartTick_ = 0;
        pendingSeekPosition_ = 0;
        if (player_)
            SetPlayerFadeScale(1.0f);
    }

    void SetPlayerFadeScale(float scale)
    {
        currentSeekFadeScale_ = std::clamp(scale, 0.0f, 1.0f);
        if (player_)
            player_->SetFadeScale(currentSeekFadeScale_);
    }

    void UpdateSeekCrossfade()
    {
        if (seekFadePhase_ == SeekFadePhase::None || !player_)
            return;
        if (!player_->IsPlaying())
        {
            EndSeekCrossfade();
            return;
        }

        const ULONGLONG now = GetTickCount64();
        const ULONGLONG elapsed = now - seekCrossfadeStartTick_;
        if (seekFadePhase_ == SeekFadePhase::FadingOut)
        {
            const double progress = std::clamp((double)elapsed / (double)kSeekFadeOutMs, 0.0, 1.0);
            const double eased = progress * progress * (3.0 - 2.0 * progress);
            const float scale = (float)(seekFadeOutStartScale_ * (1.0 - eased));
            SetPlayerFadeScale(scale);
            if (progress < 1.0)
                return;

            SetPlayerFadeScale(0.0f);
            if (pendingSeekActive_)
                player_->Seek(pendingSeekPosition_);
            pendingSeekActive_ = false;
            seekFadePhase_ = SeekFadePhase::FadingIn;
            seekCrossfadeStartTick_ = now;
            lastPlaybackClockTick_ = now;
            lastMediaPositionQueryTick_ = now;
            return;
        }

        const double progress = std::clamp((double)elapsed / (double)kSeekFadeInMs, 0.0, 1.0);
        const float scale = (float)(progress * progress * (3.0 - 2.0 * progress));
        SetPlayerFadeScale(scale);
        if (progress >= 1.0)
            EndSeekCrossfade();
    }

    void ToggleOrder()
    {
        if (order_ == PlaybackOrder::Sequential)
            order_ = PlaybackOrder::RepeatOne;
        else if (order_ == PlaybackOrder::RepeatOne)
            order_ = PlaybackOrder::Shuffle;
        else
            order_ = PlaybackOrder::Sequential;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void ToggleMute()
    {
        if (volume_ <= 0)
            SetVolume(previousVolume_ > 0 ? previousVolume_ : 0.8f);
        else
        {
            previousVolume_ = volume_;
            SetVolume(0);
        }
    }

    void SetVolume(float value)
    {
        volume_ = std::clamp(value, 0.0f, 1.0f);
        if (player_)
            player_->SetVolume(volume_);
        InvalidateRect(hwnd_, &bottomRect_, FALSE);
    }

    void SetVolumeFromX(int x)
    {
        const int width = volumeRect_.right - volumeRect_.left;
        SetVolume((float)(x - volumeRect_.left) / std::max(1, width));
    }

    void SeekFromX(int x)
    {
        const int width = waveRect_.right - waveRect_.left;
        const double progress = (double)(x - waveRect_.left) / std::max(1, width);
        Seek(Duration() * std::clamp(progress, 0.0, 1.0));
    }

    void Seek(double seconds)
    {
        if (!player_)
            return;
        const double previousPosition = playbackPosition_;
        const bool wasPlaying = player_->IsPlaying();
        playbackPosition_ = std::clamp(seconds, 0.0, Duration());
        const ULONGLONG now = GetTickCount64();
        if (wasPlaying)
            BeginSeekFadeOut(now, playbackPosition_);
        else
            player_->Seek(playbackPosition_);
        lastPlaybackClockTick_ = wasPlaying ? now : 0;
        lastMediaPositionQueryTick_ = now;
        UpdateRms();
        InvalidatePlaybackCursor(previousPosition, playbackPosition_);
        InvalidateRect(hwnd_, &bottomRect_, FALSE);
    }

    void LoadFile(const std::wstring& path, bool autoplay)
    {
        if (!IsSupportedAudio(path))
        {
            MessageBox(hwnd_, L"Supported formats: WAV, MP3, M4A/MP4, AAC, WMA, FLAC.", L"Unsupported file", MB_ICONINFORMATION);
            return;
        }
        EndSeekCrossfade();
        currentFile_ = path;
        currentFolder_ = fs::path(path).parent_path().wstring();
        formatLine_ = L"Loading with Media Foundation...";
        playbackPosition_ = 0;
        lastPlaybackClockTick_ = 0;
        lastMediaPositionQueryTick_ = 0;
        waveform_ = {};
        ++waveformVersion_;
        ClearWaveCache();
        leftDb_ = rightDb_ = kRmsFloorDb;
        SetCurrentFolder(currentFolder_, path);
        SelectTreePath(currentFolder_);
        InvalidateRect(hwnd_, nullptr, TRUE);

        HRESULT openError = S_OK;
        if (!player_->Open(path, selectedOutputId_, &openError))
        {
            MessageBox(hwnd_,
                std::format(L"Media Foundation could not open this file.\n\nError: {}", FormatHResult(openError)).c_str(),
                L"Playback error",
                MB_ICONERROR);
            return;
        }
        if (autoplay)
            player_->Play();

        const auto token = ++loadToken_;
        if (TryLoadCachedWaveform(path))
        {
            InvalidateRect(hwnd_, nullptr, TRUE);
            return;
        }

        const HWND hwnd = hwnd_;
        ++gWaveformWorkers;
        std::thread([path, token, hwnd]() {
            auto* result = new WaveformResult();
            result->token = token;
            result->path = path;
            std::wstring error;
            result->ok = BuildWaveform(path, result->data, error);
            if (!PostMessage(hwnd, WM_APP + 3, 0, reinterpret_cast<LPARAM>(result)))
                delete result;
            --gWaveformWorkers;
        }).detach();
    }

    bool TryLoadCachedWaveform(const std::wstring& path)
    {
        const auto it = waveformSessionCache_.find(WaveformCacheKey(path));
        if (it == waveformSessionCache_.end())
            return false;

        waveform_ = it->second;
        ++waveformVersion_;
        ClearWaveCache();
        formatLine_ = FormatLine();
        return true;
    }

    const WaveformData& StoreCachedWaveform(const std::wstring& path, WaveformData data)
    {
        auto [it, inserted] = waveformSessionCache_.insert_or_assign(WaveformCacheKey(path), std::move(data));
        UNREFERENCED_PARAMETER(inserted);
        return it->second;
    }

    static std::wstring WaveformCacheKey(const std::wstring& path)
    {
        std::error_code ec;
        auto absolute = fs::weakly_canonical(path, ec);
        if (ec)
        {
            ec.clear();
            absolute = fs::absolute(path, ec);
        }
        std::wstring key = ec ? path : absolute.wstring();
        std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
        return key;
    }

    static bool BuildWaveform(const std::wstring& path, WaveformData& data, std::wstring&)
    {
        IMFSourceReader* reader = nullptr;
        if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader)))
            return false;

        IMFMediaType* type = nullptr;
        IMFMediaType* nativeType = nullptr;
        UINT32 channels = 0, sampleRate = 0, nativeSampleRate = 0, nativeBits = 0;
        reader->GetNativeMediaType(kAudioStream, 0, &nativeType);
        if (nativeType)
        {
            nativeType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
            nativeType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &nativeSampleRate);
            nativeType->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &nativeBits);
            nativeType->Release();
        }
        sampleRate = nativeSampleRate;

        MFCreateMediaType(&type);
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        if (channels > 0)
            type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
        if (sampleRate > 0)
            type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
        if (FAILED(reader->SetCurrentMediaType(kAudioStream, nullptr, type)))
        {
            SafeRelease(&type);
            SafeRelease(&reader);
            return false;
        }
        SafeRelease(&type);

        IMFMediaType* current = nullptr;
        reader->GetCurrentMediaType(kAudioStream, &current);
        if (current)
        {
            current->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
            current->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sampleRate);
            current->Release();
        }
        channels = std::max<UINT32>(1, channels);
        sampleRate = std::max<UINT32>(1, sampleRate);

        PROPVARIANT duration{};
        PropVariantInit(&duration);
        if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration)) && duration.vt == VT_UI8)
            data.durationSeconds = (double)duration.uhVal.QuadPart / 10000000.0;
        PropVariantClear(&duration);

        data.channels = (int)channels;
        data.sampleRate = nativeSampleRate > 0 ? (int)nativeSampleRate : (int)sampleRate;
        data.bitsPerSample = nativeBits > 0 ? (int)nativeBits : 0;
        std::error_code sizeError;
        const auto sourceSize = fs::file_size(path, sizeError);
        if (!sizeError && data.durationSeconds > 0)
            data.bitrateKbps = (int)std::round((double)sourceSize * 8.0 / data.durationSeconds / 1000.0);

        const long long estimatedFrames = data.durationSeconds > 0 ? (long long)(data.durationSeconds * sampleRate) : sampleRate * 60LL;
        const long long peakFrames = std::max<long long>(1, estimatedFrames / kWaveformPeakTarget);
        const long long rmsFrames = std::max<long long>(1, estimatedFrames / kWaveformRmsTarget);

        std::vector<float> mins(channels, 0), maxes(channels, 0);
        double rmsSum[2]{};
        long long globalFrame = 0, peakBucket = -1, rmsBucket = -1, rmsSamples = 0;

        auto flushPeak = [&]() {
            for (UINT32 ch = 0; ch < channels; ++ch)
            {
                data.peaks.push_back(mins[ch]);
                data.peaks.push_back(maxes[ch]);
            }
        };
        auto flushRms = [&]() {
            if (rmsSamples <= 0)
                return;
            data.rms.push_back((float)std::sqrt(rmsSum[0] / rmsSamples));
            data.rms.push_back((float)std::sqrt(rmsSum[1] / rmsSamples));
        };

        while (true)
        {
            DWORD streamIndex = 0, flags = 0;
            LONGLONG timestamp = 0;
            IMFSample* sample = nullptr;
            HRESULT hr = reader->ReadSample(kAudioStream, 0, &streamIndex, &flags, &timestamp, &sample);
            if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM))
            {
                SafeRelease(&sample);
                break;
            }
            if (!sample)
                continue;
            IMFMediaBuffer* buffer = nullptr;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)))
            {
                BYTE* bytes = nullptr;
                DWORD maxLength = 0, currentLength = 0;
                if (SUCCEEDED(buffer->Lock(&bytes, &maxLength, &currentLength)))
                {
                    const auto* values = reinterpret_cast<const float*>(bytes);
                    const DWORD frameCount = currentLength / (sizeof(float) * channels);
                    for (DWORD frame = 0; frame < frameCount; ++frame, ++globalFrame)
                    {
                        const long long pb = globalFrame / peakFrames;
                        if (pb != peakBucket)
                        {
                            if (peakBucket >= 0)
                                flushPeak();
                            peakBucket = pb;
                            std::fill(mins.begin(), mins.end(), 0.0f);
                            std::fill(maxes.begin(), maxes.end(), 0.0f);
                        }
                        const long long rb = globalFrame / rmsFrames;
                        if (rb != rmsBucket)
                        {
                            if (rmsBucket >= 0)
                                flushRms();
                            rmsBucket = rb;
                            rmsSum[0] = rmsSum[1] = 0;
                            rmsSamples = 0;
                        }
                        float left = 0, right = 0;
                        for (UINT32 ch = 0; ch < channels; ++ch)
                        {
                            const float sampleValue = std::clamp(values[frame * channels + ch], -1.0f, 1.0f);
                            mins[ch] = std::min(mins[ch], sampleValue);
                            maxes[ch] = std::max(maxes[ch], sampleValue);
                            if (ch == 0)
                                left = sampleValue;
                            else if (ch == 1)
                                right = sampleValue;
                        }
                        if (channels == 1)
                            right = left;
                        rmsSum[0] += left * left;
                        rmsSum[1] += right * right;
                        ++rmsSamples;
                    }
                    buffer->Unlock();
                }
                buffer->Release();
            }
            sample->Release();
        }
        if (peakBucket >= 0)
            flushPeak();
        if (rmsSamples > 0)
            flushRms();
        reader->Release();
        return !data.peaks.empty();
    }

    static std::optional<double> ReadTrackDuration(const std::wstring& path)
    {
        IMFSourceReader* reader = nullptr;
        if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader)))
            return std::nullopt;

        std::optional<double> seconds;
        PROPVARIANT duration{};
        PropVariantInit(&duration);
        if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration)) && duration.vt == VT_UI8)
            seconds = (double)duration.uhVal.QuadPart / 10000000.0;
        PropVariantClear(&duration);
        reader->Release();
        return seconds;
    }

    void OnWaveformMessage(WaveformResult* result)
    {
        std::unique_ptr<WaveformResult> owned(result);
        if (!owned || owned->token != loadToken_)
            return;
        if (owned->ok)
        {
            waveform_ = StoreCachedWaveform(owned->path, std::move(owned->data));
            ++waveformVersion_;
            ClearWaveCache();
            formatLine_ = FormatLine();
        }
        else
        {
            formatLine_ = L"Waveform unavailable for this file";
        }
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void UpdatePlaybackUi(bool force = false)
    {
        if (!player_ || !player_->IsPlaying())
            return;
        const ULONGLONG now = GetTickCount64();
        if (!force && now - lastPlaybackUiTick_ < kPlaybackUiIntervalMs)
            return;
        lastPlaybackUiTick_ = now;
        const double previousPosition = playbackPosition_;
        const bool browserScrolling = IsBrowserSmoothScrolling();
        AdvancePlaybackPosition(now, force, browserScrolling);
        if (!force && browserScrolling)
        {
            InvalidatePlaybackCursor(previousPosition, playbackPosition_);
            return;
        }
        UpdateRms();
        RECT invalid = waveRect_;
        UnionRect(&invalid, &invalid, &bottomRect_);
        InvalidateRect(hwnd_, &invalid, FALSE);
    }

    void OnTimer()
    {
        if (dragMode_ == DragMode::HorizontalSplitter || dragMode_ == DragMode::VerticalSplitter)
            UpdateSplitterDragFromCursor();

        if (player_)
            player_->PumpEvents();
        UpdateSeekCrossfade();
        UpdatePlaybackUi();
        UpdateWaveHoverFromCursor();
        StepSmoothScroll();
    }

    void AdvancePlaybackPosition(ULONGLONG now, bool force, bool avoidMediaQuery = false)
    {
        if (!force && avoidMediaQuery)
        {
            PredictPlaybackPosition(now);
            return;
        }
        if (force || lastMediaPositionQueryTick_ == 0 || now - lastMediaPositionQueryTick_ >= kMediaPositionQueryIntervalMs)
        {
            playbackPosition_ = player_->Position();
            lastMediaPositionQueryTick_ = now;
            lastPlaybackClockTick_ = now;
            return;
        }

        PredictPlaybackPosition(now);
    }

    void PredictPlaybackPosition(ULONGLONG now)
    {
        if (lastPlaybackClockTick_ != 0)
        {
            playbackPosition_ += (double)(now - lastPlaybackClockTick_) / 1000.0;
            if (Duration() > 0)
                playbackPosition_ = std::min(playbackPosition_, Duration());
        }
        lastPlaybackClockTick_ = now;
    }

    RECT PlaybackCursorRect(double position) const
    {
        RECT empty{};
        const int width = waveRect_.right - waveRect_.left;
        const int height = waveRect_.bottom - waveRect_.top;
        if (width <= 0 || height <= 0 || Duration() <= 0)
            return empty;

        const double progress = std::clamp(position / Duration(), 0.0, 1.0);
        const int markerX = waveRect_.left + (int)std::round(width * progress);
        RECT markerRect{ markerX - S(5), waveRect_.top, markerX + S(6), waveRect_.bottom };

        const int labelWidth = S(78);
        const int labelHeight = S(25);
        int labelX = markerX + S(8);
        if (labelX + labelWidth > waveRect_.right - S(4))
            labelX = markerX - labelWidth - S(8);
        labelX = std::clamp(labelX, (int)waveRect_.left + S(4), (int)waveRect_.right - labelWidth - S(4));
        const int labelY = std::clamp((int)waveRect_.bottom - S(46),
            (int)waveRect_.top + S(6),
            (int)waveRect_.bottom - labelHeight - S(6));
        RECT labelRect{ labelX, labelY, labelX + labelWidth, labelY + labelHeight };

        RECT result{};
        UnionRect(&result, &markerRect, &labelRect);
        InflateRect(&result, S(2), S(2));
        RECT clipped{};
        IntersectRect(&clipped, &result, &waveRect_);
        return clipped;
    }

    void InvalidatePlaybackCursor(double previousPosition, double currentPosition)
    {
        RECT previousRect = PlaybackCursorRect(previousPosition);
        RECT currentRect = PlaybackCursorRect(currentPosition);
        RECT invalid{};
        UnionRect(&invalid, &previousRect, &currentRect);
        if (invalid.right > invalid.left && invalid.bottom > invalid.top)
            InvalidateRect(hwnd_, &invalid, FALSE);
    }

    std::wstring FormatLine() const
    {
        if (waveform_.sampleRate <= 0)
            return L"Audio";
        std::wstring channels;
        if (waveform_.channels == 1)
            channels = L"Mono";
        else if (waveform_.channels == 2)
            channels = L"Stereo";
        else
            channels = std::format(L"{} ch", waveform_.channels);
        const auto bitrate = waveform_.bitrateKbps > 0 ? std::format(L", {} kbps", waveform_.bitrateKbps) : std::wstring();
        if (waveform_.bitsPerSample > 0)
        {
            return std::format(L"{} Hz, {}, {}-bit{}",
                waveform_.sampleRate,
                channels,
                waveform_.bitsPerSample,
                bitrate);
        }
        return std::format(L"{} Hz, {}, Lossy{}",
            waveform_.sampleRate,
            channels,
            bitrate);
    }

    std::wstring FormatName() const
    {
        auto ext = LowerExt(currentFile_);
        if (ext.empty())
            return L"";
        auto value = ext.substr(1);
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return (wchar_t)towupper(c); });
        return value;
    }

    void UpdateRms()
    {
        if (waveform_.rms.size() < 2 || Duration() <= 0)
        {
            leftDb_ = rightDb_ = kRmsFloorDb;
            return;
        }
        const double progress = std::clamp(CurrentPosition() / Duration(), 0.0, 1.0);
        const int points = (int)waveform_.rms.size() / 2;
        const int index = std::clamp((int)std::floor(progress * points), 0, points - 1);
        leftDb_ = RmsToDb(waveform_.rms[index * 2]);
        rightDb_ = RmsToDb(waveform_.rms[index * 2 + 1]);
    }

    void OnPlaybackEnded()
    {
        if (order_ == PlaybackOrder::RepeatOne)
        {
            Seek(0);
            player_->Play();
            return;
        }
        PlayAdjacent(1);
    }

    void PlayAdjacent(int direction)
    {
        if (files_.empty())
            return;
        int current = -1;
        for (int i = 0; i < (int)files_.size(); ++i)
        {
            if (_wcsicmp(files_[i].path.c_str(), currentFile_.c_str()) == 0)
            {
                current = i;
                break;
            }
        }
        int next = 0;
        if (order_ == PlaybackOrder::Shuffle && files_.size() > 1)
        {
            std::uniform_int_distribution<int> dist(0, (int)files_.size() - 1);
            do { next = dist(random_); } while (next == current);
        }
        else
        {
            next = current < 0 ? 0 : (current + direction + (int)files_.size()) % (int)files_.size();
        }
        LoadFile(files_[next].path, true);
    }

    double CurrentPosition() const
    {
        return playbackPosition_;
    }

    double Duration() const
    {
        return waveform_.durationSeconds;
    }

    HTREEITEM AddTreeItem(HTREEITEM parent, const std::wstring& text, const std::wstring& path, bool hasChildren)
    {
        const int icon = ShellIconIndex(path.empty() ? L"C:\\" : path, FILE_ATTRIBUTE_DIRECTORY);
        TVINSERTSTRUCT insert{};
        insert.hParent = parent;
        insert.hInsertAfter = TVI_SORT;
        insert.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
        insert.item.pszText = const_cast<wchar_t*>(text.c_str());
        insert.item.iImage = icon;
        insert.item.iSelectedImage = icon;
        HTREEITEM item = TreeView_InsertItem(treeModel_, &insert);
        if (!item)
            return nullptr;
        treePaths_[item] = path;
        treeLoaded_[item] = false;
        if (hasChildren)
            AddTreeItem(item, L"Loading...", L"", false);
        return item;
    }

    void PopulateTreeRoots()
    {
        treePaths_.clear();
        treeLoaded_.clear();
        selectedTreeItem_ = nullptr;
        TreeView_DeleteAllItems(treeModel_);
        const DWORD drives = GetLogicalDrives();
        for (int i = 0; i < 26; ++i)
        {
            if ((drives & (1u << i)) == 0)
                continue;
            wchar_t root[] = { wchar_t(L'A' + i), L':', L'\\', L'\0' };
            const UINT type = GetDriveType(root);
            if (type == DRIVE_NO_ROOT_DIR)
                continue;
            AddTreeItem(nullptr, root, root, true);
        }
        treeScrollOffset_ = 0;
        UpdateTreeScrollInfo();
        RedrawWindow(treeView_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE);
    }

    std::wstring GetTreePath(HTREEITEM item) const
    {
        auto found = treePaths_.find(item);
        return found == treePaths_.end() ? L"" : found->second;
    }

    bool SamePath(const std::wstring& a, const std::wstring& b) const
    {
        return _wcsicmp(a.c_str(), b.c_str()) == 0;
    }

    HTREEITEM FindTreeChildByPath(HTREEITEM parent, const std::wstring& path) const
    {
        for (HTREEITEM child = parent ? TreeView_GetChild(treeModel_, parent) : TreeView_GetRoot(treeModel_);
             child;
             child = TreeView_GetNextSibling(treeModel_, child))
        {
            if (SamePath(GetTreePath(child), path))
                return child;
        }
        return nullptr;
    }

    int TreeVisibleIndex(HTREEITEM target) const
    {
        int index = 0;
        for (HTREEITEM item = TreeView_GetRoot(treeModel_); item; item = TreeView_GetNextVisible(treeModel_, item), ++index)
        {
            if (item == target)
                return index;
        }
        return -1;
    }

    void SelectTreePath(const std::wstring& folder)
    {
        if (folder.empty())
            return;
        std::error_code ec;
        fs::path target = fs::weakly_canonical(fs::path(folder), ec);
        if (ec)
            target = fs::path(folder);
        const auto root = target.root_path().wstring();
        if (root.empty())
            return;

        HTREEITEM item = FindTreeChildByPath(nullptr, root);
        if (!item)
            return;

        fs::path current(root);
        EnsureTreeChildrenLoaded(item);
        TreeView_Expand(treeModel_, item, TVE_EXPAND);

        const fs::path relative = target.lexically_relative(current);
        for (const auto& part : relative)
        {
            if (part.empty() || part == L".")
                continue;
            current /= part;
            HTREEITEM child = FindTreeChildByPath(item, current.wstring());
            if (!child)
                break;
            item = child;
            EnsureTreeChildrenLoaded(item);
            TreeView_Expand(treeModel_, item, TVE_EXPAND);
        }

        selectedTreeItem_ = item;
        UpdateTreeScrollInfo();
        const int index = TreeVisibleIndex(item);
        if (index >= 0)
        {
            RECT client{};
            GetClientRect(treeView_, &client);
            const int rowTop = index * TreeRowHeight();
            const int rowBottom = rowTop + TreeRowHeight();
            if (rowTop < treeScrollOffset_)
                SetTreeScrollOffset(rowTop);
            else if (rowBottom > treeScrollOffset_ + (client.bottom - client.top))
                SetTreeScrollOffset(rowBottom - (client.bottom - client.top));
        }
        RedrawWindow(treeView_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE);
    }

    bool HasDirectoryChildren(const fs::path& path) const
    {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(path, fs::directory_options::skip_permission_denied, ec))
        {
            if (!ec && entry.is_directory(ec) && !IsHiddenOrSystemPath(entry.path()))
                return true;
        }
        return false;
    }

    void EnsureTreeChildrenLoaded(HTREEITEM item)
    {
        if (!item || treeLoaded_[item])
            return;
        treeLoaded_[item] = true;
        HTREEITEM child = TreeView_GetChild(treeModel_, item);
        while (child)
        {
            HTREEITEM next = TreeView_GetNextSibling(treeModel_, child);
            treePaths_.erase(child);
            treeLoaded_.erase(child);
            TreeView_DeleteItem(treeModel_, child);
            child = next;
        }

        const auto parentPath = GetTreePath(item);
        if (parentPath.empty())
            return;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(parentPath, fs::directory_options::skip_permission_denied, ec))
        {
            if (ec || !entry.is_directory(ec))
                continue;
            if (IsHiddenOrSystemPath(entry.path()))
                continue;
            const auto name = entry.path().filename().wstring();
            if (name.empty() || name[0] == L'.')
                continue;
            AddTreeItem(item, name, entry.path().wstring(), HasDirectoryChildren(entry.path()));
        }
        UpdateTreeScrollInfo();
        RedrawWindow(treeView_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE);
    }

    void SetCurrentFolder(const std::wstring& folder, const std::wstring& selected)
    {
        if (!SamePath(currentFolder_, folder))
            fileListScrollOffset_ = 0;
        currentFolder_ = folder;
        files_.clear();
        ListView_DeleteAllItems(fileList_);
        std::error_code ec;
        if (folder.empty() || !fs::exists(folder, ec))
            return;

        for (const auto& entry : fs::directory_iterator(folder, fs::directory_options::skip_permission_denied, ec))
        {
            if (ec || !entry.is_regular_file(ec))
                continue;
            if (IsHiddenOrSystemPath(entry.path()))
                continue;
            const auto path = entry.path().wstring();
            if (!IsSupportedAudio(path))
                continue;
            AudioFileItem item;
            item.name = entry.path().filename().wstring();
            item.path = path;
            if (const auto duration = ReadTrackDuration(path))
            {
                item.durationSeconds = *duration;
                item.length = FormatTrackLength(item.durationSeconds);
            }
            auto ext = LowerExt(path);
            item.format = ext.empty() ? L"" : ext.substr(1);
            std::transform(item.format.begin(), item.format.end(), item.format.begin(), [](wchar_t c) { return (wchar_t)towupper(c); });
            item.sizeBytes = entry.file_size(ec);
            item.size = FormatSize(item.sizeBytes);
            item.modifiedTime = entry.last_write_time(ec);
            item.modified = FormatFileTime(item.modifiedTime);
            files_.push_back(std::move(item));
        }
        SortFileItems();
        RebuildFileListItems(selected);
        LayoutFileColumns();
        UpdateFileListScrollInfo();
        RedrawWindow(fileList_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    }

    void ApplyCaptionTheme()
    {
        const BOOL value = darkTheme_ ? TRUE : FALSE;
        DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &value, sizeof(value));
    }

    void ApplyNativeControlTheme()
    {
        if (!fileList_)
            return;
        EnableImmersiveControlDarkMode(hwnd_, darkTheme_);
        EnableImmersiveControlDarkMode(treeModel_, darkTheme_);
        EnableImmersiveControlDarkMode(treeView_, darkTheme_);
        EnableImmersiveControlDarkMode(fileList_, darkTheme_);
        EnableImmersiveControlDarkMode(outputCombo_, darkTheme_);
        HWND header = ListView_GetHeader(fileList_);
        if (header)
        {
            EnableImmersiveControlDarkMode(header, darkTheme_);
            SetWindowTheme(header, darkTheme_ ? L"DarkMode_Explorer" : nullptr, nullptr);
        }
        SetWindowTheme(treeModel_, darkTheme_ ? L"DarkMode_Explorer" : nullptr, nullptr);
        SetWindowTheme(treeView_, darkTheme_ ? L"DarkMode_Explorer" : nullptr, nullptr);
        SetWindowTheme(fileList_, darkTheme_ ? L"DarkMode_Explorer" : nullptr, nullptr);
        SetWindowTheme(outputCombo_, darkTheme_ ? L"DarkMode_CFD" : nullptr, nullptr);
        TreeView_SetBkColor(treeModel_, darkTheme_ ? RGB(18, 20, 24) : RGB(255, 255, 255));
        TreeView_SetTextColor(treeModel_, darkTheme_ ? RGB(238, 242, 246) : RGB(30, 36, 44));
        ListView_SetBkColor(fileList_, darkTheme_ ? RGB(18, 20, 24) : RGB(255, 255, 255));
        ListView_SetTextBkColor(fileList_, darkTheme_ ? RGB(18, 20, 24) : RGB(255, 255, 255));
        ListView_SetTextColor(fileList_, darkTheme_ ? RGB(238, 242, 246) : RGB(30, 36, 44));
        ApplyTreeImageList();
        RedrawWindow(treeView_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE);
        InvalidateRect(fileList_, nullptr, FALSE);
        if (header)
            InvalidateRect(header, nullptr, TRUE);
    }

    HINSTANCE instance_{};
    HWND hwnd_{};
    HWND treeModel_{};
    HWND treeView_{};
    HWND fileList_{};
    HWND outputCombo_{};
    HFONT font_{};
    HBRUSH darkListBrush_{};
    HBRUSH lightListBrush_{};
    ULONG_PTR gdiplusToken_{};
    RECT client_{};
    RECT browserRect_{};
    RECT folderRect_{};
    RECT fileRect_{};
    RECT horizontalSplitterRect_{};
    RECT verticalSplitterRect_{};
    RECT waveRect_{};
    RECT bottomRect_{};
    RECT volumeRect_{};
    RECT outputComboRect_{};
    std::unique_ptr<MediaPlayerHost> player_;
    std::vector<Button> buttons_;
    std::vector<AudioFileItem> files_;
    std::vector<AudioOutputDevice> outputDevices_;
    std::unordered_map<HTREEITEM, std::wstring> treePaths_;
    std::unordered_map<HTREEITEM, bool> treeLoaded_;
    std::unordered_map<std::wstring, WaveformData> waveformSessionCache_;
    HTREEITEM selectedTreeItem_{};
    std::wstring currentFile_;
    std::wstring currentFolder_;
    std::wstring selectedOutputId_;
    std::wstring formatLine_ = L"WAV, MP3, M4A, AAC, WMA, FLAC";
    WaveformData waveform_;
    HBITMAP waveBaseBitmap_{};
    HBITMAP wavePlayedBitmap_{};
    int waveCacheWidth_ = 0;
    int waveCacheHeight_ = 0;
    int waveformVersion_ = 0;
    int waveCacheVersion_ = -1;
    bool waveCacheDarkTheme_ = true;
    std::mt19937 random_{ std::random_device{}() };
    PlaybackOrder order_ = PlaybackOrder::Sequential;
    std::atomic<int> loadToken_{ 0 };
    bool darkTheme_ = true;
    DragMode dragMode_ = DragMode::None;
    LONG browserHeight_ = kBrowserHeight;
    LONG treeWidth_ = kLeftBrowserWidth;
    double waveHeightRatio_ = 0.40;
    double treeWidthRatio_ = 0.32;
    bool inWindowResize_ = false;
    bool waveHover_ = false;
    bool trackingMouseLeave_ = false;
    bool timerResolutionRaised_ = false;
    bool suppressTreeSelection_ = false;
    bool suppressOutputChange_ = false;
    bool pendingSeekActive_ = false;
    SeekFadePhase seekFadePhase_ = SeekFadePhase::None;
    int treeScrollOffset_ = 0;
    int fileListScrollOffset_ = 0;
    FileSortColumn fileSortColumn_ = FileSortColumn::Name;
    bool fileSortAscending_ = true;
    double treeScrollVelocity_ = 0;
    double fileListScrollVelocity_ = 0;
    ULONGLONG lastSmoothScrollTick_ = 0;
    ULONGLONG lastPlaybackUiTick_ = 0;
    ULONGLONG lastWaveRedrawRequestTick_ = 0;
    ULONGLONG lastPlaybackClockTick_ = 0;
    ULONGLONG lastMediaPositionQueryTick_ = 0;
    ULONGLONG seekCrossfadeStartTick_ = 0;
    POINT waveHoverPoint_{};
    int headerResizeColumn_ = -1;
    int headerResizeStartX_ = 0;
    int headerResizeStartWidth_ = 0;
    POINT lastDragPoint_{};
    UINT dpi_ = 96;
    float volume_ = 0.8f;
    float previousVolume_ = 0.8f;
    float currentSeekFadeScale_ = 1.0f;
    float seekFadeOutStartScale_ = 1.0f;
    double leftDb_ = kRmsFloorDb;
    double rightDb_ = kRmsFloorDb;
    double playbackPosition_ = 0;
    double pendingSeekPosition_ = 0;
};

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int showCommand)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ComInit com;
    std::wstring startup;
    int argc = 0;
    UNREFERENCED_PARAMETER(commandLine);
    PWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    startup = FindStartupAudioFile(argv, argc);
    if (argv)
        LocalFree(argv);

    HANDLE singleInstance = CreateMutexW(nullptr, TRUE, kSingleInstanceMutexName);
    const bool alreadyRunning = singleInstance && GetLastError() == ERROR_ALREADY_EXISTS;
    if (alreadyRunning)
    {
        SendFileToExistingPlayer(startup);
        CloseHandle(singleInstance);
        return 0;
    }

    AudioWaveApp app;
    const int result = app.Run(instance, showCommand, startup);
    if (singleInstance)
        CloseHandle(singleInstance);
    return result;
}
