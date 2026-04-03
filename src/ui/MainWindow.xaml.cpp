#include "pch.h"
#include "MainWindow.xaml.h"
#include "SettingsImport.h"
#include "SettingsModels.h"
#include "SettingsStore.h"
#include "StartupRegistration.h"
#include "TextUtilities.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <winrt/Windows.Networking.h>
#include <winrt/Windows.Networking.Sockets.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.Storage.FileProperties.h>

#include <microsoft.ui.xaml.window.h>
#include <shellapi.h>

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <random>
#include <stdexcept>
#include <unordered_map>

#pragma comment(lib, "shell32.lib")

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media;
using namespace Microsoft::UI::Xaml::Media::Imaging;
using namespace Windows::Foundation;
using namespace Windows::Data::Json;
using namespace Windows::Networking;
using namespace Windows::Networking::Sockets;
using namespace Windows::UI;
using namespace Windows::Storage::Streams;

namespace winrt::Last_Rich_Presence::implementation
{
    struct MainWindow;
}

namespace
{
    struct MotionTokens
    {
        static constexpr int FastMs = 140;
        static constexpr int StandardMs = 220;
        static constexpr int SlowMs = 320;
        static constexpr int ProgressStepMs = 860;
        static constexpr int PageTransitionMs = 170;
        static constexpr int LivePulseMs = 980;
        static constexpr int SkeletonPulseMs = 700;
        static constexpr int SkeletonDwellMs = 600;
        static constexpr int WaveShiftMs = 980;
        static constexpr int WaveGlowMs = 760;
        static constexpr int HomeWaveShiftMs = 900;
        static constexpr int HomeWaveGlowMs = 700;
        static constexpr int InfoBarFadeMs = 260;
    };

    Microsoft::UI::Xaml::Duration MotionDuration(int milliseconds)
    {
        return DurationHelper::FromTimeSpan(std::chrono::milliseconds(milliseconds));
    }

    Microsoft::UI::Xaml::Media::Animation::CubicEase MotionEaseOut()
    {
        auto ease = Microsoft::UI::Xaml::Media::Animation::CubicEase();
        ease.EasingMode(Microsoft::UI::Xaml::Media::Animation::EasingMode::EaseOut);
        return ease;
    }

    Microsoft::UI::Xaml::Media::Animation::CubicEase MotionEaseIn()
    {
        auto ease = Microsoft::UI::Xaml::Media::Animation::CubicEase();
        ease.EasingMode(Microsoft::UI::Xaml::Media::Animation::EasingMode::EaseIn);
        return ease;
    }

    Microsoft::UI::Xaml::Media::Animation::SineEase MotionEaseInOutSine()
    {
        auto ease = Microsoft::UI::Xaml::Media::Animation::SineEase();
        ease.EasingMode(Microsoft::UI::Xaml::Media::Animation::EasingMode::EaseInOut);
        return ease;
    }

    constexpr UINT kTrayCallbackMessage = WM_APP + 0x52;
    constexpr UINT kTrayIconId = 1;
    constexpr uint32_t kTrayMenuShowHide = 31001;
    constexpr uint32_t kTrayMenuTogglePresence = 31002;
    constexpr uint32_t kTrayMenuExit = 31003;
    constexpr wchar_t kSettingsExportFileName[] = L"settings-export.json";
    constexpr wchar_t kStartupTaskId[] = L"LastRichPresenceStartupTask";
    constexpr wchar_t kStartMinimizedArgument[] = L"--start-minimized";
    constexpr int32_t kDefaultWindowWidth = 1230;
    constexpr int32_t kDefaultWindowHeight = 845;

    struct TrayWindowState
    {
        winrt::Last_Rich_Presence::implementation::MainWindow* window{ nullptr };
        WNDPROC originalWndProc{ nullptr };
    };

    std::mutex& GetTrayWindowStateMutex()
    {
        static std::mutex m;
        return m;
    }

    std::unordered_map<HWND, TrayWindowState>& GetTrayWindowStateMap()
    {
        static std::unordered_map<HWND, TrayWindowState> map;
        return map;
    }

    struct BrowserHint
    {
        std::wstring siteKey;
        std::wstring pageHost;
        std::wstring service;
        std::wstring mediaKind;
        std::wstring title;
        std::wstring artist;
        std::wstring album;
        std::wstring rule;
        bool isPlaying{ false };
        int64_t sequence{ 0 };
        int positionSeconds{ 0 };
        int durationSeconds{ 0 };
        int confidence{ 0 };
        std::chrono::steady_clock::time_point updatedAt{};
    };

    struct BrowserHintServerState
    {
        std::mutex mutex;
        StreamSocketListener listener{ nullptr };
        winrt::event_token token{};
        bool started{ false };
        bool binding{ false };
        std::string authToken;
        bool extensionSeen{ false };
        std::chrono::steady_clock::time_point extensionSeenAt{};
        bool hasHint{ false };
        int64_t lastAcceptedSequence{ -1 };
        BrowserHint hint{};
    };

    enum class MergeMode
    {
        DetectorOnly,
        ExtensionApplied,
        ExtensionSynthesized,
        ExtensionPending,
        NoMedia
    };

    struct MergeResult
    {
        MediaInfo media;
        MergeMode mode{ MergeMode::DetectorOnly };
        bool extensionConnected{ false };
        bool hintApplied{ false };
    };

    BrowserHintServerState& GetHintServerState()
    {
        static BrowserHintServerState state;
        return state;
    }

    std::mutex& GetHintCallbackMutex()
    {
        static std::mutex m;
        return m;
    }

    std::function<void()>& GetHintUpdateCallback()
    {
        static std::function<void()> cb;
        return cb;
    }

    void SetBrowserHintUpdateCallback(std::function<void()> cb)
    {
        std::lock_guard<std::mutex> lock(GetHintCallbackMutex());
        GetHintUpdateCallback() = std::move(cb);
    }

    void NotifyBrowserHintUpdated()
    {
        std::function<void()> cb;
        {
            std::lock_guard<std::mutex> lock(GetHintCallbackMutex());
            cb = GetHintUpdateCallback();
        }

        if (cb)
        {
            try { cb(); } catch (...) {}
        }
    }

    bool IsExtensionConnected(BrowserHintServerState const& state)
    {
        if (!state.extensionSeen)
            return false;

        auto age = std::chrono::steady_clock::now() - state.extensionSeenAt;
        return age <= std::chrono::seconds(45);
    }

    std::wstring BuildBridgeStateSummary()
    {
        auto& state = GetHintServerState();
        std::lock_guard<std::mutex> lock(state.mutex);

        auto extensionConnected = IsExtensionConnected(state);
        auto hintAgeMs = int64_t(-1);
        if (state.hasHint)
        {
            auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - state.hint.updatedAt).count();
            hintAgeMs = age;
        }

        return
            L"ext=" + std::wstring(extensionConnected ? L"1" : L"0") +
            L", hasHint=" + std::wstring(state.hasHint ? L"1" : L"0") +
            L", lastSeq=" + std::to_wstring(state.lastAcceptedSequence) +
            L", hintAgeMs=" + std::to_wstring(hintAgeMs);
    }

    std::wstring ToLowerCopy(std::wstring value)
    {
        for (auto& ch : value)
            ch = towlower(ch);
        return value;
    }

    std::wstring TrimCopy(std::wstring value)
    {
        auto isWs = [](wchar_t ch)
        {
            return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
        };

        while (!value.empty() && isWs(value.front()))
            value.erase(value.begin());
        while (!value.empty() && isWs(value.back()))
            value.pop_back();
        return value;
    }

    bool HasPackageIdentity()
    {
        return lrp::startup::HasPackageIdentity();
    }

    bool IsStartMinimizedLaunchRequested()
    {
        int argCount = 0;
        auto args = CommandLineToArgvW(GetCommandLineW(), &argCount);
        if (!args)
            return false;

        bool requested = false;
        for (int index = 1; index < argCount; ++index)
        {
            auto arg = ToLowerCopy(TrimCopy(args[index]));
            if (arg == L"--start-minimized" || arg == L"/start-minimized")
            {
                requested = true;
                break;
            }
        }

        LocalFree(args);
        return requested;
    }

    bool IsRunStartupEnabledForCurrentExecutable()
    {
        return lrp::startup::IsRunStartupEnabledForCurrentExecutable();
    }

    bool SetRunStartupEnabledForCurrentExecutable(bool enabled, bool startMinimizedToTray)
    {
        return lrp::startup::SetRunStartupEnabledForCurrentExecutable(enabled, startMinimizedToTray);
    }

    bool TryReadUserPreferenceBool(const wchar_t* valueName, bool& valueOut)
    {
        return lrp::startup::TryReadUserPreferenceBool(valueName, valueOut);
    }

    bool WriteUserPreferenceBool(const wchar_t* valueName, bool value)
    {
        return lrp::startup::WriteUserPreferenceBool(valueName, value);
    }

    std::wstring NormalizeForMatch(const std::wstring& value)
    {
        return lrp::NormalizeForMatch(value);
    }

    std::vector<std::wstring> ParseBlockedTerms(const std::wstring& raw)
    {
        return lrp::ParseDelimitedTerms(raw);
    }

    struct BoolValueGuard
    {
        BoolValueGuard(bool& target, bool replacement)
            : m_target(target)
            , m_original(target)
        {
            m_target = replacement;
        }

        ~BoolValueGuard()
        {
            m_target = m_original;
        }

    private:
        bool& m_target;
        bool m_original;
    };

    std::wstring NarrowToWide(const std::string& value)
    {
        return std::wstring(winrt::to_hstring(value).c_str());
    }

    JsonObject BuildDiscordStatusJson(const DiscordRpcStatus& status)
    {
        JsonObject json;
        json.Insert(L"connected", JsonValue::CreateBooleanValue(status.connected));
        json.Insert(L"running", JsonValue::CreateBooleanValue(status.running));
        json.Insert(L"lastResult", JsonValue::CreateStringValue(
            winrt::to_hstring(DiscordRPC::TransportResultLabel(status.lastResult))));
        json.Insert(L"retryCount", JsonValue::CreateNumberValue(static_cast<double>(status.retryCount)));
        json.Insert(L"lastSuccessfulHandshakeUnixSeconds",
            JsonValue::CreateNumberValue(static_cast<double>(status.lastSuccessfulHandshakeUnixSeconds)));
        json.Insert(L"lastDisconnectUnixSeconds",
            JsonValue::CreateNumberValue(static_cast<double>(status.lastDisconnectUnixSeconds)));
        json.Insert(L"lastErrorDetail", JsonValue::CreateStringValue(NarrowToWide(status.lastErrorDetail)));
        return json;
    }

    std::wstring JoinIssueMessages(const std::vector<lrp::settings::SettingsIssue>& issues)
    {
        std::wstring message;
        for (size_t index = 0; index < issues.size(); ++index)
        {
            if (index > 0)
                message += L"; ";

            message += issues[index].key + L": " + issues[index].message;
        }

        return message;
    }

    lrp::settings::ImportedSettingMap BuildImportedSettingsMap(const JsonObject& root)
    {
        lrp::settings::ImportedSettingMap values;
        for (const auto& pair : root)
        {
            const auto key = std::wstring(pair.Key().c_str());
            const auto value = pair.Value();
            switch (value.ValueType())
            {
            case JsonValueType::Boolean:
                values[key] = value.GetBoolean();
                break;
            case JsonValueType::Number:
                values[key] = value.GetNumber();
                break;
            case JsonValueType::String:
                values[key] = std::wstring(value.GetString().c_str());
                break;
            default:
                values[key] = std::monostate{};
                break;
            }
        }

        return values;
    }

    std::wstring BuildProductiveLaneSignature(
        const ProductiveActivityInfo& info,
        const ProductivePresenceOptions& options)
    {
        return info.appKey + L"|" +
            info.appName + L"|" +
            (options.showProjectName ? info.projectHint : std::wstring{}) + L"|" +
            std::to_wstring(options.activityTypeOverride);
    }

    std::wstring BuildCreativeLaneSignature(
        const CreativeActivityInfo& info,
        const CreativePresenceOptions& options)
    {
        return info.appKey + L"|" +
            info.appName + L"|" +
            (options.showProjectName ? info.projectHint : std::wstring{}) + L"|" +
            (options.showWindowTitle ? info.windowTitle : std::wstring{}) + L"|" +
            std::to_wstring(static_cast<int>(options.privacyMode)) + L"|" +
            std::to_wstring(options.activityTypeOverride);
    }

    std::wstring BuildLaneTransitionLogMessage(
        const std::wstring& laneLabel,
        const lrp::ActivityLaneTransition& transition)
    {
        return laneLabel + L" lane " +
            lrp::ToSettingString(transition.action) +
            L" (" + lrp::ToSettingString(transition.reason) + L")";
    }

    bool IsBlockedMessengerMedia(const MediaInfo& info)
    {
        auto containsBlockedMessengerToken = [](const std::wstring& textLower)
        {
            return
                textLower.find(L"whatsapp") != std::wstring::npos ||
                textLower.find(L"telegram") != std::wstring::npos ||
                textLower.find(L"web.whatsapp") != std::wstring::npos ||
                textLower.find(L"t.me") != std::wstring::npos;
        };

        auto sourceBlob = ToLowerCopy(
            info.sourceName + L" " +
            info.sourceDisplayName + L" " +
            info.detectedService + L" " +
            info.detectionReason);

        if (containsBlockedMessengerToken(sourceBlob))
            return true;

        // Browser sessions can surface generic source labels (e.g. Web Player),
        // so also guard against messenger terms leaking through media metadata.
        const bool browserContext =
            sourceBlob.find(L"chrome") != std::wstring::npos ||
            sourceBlob.find(L"edge") != std::wstring::npos ||
            sourceBlob.find(L"firefox") != std::wstring::npos ||
            sourceBlob.find(L"brave") != std::wstring::npos ||
            sourceBlob.find(L"opera") != std::wstring::npos ||
            sourceBlob.find(L"vivaldi") != std::wstring::npos ||
            sourceBlob.find(L"web player") != std::wstring::npos;

        if (!browserContext)
            return false;

        auto metadataBlob = ToLowerCopy(info.title + L" " + info.artist + L" " + info.albumTitle);
        return containsBlockedMessengerToken(metadataBlob);
    }

    bool IsBlockedMessengerHint()
    {
        auto& state = GetHintServerState();
        std::lock_guard<std::mutex> lock(state.mutex);

        if (!IsExtensionConnected(state) || !state.hasHint)
            return false;

        auto age = std::chrono::steady_clock::now() - state.hint.updatedAt;
        if (age > std::chrono::seconds(25))
            return false;

        auto hintBlob = ToLowerCopy(
            state.hint.pageHost + L" " +
            state.hint.siteKey + L" " +
            state.hint.service);

        return
            hintBlob.find(L"whatsapp") != std::wstring::npos ||
            hintBlob.find(L"telegram") != std::wstring::npos ||
            hintBlob.find(L"web.whatsapp") != std::wstring::npos ||
            hintBlob.find(L"t.me") != std::wstring::npos;
    }

    bool HasFreshBrowserHint()
    {
        auto& state = GetHintServerState();
        std::lock_guard<std::mutex> lock(state.mutex);

        if (!IsExtensionConnected(state) || !state.hasHint)
            return false;

        auto age = std::chrono::steady_clock::now() - state.hint.updatedAt;
        return age <= std::chrono::seconds(25);
    }

    std::wstring StripExeSuffix(std::wstring value)
    {
        auto lower = ToLowerCopy(value);
        if (lower.size() > 4 && lower.rfind(L".exe") == (lower.size() - 4))
            value.resize(value.size() - 4);
        return value;
    }

    std::wstring CanonicalCreativeLogoKeyForAppKey(const std::wstring& appKey)
    {
        if (appKey == L"PHXS") return L"photoshop";
        if (appKey == L"ILST") return L"illustrator";
        if (appKey == L"XD") return L"xd";
        if (appKey == L"BRDG") return L"bridge";
        if (appKey == L"CHAN") return L"characteranimator";
        if (appKey == L"FRSC") return L"fresco";
        if (appKey == L"DIMN") return L"dimension";
        if (appKey == L"SBPT") return L"substancepainter";
        if (appKey == L"SBDG") return L"substancedesigner";
        if (appKey == L"SBSM") return L"substancesampler";
        if (appKey == L"SBST") return L"substancestager";
        if (appKey == L"SBMD") return L"substancemodeler";
        if (appKey == L"PPRO") return L"premiere";
        if (appKey == L"AEFT") return L"aftereffects";
        if (appKey == L"IDSN") return L"indesign";
        if (appKey == L"AICY") return L"incopy";
        if (appKey == L"AUDT") return L"audition";
        if (appKey == L"DRWV") return L"dreamweaver";
        if (appKey == L"FLPR") return L"animate";
        if (appKey == L"LTRM") return L"lightroom";
        if (appKey == L"LTRC") return L"lightroomclassic";
        if (appKey == L"ACRO") return L"acrobat";
        if (appKey == L"AME") return L"mediaencoder";
        return {};
    }

    std::wstring NormalizeLogoName(std::wstring value)
    {
        value = ToLowerCopy(TrimCopy(std::move(value)));
        std::wstring out;
        out.reserve(value.size());
        for (wchar_t ch : value)
        {
            if ((ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9'))
                out.push_back(ch);
        }
        return out;
    }

    std::vector<std::wstring> BuildCreativeLogoNameCandidates(const CreativeActivityInfo& info)
    {
        std::vector<std::wstring> names;
        auto pushUnique = [&](const std::wstring& raw)
        {
            auto normalized = NormalizeLogoName(raw);
            if (normalized.empty())
                return;

            for (const auto& existing : names)
            {
                if (existing == normalized)
                    return;
            }
            names.push_back(std::move(normalized));
        };

        pushUnique(CanonicalCreativeLogoKeyForAppKey(info.appKey));
        pushUnique(info.appKey);

        auto appName = ToLowerCopy(info.appName);
        if (appName.rfind(L"adobe ", 0) == 0)
            appName = appName.substr(6);
        pushUnique(appName);

        pushUnique(info.processName);
        pushUnique(StripExeSuffix(info.processName));
        return names;
    }

    std::wstring GetExecutableDirectoryPath()
    {
        wchar_t exePath[MAX_PATH]{};
        auto len = GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
        if (len == 0 || len >= std::size(exePath))
            return {};

        try
        {
            return std::filesystem::path(exePath).parent_path().wstring();
        }
        catch (...)
        {
            return {};
        }
    }

    std::wstring TryFindCuratedCreativeLogoPath(const CreativeActivityInfo& info)
    {
        auto exeDir = GetExecutableDirectoryPath();
        if (exeDir.empty())
            return {};

        std::vector<std::filesystem::path> baseDirs
        {
            std::filesystem::path(exeDir) / L"Assets" / L"CreativeLogos",
            std::filesystem::path(exeDir) / L"Assets" / L"creative-logos",
            std::filesystem::path(exeDir) / L"Assets" / L"creative-icons"
        };

        auto names = BuildCreativeLogoNameCandidates(info);
        if (names.empty())
            return {};

        static constexpr std::wstring_view exts[] = { L".png", L".jpg", L".jpeg", L".webp" };

        for (const auto& dir : baseDirs)
        {
            std::error_code ec;
            if (!std::filesystem::exists(dir, ec) || ec)
                continue;

            for (const auto& name : names)
            {
                for (const auto& ext : exts)
                {
                    auto candidate = dir / (name + std::wstring(ext));
                    std::error_code fileEc;
                    if (std::filesystem::exists(candidate, fileEc) && !fileEc)
                        return candidate.wstring();
                }
            }
        }

        return {};
    }

    std::wstring CanonicalProductiveLogoKeyForAppKey(const std::wstring& appKey)
    {
        if (appKey == L"WORD") return L"word";
        if (appKey == L"XCEL") return L"excel";
        if (appKey == L"PPT") return L"powerpoint";
        if (appKey == L"ONEN") return L"onenote";
        if (appKey == L"ACCS") return L"access";
        if (appKey == L"PUBR") return L"publisher";
        if (appKey == L"VISI") return L"visio";
        if (appKey == L"PROJ") return L"project";
        return {};
    }

    std::vector<std::wstring> BuildProductiveLogoNameCandidates(const ProductiveActivityInfo& info)
    {
        std::vector<std::wstring> names;
        auto pushUnique = [&](const std::wstring& raw)
        {
            auto normalized = NormalizeLogoName(raw);
            if (normalized.empty())
                return;

            for (const auto& existing : names)
            {
                if (existing == normalized)
                    return;
            }

            names.push_back(std::move(normalized));
        };

        pushUnique(CanonicalProductiveLogoKeyForAppKey(info.appKey));
        pushUnique(info.appKey);

        auto appName = ToLowerCopy(info.appName);
        if (appName.rfind(L"microsoft ", 0) == 0)
            appName = appName.substr(10);
        pushUnique(appName);

        pushUnique(info.processName);
        pushUnique(StripExeSuffix(info.processName));
        return names;
    }

    std::wstring TryFindCuratedProductiveLogoPath(const ProductiveActivityInfo& info)
    {
        auto exeDir = GetExecutableDirectoryPath();
        std::vector<std::filesystem::path> baseDirs;
        if (!exeDir.empty())
        {
            baseDirs.push_back(std::filesystem::path(exeDir) / L"Assets" / L"ProductiveLogos");
            baseDirs.push_back(std::filesystem::path(exeDir) / L"Assets" / L"productive-logos");
            baseDirs.push_back(std::filesystem::path(exeDir) / L"Assets" / L"productive-icons");
        }

        // External backup set provided by user. System process icon remains the primary source.
        wchar_t* userProfileRaw = nullptr;
        size_t userProfileLen = 0;
        if (_wdupenv_s(&userProfileRaw, &userProfileLen, L"USERPROFILE") == 0 &&
            userProfileRaw && userProfileRaw[0] != L'\0')
        {
            auto userProfilePath = std::filesystem::path(userProfileRaw);
            baseDirs.push_back(userProfilePath / L"Downloads" / L"MS office logos");
            baseDirs.push_back(userProfilePath / L"Downloads" / L"MS Office Logos");
        }
        if (userProfileRaw)
            std::free(userProfileRaw);

        auto names = BuildProductiveLogoNameCandidates(info);
        if (names.empty())
            return {};

        static constexpr std::wstring_view exts[] = { L".png", L".jpg", L".jpeg", L".webp" };

        for (const auto& dir : baseDirs)
        {
            std::error_code ec;
            if (!std::filesystem::exists(dir, ec) || ec)
                continue;

            for (const auto& name : names)
            {
                for (const auto& ext : exts)
                {
                    auto candidate = dir / (name + std::wstring(ext));
                    std::error_code fileEc;
                    if (std::filesystem::exists(candidate, fileEc) && !fileEc)
                        return candidate.wstring();
                }
            }
        }

        return {};
    }

    bool IsBrowserMedia(const MediaInfo& info)
    {
        auto source = ToLowerCopy(info.sourceName + L" " + info.sourceDisplayName);
        return
            source.find(L"chrome") != std::wstring::npos ||
            source.find(L"edge") != std::wstring::npos ||
            source.find(L"firefox") != std::wstring::npos ||
            source.find(L"brave") != std::wstring::npos ||
            source.find(L"opera") != std::wstring::npos ||
            source.find(L"vivaldi") != std::wstring::npos ||
            source.find(L"web player") != std::wstring::npos;
    }

    bool TitlesLikelyMatch(const std::wstring& left, const std::wstring& right)
    {
        auto a = NormalizeForMatch(left);
        auto b = NormalizeForMatch(right);
        if (a.empty() || b.empty()) return false;
        if (a == b) return true;
        return a.find(b) != std::wstring::npos || b.find(a) != std::wstring::npos;
    }

    std::wstring BuildDetectedViaLabel(const MediaInfo& info, const std::wstring& mergeState)
    {
        if (info.title.empty())
            return L"Detected via --";

        auto reason = ToLowerCopy(info.detectionReason);
        auto merge = ToLowerCopy(mergeState);

        if (merge == L"extension-applied" || merge == L"extension-synthesized")
            return L"Detected via Browser hint";
        if (merge == L"extension-pending")
            return L"Detected via Merged";
        if (reason.find(L"timeline-fallback:detector") != std::wstring::npos)
            return L"Detected via Merged";
        if (reason.find(L"extension:") != std::wstring::npos)
            return L"Detected via Browser hint";

        return L"Detected via Windows session";
    }

    void AnimateOpacityPulse(UIElement const& element, double fromOpacity = 0.9, int durationMs = MotionTokens::StandardMs)
    {
        if (!element)
            return;

        auto animation = Microsoft::UI::Xaml::Media::Animation::DoubleAnimation();
        animation.From(fromOpacity);
        animation.To(1.0);
        animation.Duration(MotionDuration(durationMs));
        animation.EasingFunction(MotionEaseOut());

        auto storyboard = Microsoft::UI::Xaml::Media::Animation::Storyboard();
        storyboard.Children().Append(animation);
        Microsoft::UI::Xaml::Media::Animation::Storyboard::SetTarget(animation, element);
        Microsoft::UI::Xaml::Media::Animation::Storyboard::SetTargetProperty(animation, L"Opacity");

        element.Opacity(fromOpacity);
        storyboard.Begin();
    }

    bool TryGetLiveHintTimeline(const MediaInfo& info, int& posOut, int& durOut)
    {
        posOut = 0;
        durOut = 0;

        if (info.title.empty())
            return false;

        auto& state = GetHintServerState();
        std::lock_guard<std::mutex> lock(state.mutex);

        if (!IsExtensionConnected(state) || !state.hasHint)
            return false;

        const auto& hint = state.hint;
        if (hint.title.empty())
            return false;

        if (hint.confidence > 0 && hint.confidence < 60)
            return false;

        auto hintRuleLower = ToLowerCopy(hint.rule);
        auto hintSiteKeyLower = ToLowerCopy(hint.siteKey);
        auto hintServiceLower = ToLowerCopy(hint.service);
        bool isYouTubeMusicHint =
            hintSiteKeyLower == L"youtube_music" ||
            hintServiceLower == L"youtube music" ||
            hintRuleLower.find(L"youtube-music") != std::wstring::npos;

        if (isYouTubeMusicHint)
        {
            bool timelineFromClock = hintRuleLower.find(L"clock") != std::wstring::npos;
            if (!timelineFromClock || hint.confidence < 98)
                return false;
        }

        bool titleMatch = TitlesLikelyMatch(info.title, hint.title);
        bool artistCompatible =
            hint.artist.empty() ||
            info.artist.empty() ||
            TitlesLikelyMatch(info.artist, hint.artist);

        if (!titleMatch || !artistCompatible)
            return false;

        if (hint.durationSeconds <= 0 || hint.positionSeconds < 0 ||
            hint.positionSeconds > (hint.durationSeconds + 5))
            return false;

        auto age = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - hint.updatedAt);

        if (age > std::chrono::seconds(5))
            return false;

        auto effectivePos = hint.positionSeconds;
        if (hint.isPlaying && age.count() > 0)
            effectivePos += static_cast<int>(age.count());

        if (effectivePos < 0) effectivePos = 0;
        if (effectivePos > hint.durationSeconds) effectivePos = hint.durationSeconds;

        posOut = effectivePos;
        durOut = hint.durationSeconds;
        return true;
    }

    std::string ToLowerAscii(std::string value)
    {
        for (auto& ch : value)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return value;
    }

    std::string TrimAscii(std::string value)
    {
        auto isWs = [](char ch)
        {
            return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
        };

        while (!value.empty() && isWs(value.front()))
            value.erase(value.begin());
        while (!value.empty() && isWs(value.back()))
            value.pop_back();
        return value;
    }

    int ParseContentLength(const std::string& headers)
    {
        auto lower = ToLowerAscii(headers);
        const std::string key = "content-length:";
        auto pos = lower.find(key);
        if (pos == std::string::npos) return 0;

        pos += key.size();
        while (pos < lower.size() && (lower[pos] == ' ' || lower[pos] == '\t')) ++pos;

        int value = 0;
        while (pos < lower.size() && lower[pos] >= '0' && lower[pos] <= '9')
        {
            value = value * 10 + (lower[pos] - '0');
            ++pos;
        }

        return value;
    }

    std::string GetHeaderValue(const std::string& headers, const std::string& headerName)
    {
        std::istringstream stream(headers);
        std::string line;
        const std::string target = ToLowerAscii(headerName);

        bool first = true;
        while (std::getline(stream, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            if (first)
            {
                first = false;
                continue;
            }

            if (line.empty())
                break;

            auto colon = line.find(':');
            if (colon == std::string::npos)
                continue;

            auto key = ToLowerAscii(TrimAscii(line.substr(0, colon)));
            if (key != target)
                continue;

            return TrimAscii(line.substr(colon + 1));
        }

        return {};
    }

    std::string GenerateAuthToken()
    {
        std::random_device rd;
        std::mt19937_64 rng(rd());

        std::ostringstream oss;
        for (int i = 0; i < 4; ++i)
        {
            auto chunk = rng();
            oss << std::hex << std::setw(16) << std::setfill('0') << chunk;
        }
        return oss.str();
    }

    bool IsSameHintSnapshot(const BrowserHint& left, const BrowserHint& right)
    {
        return
            left.siteKey == right.siteKey &&
            left.pageHost == right.pageHost &&
            left.service == right.service &&
            left.mediaKind == right.mediaKind &&
            left.title == right.title &&
            left.artist == right.artist &&
            left.album == right.album &&
            left.rule == right.rule &&
            left.isPlaying == right.isPlaying &&
            left.positionSeconds == right.positionSeconds &&
            left.durationSeconds == right.durationSeconds &&
            left.confidence == right.confidence;
    }

    bool ParseHintPayload(
        const std::string& body,
        bool& isClear,
        BrowserHint& outHint,
        std::wstring& error)
    {
        isClear = false;
        error.clear();

        JsonObject root;
        if (!JsonObject::TryParse(winrt::to_hstring(body), root))
        {
            error = L"invalid-json";
            return false;
        }

        auto source = root.GetNamedString(L"source", L"");
        if (!source.empty() && source != L"lrp-browser-extension")
        {
            error = L"invalid-source";
            return false;
        }

        auto schemaVersion = root.GetNamedNumber(L"schemaVersion", 1.0);
        if (schemaVersion < 1.0 || schemaVersion > 1.0)
        {
            error = L"unsupported-schema-version";
            return false;
        }

        auto kind = root.GetNamedString(L"kind", L"");
        if (kind == L"clear")
        {
            isClear = true;
            return true;
        }

        if (kind != L"hint")
        {
            error = L"invalid-kind";
            return false;
        }

        auto dataValue = root.TryLookup(L"data");
        if (!dataValue || dataValue.ValueType() != JsonValueType::Object)
        {
            error = L"missing-data";
            return false;
        }

        auto data = dataValue.GetObject();

        auto readString = [&](wchar_t const* key, size_t maxLen, bool required, std::wstring& output) -> bool
        {
            output = data.GetNamedString(key, L"");
            if (required && output.empty())
            {
                error = std::wstring(L"missing-") + key;
                return false;
            }
            if (output.size() > maxLen)
            {
                output = output.substr(0, maxLen);
            }
            return true;
        };

        BrowserHint hint;
        if (!readString(L"service", 64, true, hint.service)) return false;
        if (!readString(L"siteKey", 64, false, hint.siteKey)) return false;
        if (!readString(L"pageHost", 128, false, hint.pageHost)) return false;
        if (!readString(L"mediaKind", 24, false, hint.mediaKind)) return false;
        if (!readString(L"title", 256, false, hint.title)) return false;
        if (!readString(L"artist", 256, false, hint.artist)) return false;
        if (!readString(L"album", 256, false, hint.album)) return false;
        if (!readString(L"rule", 128, false, hint.rule)) return false;

        hint.isPlaying = data.GetNamedBoolean(L"isPlaying", false);

        auto pos = data.GetNamedNumber(L"positionSeconds", 0.0);
        auto dur = data.GetNamedNumber(L"durationSeconds", 0.0);
        auto conf = data.GetNamedNumber(L"confidence", 0.0);
        auto seq = data.GetNamedNumber(L"sequence", 0.0);

        if (pos < -10 || pos > 60 * 60 * 48)
        {
            error = L"invalid-position";
            return false;
        }
        if (dur < 0 || dur > 60 * 60 * 48)
        {
            error = L"invalid-duration";
            return false;
        }
        if (conf < 0 || conf > 1000)
        {
            error = L"invalid-confidence";
            return false;
        }
        if (seq < 0 || seq > 9e15)
        {
            error = L"invalid-sequence";
            return false;
        }

        hint.positionSeconds = static_cast<int>(pos);
        hint.durationSeconds = static_cast<int>(dur);
        hint.confidence = static_cast<int>(conf);
        hint.sequence = static_cast<int64_t>(seq);
        hint.updatedAt = std::chrono::steady_clock::now();

        outHint = std::move(hint);
        return true;
    }

    bool UpdateStoredBrowserHint(const std::string& body, std::wstring* errorOut = nullptr)
    {
        auto& state = GetHintServerState();
        auto now = std::chrono::steady_clock::now();

        bool isClear = false;
        BrowserHint hint;
        std::wstring parseError;
        if (!ParseHintPayload(body, isClear, hint, parseError))
        {
            if (errorOut)
                *errorOut = parseError;
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(state.mutex);

            bool wasConnected = IsExtensionConnected(state);
            state.extensionSeen = true;
            state.extensionSeenAt = now;

            if (!wasConnected)
            {
                state.lastAcceptedSequence = -1;
            }

            if (isClear)
            {
                state.hasHint = false;
                state.hint = {};
                state.lastAcceptedSequence = -1;
            }
            else
            {
                if (hint.sequence > 0 && hint.sequence == state.lastAcceptedSequence)
                {
                    if (state.hasHint && IsSameHintSnapshot(state.hint, hint))
                    {
                        state.hint.updatedAt = now;
                        return true;
                    }
                }

                // Sequence comes from browser content scripts and can reset when a tab/script
                // reloads. Treat lower/equal sequence as stale only when the snapshot is unchanged.
                // Otherwise accept it to avoid getting stuck on an old hint.
                if (hint.sequence > 0 && hint.sequence < state.lastAcceptedSequence)
                {
                    if (state.hasHint && IsSameHintSnapshot(state.hint, hint))
                    {
                        state.hint.updatedAt = now;
                        return true;
                    }
                }

                state.hint = std::move(hint);
                state.hasHint = true;
                if (state.hint.sequence > 0)
                    state.lastAcceptedSequence = state.hint.sequence;
            }
        }

        NotifyBrowserHintUpdated();
        return true;
    }

    bool ApplyBrowserHintOverride(MediaInfo& info, bool* extensionConnectedOut = nullptr)
    {
        BrowserHint hint;
        {
            auto& state = GetHintServerState();
            std::lock_guard<std::mutex> lock(state.mutex);

            bool extensionConnected = IsExtensionConnected(state);
            if (extensionConnectedOut) *extensionConnectedOut = extensionConnected;
            if (!extensionConnected)
                return false;

            if (!state.hasHint)
                return false;

            auto age = std::chrono::steady_clock::now() - state.hint.updatedAt;
            if (age > std::chrono::seconds(25))
            {
                state.hasHint = false;
                state.hint = {};
                return false;
            }

            hint = state.hint;
        }

        if (info.title.empty())
        {
            bool sourceLooksBrowserOrUnknown =
                info.sourceName.empty() ||
                info.sourceDisplayName.empty() ||
                IsBrowserMedia(info);

            if (!sourceLooksBrowserOrUnknown || hint.title.empty())
                return false;

            info.title = hint.title;
            if (info.artist.empty()) info.artist = hint.artist;
            if (info.albumTitle.empty()) info.albumTitle = hint.album;

            auto lowerSource = ToLowerCopy(info.sourceDisplayName);
            if (info.sourceDisplayName.empty() || lowerSource == L"web player")
                info.sourceDisplayName = hint.service;

            if (info.sourceName.empty() && !hint.pageHost.empty())
                info.sourceName = hint.pageHost;

            info.detectedService = hint.service;
            auto reason = hint.rule.empty() ? std::wstring(L"browser-hint") : hint.rule;
            if (!hint.siteKey.empty())
                reason += L"@" + hint.siteKey;
            info.detectionReason = L"extension:" + reason;
            info.detectionScore = (hint.confidence > 0) ? hint.confidence : 100;

            info.isPlaying = hint.isPlaying;
            if (hint.positionSeconds >= 0)
                info.position = std::chrono::seconds(hint.positionSeconds);
            if (hint.durationSeconds > 0)
                info.duration = std::chrono::seconds(hint.durationSeconds);
            if (info.isPlaying)
                info.startTime = std::chrono::system_clock::now() - info.position;

            return true;
        }

        if (!IsBrowserMedia(info))
            return false;

        bool titleAvailable = !hint.title.empty();
        bool titleMatch = titleAvailable && TitlesLikelyMatch(info.title, hint.title);
        bool artistMatch =
            hint.artist.empty() ||
            info.artist.empty() ||
            TitlesLikelyMatch(info.artist, hint.artist);

        bool durationAvailable = hint.durationSeconds > 0;
        bool nearDuration =
            !durationAvailable ||
            static_cast<int>(info.duration.count()) <= 0 ||
            std::abs(static_cast<int>(info.duration.count()) - hint.durationSeconds) <= 8;

        auto hintAge = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - hint.updatedAt);

        auto lowerSource = ToLowerCopy(info.sourceDisplayName);
        bool weakEligible = lowerSource.empty() || lowerSource == L"web player";

        bool strictAccepted = titleMatch && artistMatch;
        bool looseAccepted = titleAvailable && weakEligible && hint.confidence >= 85 && nearDuration;
        bool freshHighConfidenceAccepted =
            titleAvailable &&
            hint.confidence >= 95 &&
            hintAge <= std::chrono::seconds(6) &&
            (!hint.siteKey.empty() || !hint.pageHost.empty());
        bool serviceOnlyAccepted =
            !titleAvailable &&
            weakEligible &&
            hint.confidence >= 90 &&
            (!hint.siteKey.empty() || !hint.pageHost.empty());

        if (!(strictAccepted || looseAccepted || freshHighConfidenceAccepted || serviceOnlyAccepted))
        {
            return false;
        }

        if (titleAvailable && (strictAccepted || looseAccepted || freshHighConfidenceAccepted))
        {
            auto detectorPos = static_cast<int>(info.position.count());
            auto detectorDur = static_cast<int>(info.duration.count());
            bool detectorTimelineValid =
                detectorDur > 0 &&
                detectorPos >= 0 &&
                detectorPos <= (detectorDur + 5);

            bool hintTimelineValid =
                hint.durationSeconds > 0 &&
                hint.positionSeconds >= 0 &&
                hint.positionSeconds <= (hint.durationSeconds + 5);

            auto hintRuleLower = ToLowerCopy(hint.rule);
            bool youtubeMusicHint =
                ToLowerCopy(hint.siteKey) == L"youtube_music" ||
                hintRuleLower.find(L"youtube-music") != std::wstring::npos;
            bool hintTimelineFromClock = hintRuleLower.find(L"clock") != std::wstring::npos;
            bool highTrustTimeline =
                hint.confidence >= 98 &&
                (!youtubeMusicHint || hintTimelineFromClock);

            bool trustFreshStrictHintTimeline =
                strictAccepted &&
                highTrustTimeline &&
                hintAge <= std::chrono::seconds(6);

            bool useHintTimeline = false;
            if (hintTimelineValid)
            {
                if (!detectorTimelineValid)
                {
                    useHintTimeline = true;
                }
                else
                {
                    auto posDelta = std::abs(detectorPos - hint.positionSeconds);
                    auto durDelta = std::abs(detectorDur - hint.durationSeconds);
                    bool closeAgreement = (posDelta <= 20 && durDelta <= 20);
                    useHintTimeline = closeAgreement || trustFreshStrictHintTimeline;
                }
            }

            info.title = hint.title;
            if (!hint.artist.empty() || info.artist.empty())
                info.artist = hint.artist;
            if (!hint.album.empty() || info.albumTitle.empty())
                info.albumTitle = hint.album;

            info.isPlaying = hint.isPlaying;
            if (useHintTimeline)
            {
                info.position = std::chrono::seconds(hint.positionSeconds);
                info.duration = std::chrono::seconds(hint.durationSeconds);
                if (info.isPlaying)
                    info.startTime = std::chrono::system_clock::now() - info.position;
            }
            else if (info.isPlaying && info.startTime.time_since_epoch().count() <= 0)
            {
                info.startTime = std::chrono::system_clock::now() - info.position;
            }
        }

        info.sourceDisplayName = hint.service;
        if (info.sourceName.empty() && !hint.pageHost.empty())
            info.sourceName = hint.pageHost;
        info.detectedService = hint.service;
        auto reason = hint.rule.empty() ? std::wstring(L"browser-hint") : hint.rule;
        if (!hint.siteKey.empty())
            reason += L"@" + hint.siteKey;
        info.detectionReason = L"extension:" + reason;
        info.detectionScore = (hint.confidence > 0) ? hint.confidence : 100;
        return true;
    }

    const wchar_t* MergeModeToText(MergeMode mode)
    {
        switch (mode)
        {
        case MergeMode::DetectorOnly: return L"detector";
        case MergeMode::ExtensionApplied: return L"extension-applied";
        case MergeMode::ExtensionSynthesized: return L"extension-synthesized";
        case MergeMode::ExtensionPending: return L"extension-pending";
        case MergeMode::NoMedia: return L"no-media";
        default: return L"unknown";
        }
    }

    MergeResult ResolveMergePolicy(const MediaInfo& incoming)
    {
        MergeResult result;
        result.media = incoming;

        result.hintApplied = ApplyBrowserHintOverride(result.media, &result.extensionConnected);

        if (result.hintApplied)
        {
            result.mode = incoming.title.empty() ? MergeMode::ExtensionSynthesized : MergeMode::ExtensionApplied;
            return result;
        }

        if (incoming.title.empty())
        {
            result.mode = MergeMode::NoMedia;
            return result;
        }

        if (result.extensionConnected && IsBrowserMedia(result.media))
        {
            if (!HasFreshBrowserHint() && !result.media.isPlaying)
            {
                result.media = {};
                result.mode = MergeMode::NoMedia;
                return result;
            }

            result.media.detectionReason = L"extension:pending";
            if (result.media.detectionScore < 30)
                result.media.detectionScore = 30;
            result.mode = MergeMode::ExtensionPending;
            return result;
        }

        result.mode = MergeMode::DetectorOnly;
        return result;
    }

    winrt::fire_and_forget ProcessBrowserHintSocket(StreamSocket socket)
    {
        auto lifetime = socket;
        constexpr int kMaxBodyBytes = 64 * 1024;

        try
        {
            DataReader reader(socket.InputStream());
            reader.InputStreamOptions(InputStreamOptions::Partial);

            std::string request;
            request.reserve(8192);

            size_t headerEnd = std::string::npos;
            int contentLength = 0;
            bool requestTooLarge = false;

            for (int i = 0; i < 64; ++i)
            {
                uint32_t loaded = co_await reader.LoadAsync(1024);
                if (!loaded) break;

                std::vector<uint8_t> chunk(loaded);
                reader.ReadBytes(chunk);
                request.append(reinterpret_cast<const char*>(chunk.data()), chunk.size());

                if (request.size() > static_cast<size_t>(kMaxBodyBytes + 8192))
                {
                    requestTooLarge = true;
                    break;
                }

                if (headerEnd == std::string::npos)
                {
                    headerEnd = request.find("\r\n\r\n");
                    if (headerEnd != std::string::npos)
                    {
                        auto headers = request.substr(0, headerEnd + 4);
                        contentLength = ParseContentLength(headers);
                        if (contentLength > kMaxBodyBytes)
                        {
                            requestTooLarge = true;
                            break;
                        }
                    }
                }

                if (headerEnd != std::string::npos)
                {
                    size_t expectedSize = headerEnd + 4 + static_cast<size_t>(contentLength);
                    if (request.size() >= expectedSize)
                        break;
                }
            }

            std::string method;
            std::string path;
            std::string body;
            std::string headers;

            auto firstLineEnd = request.find("\r\n");
            if (firstLineEnd != std::string::npos)
            {
                auto firstLine = request.substr(0, firstLineEnd);
                auto sp1 = firstLine.find(' ');
                auto sp2 = (sp1 == std::string::npos) ? std::string::npos : firstLine.find(' ', sp1 + 1);

                if (sp1 != std::string::npos && sp2 != std::string::npos)
                {
                    method = firstLine.substr(0, sp1);
                    path = firstLine.substr(sp1 + 1, sp2 - sp1 - 1);
                }
            }

            auto bodyPos = request.find("\r\n\r\n");
            if (bodyPos != std::string::npos)
            {
                headers = request.substr(0, bodyPos + 2);
                bodyPos += 4;
                if (bodyPos <= request.size())
                    body = request.substr(bodyPos);
            }

            int statusCode = 200;
            std::string statusText = "OK";
            std::string responseBody = "{\"ok\":true}";

            if (requestTooLarge)
            {
                statusCode = 413;
                statusText = "Payload Too Large";
                responseBody = "{\"ok\":false,\"error\":\"payload-too-large\"}";
            }
            else if (method == "GET" && path == "/v1/browser-hint/token")
            {
                std::string token;
                {
                    auto& state = GetHintServerState();
                    std::lock_guard<std::mutex> lock(state.mutex);
                    token = state.authToken;
                }

                if (token.empty())
                {
                    statusCode = 503;
                    statusText = "Service Unavailable";
                    responseBody = "{\"ok\":false,\"error\":\"token-unavailable\"}";
                }
                else
                {
                    responseBody = "{\"ok\":true,\"token\":\"" + token + "\"}";
                }
            }
            else if (method == "OPTIONS" && path == "/v1/browser-hint")
            {
                statusCode = 200;
                statusText = "OK";
            }
            else if (method == "POST" && path == "/v1/browser-hint")
            {
                std::string expectedToken;
                {
                    auto& state = GetHintServerState();
                    std::lock_guard<std::mutex> lock(state.mutex);
                    expectedToken = state.authToken;
                }

                auto tokenHeader = GetHeaderValue(headers, "x-lrp-token");
                if (expectedToken.empty() || tokenHeader.empty() || tokenHeader != expectedToken)
                {
                    statusCode = 401;
                    statusText = "Unauthorized";
                    responseBody = "{\"ok\":false,\"error\":\"invalid-token\"}";
                }
                else
                {
                    std::wstring parseError;
                    if (!UpdateStoredBrowserHint(body, &parseError))
                    {
                        statusCode = 400;
                        statusText = "Bad Request";
                        auto errUtf8 = winrt::to_string(parseError.empty() ? std::wstring(L"invalid-payload") : parseError);
                        responseBody = "{\"ok\":false,\"error\":\"" + errUtf8 + "\"}";
                    }
                    else
                    {
                        responseBody = "{\"ok\":true}";
                    }
                }
            }
            else
            {
                statusCode = 404;
                statusText = "Not Found";
                responseBody = "{\"ok\":false,\"error\":\"not-found\"}";
            }
            std::ostringstream response;
            response << "HTTP/1.1 " << statusCode << " " << statusText << "\r\n"
                << "Content-Type: application/json\r\n"
                << "Access-Control-Allow-Origin: *\r\n"
                << "Access-Control-Allow-Headers: content-type, x-lrp-extension, x-lrp-token\r\n"
                << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                << "Connection: close\r\n"
                << "Content-Length: " << responseBody.size() << "\r\n\r\n"
                << responseBody;

            DataWriter writer(socket.OutputStream());
            writer.UnicodeEncoding(UnicodeEncoding::Utf8);
            writer.WriteString(winrt::to_hstring(response.str()));
            co_await writer.StoreAsync();
            co_await writer.FlushAsync();
            writer.DetachStream();
        }
        catch (...) {}

        try { socket.Close(); } catch (...) {}
    }

    void StartBrowserHintServer()
    {
        auto& state = GetHintServerState();
        StreamSocketListener listener{ nullptr };

        {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (state.started || state.binding)
                return;

            try
            {
                state.listener = StreamSocketListener();
                state.token = state.listener.ConnectionReceived([](auto const&, StreamSocketListenerConnectionReceivedEventArgs const& args)
                {
                    ProcessBrowserHintSocket(args.Socket());
                });

                state.authToken = GenerateAuthToken();

                state.binding = true;
                listener = state.listener;
            }
            catch (...)
            {
                state.listener = nullptr;
                state.started = false;
                state.binding = false;
                state.authToken.clear();
                return;
            }
        }

        auto bindAsync = [](StreamSocketListener listenerRef) -> winrt::fire_and_forget
        {
            try
            {
                co_await listenerRef.BindEndpointAsync(HostName(L"127.0.0.1"), L"32145");

                auto& state = GetHintServerState();
                std::lock_guard<std::mutex> lock(state.mutex);
                if (state.listener == listenerRef)
                {
                    state.started = true;
                    state.binding = false;
                }
            }
            catch (...)
            {
                auto& state = GetHintServerState();
                std::lock_guard<std::mutex> lock(state.mutex);
                if (state.listener == listenerRef)
                {
                    try { state.listener.ConnectionReceived(state.token); } catch (...) {}
                    try { state.listener.Close(); } catch (...) {}
                    state.listener = nullptr;
                    state.started = false;
                    state.binding = false;
                    state.authToken.clear();
                }
            }
        };

        bindAsync(listener);
    }

    void StopBrowserHintServer()
    {
        auto& state = GetHintServerState();
        std::lock_guard<std::mutex> lock(state.mutex);

        if (!state.started && !state.binding)
            return;

        try { state.listener.ConnectionReceived(state.token); } catch (...) {}
        try { state.listener.Close(); } catch (...) {}

        state.listener = nullptr;
        state.started = false;
        state.binding = false;
        state.authToken.clear();
        state.extensionSeen = false;
        state.extensionSeenAt = {};
        state.hasHint = false;
        state.hint = {};
    }

    std::wstring BuildUtcTimestampString()
    {
        std::time_t now = std::time(nullptr);
        std::tm utcTime{};
        gmtime_s(&utcTime, &now);

        wchar_t buffer[32]{};
        wcsftime(buffer, 32, L"%Y-%m-%dT%H-%M-%SZ", &utcTime);
        return buffer;
    }

    std::filesystem::path GetExecutableDirectory()
    {
        wchar_t modulePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
        return std::filesystem::path(modulePath).parent_path();
    }

    constexpr int kCreativeIdleHoldSeconds = 5;

    bool UsesSeparateCreativeDiscordApp() noexcept
    {
        return std::string(CreativePresenceManager::APP_ID) != std::string(DiscordRPC::APP_ID);
    }

    CreativePriorityMode CreativePriorityModeFromComboIndex(int32_t index)
    {
        return lrp::settings::CreativePriorityModeFromComboIndex(index);
    }

    int32_t CreativePriorityModeToComboIndex(CreativePriorityMode mode)
    {
        return lrp::settings::CreativePriorityModeToComboIndex(mode);
    }

    CreativePrivacyMode CreativePrivacyModeFromComboIndex(int32_t index)
    {
        return lrp::settings::CreativePrivacyModeFromComboIndex(index);
    }

    int32_t CreativePrivacyModeToComboIndex(CreativePrivacyMode mode)
    {
        return lrp::settings::CreativePrivacyModeToComboIndex(mode);
    }

    CreativeIdleBehavior CreativeIdleBehaviorFromComboIndex(int32_t index)
    {
        return lrp::settings::CreativeIdleBehaviorFromComboIndex(index);
    }

    int32_t CreativeIdleBehaviorToComboIndex(CreativeIdleBehavior mode)
    {
        return lrp::settings::CreativeIdleBehaviorToComboIndex(mode);
    }

    int32_t CreativeDetectionModeToComboIndex(CreativeDetectionMode mode)
    {
        return lrp::settings::CreativeDetectionModeToComboIndex(mode);
    }

    CreativeDetectionMode CreativeDetectionModeFromComboIndex(int32_t index)
    {
        return lrp::settings::CreativeDetectionModeFromComboIndex(index);
    }

    int32_t ProductiveDetectionModeToComboIndex(ProductiveDetectionMode mode)
    {
        return lrp::settings::ProductiveDetectionModeToComboIndex(mode);
    }

    ProductiveDetectionMode ProductiveDetectionModeFromComboIndex(int32_t index)
    {
        return lrp::settings::ProductiveDetectionModeFromComboIndex(index);
    }

    std::wstring ToSettingString(CreativePriorityMode mode)
    {
        return lrp::settings::ToSettingString(mode);
    }

    CreativePriorityMode ParseCreativePriorityMode(const std::wstring& value)
    {
        return lrp::settings::ParseCreativePriorityMode(value);
    }

    std::wstring ToSettingString(CreativePrivacyMode mode)
    {
        return lrp::settings::ToSettingString(mode);
    }

    CreativePrivacyMode ParseCreativePrivacyMode(const std::wstring& value)
    {
        return lrp::settings::ParseCreativePrivacyMode(value);
    }

    std::wstring ToSettingString(CreativeIdleBehavior mode)
    {
        return lrp::settings::ToSettingString(mode);
    }

    CreativeIdleBehavior ParseCreativeIdleBehavior(const std::wstring& value)
    {
        return lrp::settings::ParseCreativeIdleBehavior(value);
    }

    std::wstring ToSettingString(CreativeDetectionMode mode)
    {
        return lrp::settings::ToSettingString(mode);
    }

    CreativeDetectionMode ParseCreativeDetectionMode(const std::wstring& value)
    {
        return lrp::settings::ParseCreativeDetectionMode(value);
    }

    std::wstring ToSettingString(ProductiveDetectionMode mode)
    {
        return lrp::settings::ToSettingString(mode);
    }

    ProductiveDetectionMode ParseProductiveDetectionMode(const std::wstring& value)
    {
        return lrp::settings::ParseProductiveDetectionMode(value);
    }

    std::wstring CreativePriorityModeLabel(CreativePriorityMode mode)
    {
        return lrp::settings::CreativePriorityModeLabel(mode);
    }

    std::wstring CreativePrivacyModeLabel(CreativePrivacyMode mode)
    {
        return lrp::settings::CreativePrivacyModeLabel(mode);
    }

    std::wstring CreativeIdleBehaviorLabel(CreativeIdleBehavior mode)
    {
        return lrp::settings::CreativeIdleBehaviorLabel(mode);
    }

    std::wstring CreativeDetectionModeLabel(CreativeDetectionMode mode)
    {
        return lrp::settings::CreativeDetectionModeLabel(mode);
    }

    std::wstring ProductiveDetectionModeLabel(ProductiveDetectionMode mode)
    {
        return lrp::settings::ProductiveDetectionModeLabel(mode);
    }

    int32_t ThemeModeToComboIndex(AppThemeMode mode)
    {
        return lrp::settings::ThemeModeToComboIndex(mode);
    }

    AppThemeMode ThemeModeFromComboIndex(int32_t index)
    {
        return lrp::settings::ThemeModeFromComboIndex(index);
    }

    std::wstring ToSettingString(AppThemeMode mode)
    {
        return lrp::settings::ToSettingString(mode);
    }

    AppThemeMode ParseThemeMode(const std::wstring& value)
    {
        return lrp::settings::ParseThemeMode(value);
    }

    std::wstring ThemeModeLabel(AppThemeMode mode)
    {
        return lrp::settings::ThemeModeLabel(mode);
    }

    bool IsSupportedActivityType(int value)
    {
        return lrp::settings::IsSupportedActivityType(value);
    }

    int ActivityTypeOverrideFromComboIndex(int32_t index)
    {
        return lrp::settings::ActivityTypeOverrideFromComboIndex(index);
    }

    int32_t ActivityTypeOverrideToComboIndex(int value)
    {
        return lrp::settings::ActivityTypeOverrideToComboIndex(value);
    }

    std::wstring ToSettingStringActivityTypeOverride(int value)
    {
        return lrp::settings::ToSettingStringActivityTypeOverride(value);
    }

    int ParseActivityTypeOverride(const std::wstring& value)
    {
        return lrp::settings::ParseActivityTypeOverride(value);
    }

    std::wstring ActivityTypeOverrideLabel(int value)
    {
        return lrp::settings::ActivityTypeOverrideLabel(value);
    }
}

namespace winrt::Last_Rich_Presence::implementation
{
    MainWindow::~MainWindow()
    {
        ShutdownWindow();
    }

    // =====================================================================
    // Initialization
    // =====================================================================

    void MainWindow::InitWindow()
    {
        m_isInitializing = true;
        InitializeComponent();
        m_dispatcherQueue = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        SettingsDiagnosticsPanelHost().ClearRequested({ this, &MainWindow::OnClearDiagnosticsClicked });
        SettingsDiagnosticsPanelHost().ExportJsonRequested({ this, &MainWindow::OnExportDiagnosticsJsonClicked });
        RefreshDiagnosticsPanel();

        try
        {
            auto uiSettings = Windows::UI::ViewManagement::UISettings();
            m_reduceMotionRequested = !uiSettings.AnimationsEnabled();
        }
        catch (...)
        {
            m_reduceMotionRequested = false;
        }

        SongWaveViewport().SizeChanged([this](IInspectable const&, SizeChangedEventArgs const&)
        {
            UpdateSongWaveClip(SongProgressBar().Value());
        });
        SongProgressBar().ValueChanged([this](IInspectable const&, auto const& args)
        {
            UpdateSongWaveClip(args.NewValue());
        });
        UpdateSongWaveClip(0.0);
        SetSongWaveActive(false);

        HomeMiniWaveViewport().SizeChanged([this](IInspectable const&, SizeChangedEventArgs const&)
        {
            UpdateHomeMiniWaveClip(HomeMiniProgressBar().Value());
        });
        HomeMiniProgressBar().ValueChanged([this](IInspectable const&, auto const& args)
        {
            UpdateHomeMiniWaveClip(args.NewValue());
        });
        UpdateHomeMiniWaveClip(0.0);
        SetHomeMiniWaveActive(false);
        m_activePageTag = L"Home";

        m_mediaDetector = std::make_shared<MediaDetector>();
        m_presence = std::make_shared<PresenceManager>();
        m_productiveDetector = std::make_shared<ProductiveDetector>();
        m_productivePresence = std::make_shared<ProductivePresenceManager>();
        m_creativeDetector = std::make_shared<CreativeDetector>();
        m_creativePresence = std::make_shared<CreativePresenceManager>();
        m_lifetimeToken = std::make_shared<std::atomic<bool>>(true);
        UpdateProductivePreview({});
        UpdateHomeProductivePreview();
        UpdateCreativePreview({});
        UpdateHomeCreativePreview();
        AppendDiagnosticLog(L"INFO", L"app", L"Main window initialized");

        // Window size and title bar
        auto appWindow = this->AppWindow();
        if (appWindow)
        {
            Windows::Graphics::SizeInt32 size{ kDefaultWindowWidth, kDefaultWindowHeight };
            appWindow.Resize(size);

            try
            {
                auto iconPath = GetExecutableDirectory() / L"Assets" / L"logo.ico";
                if (std::filesystem::exists(iconPath))
                    appWindow.SetIcon(iconPath.wstring());
            }
            catch (...) {}

            auto presenter = appWindow.Presenter().try_as<Microsoft::UI::Windowing::OverlappedPresenter>();
            if (presenter)
            {
                presenter.IsResizable(false);
                presenter.IsMinimizable(true);
                presenter.IsMaximizable(true);
            }

            // Keep restored window size fixed while still allowing maximize/full-screen.
            appWindow.Changed([](Microsoft::UI::Windowing::AppWindow const& sender, Microsoft::UI::Windowing::AppWindowChangedEventArgs const& args)
            {
                if (!args.DidSizeChange() && !args.DidPresenterChange())
                    return;

                bool allowCurrentSize = false;
                if (auto presenter = sender.Presenter())
                {
                    if (presenter.Kind() != Microsoft::UI::Windowing::AppWindowPresenterKind::Overlapped)
                    {
                        allowCurrentSize = true;
                    }
                    else if (auto overlapped = presenter.try_as<Microsoft::UI::Windowing::OverlappedPresenter>())
                    {
                        allowCurrentSize =
                            overlapped.State() == Microsoft::UI::Windowing::OverlappedPresenterState::Maximized ||
                            overlapped.State() == Microsoft::UI::Windowing::OverlappedPresenterState::Minimized;
                    }
                }

                if (allowCurrentSize)
                    return;

                auto currentSize = sender.Size();
                if (currentSize.Width != kDefaultWindowWidth || currentSize.Height != kDefaultWindowHeight)
                    sender.Resize({ kDefaultWindowWidth, kDefaultWindowHeight });
            });

            // Custom title bar
            auto titleBar = appWindow.TitleBar();
            if (titleBar)
            {
                titleBar.ExtendsContentIntoTitleBar(true);
                titleBar.ButtonBackgroundColor(Windows::UI::Colors::Transparent());
                titleBar.ButtonInactiveBackgroundColor(Windows::UI::Colors::Transparent());
            }
        }

        SetTitleBar(AppTitleBar());
        ApplyThemeMode();
        InitializeSystemTray();

        SourceDebugToggle().Toggled({ this, &MainWindow::OnSourceDebugToggled });

        Closed([this](auto const&, auto const&)
        {
            ShutdownWindow();
        });

        // Load persisted settings
        LoadSettings();

        m_isInitializing = false;

        ApplyLaunchOnStartupState(m_launchOnStartup, false);

        if (m_startMinimizedToTray || IsStartMinimizedLaunchRequested())
        {
            if (!m_isShuttingDown)
                HideWindowToTray();
        }

        // Route detector events to UI thread first, then update UI + presence there
        auto lifetimeToken = m_lifetimeToken;
        m_mediaDetector->SetCallback([this, lifetimeToken, dispatcher = m_dispatcherQueue](const MediaInfo& info)
        {
            if (!dispatcher || !lifetimeToken || !lifetimeToken->load()) return;
            MediaInfo infoCopy = info;
            dispatcher.TryEnqueue([this, lifetimeToken, infoCopy]()
            {
                if (!lifetimeToken || !lifetimeToken->load()) return;
                OnMediaChanged(infoCopy);
            });
        });

        if (m_productiveDetector)
        {
            m_productiveDetector->SetCallback([this, lifetimeToken, dispatcher = m_dispatcherQueue](const ProductiveActivityInfo& info)
            {
                if (!dispatcher || !lifetimeToken || !lifetimeToken->load()) return;
                ProductiveActivityInfo infoCopy = info;
                dispatcher.TryEnqueue([this, lifetimeToken, infoCopy]()
                {
                    if (!lifetimeToken || !lifetimeToken->load()) return;
                    OnProductiveActivityChanged(infoCopy);
                });
            });
        }

        if (m_creativeDetector)
        {
            m_creativeDetector->SetCallback([this, lifetimeToken, dispatcher = m_dispatcherQueue](const CreativeActivityInfo& info)
            {
                if (!dispatcher || !lifetimeToken || !lifetimeToken->load()) return;
                CreativeActivityInfo infoCopy = info;
                dispatcher.TryEnqueue([this, lifetimeToken, infoCopy]()
                {
                    if (!lifetimeToken || !lifetimeToken->load()) return;
                    OnCreativeActivityChanged(infoCopy);
                });
            });
        }

        SetBrowserHintUpdateCallback([this, lifetimeToken, dispatcher = m_dispatcherQueue]()
        {
            if (!dispatcher || !lifetimeToken || !lifetimeToken->load()) return;
            dispatcher.TryEnqueue([this, lifetimeToken]()
            {
                if (!lifetimeToken || !lifetimeToken->load()) return;
                if (m_isShuttingDown || !m_enabled || !m_mediaDetector) return;

                auto current = m_mediaDetector->GetCurrentMedia();
                OnMediaChanged(current);
            });
        });

        // Start everything
        if (m_enabled)
            m_mediaDetector->Start();
        if (m_productiveDetector)
        {
            m_productiveDetector->SetDetectionMode(m_productiveDetectionMode);
            if (m_enabled && m_productiveEnabled)
            {
                m_productiveDetector->Start();
                AppendDiagnosticLog(L"INFO", L"productive", L"Productive detector started");
            }
            else
            {
                AppendDiagnosticLog(L"INFO", L"productive", L"Productive detector paused by settings");
            }
        }

        if (m_creativeDetector)
        {
            m_creativeDetector->SetDetectionMode(m_creativeDetectionMode);
            if (m_enabled && m_creativeEnabled)
            {
                m_creativeDetector->Start();
                AppendDiagnosticLog(L"INFO", L"creative", L"Creativity detector MVP started");
            }
            else if (!m_enabled)
            {
                AppendDiagnosticLog(L"INFO", L"creative", L"Creativity detector paused (global RPC disabled)");
            }
            else
            {
                AppendDiagnosticLog(L"INFO", L"creative", L"Creativity detector disabled by settings");
            }
        }
        if (m_enabled)
            m_presence->Initialize();
        if (m_enabled && m_productiveEnabled && m_productivePresence && !m_productivePresenceRunning)
        {
            m_productivePresence->Initialize();
            m_productivePresenceRunning = true;
            AppendDiagnosticLog(L"INFO", L"productive", L"Productive Discord RPC started (separate app ID)");
        }
        if (m_enabled && m_creativeEnabled && m_creativePresence && !m_creativePresenceRunning)
        {
            m_creativePresence->Initialize();
            m_creativePresenceRunning = true;
            AppendDiagnosticLog(L"INFO", L"creative", L"Creativity Discord RPC started (separate app ID)");
        }
        StartProgressTimer();
        StartBrowserHintServer();
        AppendDiagnosticLog(L"INFO", L"bridge", L"Browser hint bridge started");
        SyncProductiveRpcOutput();
        SyncCreativeRpcOutput();
        if (!m_enabled)
            ApplyGlobalEnableRuntimeState();
        else
            UpdateConnectionStatus();
    }

    void MainWindow::HandleRedirectedActivation()
    {
        AppendDiagnosticLog(L"INFO", L"app", L"Received redirected activation");
        ShowWindowFromTray();
    }

    void MainWindow::InitializeSystemTray()
    {
        if (m_windowHandle)
            return;

        auto windowNative = this->try_as<IWindowNative>();
        if (!windowNative)
            return;

        HWND hwnd{};
        if (FAILED(windowNative->get_WindowHandle(&hwnd)) || !hwnd)
            return;

        m_windowHandle = hwnd;

        SetLastError(0);
        auto previousWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
            m_windowHandle,
            GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(&MainWindow::TrayWindowProc)));
        if (!previousWndProc && GetLastError() != 0)
        {
            m_windowHandle = nullptr;
            AppendDiagnosticLog(L"WARN", L"tray", L"Failed to hook window procedure");
            return;
        }

        m_originalWndProc = previousWndProc;
        {
            std::lock_guard<std::mutex> lock(GetTrayWindowStateMutex());
            GetTrayWindowStateMap()[m_windowHandle] = { this, m_originalWndProc };
        }

        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = m_windowHandle;
        nid.uID = kTrayIconId;
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        nid.uCallbackMessage = kTrayCallbackMessage;
        wcscpy_s(nid.szTip, L"Last Rich Presence");

        auto iconPath = GetExecutableDirectory() / L"Assets" / L"logo.ico";
        if (std::filesystem::exists(iconPath))
        {
            m_trayIconHandle = static_cast<HICON>(LoadImageW(
                nullptr,
                iconPath.c_str(),
                IMAGE_ICON,
                0,
                0,
                LR_LOADFROMFILE | LR_DEFAULTSIZE));
            m_trayIconOwned = (m_trayIconHandle != nullptr);
        }

        if (!m_trayIconHandle)
        {
            m_trayIconHandle = LoadIconW(nullptr, IDI_APPLICATION);
            m_trayIconOwned = false;
        }

        nid.hIcon = m_trayIconHandle;

        if (Shell_NotifyIconW(NIM_ADD, &nid))
        {
            nid.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &nid);
            m_trayIconAdded = true;
            AppendDiagnosticLog(L"INFO", L"tray", L"System tray icon initialized");
        }
        else
        {
            AppendDiagnosticLog(L"WARN", L"tray", L"Failed to add system tray icon");
            CleanupSystemTray();
        }
    }

    void MainWindow::CleanupSystemTray()
    {
        if (m_trayIconAdded && m_windowHandle)
        {
            NOTIFYICONDATAW nid{};
            nid.cbSize = sizeof(nid);
            nid.hWnd = m_windowHandle;
            nid.uID = kTrayIconId;
            Shell_NotifyIconW(NIM_DELETE, &nid);
            m_trayIconAdded = false;
        }

        if (m_trayIconHandle && m_trayIconOwned)
            DestroyIcon(m_trayIconHandle);
        m_trayIconHandle = nullptr;
        m_trayIconOwned = false;

        if (m_windowHandle && m_originalWndProc)
        {
            SetWindowLongPtrW(
                m_windowHandle,
                GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(m_originalWndProc));
        }

        if (m_windowHandle)
        {
            std::lock_guard<std::mutex> lock(GetTrayWindowStateMutex());
            GetTrayWindowStateMap().erase(m_windowHandle);
        }

        m_originalWndProc = nullptr;
        m_windowHandle = nullptr;
    }

    void MainWindow::ShowTrayContextMenu()
    {
        if (!m_windowHandle)
            return;

        auto menu = CreatePopupMenu();
        if (!menu)
            return;

        auto showHideText = IsWindowVisible(m_windowHandle) ? L"Hide window" : L"Show window";
        auto togglePresenceText = m_enabled ? L"Disable Rich Presence" : L"Enable Rich Presence";

        AppendMenuW(menu, MF_STRING, kTrayMenuShowHide, showHideText);
        AppendMenuW(menu, MF_STRING, kTrayMenuTogglePresence, togglePresenceText);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kTrayMenuExit, L"Exit");

        POINT point{};
        GetCursorPos(&point);
        SetForegroundWindow(m_windowHandle);

        auto command = TrackPopupMenu(
            menu,
            TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
            point.x,
            point.y,
            0,
            m_windowHandle,
            nullptr);

        DestroyMenu(menu);

        if (command != 0)
            HandleTrayMenuCommand(static_cast<uint32_t>(command));

        PostMessageW(m_windowHandle, WM_NULL, 0, 0);
    }

    void MainWindow::ShowWindowFromTray()
    {
        if (!m_windowHandle)
            return;

        // If app started hidden, the XAML window may not have been activated yet.
        Activate();
        ShowWindow(m_windowHandle, SW_SHOW);
        ShowWindow(m_windowHandle, SW_RESTORE);
        SetForegroundWindow(m_windowHandle);
        m_hiddenToTray = false;

        bool resumeWave = !m_lastMedia.title.empty() && m_lastMedia.isPlaying;
        SetSongWaveActive(resumeWave);
        SetHomeMiniWaveActive(resumeWave);
        UpdateConnectionStatus();
    }

    void MainWindow::HideWindowToTray()
    {
        if (!m_windowHandle || m_hiddenToTray)
            return;

        ShowWindow(m_windowHandle, SW_HIDE);
        m_hiddenToTray = true;
        SetSongWaveActive(false);
        SetHomeMiniWaveActive(false);
        SetLivePulseActive(false);
        HideTrackTransitionSkeleton();
        AppendDiagnosticLog(L"INFO", L"tray", L"Window minimized to system tray");
    }

    void MainWindow::ToggleWindowVisibilityFromTray()
    {
        auto now = std::chrono::steady_clock::now();
        if (m_lastTrayToggleAt.time_since_epoch().count() > 0)
        {
            auto sinceLastToggle = now - m_lastTrayToggleAt;
            if (sinceLastToggle < std::chrono::milliseconds(250))
                return;
        }
        m_lastTrayToggleAt = now;

        if (m_windowHandle && IsWindowVisible(m_windowHandle))
            HideWindowToTray();
        else
            ShowWindowFromTray();
    }

    void MainWindow::ApplyLaunchOnStartupState(bool enabled, bool userInitiated)
    {
        auto applyTask = [](winrt::weak_ref<MainWindow> weak, bool enabledValue, bool userInitiatedValue) -> winrt::fire_and_forget
        {
            auto strong = weak.get();
            if (!strong)
                co_return;

            bool applied = false;
            bool usedRunRegistryFallback = false;
            bool shouldUseRunRegistryFallback = !HasPackageIdentity();

            if (!shouldUseRunRegistryFallback)
            {
                try
                {
                    auto startupTask = co_await Windows::ApplicationModel::StartupTask::GetAsync(kStartupTaskId);
                    auto state = startupTask.State();

                    if (enabledValue)
                    {
                        if (state == Windows::ApplicationModel::StartupTaskState::Disabled)
                            state = co_await startupTask.RequestEnableAsync();

                        applied =
                            state == Windows::ApplicationModel::StartupTaskState::Enabled ||
                            state == Windows::ApplicationModel::StartupTaskState::EnabledByPolicy;

                        if (userInitiatedValue && !applied)
                        {
                            strong->AppendDiagnosticLog(
                                L"WARN",
                                L"settings",
                                L"Startup enable request was denied by system policy/user setting");
                        }
                    }
                    else
                    {
                        startupTask.Disable();
                        applied = false;
                    }
                }
                catch (...)
                {
                    shouldUseRunRegistryFallback = true;
                }
            }

            if (shouldUseRunRegistryFallback)
            {
                usedRunRegistryFallback = true;
                auto registryWriteOk = SetRunStartupEnabledForCurrentExecutable(
                    enabledValue,
                    strong->m_startMinimizedToTray);
                if (registryWriteOk)
                {
                    applied = enabledValue ? IsRunStartupEnabledForCurrentExecutable() : false;
                }
                else
                {
                    applied = IsRunStartupEnabledForCurrentExecutable();
                }

                if (userInitiatedValue && !applied && enabledValue)
                    strong->AppendDiagnosticLog(L"WARN", L"settings", L"Failed to enable launch on startup");
            }
            else
            {
                auto runConfigured = IsRunStartupEnabledForCurrentExecutable();

                if (enabledValue)
                {
                    // If Run-key startup is already present (for example, from an Inno install),
                    // keep it in sync with the minimized startup switch and treat it as valid.
                    if (runConfigured)
                    {
                        usedRunRegistryFallback = true;
                        if (SetRunStartupEnabledForCurrentExecutable(true, strong->m_startMinimizedToTray))
                            runConfigured = IsRunStartupEnabledForCurrentExecutable();
                    }

                    // If StartupTask could not be enabled, fall back to Run key even with package identity.
                    if (!applied && !runConfigured)
                    {
                        usedRunRegistryFallback = true;
                        if (SetRunStartupEnabledForCurrentExecutable(true, strong->m_startMinimizedToTray))
                            runConfigured = IsRunStartupEnabledForCurrentExecutable();
                    }

                    applied = applied || runConfigured;

                    if (userInitiatedValue && !applied)
                        strong->AppendDiagnosticLog(L"WARN", L"settings", L"Failed to enable launch on startup");
                }
                else
                {
                    // Disable any existing Run-key entry to avoid mixed startup sources.
                    if (runConfigured)
                    {
                        usedRunRegistryFallback = true;
                        SetRunStartupEnabledForCurrentExecutable(false, false);
                        runConfigured = IsRunStartupEnabledForCurrentExecutable();
                    }

                    applied = applied || runConfigured;
                }
            }

            strong = weak.get();
            if (!strong)
                co_return;

            strong->m_launchOnStartup = applied;

            bool initBefore = strong->m_isInitializing;
            strong->m_isInitializing = true;
            strong->LaunchOnStartupToggle().IsOn(applied);
            strong->m_isInitializing = initBefore;

            strong->SaveSettings();

            if (userInitiatedValue)
            {
                if (usedRunRegistryFallback)
                {
                    strong->AppendDiagnosticLog(
                        L"INFO",
                        L"settings",
                        L"Launch on startup configured using registry fallback");
                }

                strong->AppendDiagnosticLog(
                    L"INFO",
                    L"settings",
                    applied ? L"Launch on startup enabled" : L"Launch on startup disabled");
            }
        };

        applyTask(get_weak(), enabled, userInitiated);
    }

    void MainWindow::HandleTrayMenuCommand(uint32_t commandId)
    {
        switch (commandId)
        {
        case kTrayMenuShowHide:
            ToggleWindowVisibilityFromTray();
            break;
        case kTrayMenuTogglePresence:
            EnableToggle().IsOn(!EnableToggle().IsOn());
            break;
        case kTrayMenuExit:
            m_exitRequested = true;
            AppendDiagnosticLog(L"INFO", L"tray", L"Exit requested from system tray");
            CleanupSystemTray();
            Close();
            break;
        default:
            break;
        }
    }

    bool MainWindow::TryHandleTrayWindowMessage(HWND, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result)
    {
        result = 0;

        if (message == WM_CLOSE)
        {
            if (!m_isShuttingDown && !m_exitRequested && m_closeToTrayOnClose && GetSystemMetrics(SM_SHUTTINGDOWN) == 0)
            {
                HideWindowToTray();
                return true;
            }

            return false;
        }

        if (message == WM_COMMAND)
        {
            auto commandId = static_cast<uint32_t>(LOWORD(wParam));
            if (commandId == kTrayMenuShowHide || commandId == kTrayMenuTogglePresence || commandId == kTrayMenuExit)
            {
                HandleTrayMenuCommand(commandId);
                return true;
            }

            return false;
        }

        if (message == kTrayCallbackMessage)
        {
            UINT iconId = 0;
            UINT eventCode = 0;

            auto eventLowWord = LOWORD(static_cast<DWORD_PTR>(lParam));
            auto eventHighWord = HIWORD(static_cast<DWORD_PTR>(lParam));

            bool looksLikeV4Message =
                eventLowWord == WM_CONTEXTMENU ||
                eventLowWord == WM_RBUTTONUP ||
                eventLowWord == WM_LBUTTONUP ||
                eventLowWord == WM_LBUTTONDBLCLK ||
                eventLowWord == NIN_SELECT ||
                eventLowWord == NIN_KEYSELECT;

            if (looksLikeV4Message)
            {
                iconId = eventHighWord;
                eventCode = eventLowWord;
            }
            else
            {
                iconId = static_cast<UINT>(wParam);
                eventCode = static_cast<UINT>(lParam);
            }

            if (iconId != kTrayIconId)
                return false;

            if (eventCode == WM_CONTEXTMENU || eventCode == WM_RBUTTONUP)
            {
                ShowTrayContextMenu();
                return true;
            }

            if (eventCode == WM_LBUTTONUP || eventCode == WM_LBUTTONDBLCLK || eventCode == NIN_SELECT || eventCode == NIN_KEYSELECT)
            {
                if (m_trayLeftClickToggles)
                    ToggleWindowVisibilityFromTray();
                else
                    ShowWindowFromTray();
                return true;
            }
        }

        return false;
    }

    LRESULT CALLBACK MainWindow::TrayWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        MainWindow* currentWindow = nullptr;
        WNDPROC originalWndProc = nullptr;

        {
            std::lock_guard<std::mutex> lock(GetTrayWindowStateMutex());
            auto it = GetTrayWindowStateMap().find(hwnd);
            if (it != GetTrayWindowStateMap().end())
            {
                currentWindow = it->second.window;
                originalWndProc = it->second.originalWndProc;
            }
        }

        LRESULT result = 0;
        if (currentWindow && currentWindow->TryHandleTrayWindowMessage(hwnd, message, wParam, lParam, result))
            return result;

        if (originalWndProc)
            return CallWindowProcW(originalWndProc, hwnd, message, wParam, lParam);

        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    // =====================================================================
    // Navigation
    // =====================================================================

    bool MainWindow::IsMotionEnabled() const
    {
        if (m_isShuttingDown || m_hiddenToTray || m_reduceMotionRequested)
            return false;

        if (m_windowHandle && IsIconic(m_windowHandle))
            return false;

        return true;
    }

    void MainWindow::ApplyThemeMode()
    {
        auto root = Content().try_as<FrameworkElement>();
        if (!root)
            return;

        switch (m_themeMode)
        {
        case AppThemeMode::Light:
            root.RequestedTheme(ElementTheme::Light);
            break;
        case AppThemeMode::Dark:
            root.RequestedTheme(ElementTheme::Dark);
            break;
        default:
            root.RequestedTheme(ElementTheme::Default);
            break;
        }

        auto appWindow = this->AppWindow();
        if (!appWindow)
            return;

        auto titleBar = appWindow.TitleBar();
        if (!titleBar)
            return;

        bool useDarkGlyphs = false;
        if (m_themeMode == AppThemeMode::Light)
        {
            useDarkGlyphs = true;
        }
        else if (m_themeMode == AppThemeMode::FollowSystem)
        {
            useDarkGlyphs = (root.ActualTheme() == ElementTheme::Light);
        }

        auto foreground = useDarkGlyphs
            ? ColorHelper::FromArgb(0xFF, 0x16, 0x16, 0x16)
            : ColorHelper::FromArgb(0xFF, 0xF2, 0xF2, 0xF2);
        auto inactiveForeground = useDarkGlyphs
            ? ColorHelper::FromArgb(0x99, 0x16, 0x16, 0x16)
            : ColorHelper::FromArgb(0x99, 0xF2, 0xF2, 0xF2);
        auto hoverBackground = useDarkGlyphs
            ? ColorHelper::FromArgb(0x14, 0x00, 0x00, 0x00)
            : ColorHelper::FromArgb(0x22, 0xFF, 0xFF, 0xFF);
        auto pressedBackground = useDarkGlyphs
            ? ColorHelper::FromArgb(0x22, 0x00, 0x00, 0x00)
            : ColorHelper::FromArgb(0x33, 0xFF, 0xFF, 0xFF);

        titleBar.ButtonBackgroundColor(Windows::UI::Colors::Transparent());
        titleBar.ButtonInactiveBackgroundColor(Windows::UI::Colors::Transparent());
        titleBar.ButtonForegroundColor(foreground);
        titleBar.ButtonHoverForegroundColor(foreground);
        titleBar.ButtonPressedForegroundColor(foreground);
        titleBar.ButtonInactiveForegroundColor(inactiveForeground);
        titleBar.ButtonHoverBackgroundColor(hoverBackground);
        titleBar.ButtonPressedBackgroundColor(pressedBackground);
    }

    void MainWindow::TransitionElementVisibility(FrameworkElement const& element, bool show, double offsetY)
    {
        if (!element)
            return;

        auto transform = element.RenderTransform().try_as<TranslateTransform>();
        if (!transform)
        {
            transform = TranslateTransform();
            element.RenderTransform(transform);
        }

        if (!IsMotionEnabled())
        {
            element.Tag(box_value(show ? L"show" : L"hide"));
            element.Visibility(show ? Visibility::Visible : Visibility::Collapsed);
            element.Opacity(1.0);
            transform.Y(0.0);
            return;
        }

        if (show)
        {
            element.Tag(box_value(L"show"));

            if (element.Visibility() == Visibility::Visible && element.Opacity() >= 0.99)
                return;

            element.Visibility(Visibility::Visible);
            element.Opacity(0.0);
            transform.Y(offsetY);

            auto fade = Microsoft::UI::Xaml::Media::Animation::DoubleAnimation();
            fade.From(0.0);
            fade.To(1.0);
            fade.Duration(MotionDuration(MotionTokens::StandardMs));
            fade.EasingFunction(MotionEaseOut());

            auto slide = Microsoft::UI::Xaml::Media::Animation::DoubleAnimation();
            slide.From(offsetY);
            slide.To(0.0);
            slide.Duration(MotionDuration(MotionTokens::StandardMs));
            slide.EasingFunction(MotionEaseOut());

            auto storyboard = Microsoft::UI::Xaml::Media::Animation::Storyboard();
            storyboard.Children().Append(fade);
            storyboard.Children().Append(slide);
            Microsoft::UI::Xaml::Media::Animation::Storyboard::SetTarget(fade, element);
            Microsoft::UI::Xaml::Media::Animation::Storyboard::SetTargetProperty(fade, L"Opacity");
            Microsoft::UI::Xaml::Media::Animation::Storyboard::SetTarget(slide, transform);
            Microsoft::UI::Xaml::Media::Animation::Storyboard::SetTargetProperty(slide, L"Y");

            auto weakElement = make_weak(element);
            auto weakTransform = make_weak(transform);
            storyboard.Completed([weakElement, weakTransform](IInspectable const&, IInspectable const&)
            {
                if (auto strongElement = weakElement.get())
                {
                    auto tag = unbox_value_or<hstring>(strongElement.Tag(), L"");
                    if (tag == L"show")
                        strongElement.Opacity(1.0);
                }
                if (auto strongTransform = weakTransform.get())
                    strongTransform.Y(0.0);
            });

            storyboard.Begin();
            return;
        }

        if (element.Visibility() != Visibility::Visible)
            return;

        element.Tag(box_value(L"hide"));

        auto fade = Microsoft::UI::Xaml::Media::Animation::DoubleAnimation();
        fade.From(element.Opacity());
        fade.To(0.0);
        fade.Duration(MotionDuration(MotionTokens::FastMs));
        fade.EasingFunction(MotionEaseIn());

        auto slide = Microsoft::UI::Xaml::Media::Animation::DoubleAnimation();
        slide.From(0.0);
        slide.To(-offsetY * 0.55);
        slide.Duration(MotionDuration(MotionTokens::FastMs));
        slide.EasingFunction(MotionEaseIn());

        auto storyboard = Microsoft::UI::Xaml::Media::Animation::Storyboard();
        storyboard.Children().Append(fade);
        storyboard.Children().Append(slide);
        Microsoft::UI::Xaml::Media::Animation::Storyboard::SetTarget(fade, element);
        Microsoft::UI::Xaml::Media::Animation::Storyboard::SetTargetProperty(fade, L"Opacity");
        Microsoft::UI::Xaml::Media::Animation::Storyboard::SetTarget(slide, transform);
        Microsoft::UI::Xaml::Media::Animation::Storyboard::SetTargetProperty(slide, L"Y");

        auto weakElement = make_weak(element);
        auto weakTransform = make_weak(transform);
        storyboard.Completed([weakElement, weakTransform](IInspectable const&, IInspectable const&)
        {
            if (auto strongElement = weakElement.get())
            {
                auto tag = unbox_value_or<hstring>(strongElement.Tag(), L"");
                if (tag == L"hide")
                {
                    strongElement.Visibility(Visibility::Collapsed);
                    strongElement.Opacity(1.0);
                }
            }
            if (auto strongTransform = weakTransform.get())
                strongTransform.Y(0.0);
        });

        storyboard.Begin();
    }

    void MainWindow::OnNavSelectionChanged(NavigationView const&, NavigationViewSelectionChangedEventArgs const& args)
    {
        auto selectedItem = args.SelectedItem().try_as<NavigationViewItem>();
        if (!selectedItem) return;

        auto tag = unbox_value_or<hstring>(selectedItem.Tag(), L"");
        if (tag.empty()) return;

        auto pageForTag = [this](hstring const& pageTag) -> FrameworkElement
        {
            if (pageTag == L"Home") return HomePage();
            if (pageTag == L"Music") return MusicPage();
            if (pageTag == L"Productivity") return ProductivityPage();
            if (pageTag == L"Creative") return CreativePage();
            if (pageTag == L"Settings") return SettingsPage();
            return nullptr;
        };

        auto showOnlyPage = [this](hstring const& visibleTag)
        {
            HomePage().Visibility(visibleTag == L"Home" ? Visibility::Visible : Visibility::Collapsed);
            MusicPage().Visibility(visibleTag == L"Music" ? Visibility::Visible : Visibility::Collapsed);
            ProductivityPage().Visibility(visibleTag == L"Productivity" ? Visibility::Visible : Visibility::Collapsed);
            CreativePage().Visibility(visibleTag == L"Creative" ? Visibility::Visible : Visibility::Collapsed);
            SettingsPage().Visibility(visibleTag == L"Settings" ? Visibility::Visible : Visibility::Collapsed);
        };

        auto resetScrollTop = [](FrameworkElement const& page)
        {
            auto scrollViewer = page.try_as<ScrollViewer>();
            if (!scrollViewer)
                return;

            if (scrollViewer.VerticalOffset() <= 0.5)
                return;

            try
            {
                scrollViewer.ChangeView(nullptr, 0.0, nullptr, true);
            }
            catch (...) {}
        };

        auto incoming = pageForTag(tag);
        if (!incoming)
            return;

        if (m_pageTransitionInProgress)
        {
            if (tag != hstring(m_activePageTag))
                m_queuedPageTag = tag.c_str();
            return;
        }

        if (tag == hstring(m_activePageTag))
            return;

        auto outgoing = pageForTag(hstring(m_activePageTag));

        auto pageOrder = [](hstring const& value)
        {
            if (value == L"Home") return 0;
            if (value == L"Music") return 1;
            if (value == L"Creative") return 2;
            if (value == L"Productivity") return 3;
            if (value == L"Settings") return 4;
            return 0;
        };

        bool forward = pageOrder(tag) >= pageOrder(hstring(m_activePageTag));
        bool involvesSettings = (tag == L"Settings" || hstring(m_activePageTag) == L"Settings");
        double incomingOffset = involvesSettings ? 10.0 : 16.0;
        double outgoingOffset = involvesSettings ? 6.0 : 10.0;

        if (m_pageTransitionStoryboard)
        {
            try { m_pageTransitionStoryboard.Stop(); } catch (...) {}
            m_pageTransitionStoryboard = nullptr;
            m_pageTransitionInProgress = false;
            showOnlyPage(hstring(m_activePageTag));
        }

        resetScrollTop(incoming);

        if (!IsMotionEnabled() || !outgoing || outgoing == incoming)
        {
            showOnlyPage(tag);
            m_activePageTag = tag.c_str();
            m_pageTransitionInProgress = false;
            return;
        }

        auto outgoingTransform = outgoing.RenderTransform().try_as<TranslateTransform>();
        if (!outgoingTransform)
        {
            outgoingTransform = TranslateTransform();
            outgoing.RenderTransform(outgoingTransform);
        }

        auto incomingTransform = incoming.RenderTransform().try_as<TranslateTransform>();
        if (!incomingTransform)
        {
            incomingTransform = TranslateTransform();
            incoming.RenderTransform(incomingTransform);
        }

        outgoing.Visibility(Visibility::Visible);
        outgoing.Opacity(1.0);
        outgoingTransform.X(0.0);

        incoming.Visibility(Visibility::Visible);
        incoming.Opacity(0.0);
        incomingTransform.X(forward ? incomingOffset : -incomingOffset);

        auto outFade = Microsoft::UI::Xaml::Media::Animation::DoubleAnimation();
        outFade.From(1.0);
        outFade.To(0.0);
        outFade.Duration(MotionDuration(MotionTokens::PageTransitionMs));
        outFade.EasingFunction(MotionEaseIn());

        auto outShift = Microsoft::UI::Xaml::Media::Animation::DoubleAnimation();
        outShift.From(0.0);
        outShift.To(forward ? -outgoingOffset : outgoingOffset);
        outShift.Duration(MotionDuration(MotionTokens::PageTransitionMs));
        outShift.EasingFunction(MotionEaseIn());

        auto inFade = Microsoft::UI::Xaml::Media::Animation::DoubleAnimation();
        inFade.From(0.0);
        inFade.To(1.0);
        inFade.Duration(MotionDuration(MotionTokens::PageTransitionMs));
        inFade.EasingFunction(MotionEaseOut());

        auto inShift = Microsoft::UI::Xaml::Media::Animation::DoubleAnimation();
        inShift.From(forward ? incomingOffset : -incomingOffset);
        inShift.To(0.0);
        inShift.Duration(MotionDuration(MotionTokens::PageTransitionMs));
        inShift.EasingFunction(MotionEaseOut());

        auto storyboard = Microsoft::UI::Xaml::Media::Animation::Storyboard();
        storyboard.Children().Append(outFade);
        storyboard.Children().Append(outShift);
        storyboard.Children().Append(inFade);
        storyboard.Children().Append(inShift);
        Microsoft::UI::Xaml::Media::Animation::Storyboard::SetTarget(outFade, outgoing);
        Microsoft::UI::Xaml::Media::Animation::Storyboard::SetTargetProperty(outFade, L"Opacity");
        Microsoft::UI::Xaml::Media::Animation::Storyboard::SetTarget(outShift, outgoingTransform);
        Microsoft::UI::Xaml::Media::Animation::Storyboard::SetTargetProperty(outShift, L"X");
        Microsoft::UI::Xaml::Media::Animation::Storyboard::SetTarget(inFade, incoming);
        Microsoft::UI::Xaml::Media::Animation::Storyboard::SetTargetProperty(inFade, L"Opacity");
        Microsoft::UI::Xaml::Media::Animation::Storyboard::SetTarget(inShift, incomingTransform);
        Microsoft::UI::Xaml::Media::Animation::Storyboard::SetTargetProperty(inShift, L"X");

        auto weak = get_weak();
        auto outgoingTag = hstring(m_activePageTag);
        auto incomingTag = tag;
        storyboard.Completed([weak, outgoingTag, incomingTag](IInspectable const&, IInspectable const&)
        {
            auto strong = weak.get();
            if (!strong)
                return;

            auto pageForTagInner = [strong](hstring const& pageTag) -> FrameworkElement
            {
                if (pageTag == L"Home") return strong->HomePage();
                if (pageTag == L"Music") return strong->MusicPage();
                if (pageTag == L"Productivity") return strong->ProductivityPage();
                if (pageTag == L"Creative") return strong->CreativePage();
                if (pageTag == L"Settings") return strong->SettingsPage();
                return nullptr;
            };

            auto outgoingPage = pageForTagInner(outgoingTag);
            auto incomingPage = pageForTagInner(incomingTag);

            if (outgoingPage)
            {
                outgoingPage.Visibility(Visibility::Collapsed);
                outgoingPage.Opacity(1.0);
                if (auto transform = outgoingPage.RenderTransform().try_as<TranslateTransform>())
                    transform.X(0.0);
            }

            if (incomingPage)
            {
                incomingPage.Visibility(Visibility::Visible);
                incomingPage.Opacity(1.0);
                if (auto transform = incomingPage.RenderTransform().try_as<TranslateTransform>())
                    transform.X(0.0);
            }

            strong->m_pageTransitionStoryboard = nullptr;
            strong->m_pageTransitionInProgress = false;

            auto queuedTag = hstring(strong->m_queuedPageTag);
            strong->m_queuedPageTag.clear();
            if (queuedTag.empty() || queuedTag == incomingTag)
                return;

            auto queuedPage = pageForTagInner(queuedTag);
            if (!queuedPage)
                return;

            auto navItemForTagInner = [strong](hstring const& pageTag) -> NavigationViewItem
            {
                if (pageTag == L"Home") return strong->HomeNavItem();
                if (pageTag == L"Music") return strong->MusicNavItem();
                if (pageTag == L"Productivity") return strong->ProductivityNavItem();
                if (pageTag == L"Creative") return strong->CreativeNavItem();
                if (pageTag == L"Settings") return strong->SettingsNavItem();
                return nullptr;
            };

            auto queuedItem = navItemForTagInner(queuedTag);
            if (!queuedItem)
                return;

            auto navView = strong->NavView();
            auto selected = navView.SelectedItem().try_as<NavigationViewItem>();
            auto selectedTag = selected ? unbox_value_or<hstring>(selected.Tag(), L"") : hstring{};
            if (selectedTag != queuedTag)
            {
                navView.SelectedItem(queuedItem);
                return;
            }

            auto currentItem = navItemForTagInner(incomingTag);
            if (currentItem)
                navView.SelectedItem(currentItem);
            navView.SelectedItem(queuedItem);
        });

        m_queuedPageTag.clear();
        m_pageTransitionInProgress = true;
        m_activePageTag = tag.c_str();
        m_pageTransitionStoryboard = storyboard;
        storyboard.Begin();
    }

    // =====================================================================
    // Toggle handlers
    // =====================================================================

    void MainWindow::ApplyGlobalEnableRuntimeState()
    {
        if (m_enabled)
        {
            if (m_mediaDetector)
                m_mediaDetector->Start();
            if (m_presence && !m_presence->IsConnected())
                m_presence->Initialize();
        }
        else
        {
            if (m_mediaDetector)
                m_mediaDetector->Stop();
            if (m_presence)
                m_presence->ClearPresence();
            m_lastPresencePushPlaying = false;

            // Reset media-facing UI while the master switch is off.
            SongTitle().Text(L"Nothing playing");
            ArtistName().Text(L"\x2014");
            AlbumName().Text(L"");
            PositionText().Text(L"0:00");
            DurationText().Text(L"0:00");
            SetSongProgress(0);
            SetHomeMiniProgress(0);
            SourceText().Text(L"No source");
            PlayPauseIcon().Glyph(L"\xE768");
            PlaybackStateText().Text(L"Idle");
            HomeSourceValue().Text(L"No active source");
            HomeSourceSubtext().Text(L"Waiting for active session");
            HomeDetectedViaText().Text(L"Detected via --");
            HomeMiniTitle().Text(L"Nothing playing");
            HomeMiniArtist().Text(L"Start playback or activity");
            HomeMiniTimer().Text(L"0:00 / 0:00");
            HomeMiniPlayIcon().Glyph(L"\xE711");
            HomeMiniStatusText().Text(L"Off");
            auto mutedBrush = Application::Current().Resources()
                .Lookup(box_value(L"TextFillColorSecondaryBrush"))
                .as<Brush>();
            HomeMiniPlayIcon().Foreground(mutedBrush);
            HomeMiniStatusText().Foreground(mutedBrush);
            TransitionElementVisibility(HomePausedChip(), false, 4.0);

            MusicEmptyState().Visibility(Visibility::Visible);
            NowPlayingCard().Visibility(Visibility::Collapsed);
            TransitionElementVisibility(SourceBadge(), false, 6.0);
            SourceDebugText().Visibility(Visibility::Collapsed);
            SourceDebugText().Text(L"");
            HomeMiniPlayer().Visibility(Visibility::Visible);

            AlbumThumbnail().Source(nullptr);
            HomeMiniThumbnail().Source(nullptr);
            HomeHealthText().Text(L"Discord: OFF | Bridge: OFF | Hint age: --");
            SetLivePulseActive(false);
            SetHomeMiniWaveActive(false);
            SetSongWaveActive(false);
            HideTrackTransitionSkeleton();
        }

        UpdateProductivePreview(m_lastProductiveActivity);
        UpdateCreativePreview(m_lastCreativeActivity);
        UpdateHomeCreativePreview();
        UpdateConnectionStatus();
    }

    void MainWindow::OnEnableToggled(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_isInitializing) return;

        m_enabled = EnableToggle().IsOn();
        ApplyProductiveRuntimeState();
        ApplyCreativeDetectorRuntimeState();
        ApplyGlobalEnableRuntimeState();
        SaveSettings();
    }

    void MainWindow::OnTimestampToggled(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_isInitializing) return;
        m_presence->SetShowTimestamps(TimestampToggle().IsOn());
        SaveSettings();
        RefreshMediaPresenceOutput();
    }

    void MainWindow::OnSourceToggled(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_isInitializing) return;
        m_presence->SetShowSource(SourceToggle().IsOn());
        SaveSettings();

        UpdateSourceBadge(m_lastMedia);

        RefreshMediaPresenceOutput();
    }

    void MainWindow::OnSourceDebugToggled(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_isInitializing) return;
        m_sourceDebugMode = SourceDebugToggle().IsOn();
        SaveSettings();
        UpdateSourceBadge(m_lastMedia);
    }

    void MainWindow::OnPausedToggled(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_isInitializing) return;
        m_presence->SetShowPaused(PausedToggle().IsOn());
        SaveSettings();
        RefreshMediaPresenceOutput();
    }

    void MainWindow::OnAlbumArtToggled(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_isInitializing) return;
        m_presence->SetShowAlbumArt(AlbumArtToggle().IsOn());
        SaveSettings();
        RefreshMediaPresenceOutput();
    }

    void MainWindow::OnDefaultIdleStatusToggled(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_isInitializing) return;
        m_showDefaultIdleStatus = DefaultIdleStatusToggle().IsOn();
        m_presence->SetShowIdleStatus(m_showDefaultIdleStatus);
        WriteUserPreferenceBool(lrp::startup::kShowDefaultIdleStatusRegistryValueName, m_showDefaultIdleStatus);
        SaveSettings();
        RefreshMediaPresenceOutput();
    }

    void MainWindow::OnCloseToTrayToggled(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_isInitializing) return;
        m_closeToTrayOnClose = CloseToTrayToggle().IsOn();
        SaveSettings();
    }

    void MainWindow::OnLaunchOnStartupToggled(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_isInitializing) return;
        m_launchOnStartup = LaunchOnStartupToggle().IsOn();
        ApplyLaunchOnStartupState(m_launchOnStartup, true);
    }

    void MainWindow::OnStartMinimizedToggled(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_isInitializing) return;
        m_startMinimizedToTray = StartMinimizedToggle().IsOn();
        SaveSettings();

        // Keep startup command-line switches in sync when launch-on-startup is enabled.
        if (m_launchOnStartup)
            ApplyLaunchOnStartupState(true, false);
    }

    void MainWindow::OnTrayLeftClickToggleToggled(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_isInitializing) return;
        m_trayLeftClickToggles = TrayLeftClickToggle().IsOn();
        SaveSettings();
    }

    void MainWindow::OnSensitiveKeywordFilterToggled(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_isInitializing) return;
        m_sensitiveKeywordFilter = SensitiveFilterToggle().IsOn();
        m_presence->SetSensitiveKeywordFilter(m_sensitiveKeywordFilter);
        SaveSettings();
        RefreshMediaPresenceOutput();
    }

    void MainWindow::OnStrictBrowserPrivacyToggled(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_isInitializing) return;
        m_strictBrowserPrivacy = StrictBrowserPrivacyToggle().IsOn();
        m_presence->SetStrictBrowserPrivacy(m_strictBrowserPrivacy);
        SaveSettings();
        RefreshMediaPresenceOutput();
    }

    void MainWindow::OnSuppressBrowserArtToggled(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_isInitializing) return;
        m_suppressBrowserAlbumArt = SuppressBrowserArtToggle().IsOn();
        m_presence->SetSuppressBrowserAlbumArt(m_suppressBrowserAlbumArt);
        SaveSettings();
        RefreshMediaPresenceOutput();
    }

    void MainWindow::OnThemeSelectionChanged(IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (m_isInitializing) return;
        m_themeMode = ThemeModeFromComboIndex(ThemeModeCombo().SelectedIndex());
        ApplyThemeMode();
        SaveSettings();
        AppendDiagnosticLog(L"INFO", L"settings", L"Theme updated: " + ThemeModeLabel(m_themeMode));
    }

    void MainWindow::SyncActivityTypeOverridesFromControls()
    {
        m_mediaActivityTypeOverride = ActivityTypeOverrideFromComboIndex(MediaActivityTypeCombo().SelectedIndex());
        m_creativeActivityTypeOverride = ActivityTypeOverrideFromComboIndex(CreativeActivityTypeCombo().SelectedIndex());
        m_productiveActivityTypeOverride = ActivityTypeOverrideFromComboIndex(ProductiveActivityTypeCombo().SelectedIndex());
    }

    void MainWindow::ApplyActivityTypeOverrides()
    {
        if (m_presence)
            m_presence->SetActivityTypeOverride(m_mediaActivityTypeOverride);
    }

    void MainWindow::OnActivityTypeSelectionChanged(IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (m_isInitializing) return;

        SyncActivityTypeOverridesFromControls();
        ApplyActivityTypeOverrides();

        if (m_presence)
            RefreshMediaPresenceOutput();

        SyncProductiveRpcOutput();
        SyncCreativeRpcOutput();
        SaveSettings();

        AppendDiagnosticLog(L"INFO", L"settings",
            L"Activity types updated: media=" + ActivityTypeOverrideLabel(m_mediaActivityTypeOverride) +
            L", productive=" + ActivityTypeOverrideLabel(m_productiveActivityTypeOverride) +
            L", creativity=" + ActivityTypeOverrideLabel(m_creativeActivityTypeOverride));
    }

    void MainWindow::OnProductiveToggleChanged(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_isInitializing) return;

        SyncProductiveSettingsFromControls();
        ApplyProductiveRuntimeState();
        SaveSettings();
        UpdateProductivePreview(m_lastProductiveActivity);
        AppendDiagnosticLog(L"INFO", L"productive",
            m_productiveEnabled ? L"Productive RPC enabled" : L"Productive RPC disabled");
    }

    void MainWindow::OnProductiveMetadataToggled(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_isInitializing) return;

        SyncProductiveSettingsFromControls();
        SaveSettings();
        UpdateProductivePreview(m_lastProductiveActivity);
        SyncProductiveRpcOutput();
        AppendDiagnosticLog(L"INFO", L"productive",
            m_productiveShowProjectName
                ? L"Productivity project/file name display enabled"
                : L"Productivity project/file name display disabled");
    }

    void MainWindow::OnProductiveSelectionChanged(IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (m_isInitializing) return;

        SyncProductiveSettingsFromControls();
        ApplyProductiveRuntimeState();
        SaveSettings();
        UpdateProductivePreview(m_lastProductiveActivity);

        AppendDiagnosticLog(L"INFO", L"productive",
            L"Productivity detection mode updated: " + ProductiveDetectionModeLabel(m_productiveDetectionMode));
    }

    void MainWindow::OnProductiveAppFilterToggled(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_isInitializing) return;

        SyncProductiveSettingsFromControls();
        SaveSettings();
        UpdateProductivePreview(m_lastProductiveActivity);
        SyncProductiveRpcOutput();

        AppendDiagnosticLog(L"INFO", L"productive", L"Updated Productivity per-app filters");
    }

    void MainWindow::OnProductiveSelectAllAppsClicked(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_isInitializing) return;

        bool initBefore = m_isInitializing;
        m_isInitializing = true;
        SetAllProductiveAppFilterChecks(true);
        m_isInitializing = initBefore;

        SyncProductiveSettingsFromControls();
        SaveSettings();
        UpdateProductivePreview(m_lastProductiveActivity);
        SyncProductiveRpcOutput();
        AppendDiagnosticLog(L"INFO", L"productive", L"Enabled all Productivity app filters");
    }

    void MainWindow::OnProductiveDeselectAllAppsClicked(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_isInitializing) return;

        bool initBefore = m_isInitializing;
        m_isInitializing = true;
        SetAllProductiveAppFilterChecks(false);
        m_isInitializing = initBefore;

        SyncProductiveSettingsFromControls();
        SaveSettings();
        UpdateProductivePreview(m_lastProductiveActivity);
        SyncProductiveRpcOutput();
        AppendDiagnosticLog(L"INFO", L"productive", L"Disabled all Productivity app filters");
    }

    void MainWindow::OnApplyBlockedAppSitesClicked(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_isInitializing) return;

        m_blockedAppSiteTermsRaw = BlockedAppSitesBox().Text().c_str();
        auto terms = ParseBlockedTerms(m_blockedAppSiteTermsRaw);
        m_presence->SetBlockedAppSiteTerms(std::move(terms));
        SaveSettings();
        RefreshMediaPresenceOutput();
        AppendDiagnosticLog(L"INFO", L"settings", L"Updated blocked apps/sites list");
    }

    void MainWindow::ApplyProductiveRuntimeState()
    {
        if (ProductiveSettingsControlsPanel())
        {
            ProductiveSettingsControlsPanel().IsHitTestVisible(m_productiveEnabled);
            ProductiveSettingsControlsPanel().Opacity(m_productiveEnabled ? 1.0 : 0.55);
        }

        if (m_productiveDetector)
            m_productiveDetector->SetDetectionMode(m_productiveDetectionMode);

        if (m_isInitializing || !m_productiveDetector)
            return;

        if (m_productivePresence)
        {
            if (m_productiveEnabled && m_enabled && !m_productivePresenceRunning)
            {
                m_productivePresence->Initialize();
                m_productivePresenceRunning = true;
                AppendDiagnosticLog(L"INFO", L"productive", L"Productive Discord RPC started (separate app ID)");
            }
            else if (!m_productiveEnabled && m_productivePresenceRunning)
            {
                m_productivePresence->ClearProductiveActivity();
                m_productivePresence->Shutdown();
                m_productivePresenceRunning = false;
                lrp::ResetActivityLaneState(m_productiveLaneState);
                AppendDiagnosticLog(L"INFO", L"productive", L"Productive Discord RPC stopped");
            }
        }

        if (m_productiveEnabled && m_enabled)
        {
            m_productiveDetector->Start();
        }
        else
        {
            m_productiveDetector->Stop();
            m_lastProductiveActivity = {};
            lrp::ResetActivityLaneState(m_productiveLaneState);
        }

        SyncProductiveRpcOutput();
    }

    void MainWindow::SyncProductiveRpcOutput()
    {
        if (m_isShuttingDown || !m_productivePresence)
            return;

        ProductivePresenceOptions options{};
        options.privacyMode = ProductivePresencePrivacyMode::Normal;
        options.showProjectName = m_productiveShowProjectName;
        options.showWindowTitle = false;
        options.activityTypeOverride = m_productiveActivityTypeOverride;

        const bool appEnabled = !m_lastProductiveActivity.active || IsProductiveAppEnabled(m_lastProductiveActivity);
        auto transition = lrp::ResolveActivityLaneTransition(
            m_productiveLaneState,
            m_enabled,
            m_productiveEnabled,
            m_lastProductiveActivity.active && appEnabled,
            false,
            m_lastProductiveActivity.active && !appEnabled,
            false,
            false,
            BuildProductiveLaneSignature(m_lastProductiveActivity, options),
            lrp::ActivityLaneReason::AppClosed);

        if (transition.shouldEnsureRunning && !m_productivePresenceRunning)
        {
            m_productivePresence->Initialize();
            m_productivePresenceRunning = true;
            AppendDiagnosticLog(L"INFO", L"productive", L"Productive Discord RPC auto-restarted");
        }

        if (!m_productivePresenceRunning)
        {
            if (!transition.duplicate)
            {
                AppendDiagnosticLog(L"INFO", L"productive", BuildLaneTransitionLogMessage(L"Productivity", transition));
                lrp::CommitActivityLaneTransition(m_productiveLaneState, transition);
            }
            return;
        }

        if (transition.duplicate)
            return;

        if (lrp::IsPublishAction(transition.action))
        {
            m_productivePresence->UpdateProductiveActivity(m_lastProductiveActivity, options);
        }
        else
        {
            m_productivePresence->ClearProductiveActivity();
        }

        AppendDiagnosticLog(L"INFO", L"productive", BuildLaneTransitionLogMessage(L"Productivity", transition));
        lrp::CommitActivityLaneTransition(m_productiveLaneState, transition);
    }

    void MainWindow::SyncProductiveSettingsFromControls()
    {
        auto readCheck = [](Windows::Foundation::IReference<bool> const& value, bool fallback)
        {
            if (!value) return fallback;
            try { return value.Value(); }
            catch (...) { return fallback; }
        };

        m_productiveEnabled = ProductiveEnableToggle().IsOn();
        m_productiveDetectionMode = ProductiveDetectionModeFromComboIndex(ProductiveDetectionModeCombo().SelectedIndex());
        m_productiveShowProjectName = ProductiveShowProjectToggle().IsOn();
        m_productiveWordEnabled = readCheck(ProductiveAppWordCheck().IsChecked(), false);
        m_productiveExcelEnabled = readCheck(ProductiveAppExcelCheck().IsChecked(), false);
        m_productivePowerPointEnabled = readCheck(ProductiveAppPowerPointCheck().IsChecked(), false);
        m_productiveOneNoteEnabled = readCheck(ProductiveAppOneNoteCheck().IsChecked(), false);
        m_productiveAccessEnabled = readCheck(ProductiveAppAccessCheck().IsChecked(), false);
        m_productivePublisherEnabled = readCheck(ProductiveAppPublisherCheck().IsChecked(), false);
        m_productiveVisioEnabled = readCheck(ProductiveAppVisioCheck().IsChecked(), false);
        m_productiveProjectEnabled = readCheck(ProductiveAppProjectCheck().IsChecked(), false);
        m_productiveCodexEnabled = readCheck(ProductiveAppCodexCheck().IsChecked(), false);
    }

    void MainWindow::SetAllProductiveAppFilterChecks(bool enabled)
    {
        ProductiveAppWordCheck().IsChecked(enabled);
        ProductiveAppExcelCheck().IsChecked(enabled);
        ProductiveAppPowerPointCheck().IsChecked(enabled);
        ProductiveAppOneNoteCheck().IsChecked(enabled);
        ProductiveAppAccessCheck().IsChecked(enabled);
        ProductiveAppPublisherCheck().IsChecked(enabled);
        ProductiveAppVisioCheck().IsChecked(enabled);
        ProductiveAppProjectCheck().IsChecked(enabled);
        ProductiveAppCodexCheck().IsChecked(enabled);
    }

    bool MainWindow::IsProductiveAppEnabled(const ProductiveActivityInfo& info) const
    {
        if (!info.active || info.appKey.empty())
            return true;

        if (info.appKey == L"WORD") return m_productiveWordEnabled;
        if (info.appKey == L"XCEL") return m_productiveExcelEnabled;
        if (info.appKey == L"PPT") return m_productivePowerPointEnabled;
        if (info.appKey == L"ONEN") return m_productiveOneNoteEnabled;
        if (info.appKey == L"ACCS") return m_productiveAccessEnabled;
        if (info.appKey == L"PUBR") return m_productivePublisherEnabled;
        if (info.appKey == L"VISI") return m_productiveVisioEnabled;
        if (info.appKey == L"PROJ") return m_productiveProjectEnabled;
        if (info.appKey == L"CODX") return m_productiveCodexEnabled;
        return true;
    }

    void MainWindow::SyncCreativeSettingsFromControls()
    {
        auto readCheck = [](Windows::Foundation::IReference<bool> const& value, bool fallback)
        {
            if (!value) return fallback;
            try { return value.Value(); }
            catch (...) { return fallback; }
        };

        m_creativeEnabled = CreativeEnableToggle().IsOn();
        m_creativePriority = CreativePriorityModeFromComboIndex(CreativePriorityCombo().SelectedIndex());
        m_creativeDetectionMode = CreativeDetectionModeFromComboIndex(CreativeDetectionModeCombo().SelectedIndex());
        m_creativeShowProjectName = CreativeShowProjectToggle().IsOn();
        m_creativeShowWindowTitle = CreativeShowWindowTitleToggle().IsOn();
        m_creativePhotoshopEnabled = readCheck(CreativeAppPhotoshopCheck().IsChecked(), false);
        m_creativeIllustratorEnabled = readCheck(CreativeAppIllustratorCheck().IsChecked(), false);
        m_creativePremiereEnabled = readCheck(CreativeAppPremiereCheck().IsChecked(), false);
        m_creativeAfterEffectsEnabled = readCheck(CreativeAppAfterEffectsCheck().IsChecked(), false);
        m_creativeInDesignEnabled = readCheck(CreativeAppInDesignCheck().IsChecked(), false);
        m_creativeAuditionEnabled = readCheck(CreativeAppAuditionCheck().IsChecked(), false);
        m_creativeMediaEncoderEnabled = readCheck(CreativeAppMediaEncoderCheck().IsChecked(), false);
        m_creativeLightroomEnabled = readCheck(CreativeAppLightroomCheck().IsChecked(), false);
        m_creativeLightroomClassicEnabled = readCheck(CreativeAppLightroomClassicCheck().IsChecked(), false);
        m_creativeInCopyEnabled = readCheck(CreativeAppInCopyCheck().IsChecked(), false);
        m_creativeDreamweaverEnabled = readCheck(CreativeAppDreamweaverCheck().IsChecked(), false);
        m_creativeAnimateEnabled = readCheck(CreativeAppAnimateCheck().IsChecked(), false);
        m_creativeXdEnabled = readCheck(CreativeAppXdCheck().IsChecked(), false);
        m_creativeBridgeEnabled = readCheck(CreativeAppBridgeCheck().IsChecked(), false);
        m_creativeCharacterAnimatorEnabled = readCheck(CreativeAppCharacterAnimatorCheck().IsChecked(), false);
        m_creativeFrescoEnabled = readCheck(CreativeAppFrescoCheck().IsChecked(), false);
        m_creativeDimensionEnabled = readCheck(CreativeAppDimensionCheck().IsChecked(), false);
        m_creativeSubstanceEnabled = readCheck(CreativeAppSubstanceCheck().IsChecked(), false);
        m_creativeAcrobatEnabled = readCheck(CreativeAppAcrobatCheck().IsChecked(), false);
        m_creativeOtherAdobeEnabled = readCheck(CreativeAppOtherAdobeCheck().IsChecked(), false);
        m_creativePrivacyMode = CreativePrivacyModeFromComboIndex(CreativePrivacyCombo().SelectedIndex());
        m_creativeIdleBehavior = CreativeIdleBehaviorFromComboIndex(CreativeIdleBehaviorCombo().SelectedIndex());

        if (m_lastCreativeAcceptedActivity.active && !IsCreativeAppEnabled(m_lastCreativeAcceptedActivity))
        {
            m_lastCreativeAcceptedActivity = {};
            m_lastCreativeActiveSeenAt = {};
        }
    }

    void MainWindow::SetAllCreativeAppFilterChecks(bool enabled)
    {
        CreativeAppPhotoshopCheck().IsChecked(enabled);
        CreativeAppIllustratorCheck().IsChecked(enabled);
        CreativeAppPremiereCheck().IsChecked(enabled);
        CreativeAppAfterEffectsCheck().IsChecked(enabled);
        CreativeAppInDesignCheck().IsChecked(enabled);
        CreativeAppAuditionCheck().IsChecked(enabled);
        CreativeAppMediaEncoderCheck().IsChecked(enabled);
        CreativeAppLightroomCheck().IsChecked(enabled);
        CreativeAppLightroomClassicCheck().IsChecked(enabled);
        CreativeAppInCopyCheck().IsChecked(enabled);
        CreativeAppDreamweaverCheck().IsChecked(enabled);
        CreativeAppAnimateCheck().IsChecked(enabled);
        CreativeAppXdCheck().IsChecked(enabled);
        CreativeAppBridgeCheck().IsChecked(enabled);
        CreativeAppCharacterAnimatorCheck().IsChecked(enabled);
        CreativeAppFrescoCheck().IsChecked(enabled);
        CreativeAppDimensionCheck().IsChecked(enabled);
        CreativeAppSubstanceCheck().IsChecked(enabled);
        CreativeAppAcrobatCheck().IsChecked(enabled);
        CreativeAppOtherAdobeCheck().IsChecked(enabled);
    }

    void MainWindow::ApplyCreativeDetectorRuntimeState()
    {
        if (CreativeSettingsControlsPanel())
        {
            CreativeSettingsControlsPanel().IsHitTestVisible(m_creativeEnabled);
            CreativeSettingsControlsPanel().Opacity(m_creativeEnabled ? 1.0 : 0.55);
        }

        if (m_creativeDetector)
            m_creativeDetector->SetDetectionMode(m_creativeDetectionMode);

        if (m_isInitializing || !m_creativeDetector)
            return;

        if (m_creativePresence)
        {
            if (m_creativeEnabled && m_enabled && !m_creativePresenceRunning)
            {
                m_creativePresence->Initialize();
                m_creativePresenceRunning = true;
                AppendDiagnosticLog(L"INFO", L"creative", L"Creativity Discord RPC started (separate app ID)");
            }
            else if (!m_creativeEnabled && m_creativePresenceRunning)
            {
                m_creativePresence->ClearCreativeActivity();
                m_creativePresence->Shutdown();
                m_creativePresenceRunning = false;
                lrp::ResetActivityLaneState(m_creativeLaneState);
                AppendDiagnosticLog(L"INFO", L"creative", L"Creativity Discord RPC stopped");
            }
        }

        if (m_creativeEnabled && m_enabled)
        {
            m_creativeDetector->Start();
        }
        else
        {
            m_creativeDetector->Stop();
            m_lastCreativeActivity = {};
            m_lastCreativeAcceptedActivity = {};
            m_lastCreativeActiveSeenAt = {};
            lrp::ResetActivityLaneState(m_creativeLaneState);
        }

        SyncCreativeRpcOutput();
    }

    void MainWindow::RefreshCreativePreviewFromCurrentState()
    {
        UpdateCreativePreview(m_lastCreativeActivity);
        UpdateHomeCreativePreview();
        SyncCreativeRpcOutput();
    }

    bool MainWindow::IsCreativeAppEnabled(const CreativeActivityInfo& info) const
    {
        if (!info.active || info.appKey.empty())
            return true;

        if (info.appKey == L"PHXS") return m_creativePhotoshopEnabled;
        if (info.appKey == L"ILST") return m_creativeIllustratorEnabled;
        if (info.appKey == L"PPRO") return m_creativePremiereEnabled;
        if (info.appKey == L"AEFT") return m_creativeAfterEffectsEnabled;
        if (info.appKey == L"IDSN") return m_creativeInDesignEnabled;
        if (info.appKey == L"AUDT") return m_creativeAuditionEnabled;
        if (info.appKey == L"AME") return m_creativeMediaEncoderEnabled;
        if (info.appKey == L"LTRM") return m_creativeLightroomEnabled;
        if (info.appKey == L"LTRC") return m_creativeLightroomClassicEnabled;
        if (info.appKey == L"AICY") return m_creativeInCopyEnabled;
        if (info.appKey == L"DRWV") return m_creativeDreamweaverEnabled;
        if (info.appKey == L"FLPR") return m_creativeAnimateEnabled;
        if (info.appKey == L"XD") return m_creativeXdEnabled;
        if (info.appKey == L"BRDG") return m_creativeBridgeEnabled;
        if (info.appKey == L"CHAN") return m_creativeCharacterAnimatorEnabled;
        if (info.appKey == L"FRSC") return m_creativeFrescoEnabled;
        if (info.appKey == L"DIMN") return m_creativeDimensionEnabled;
        if (info.appKey == L"SBPT" || info.appKey == L"SBDG" || info.appKey == L"SBSM" || info.appKey == L"SBST" || info.appKey == L"SBMD")
            return m_creativeSubstanceEnabled;
        if (info.appKey == L"ACRO") return m_creativeAcrobatEnabled;
        return m_creativeOtherAdobeEnabled;
    }

    bool MainWindow::TryGetEffectiveCreativeActivityForRpc(CreativeActivityInfo& infoOut, bool& heldOut) const
    {
        infoOut = {};
        heldOut = false;

        if (!m_creativeEnabled)
            return false;

        if (m_lastCreativeActivity.active && IsCreativeAppEnabled(m_lastCreativeActivity))
        {
            infoOut = m_lastCreativeActivity;
            return true;
        }

        if (m_creativeIdleBehavior != CreativeIdleBehavior::HoldLast5Seconds)
            return false;

        if (!m_lastCreativeAcceptedActivity.active || !IsCreativeAppEnabled(m_lastCreativeAcceptedActivity))
            return false;

        if (m_lastCreativeActiveSeenAt.time_since_epoch().count() <= 0)
            return false;

        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - m_lastCreativeActiveSeenAt);
        if (elapsed > std::chrono::seconds(kCreativeIdleHoldSeconds))
            return false;

        infoOut = m_lastCreativeAcceptedActivity;
        heldOut = true;
        return true;
    }

    void MainWindow::SyncCreativeRpcOutput()
    {
        if (m_isShuttingDown || !m_creativePresence)
            return;

        CreativeActivityInfo effective{};
        bool heldActivity = false;
        bool hasEffectiveActivity = TryGetEffectiveCreativeActivityForRpc(effective, heldActivity);
        bool filteredBySettings = m_lastCreativeActivity.active && !IsCreativeAppEnabled(m_lastCreativeActivity) && !heldActivity;
        bool mediaActive = !m_lastMedia.title.empty() && m_lastMedia.isPlaying;
        const bool separateCreativeApp = UsesSeparateCreativeDiscordApp();
        const bool suppressedByPriority =
            hasEffectiveActivity &&
            !separateCreativeApp &&
            ((m_creativePriority == CreativePriorityMode::PreferMedia && mediaActive) ||
             (m_creativePriority == CreativePriorityMode::Auto && mediaActive && heldActivity));
        const bool hiddenByPrivateMode =
            m_creativePrivacyMode == CreativePrivacyMode::Private &&
            (m_lastCreativeActivity.active || m_lastCreativeAcceptedActivity.active);

        CreativePresenceOptions options{};
        options.showProjectName = m_creativeShowProjectName;
        options.showWindowTitle = m_creativeShowWindowTitle;
        options.heldActivity = heldActivity;
        options.activityTypeOverride = m_creativeActivityTypeOverride;
        switch (m_creativePrivacyMode)
        {
        case CreativePrivacyMode::AppOnly:
            options.privacyMode = CreativePresencePrivacyMode::AppOnly;
            break;
        case CreativePrivacyMode::Private:
            options.privacyMode = CreativePresencePrivacyMode::Private;
            break;
        default:
            options.privacyMode = CreativePresencePrivacyMode::Normal;
            break;
        }

        auto transition = lrp::ResolveActivityLaneTransition(
            m_creativeLaneState,
            m_enabled,
            m_creativeEnabled,
            hasEffectiveActivity && !suppressedByPriority && !hiddenByPrivateMode && !filteredBySettings,
            heldActivity,
            filteredBySettings,
            hiddenByPrivateMode,
            suppressedByPriority,
            BuildCreativeLaneSignature(effective, options),
            lrp::ActivityLaneReason::DetectorIdle);

        if (transition.shouldEnsureRunning && !m_creativePresenceRunning)
        {
            m_creativePresence->Initialize();
            m_creativePresenceRunning = true;
            AppendDiagnosticLog(L"INFO", L"creative", L"Creativity Discord RPC auto-restarted");
        }

        if (!m_creativePresenceRunning)
        {
            if (!transition.duplicate)
            {
                AppendDiagnosticLog(L"INFO", L"creative", BuildLaneTransitionLogMessage(L"Creativity", transition));
                lrp::CommitActivityLaneTransition(m_creativeLaneState, transition);
            }
            return;
        }

        if (transition.duplicate)
            return;

        if (lrp::IsPublishAction(transition.action))
        {
            m_creativePresence->UpdateCreativeActivity(effective, options);
        }
        else
        {
            m_creativePresence->ClearCreativeActivity();
        }

        AppendDiagnosticLog(L"INFO", L"creative", BuildLaneTransitionLogMessage(L"Creativity", transition));
        lrp::CommitActivityLaneTransition(m_creativeLaneState, transition);
    }

    void MainWindow::OnCreativeToggleChanged(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_isInitializing) return;

        SyncCreativeSettingsFromControls();
        ApplyCreativeDetectorRuntimeState();
        SaveSettings();
        RefreshCreativePreviewFromCurrentState();

        AppendDiagnosticLog(L"INFO", L"creative",
            m_creativeEnabled ? L"Creativity settings updated (enabled)" : L"Creativity settings updated (disabled)");
    }

    void MainWindow::OnCreativeSelectionChanged(IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (m_isInitializing) return;

        SyncCreativeSettingsFromControls();
        ApplyCreativeDetectorRuntimeState();
        SaveSettings();
        RefreshCreativePreviewFromCurrentState();

        AppendDiagnosticLog(L"INFO", L"creative",
            L"Creativity mode updated: priority=" + CreativePriorityModeLabel(m_creativePriority) +
            L", detection=" + CreativeDetectionModeLabel(m_creativeDetectionMode) +
            L", privacy=" + CreativePrivacyModeLabel(m_creativePrivacyMode) +
            L", idle=" + CreativeIdleBehaviorLabel(m_creativeIdleBehavior));
    }

    void MainWindow::OnCreativeAppFilterToggled(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_isInitializing) return;

        SyncCreativeSettingsFromControls();
        SaveSettings();
        RefreshCreativePreviewFromCurrentState();

        AppendDiagnosticLog(L"INFO", L"creative", L"Updated Creativity per-app filters");
    }

    void MainWindow::OnCreativeSelectAllAppsClicked(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_isInitializing) return;

        bool initBefore = m_isInitializing;
        m_isInitializing = true;
        SetAllCreativeAppFilterChecks(true);
        m_isInitializing = initBefore;

        SyncCreativeSettingsFromControls();
        SaveSettings();
        RefreshCreativePreviewFromCurrentState();
        AppendDiagnosticLog(L"INFO", L"creative", L"Enabled all Creativity app filters");
    }

    void MainWindow::OnCreativeDeselectAllAppsClicked(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_isInitializing) return;

        bool initBefore = m_isInitializing;
        m_isInitializing = true;
        SetAllCreativeAppFilterChecks(false);
        m_isInitializing = initBefore;

        SyncCreativeSettingsFromControls();
        SaveSettings();
        RefreshCreativePreviewFromCurrentState();
        AppendDiagnosticLog(L"INFO", L"creative", L"Disabled all Creativity app filters");
    }

    void MainWindow::OnResetSettingsClicked(IInspectable const&, RoutedEventArgs const&)
    {
        const lrp::settings::PersistedSettings defaults;
        bool initBefore = m_isInitializing;
        m_isInitializing = true;

        TimestampToggle().IsOn(defaults.media.showTimestamps);
        SourceToggle().IsOn(defaults.media.showSource);
        SourceDebugToggle().IsOn(defaults.media.sourceDebugMode);
        PausedToggle().IsOn(defaults.media.showPaused);
        AlbumArtToggle().IsOn(defaults.media.showAlbumArt);
        DefaultIdleStatusToggle().IsOn(defaults.media.showDefaultIdleStatus);

        EnableToggle().IsOn(defaults.behavior.richPresenceEnabled);
        CloseToTrayToggle().IsOn(defaults.behavior.closeToTrayOnClose);
        LaunchOnStartupToggle().IsOn(defaults.behavior.launchOnStartup);
        StartMinimizedToggle().IsOn(defaults.behavior.startMinimizedToTray);
        TrayLeftClickToggle().IsOn(defaults.behavior.trayLeftClickToggles);

        SensitiveFilterToggle().IsOn(defaults.media.sensitiveKeywordFilter);
        StrictBrowserPrivacyToggle().IsOn(defaults.media.strictBrowserPrivacy);
        SuppressBrowserArtToggle().IsOn(defaults.media.suppressBrowserAlbumArt);
        ThemeModeCombo().SelectedIndex(ThemeModeToComboIndex(defaults.behavior.themeMode));
        BlockedAppSitesBox().Text(defaults.media.blockedAppSiteTermsRaw);
        MediaActivityTypeCombo().SelectedIndex(ActivityTypeOverrideToComboIndex(defaults.media.activityTypeOverride));
        CreativeActivityTypeCombo().SelectedIndex(ActivityTypeOverrideToComboIndex(defaults.creative.activityTypeOverride));
        ProductiveActivityTypeCombo().SelectedIndex(ActivityTypeOverrideToComboIndex(defaults.productive.activityTypeOverride));

        ProductiveEnableToggle().IsOn(defaults.productive.enabled);
        ProductiveDetectionModeCombo().SelectedIndex(ProductiveDetectionModeToComboIndex(defaults.productive.detectionMode));
        ProductiveShowProjectToggle().IsOn(defaults.productive.showProjectName);
        SetAllProductiveAppFilterChecks(true);
        CreativeEnableToggle().IsOn(defaults.creative.enabled);
        CreativePriorityCombo().SelectedIndex(CreativePriorityModeToComboIndex(defaults.creative.priority));
        CreativeDetectionModeCombo().SelectedIndex(CreativeDetectionModeToComboIndex(defaults.creative.detectionMode));
        CreativeShowProjectToggle().IsOn(defaults.creative.showProjectName);
        CreativeShowWindowTitleToggle().IsOn(defaults.creative.showWindowTitle);
        SetAllCreativeAppFilterChecks(true);
        CreativePrivacyCombo().SelectedIndex(CreativePrivacyModeToComboIndex(defaults.creative.privacyMode));
        CreativeIdleBehaviorCombo().SelectedIndex(CreativeIdleBehaviorToComboIndex(defaults.creative.idleBehavior));

        m_sourceDebugMode = defaults.media.sourceDebugMode;
        m_enabled = defaults.behavior.richPresenceEnabled;
        m_closeToTrayOnClose = defaults.behavior.closeToTrayOnClose;
        m_launchOnStartup = defaults.behavior.launchOnStartup;
        m_startMinimizedToTray = defaults.behavior.startMinimizedToTray;
        m_trayLeftClickToggles = defaults.behavior.trayLeftClickToggles;
        m_sensitiveKeywordFilter = defaults.media.sensitiveKeywordFilter;
        m_strictBrowserPrivacy = defaults.media.strictBrowserPrivacy;
        m_suppressBrowserAlbumArt = defaults.media.suppressBrowserAlbumArt;
        m_showDefaultIdleStatus = defaults.media.showDefaultIdleStatus;
        m_themeMode = defaults.behavior.themeMode;
        m_blockedAppSiteTermsRaw = defaults.media.blockedAppSiteTermsRaw;
        m_mediaActivityTypeOverride = defaults.media.activityTypeOverride;
        m_creativeActivityTypeOverride = defaults.creative.activityTypeOverride;
        m_productiveActivityTypeOverride = defaults.productive.activityTypeOverride;
        m_productiveEnabled = defaults.productive.enabled;
        SyncProductiveSettingsFromControls();
        SyncCreativeSettingsFromControls();
        ApplyActivityTypeOverrides();

        m_presence->SetShowTimestamps(defaults.media.showTimestamps);
        m_presence->SetShowSource(defaults.media.showSource);
        m_presence->SetShowPaused(defaults.media.showPaused);
        m_presence->SetShowAlbumArt(defaults.media.showAlbumArt);
        m_presence->SetShowIdleStatus(defaults.media.showDefaultIdleStatus);
        m_presence->SetSensitiveKeywordFilter(defaults.media.sensitiveKeywordFilter);
        m_presence->SetStrictBrowserPrivacy(defaults.media.strictBrowserPrivacy);
        m_presence->SetSuppressBrowserAlbumArt(defaults.media.suppressBrowserAlbumArt);
        m_presence->SetBlockedAppSiteTerms({});

        m_isInitializing = initBefore;
        ApplyThemeMode();
        ApplyLaunchOnStartupState(defaults.behavior.launchOnStartup, false);
        ApplyProductiveRuntimeState();
        ApplyCreativeDetectorRuntimeState();
        SaveSettings();
        UpdateSourceBadge(m_lastMedia);
        ApplyGlobalEnableRuntimeState();
        AppendDiagnosticLog(L"INFO", L"settings", L"Reset settings to defaults");
    }

    void MainWindow::OnExportSettingsJsonClicked(IInspectable const&, RoutedEventArgs const&)
    {
        try
        {
            auto payload = BuildSettingsSnapshotJson();

            auto basePath = std::filesystem::path(std::wstring(Windows::Storage::ApplicationData::Current().LocalFolder().Path()));
            auto outputPath = basePath / kSettingsExportFileName;

            auto payloadUtf8 = winrt::to_string(winrt::hstring(payload));
            std::ofstream out(outputPath, std::ios::out | std::ios::trunc | std::ios::binary);
            out.write(payloadUtf8.data(), static_cast<std::streamsize>(payloadUtf8.size()));
            out.flush();

            if (!out.good())
                throw std::runtime_error("write-failed");

            AppendDiagnosticLog(L"INFO", L"settings", L"Exported settings to " + outputPath.wstring());
        }
        catch (...)
        {
            AppendDiagnosticLog(L"ERROR", L"settings", L"Failed to export settings");
        }
    }

    void MainWindow::OnImportSettingsJsonClicked(IInspectable const&, RoutedEventArgs const&)
    {
        try
        {
            auto basePath = std::filesystem::path(std::wstring(Windows::Storage::ApplicationData::Current().LocalFolder().Path()));
            auto inputPath = basePath / kSettingsExportFileName;

            if (!std::filesystem::exists(inputPath))
            {
                AppendDiagnosticLog(L"WARN", L"settings", L"No settings-export.json found in local app folder");
                return;
            }

            std::ifstream in(inputPath, std::ios::in | std::ios::binary);
            std::string payloadUtf8((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

            JsonObject root;
            if (!JsonObject::TryParse(winrt::to_hstring(payloadUtf8), root))
            {
                AppendDiagnosticLog(L"WARN", L"settings", L"settings-export.json is not valid JSON");
                return;
            }
            auto imported = lrp::settings::ParseImportedSettings(
                BuildImportedSettingsMap(root),
                BuildPersistedSettingsSnapshot());

            {
                BoolValueGuard initializingGuard(m_isInitializing, true);
                ApplyPersistedSettingsSnapshot(imported.settings);
                SyncProductiveSettingsFromControls();
                SyncCreativeSettingsFromControls();
            }

            ApplyLaunchOnStartupState(m_launchOnStartup, false);
            ApplyProductiveRuntimeState();
            ApplyCreativeDetectorRuntimeState();
            UpdateProductivePreview(m_lastProductiveActivity);
            RefreshCreativePreviewFromCurrentState();
            UpdateSourceBadge(m_lastMedia);
            ApplyGlobalEnableRuntimeState();
            auto saveResult = SaveSettings();
            AppendSettingsIssues(L"Import settings", imported.issues);
            const bool importHasWarnings = !imported.issues.empty() || !saveResult.issues.empty();
            AppendDiagnosticLog(
                importHasWarnings ? L"WARN" : L"INFO",
                L"settings",
                importHasWarnings
                    ? L"Imported settings from " + inputPath.wstring() + L" with warnings"
                    : L"Imported settings from " + inputPath.wstring());
        }
        catch (...)
        {
            AppendDiagnosticLog(L"ERROR", L"settings", L"Failed to import settings");
        }
    }

    void MainWindow::OnClearDiagnosticsClicked(IInspectable const&, RoutedEventArgs const&)
    {
        m_diagnosticLog.Clear();
        RefreshDiagnosticsPanel();
        AppendDiagnosticLog(L"INFO", L"diagnostics", L"Log cleared");
    }

    void MainWindow::OnExportDiagnosticsJsonClicked(IInspectable const&, RoutedEventArgs const&)
    {
        try
        {
            auto payload = BuildDiagnosticsSnapshotJson();

            auto basePath = std::filesystem::path(std::wstring(Windows::Storage::ApplicationData::Current().LocalFolder().Path()));
            auto fileName = std::wstring(L"diagnostics-") + BuildUtcTimestampString() + L".json";
            auto outputPath = basePath / fileName;

            auto payloadUtf8 = winrt::to_string(winrt::hstring(payload));
            std::ofstream out(outputPath, std::ios::out | std::ios::trunc | std::ios::binary);
            out.write(payloadUtf8.data(), static_cast<std::streamsize>(payloadUtf8.size()));
            out.flush();

            if (!out.good())
                throw std::runtime_error("write-failed");

            AppendDiagnosticLog(L"INFO", L"diagnostics", L"Exported JSON to " + outputPath.wstring());
        }
        catch (...)
        {
            AppendDiagnosticLog(L"ERROR", L"diagnostics", L"Failed to export JSON snapshot");
        }
    }

    // =====================================================================
    // Media change handling
    // =====================================================================

    void MainWindow::ShutdownWindow()
    {
        if (m_isShuttingDown) return;
        m_isShuttingDown = true;
        m_exitRequested = true;
        AppendDiagnosticLog(L"INFO", L"app", L"Shutting down");

        if (m_lifetimeToken)
            m_lifetimeToken->store(false);

        // Cancel any pending thumbnail update
        if (m_thumbnailUpdateTask)
        {
            m_thumbnailUpdateTask.Cancel();
            m_thumbnailUpdateTask = nullptr;
        }

        if (m_productiveIconUpdateTask)
        {
            m_productiveIconUpdateTask.Cancel();
            m_productiveIconUpdateTask = nullptr;
        }

        if (m_creativeIconUpdateTask)
        {
            m_creativeIconUpdateTask.Cancel();
            m_creativeIconUpdateTask = nullptr;
        }

        if (m_connectionInfoBarTimer)
        {
            m_connectionInfoBarTimer.Stop();
            m_connectionInfoBarTimer = nullptr;
        }

        if (m_trackSkeletonTimer)
        {
            m_trackSkeletonTimer.Stop();
            m_trackSkeletonTimer = nullptr;
        }

        if (m_connectionInfoBarFadeStoryboard)
        {
            try { m_connectionInfoBarFadeStoryboard.Stop(); } catch (...) {}
            m_connectionInfoBarFadeStoryboard = nullptr;
        }

        if (m_pageTransitionStoryboard)
        {
            try { m_pageTransitionStoryboard.Stop(); } catch (...) {}
            m_pageTransitionStoryboard = nullptr;
        }
        m_pageTransitionInProgress = false;
        m_queuedPageTag.clear();

        if (m_songProgressStoryboard)
        {
            try { m_songProgressStoryboard.Stop(); } catch (...) {}
            m_songProgressStoryboard = nullptr;
        }

        if (m_homeMiniProgressStoryboard)
        {
            try { m_homeMiniProgressStoryboard.Stop(); } catch (...) {}
            m_homeMiniProgressStoryboard = nullptr;
        }

        if (m_livePulseStoryboard)
        {
            try { m_livePulseStoryboard.Stop(); } catch (...) {}
            m_livePulseStoryboard = nullptr;
        }

        if (m_songWaveStoryboard)
        {
            try { m_songWaveStoryboard.Stop(); } catch (...) {}
            m_songWaveStoryboard = nullptr;
        }

        if (m_homeMiniWaveStoryboard)
        {
            try { m_homeMiniWaveStoryboard.Stop(); } catch (...) {}
            m_homeMiniWaveStoryboard = nullptr;
        }

        if (m_trackSkeletonStoryboard)
        {
            try { m_trackSkeletonStoryboard.Stop(); } catch (...) {}
            m_trackSkeletonStoryboard = nullptr;
        }

        StopProgressTimer();

        if (m_mediaDetector)
        {
            m_mediaDetector->SetCallback({});
            m_mediaDetector->Stop();
        }

        if (m_productiveDetector)
        {
            m_productiveDetector->SetCallback({});
            m_productiveDetector->Stop();
        }

        if (m_creativeDetector)
        {
            m_creativeDetector->SetCallback({});
            m_creativeDetector->Stop();
        }

        if (m_productivePresence)
        {
            if (m_productivePresenceRunning)
            {
                m_productivePresence->ClearProductiveActivity();
                m_productivePresence->Shutdown();
                m_productivePresenceRunning = false;
            }
        }

        if (m_creativePresence)
        {
            if (m_creativePresenceRunning)
            {
                m_creativePresence->ClearCreativeActivity();
                m_creativePresence->Shutdown();
                m_creativePresenceRunning = false;
            }
        }

        if (m_presence)
            m_presence->Shutdown();

        CleanupSystemTray();

        SetBrowserHintUpdateCallback({});
        StopBrowserHintServer();
        AppendDiagnosticLog(L"INFO", L"bridge", L"Browser hint bridge stopped");
    }

    void MainWindow::OnMediaChanged(const MediaInfo& info)
    {
        if (!m_enabled || m_isShuttingDown) return;

        auto merge = ResolveMergePolicy(info);
        if (IsBlockedMessengerMedia(info) || IsBlockedMessengerMedia(merge.media) || IsBlockedMessengerHint())
        {
            merge.media = {};
            merge.mode = MergeMode::NoMedia;
        }

        if (merge.mode == MergeMode::ExtensionApplied &&
            IsBrowserMedia(merge.media) &&
            IsBrowserMedia(info) &&
            !merge.media.title.empty() &&
            !info.title.empty() &&
            TitlesLikelyMatch(merge.media.title, info.title))
        {
            bool artistCompatible =
                merge.media.artist.empty() ||
                info.artist.empty() ||
                TitlesLikelyMatch(merge.media.artist, info.artist);

            if (artistCompatible)
            {
                auto detectorPos = info.position.count();
                auto detectorDur = info.duration.count();
                auto hintPos = merge.media.position.count();
                auto hintDur = merge.media.duration.count();

                bool detectorTimelineValid =
                    detectorDur > 0 &&
                    detectorPos >= 0 &&
                    detectorPos <= (detectorDur + 5);

                bool hintTimelineValid =
                    hintDur > 0 &&
                    hintPos >= 0 &&
                    hintPos <= (hintDur + 5);

                bool hugeDurationDrift =
                    detectorTimelineValid &&
                    hintTimelineValid &&
                    std::llabs(hintDur - detectorDur) > 90;

                bool hugePositionDrift =
                    detectorTimelineValid &&
                    hintTimelineValid &&
                    std::llabs(hintPos - detectorPos) > 90;

                bool strongFreshHintTimeline = false;
                {
                    auto& state = GetHintServerState();
                    std::lock_guard<std::mutex> lock(state.mutex);

                    if (IsExtensionConnected(state) && state.hasHint)
                    {
                        auto hintAge = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - state.hint.updatedAt);

                        auto hintRuleLower = ToLowerCopy(state.hint.rule);
                        bool youtubeMusicHint =
                            ToLowerCopy(state.hint.siteKey) == L"youtube_music" ||
                            hintRuleLower.find(L"youtube-music") != std::wstring::npos;
                        bool hintTimelineFromClock = hintRuleLower.find(L"clock") != std::wstring::npos;
                        bool highTrustTimeline =
                            state.hint.confidence >= 98 &&
                            (!youtubeMusicHint || hintTimelineFromClock);

                        bool titleAligned =
                            !state.hint.title.empty() &&
                            TitlesLikelyMatch(state.hint.title, merge.media.title);
                        bool artistAligned =
                            state.hint.artist.empty() ||
                            merge.media.artist.empty() ||
                            TitlesLikelyMatch(state.hint.artist, merge.media.artist);

                        strongFreshHintTimeline =
                            hintAge <= std::chrono::seconds(6) &&
                            highTrustTimeline &&
                            titleAligned &&
                            artistAligned;
                    }
                }

                if (hugeDurationDrift && hugePositionDrift && !strongFreshHintTimeline)
                {
                    merge.media.position = info.position;
                    merge.media.duration = info.duration;
                    merge.media.startTime = info.startTime;

                    if (!merge.media.detectionReason.empty())
                        merge.media.detectionReason += L"|";
                    merge.media.detectionReason += L"timeline-fallback:detector";
                }
            }
        }

        if (merge.mode == MergeMode::ExtensionPending &&
            IsBrowserMedia(merge.media) &&
            !m_lastMedia.title.empty() &&
            TitlesLikelyMatch(merge.media.title, m_lastMedia.title))
        {
            bool artistCompatible =
                merge.media.artist.empty() ||
                m_lastMedia.artist.empty() ||
                TitlesLikelyMatch(merge.media.artist, m_lastMedia.artist);

            if (artistCompatible)
            {
                auto incomingDur = merge.media.duration.count();
                auto previousDur = m_lastMedia.duration.count();
                auto incomingPos = merge.media.position.count();
                auto previousPos = m_lastMedia.position.count();
                auto previousEstimatedPos = previousPos;

                if (m_lastMedia.isPlaying && m_lastMedia.startTime.time_since_epoch().count() > 0)
                {
                    auto now = std::chrono::system_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_lastMedia.startTime).count();
                    if (elapsed > 0)
                        previousEstimatedPos = elapsed;
                }

                bool positionContinuous = std::llabs(incomingPos - previousEstimatedPos) <= 20;

                bool incomingDurationMissing = incomingDur <= 0;
                bool incomingPositionInvalid = incomingDur > 0 && incomingPos > (incomingDur + 5);
                bool likelyDurationNoise =
                    previousDur > 0 &&
                    incomingDur > (previousDur + 45) &&
                    std::llabs(incomingPos - previousPos) <= 5;

                bool durationLooksBad =
                    previousDur > 0 &&
                    positionContinuous &&
                    (incomingDurationMissing || incomingPositionInvalid || likelyDurationNoise);

                if (durationLooksBad)
                {
                    merge.media.duration = m_lastMedia.duration;

                    auto now = std::chrono::system_clock::now();
                    if (merge.media.isPlaying && m_lastMedia.startTime.time_since_epoch().count() > 0)
                    {
                        auto estimated = std::chrono::duration_cast<std::chrono::seconds>(now - m_lastMedia.startTime);
                        if (estimated.count() < 0)
                            estimated = std::chrono::seconds(0);
                        if (merge.media.duration.count() > 0 && estimated > merge.media.duration)
                            estimated = merge.media.duration;

                        merge.media.position = estimated;
                        merge.media.startTime = now - estimated;
                    }
                    else
                    {
                        merge.media.position = m_lastMedia.position;
                        merge.media.startTime = m_lastMedia.startTime;
                    }
                }
            }
        }

        std::wstring modeText = MergeModeToText(merge.mode);
        if (m_lastMergeState != modeText)
        {
            m_lastMergeState = modeText;

            std::wstring message = L"mode=" + modeText;
            if (!merge.media.sourceDisplayName.empty())
                message += L", source=" + merge.media.sourceDisplayName;
            if (!merge.media.detectionReason.empty())
                message += L", rule=" + merge.media.detectionReason;
            if (merge.mode == MergeMode::NoMedia)
                message += L", bridge=" + BuildBridgeStateSummary();
            AppendDiagnosticLog(L"INFO", L"merge", message);
        }

        UpdateUI(merge.media);
        m_presence->UpdateMedia(merge.media);
        m_lastPresencePushAt = std::chrono::steady_clock::now();
        m_lastPresencePushPlaying = !merge.media.title.empty() && merge.media.isPlaying;
    }

    void MainWindow::OnCreativeActivityChanged(const CreativeActivityInfo& info)
    {
        if (m_isShuttingDown)
            return;

        m_lastCreativeActivity = info;

        if (m_creativeEnabled && info.active && IsCreativeAppEnabled(info))
        {
            m_lastCreativeAcceptedActivity = info;
            m_lastCreativeActiveSeenAt = std::chrono::steady_clock::now();
        }

        UpdateCreativePreview(info);
        UpdateHomeCreativePreview();
        SyncCreativeRpcOutput();
    }

    void MainWindow::OnProductiveActivityChanged(const ProductiveActivityInfo& info)
    {
        if (m_isShuttingDown)
            return;

        m_lastProductiveActivity = info;
        UpdateProductivePreview(info);

        SyncProductiveRpcOutput();
    }

    void MainWindow::RefreshMediaPresenceOutput()
    {
        if (!m_presence)
            return;

        if (m_enabled)
            m_presence->RefreshPresence();
        else
            m_presence->ClearPresence();
    }

    void MainWindow::UpdateProductivePreview(const ProductiveActivityInfo& info)
    {
        if (!ProductiveDetectedAppText() ||
            !ProductiveDetectedProjectText() ||
            !ProductiveDetectedWindowText() ||
            !ProductiveDetectedProcessText() ||
            !ProductiveRuntimeSummaryText())
        {
            UpdateProductiveAppIcon(info);
            UpdateHomeProductivePreview();
            return;
        }

        auto buildAppLabel = [](const ProductiveActivityInfo& activity)
        {
            std::wstring label = activity.appName.empty() ? std::wstring(L"Microsoft Office") : activity.appName;
            if (!activity.appKey.empty())
                label += L" (" + activity.appKey + L")";
            return label;
        };

        auto buildProcessLabel = [](const ProductiveActivityInfo& activity)
        {
            std::wstring label = activity.processName.empty() ? std::wstring(L"None") : activity.processName;
            if (activity.processId != 0)
                label += L" (PID " + std::to_wstring(activity.processId) + L")";
            return label;
        };

        bool rawActive = info.active;
        bool rawAllowed = !rawActive || IsProductiveAppEnabled(info);
        bool rawFiltered = rawActive && !rawAllowed;

        if (!m_productiveEnabled)
        {
            ProductiveDetectedAppText().Text(L"Productive RPC disabled");
            ProductiveDetectedProjectText().Text(L"Enable Productive RPC to publish Office activity.");
            ProductiveDetectedWindowText().Text(L"None");
            ProductiveDetectedProcessText().Text(L"None");
            ProductiveRuntimeSummaryText().Text(L"Productive pipeline is disabled.");
            UpdateProductiveAppIcon({});
            UpdateHomeProductivePreview();
            return;
        }

        if (rawFiltered)
        {
            ProductiveDetectedAppText().Text(buildAppLabel(info) + L" (filtered)");
            ProductiveDetectedProjectText().Text(L"Enable this app in the per-app list to allow Productivity RPC.");
            ProductiveDetectedWindowText().Text(info.windowTitle.empty() ? L"None" : info.windowTitle);
            ProductiveDetectedProcessText().Text(buildProcessLabel(info));
            ProductiveRuntimeSummaryText().Text(
                L"Detected app is blocked by Productivity per-app filters. Detection mode: " +
                ProductiveDetectionModeLabel(m_productiveDetectionMode) + L".");
            UpdateProductiveAppIcon(info);
            UpdateHomeProductivePreview();
            return;
        }

        if (!info.active)
        {
            ProductiveDetectedAppText().Text(L"Awaiting Productive Activity");
            ProductiveDetectedProjectText().Text(L"Launch a supported Office app to update your status.");
            ProductiveDetectedWindowText().Text(L"None");
            ProductiveDetectedProcessText().Text(L"None");
            ProductiveRuntimeSummaryText().Text(
                L"Waiting for supported apps (Word, Excel, PowerPoint, OneNote, Access, Publisher, Visio, Project). "
                L"Detection mode: " + ProductiveDetectionModeLabel(m_productiveDetectionMode) + L".");
            UpdateProductiveAppIcon({});
            UpdateHomeProductivePreview();
            return;
        }

        std::wstring appLabel = buildAppLabel(info);

        std::wstring subtitle;
        if (m_productiveShowProjectName && !info.projectHint.empty())
            subtitle = L"Working on " + info.projectHint;
        else
            subtitle = L"Working in " + (info.appName.empty() ? std::wstring(L"Microsoft Office") : info.appName);

        ProductiveDetectedAppText().Text(appLabel);
        ProductiveDetectedProjectText().Text(subtitle);
        ProductiveDetectedWindowText().Text(info.windowTitle.empty() ? L"None" : info.windowTitle);
        ProductiveDetectedProcessText().Text(buildProcessLabel(info));
        ProductiveRuntimeSummaryText().Text(
            L"Productive detector active. Detection mode: " + ProductiveDetectionModeLabel(m_productiveDetectionMode) +
            L". Outlook, Teams, and background Office helpers are excluded.");
        UpdateProductiveAppIcon(info);
        UpdateHomeProductivePreview();
    }

    void MainWindow::UpdateCreativePreview(const CreativeActivityInfo& info)
    {
        if (!CreativeMvpHeadlineText() || !CreativeMvpSummaryText())
            return;

        if (CreativeSettingsControlsPanel())
        {
            CreativeSettingsControlsPanel().IsHitTestVisible(m_creativeEnabled);
            CreativeSettingsControlsPanel().Opacity(m_creativeEnabled ? 1.0 : 0.55);
        }

        auto setPreviewRows = [&](const std::wstring& app, const std::wstring& project,
                                  const std::wstring& window, const std::wstring& process)
        {
            CreativeDetectedAppText().Text(hstring(app));
            CreativeDetectedProjectText().Text(hstring(project));
            CreativeDetectedWindowText().Text(hstring(window));
            CreativeDetectedProcessText().Text(hstring(process));
        };

        std::wstring policySummary =
            L"Priority: " + CreativePriorityModeLabel(m_creativePriority) +
            L" | Detection: " + CreativeDetectionModeLabel(m_creativeDetectionMode) +
            L" | Privacy: " + CreativePrivacyModeLabel(m_creativePrivacyMode) +
            L" | Idle: " + CreativeIdleBehaviorLabel(m_creativeIdleBehavior) + L". ";

        if (!m_creativeEnabled)
        {
            CreativeMvpHeadlineText().Text(L"Creativity RPC (disabled)");
            CreativeMvpSummaryText().Text(hstring(
                policySummary +
                L"Creativity detector/pipeline is disabled. Settings are preserved and can be re-enabled later."));
            setPreviewRows(L"Disabled", L"Creativity detection is turned off.", L"--", L"--");
            UpdateCreativeAppIcon({});
            return;
        }

        auto buildAppLabel = [](const CreativeActivityInfo& activity)
        {
            std::wstring label = activity.appName.empty() ? std::wstring(L"Adobe creativity app") : activity.appName;
            if (!activity.appKey.empty())
                label += L" (" + activity.appKey + L")";
            return label;
        };

        auto buildProcessLabel = [](const CreativeActivityInfo& activity)
        {
            std::wstring label = activity.processName.empty() ? std::wstring(L"--") : activity.processName;
            if (activity.processId != 0)
                label += L" (PID " + std::to_wstring(activity.processId) + L")";
            return label;
        };

        auto now = std::chrono::steady_clock::now();
        bool rawActive = info.active;
        bool rawAllowed = !rawActive || IsCreativeAppEnabled(info);
        bool rawFiltered = rawActive && !rawAllowed;

        CreativeActivityInfo displayInfo{};
        bool showDisplay = false;
        bool showingHeld = false;
        bool clearImmediatelyMode = false;

        if (rawActive && rawAllowed)
        {
            displayInfo = info;
            showDisplay = true;
        }
        else
        {
            clearImmediatelyMode = (m_creativeIdleBehavior == CreativeIdleBehavior::ClearImmediately);
            if (m_creativeIdleBehavior == CreativeIdleBehavior::HoldLast5Seconds &&
                m_lastCreativeAcceptedActivity.active &&
                IsCreativeAppEnabled(m_lastCreativeAcceptedActivity) &&
                m_lastCreativeActiveSeenAt.time_since_epoch().count() > 0)
            {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_lastCreativeActiveSeenAt);
                if (elapsed <= std::chrono::seconds(kCreativeIdleHoldSeconds))
                {
                    displayInfo = m_lastCreativeAcceptedActivity;
                    showDisplay = true;
                    showingHeld = true;
                }
            }
        }

        CreativeActivityInfo iconInfo{};
        if (m_creativePrivacyMode != CreativePrivacyMode::Private)
        {
            if (showDisplay)
                iconInfo = displayInfo;
            else if (rawFiltered && info.active)
                iconInfo = info;
        }
        UpdateCreativeAppIcon(iconInfo);

        if (!showDisplay)
        {
            std::wstring headline = L"Creativity RPC (MVP detector idle)";
            std::wstring summary = policySummary;

            if (rawFiltered)
            {
                headline = L"Creativity RPC (filtered by app settings)";
                summary += L"An Adobe app is detected but blocked by your per-app Creativity filter list.";

                if (m_creativePrivacyMode == CreativePrivacyMode::Private)
                {
                    setPreviewRows(L"Creativity activity hidden", L"(Filtered and hidden by privacy mode)", L"--", L"--");
                }
                else if (m_creativePrivacyMode == CreativePrivacyMode::AppOnly)
                {
                    setPreviewRows(buildAppLabel(info) + L" (filtered)", L"(Hidden by privacy mode)", L"(Hidden by privacy mode)", L"(Hidden by privacy mode)");
                }
                else
                {
                    setPreviewRows(buildAppLabel(info) + L" (filtered)",
                                   L"Enable this app in the per-app list to allow Creativity RPC.",
                                   m_creativeShowWindowTitle && !info.windowTitle.empty() ? info.windowTitle : std::wstring(L"(Hidden by setting)"),
                    buildProcessLabel(info));
                }
            }
            else if (clearImmediatelyMode)
            {
                headline = L"Creativity RPC (idle - clear mode)";
                summary += L"No supported app detected. Creativity activity clears immediately.";
                setPreviewRows(L"Awaiting Creative Activity", L"Creativity preview is idle.", L"None", L"None");
            }
            else
            {
                summary += L"Listening for supported apps. Detector remains isolated from the current media pipeline.";
                setPreviewRows(L"Awaiting Creative Activity", L"Launch a supported app to update your status.", L"None", L"None");
            }

            CreativeMvpHeadlineText().Text(hstring(headline));
            CreativeMvpSummaryText().Text(hstring(summary));
            return;
        }

        std::wstring headline = showingHeld
            ? std::wstring(L"Creativity RPC (holding last activity)")
            : std::wstring(L"Creativity RPC (MVP detector active)");
        std::wstring summary = policySummary;
        if (showingHeld)
        {
            int secondsLeft = kCreativeIdleHoldSeconds;
            if (m_lastCreativeActiveSeenAt.time_since_epoch().count() > 0)
            {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_lastCreativeActiveSeenAt).count();
                if (elapsed < 0) elapsed = 0;
                secondsLeft = static_cast<int>(kCreativeIdleHoldSeconds - elapsed);
                if (secondsLeft < 0) secondsLeft = 0;
            }

            summary += L"Holding the last allowed Creativity activity for " + std::to_wstring(secondsLeft) + L"s.";
            if (rawFiltered)
                summary += L" Current detected Adobe app is filtered.";
            else if (!rawActive)
                summary += L" No Adobe app is currently detected.";
        }
        else
        {
            summary += L"Adobe app detected natively. Creativity settings are active, but Discord publishing remains separate from the media pipeline for now.";
        }

        CreativeMvpHeadlineText().Text(hstring(headline));
        CreativeMvpSummaryText().Text(hstring(summary));

        if (m_creativePrivacyMode == CreativePrivacyMode::Private)
        {
            setPreviewRows(L"Creativity activity hidden", L"(Private mode)", L"(Private mode)", L"(Private mode)");
            return;
        }

        std::wstring appLabel = buildAppLabel(displayInfo);
        if (showingHeld)
            appLabel += L" (held)";

        if (m_creativePrivacyMode == CreativePrivacyMode::AppOnly)
        {
            setPreviewRows(appLabel, L"(Hidden by privacy mode)", L"(Hidden by privacy mode)", L"(Hidden by privacy mode)");
            return;
        }

        std::wstring projectText;
        if (!m_creativeShowProjectName)
            projectText = L"(Hidden by setting)";
        else if (displayInfo.projectHint.empty())
            projectText = L"(No project title heuristic)";
        else
            projectText = displayInfo.projectHint;

        std::wstring windowText;
        if (!m_creativeShowWindowTitle)
            windowText = L"(Hidden by debug setting)";
        else if (displayInfo.windowTitle.empty())
            windowText = L"--";
        else
            windowText = displayInfo.windowTitle;

        setPreviewRows(appLabel, projectText, windowText, buildProcessLabel(displayInfo));
    }

    void MainWindow::UpdateHomeProductivePreview()
    {
        if (!HomeProductiveTitle() || !HomeProductiveSubtitle() ||
            !HomeProductiveWindowText() || !HomeProductiveProcessText() ||
            !HomeProductiveStatusText() || !HomeProductiveStatusIcon() ||
            !HomeProductiveSourceValue() || !HomeProductiveSourceSubtext() || !HomeProductiveDetectedViaText() ||
            !HomeProductiveDetectorIndicator() || !HomeProductiveDetectorText() || !HomeProductiveDetectorSubtext())
        {
            return;
        }

        auto setStatus = [this](const std::wstring& status, const std::wstring& glyph, bool accent)
        {
            auto brushKey = accent ? L"AccentTextFillColorPrimaryBrush" : L"TextFillColorSecondaryBrush";
            auto brush = Application::Current().Resources().Lookup(box_value(brushKey)).as<Brush>();
            HomeProductiveStatusText().Text(hstring(status));
            HomeProductiveStatusIcon().Glyph(hstring(glyph));
            HomeProductiveStatusText().Foreground(brush);
            HomeProductiveStatusIcon().Foreground(brush);
        };

        auto setRows = [this](const std::wstring& title, const std::wstring& subtitle,
                              const std::wstring& window, const std::wstring& process)
        {
            HomeProductiveTitle().Text(hstring(title));
            HomeProductiveSubtitle().Text(hstring(subtitle));
            HomeProductiveWindowText().Text(hstring(window));
            HomeProductiveProcessText().Text(hstring(process));
        };

        auto setSourceRows = [this](const std::wstring& source, const std::wstring& subtext, const std::wstring& detectedVia)
        {
            HomeProductiveSourceValue().Text(hstring(source));
            HomeProductiveSourceSubtext().Text(hstring(subtext));
            HomeProductiveDetectedViaText().Text(hstring(detectedVia));
        };

        auto setDetectorState = [this](const std::wstring& title, const std::wstring& subtext, const wchar_t* brushKey)
        {
            auto brush = Application::Current().Resources().Lookup(box_value(brushKey)).as<Brush>();
            HomeProductiveDetectorIndicator().Fill(brush);
            HomeProductiveDetectorText().Text(hstring(title));
            HomeProductiveDetectorSubtext().Text(hstring(subtext));
        };

        auto productiveDetectedViaLabel = [this]() -> std::wstring
        {
            switch (m_productiveDetectionMode)
            {
            case ProductiveDetectionMode::ForegroundOnly:
                return L"Detected via foreground window";
            case ProductiveDetectionMode::VisibleWindowOnly:
                return L"Detected via visible Office window";
            default:
                return L"Detected via foreground + fallback";
            }
        };

        auto buildAppLabel = [](const ProductiveActivityInfo& activity)
        {
            std::wstring label = activity.appName.empty() ? std::wstring(L"Microsoft Office") : activity.appName;
            if (!activity.appKey.empty())
                label += L" (" + activity.appKey + L")";
            return label;
        };

        auto buildProcessLabel = [](const ProductiveActivityInfo& activity)
        {
            std::wstring label = activity.processName.empty() ? std::wstring(L"None") : activity.processName;
            if (activity.processId != 0)
                label += L" (PID " + std::to_wstring(activity.processId) + L")";
            return label;
        };

        if (!m_enabled)
        {
            setStatus(L"Paused", L"\xE769", false);
            setRows(L"Rich Presence is off",
                    L"Enable Rich Presence to run the Productive detector.",
                    L"None",
                    L"None");
            setSourceRows(L"Productivity pipeline paused",
                          L"Rich Presence is disabled.",
                          L"Detected via --");
            setDetectorState(L"Productivity detector paused",
                             L"Enable Rich Presence to resume.",
                             L"StatusDisconnectedBrush");
            UpdateProductiveAppIcon({});
            return;
        }

        if (!m_productiveEnabled)
        {
            setStatus(L"Disabled", L"\xE711", false);
            setRows(L"Productive RPC disabled",
                    L"Turn on Productive RPC in the Productive section.",
                    L"None",
                    L"None");
            setSourceRows(L"Productivity source disabled",
                          L"Turn on Productive RPC in Productive settings.",
                          L"Detected via --");
            setDetectorState(L"Productivity detector off",
                             L"Productive pipeline toggle is off.",
                             L"StatusDisconnectedBrush");
            UpdateProductiveAppIcon({});
            return;
        }

        const auto& rawInfo = m_lastProductiveActivity;
        bool rawActive = rawInfo.active;
        bool rawAllowed = !rawActive || IsProductiveAppEnabled(rawInfo);
        bool rawFiltered = rawActive && !rawAllowed;

        if (rawFiltered)
        {
            auto filteredLabel = buildAppLabel(rawInfo);
            setStatus(L"Filtered", L"\xE711", false);
            setRows(filteredLabel + L" (filtered)",
                    L"Enable this app in Productivity filters to allow activity.",
                    rawInfo.windowTitle.empty() ? std::wstring(L"None") : rawInfo.windowTitle,
                    buildProcessLabel(rawInfo));
            setSourceRows(filteredLabel + L" (filtered)",
                          L"Blocked by your Productivity per-app filters.",
                          productiveDetectedViaLabel());
            setDetectorState(L"Productivity detector filtering",
                             L"App blocked by your Productivity app filter.",
                             L"StatusConnectingBrush");
            return;
        }

        if (!rawActive)
        {
            setStatus(L"Waiting", L"\xE160", false);
            setRows(L"Awaiting Productive Activity",
                    L"Launch a supported Office app to update your status.",
                    L"None",
                    L"None");
            setSourceRows(L"No active productivity source",
                          L"Waiting for supported apps...",
                          productiveDetectedViaLabel());
            setDetectorState(L"Productivity detector waiting",
                             L"Waiting for supported apps...",
                             L"StatusConnectingBrush");
            return;
        }

        std::wstring appLabel = buildAppLabel(rawInfo);

        std::wstring subtitle;
        if (m_productiveShowProjectName && !rawInfo.projectHint.empty())
            subtitle = L"Working on " + rawInfo.projectHint;
        else
            subtitle = L"Working in " + (rawInfo.appName.empty()
                ? std::wstring(L"Microsoft Office")
                : rawInfo.appName);

        setStatus(L"Active", L"\xE768", true);
        setRows(appLabel,
                subtitle,
                rawInfo.windowTitle.empty() ? std::wstring(L"None") : rawInfo.windowTitle,
                buildProcessLabel(rawInfo));
        setSourceRows(appLabel,
                      L"Publishing current Office app.",
                      productiveDetectedViaLabel());
        setDetectorState(L"Productivity detector active",
                         L"Mode: " + ProductiveDetectionModeLabel(m_productiveDetectionMode),
                         L"StatusConnectedBrush");
    }

    void MainWindow::UpdateHomeCreativePreview()
    {
        if (!HomeCreativeTitle() || !HomeCreativeSubtitle() || !HomeCreativeWindowText() || !HomeCreativeProcessText() ||
            !HomeCreativeSourceValue() || !HomeCreativeSourceSubtext() || !HomeCreativeDetectedViaText() ||
            !HomeCreativeDetectorIndicator() || !HomeCreativeDetectorText() || !HomeCreativeDetectorSubtext())
            return;

        auto setStatus = [this](const std::wstring& status, const std::wstring& glyph, bool accent)
        {
            auto brushKey = accent ? L"AccentTextFillColorPrimaryBrush" : L"TextFillColorSecondaryBrush";
            auto brush = Application::Current().Resources().Lookup(box_value(brushKey)).as<Brush>();
            HomeCreativeStatusText().Text(hstring(status));
            HomeCreativeStatusIcon().Glyph(hstring(glyph));
            HomeCreativeStatusText().Foreground(brush);
            HomeCreativeStatusIcon().Foreground(brush);
        };

        auto setRows = [this](const std::wstring& title, const std::wstring& subtitle,
                              const std::wstring& window, const std::wstring& process)
        {
            HomeCreativeTitle().Text(hstring(title));
            HomeCreativeSubtitle().Text(hstring(subtitle));
            HomeCreativeWindowText().Text(hstring(window));
            HomeCreativeProcessText().Text(hstring(process));
        };

        auto setSourceRows = [this](const std::wstring& source, const std::wstring& subtext, const std::wstring& detectedVia)
        {
            HomeCreativeSourceValue().Text(hstring(source));
            HomeCreativeSourceSubtext().Text(hstring(subtext));
            HomeCreativeDetectedViaText().Text(hstring(detectedVia));
        };

        auto setDetectorState = [this](const std::wstring& title, const std::wstring& subtext, const wchar_t* brushKey)
        {
            auto brush = Application::Current().Resources().Lookup(box_value(brushKey)).as<Brush>();
            HomeCreativeDetectorIndicator().Fill(brush);
            HomeCreativeDetectorText().Text(hstring(title));
            HomeCreativeDetectorSubtext().Text(hstring(subtext));
        };

        auto creativeDetectedViaLabel = [this]() -> std::wstring
        {
            switch (m_creativeDetectionMode)
            {
            case CreativeDetectionMode::ForegroundOnly:
                return L"Detected via foreground window";
            case CreativeDetectionMode::VisibleWindowOnly:
                return L"Detected via visible Adobe window";
            default:
                return L"Detected via foreground + fallback";
            }
        };

        if (!m_enabled)
        {
            setStatus(L"Paused", L"\xE769", false);
            setRows(L"Rich Presence is off",
                    L"Enable Rich Presence to run the Creativity detector.",
                    L"--",
                    L"--");
            setSourceRows(L"Creativity pipeline paused",
                          L"Rich Presence is disabled.",
                          L"Detected via --");
            setDetectorState(L"Creativity detector paused",
                             L"Enable Rich Presence to resume.",
                             L"StatusDisconnectedBrush");
            UpdateCreativeAppIcon({});
            return;
        }

        if (!m_creativeEnabled)
        {
            setStatus(L"Disabled", L"\xE711", false);
            setRows(L"Creativity RPC disabled",
                    L"Turn on Creativity RPC in the Creativity section.",
                    L"--",
                    L"--");
            setSourceRows(L"Creativity source disabled",
                          L"Turn on Creativity RPC in Creativity settings.",
                          L"Detected via --");
            setDetectorState(L"Creativity detector off",
                             L"Creativity pipeline toggle is off.",
                             L"StatusDisconnectedBrush");
            UpdateCreativeAppIcon({});
            return;
        }

        auto buildAppLabel = [](const CreativeActivityInfo& activity)
        {
            std::wstring label = activity.appName.empty() ? std::wstring(L"Adobe creativity app") : activity.appName;
            if (!activity.appKey.empty())
                label += L" (" + activity.appKey + L")";
            return label;
        };

        auto buildProcessLabel = [](const CreativeActivityInfo& activity)
        {
            std::wstring label = activity.processName.empty() ? std::wstring(L"--") : activity.processName;
            if (activity.processId != 0)
                label += L" (PID " + std::to_wstring(activity.processId) + L")";
            return label;
        };

        const auto& rawInfo = m_lastCreativeActivity;
        bool rawActive = rawInfo.active;
        bool rawAllowed = !rawActive || IsCreativeAppEnabled(rawInfo);
        bool rawFiltered = rawActive && !rawAllowed;
        bool clearImmediatelyMode = (m_creativeIdleBehavior == CreativeIdleBehavior::ClearImmediately);

        CreativeActivityInfo displayInfo{};
        bool showingHeld = false;
        bool showDisplay = TryGetEffectiveCreativeActivityForRpc(displayInfo, showingHeld);

        CreativeActivityInfo iconInfo{};
        if (m_creativePrivacyMode != CreativePrivacyMode::Private)
        {
            if (showDisplay)
                iconInfo = displayInfo;
            else if (rawFiltered && rawInfo.active)
                iconInfo = rawInfo;
        }
        UpdateCreativeAppIcon(iconInfo);

        if (!showDisplay)
        {
            if (rawFiltered)
            {
                setStatus(L"Filtered", L"\xE711", false);
                if (m_creativePrivacyMode == CreativePrivacyMode::Private)
                {
                    setRows(L"Creativity activity hidden",
                            L"(Filtered and hidden by privacy mode)",
                            L"--",
                            L"--");
                    setSourceRows(L"Filtered creativity app",
                                  L"Hidden by privacy mode.",
                                  creativeDetectedViaLabel());
                }
                else if (m_creativePrivacyMode == CreativePrivacyMode::AppOnly)
                {
                    setRows(buildAppLabel(rawInfo) + L" (filtered)",
                            L"Filtered by your per-app Creativity settings.",
                            L"(Hidden by privacy mode)",
                            L"(Hidden by privacy mode)");
                    setSourceRows(buildAppLabel(rawInfo) + L" (filtered)",
                                  L"Blocked by your per-app Creativity filters.",
                                  creativeDetectedViaLabel());
                }
                else
                {
                    std::wstring windowText =
                        (m_creativeShowWindowTitle && !rawInfo.windowTitle.empty())
                            ? rawInfo.windowTitle
                            : std::wstring(L"(Hidden by setting)");

                    setRows(buildAppLabel(rawInfo) + L" (filtered)",
                            L"Enable this app in Creativity filters to allow activity.",
                            windowText,
                            buildProcessLabel(rawInfo));
                    setSourceRows(buildAppLabel(rawInfo) + L" (filtered)",
                                  L"Enable this app in Creativity filters.",
                                  creativeDetectedViaLabel());
                }
                setDetectorState(L"Creativity detector filtering",
                                 L"App blocked by your Creativity app filter.",
                                 L"StatusConnectingBrush");
                return;
            }

            if (clearImmediatelyMode)
            {
                setStatus(L"Idle", L"\xE160", false);
                setRows(L"Awaiting Creative Activity",
                        L"No supported app detected (clear mode).",
                        L"None",
                        L"None");
                setSourceRows(L"No active creativity source",
                              L"Clear mode: idle source is cleared immediately.",
                              creativeDetectedViaLabel());
                setDetectorState(L"Creativity detector idle",
                                 L"Waiting for supported apps...",
                                 L"StatusDisconnectedBrush");
            }
            else
            {
                setStatus(L"Waiting", L"\xE160", false);
                setRows(L"Awaiting Creative Activity",
                        L"Launch a supported app to update your status.",
                        L"None",
                        L"None");
                setSourceRows(L"No active creativity source",
                              L"Hold mode: waits before clearing recent app.",
                              creativeDetectedViaLabel());
                setDetectorState(L"Creativity detector waiting",
                                 L"Waiting for supported apps...",
                                 L"StatusConnectingBrush");
            }
            return;
        }

        if (m_creativePrivacyMode == CreativePrivacyMode::Private)
        {
            setStatus(L"Private", L"\xE72E", false);
            setRows(L"Creativity activity hidden",
                    L"Private mode is enabled.",
                    L"(Private mode)",
                    L"(Private mode)");
            setSourceRows(L"Creativity source hidden",
                          L"Private mode is enabled.",
                          creativeDetectedViaLabel());
            setDetectorState(L"Creativity detector active",
                             L"Activity captured, details hidden.",
                             L"StatusConnectedBrush");
            return;
        }

        std::wstring appLabel = buildAppLabel(displayInfo);
        if (showingHeld)
            appLabel += L" (held)";

        if (m_creativePrivacyMode == CreativePrivacyMode::AppOnly)
        {
            setStatus(showingHeld ? L"Held" : L"Active", showingHeld ? L"\xE823" : L"\xE768", true);
            setRows(appLabel,
                    L"(Details hidden by privacy mode)",
                    L"(Hidden by privacy mode)",
                    L"(Hidden by privacy mode)");
            setSourceRows(appLabel,
                          L"App-only privacy hides project and window details.",
                          creativeDetectedViaLabel());
            setDetectorState(showingHeld ? L"Creativity detector holding" : L"Creativity detector active",
                             showingHeld
                                 ? L"Using hold-last behavior while app is idle."
                                 : L"Publishing current Adobe app.",
                             showingHeld ? L"StatusConnectingBrush" : L"StatusConnectedBrush");
            return;
        }

        std::wstring projectText;
        if (!m_creativeShowProjectName)
            projectText = L"(Hidden by setting)";
        else if (displayInfo.projectHint.empty())
            projectText = L"(No project title heuristic)";
        else
            projectText = displayInfo.projectHint;

        std::wstring windowText;
        if (!m_creativeShowWindowTitle)
            windowText = L"(Hidden by debug setting)";
        else if (displayInfo.windowTitle.empty())
            windowText = L"--";
        else
            windowText = displayInfo.windowTitle;

        setStatus(showingHeld ? L"Held" : L"Active", showingHeld ? L"\xE823" : L"\xE768", true);
        setRows(appLabel, projectText, windowText, buildProcessLabel(displayInfo));
        setSourceRows(appLabel,
                      showingHeld ? L"Holding last detected Adobe app." : L"Publishing current Adobe app.",
                      creativeDetectedViaLabel());
        setDetectorState(showingHeld ? L"Creativity detector holding" : L"Creativity detector active",
                         L"Mode: " + CreativeDetectionModeLabel(m_creativeDetectionMode) +
                             L" | Idle: " + CreativeIdleBehaviorLabel(m_creativeIdleBehavior),
                         showingHeld ? L"StatusConnectingBrush" : L"StatusConnectedBrush");
    }

    void MainWindow::UpdateProductiveAppIcon(const ProductiveActivityInfo& info)
    {
        bool hasProductiveIconControls = ProductiveDetectedAppIcon() && ProductiveDetectedAppIconFallback();
        bool hasHomeIconControls = HomeProductiveAppIcon() && HomeProductiveAppIconFallback();
        if (!hasProductiveIconControls && !hasHomeIconControls)
            return;

        auto showFallback = [this, hasProductiveIconControls, hasHomeIconControls]()
        {
            if (hasProductiveIconControls)
            {
                ProductiveDetectedAppIcon().Source(nullptr);
                ProductiveDetectedAppIcon().Visibility(Visibility::Collapsed);
                ProductiveDetectedAppIconFallback().Visibility(Visibility::Visible);
            }

            if (hasHomeIconControls)
            {
                HomeProductiveAppIcon().Source(nullptr);
                HomeProductiveAppIcon().Visibility(Visibility::Collapsed);
                HomeProductiveAppIconFallback().Visibility(Visibility::Visible);
            }
        };

        if (!info.active)
        {
            if (m_productiveIconUpdateTask)
            {
                m_productiveIconUpdateTask.Cancel();
                m_productiveIconUpdateTask = nullptr;
            }
            m_lastProductiveIconCacheKey.clear();
            showFallback();
            return;
        }

        auto curatedPath = TryFindCuratedProductiveLogoPath(info);

        std::wstring cacheKey;
        if (!info.processPath.empty())
        {
            cacheKey = L"procpath|" + ToLowerCopy(info.processPath);
            if (!curatedPath.empty())
                cacheKey += L"|backup|" + ToLowerCopy(curatedPath);
        }
        else if (!curatedPath.empty())
        {
            cacheKey = L"backup|" + ToLowerCopy(curatedPath);
        }
        else if (!info.processName.empty())
        {
            cacheKey = L"procname|" + ToLowerCopy(info.processName);
        }
        else
        {
            cacheKey = L"fallback";
        }

        if (cacheKey == m_lastProductiveIconCacheKey)
            return;

        if (m_productiveIconUpdateTask)
        {
            m_productiveIconUpdateTask.Cancel();
            m_productiveIconUpdateTask = nullptr;
        }

        m_lastProductiveIconCacheKey = cacheKey;
        showFallback();

        auto requestId = ++m_productiveIconRequestId;
        auto updateTask = [](auto strongThis,
                             ProductiveActivityInfo iconInfo,
                             std::wstring key,
                             std::wstring curated,
                             uint64_t requestIdValue) -> winrt::Windows::Foundation::IAsyncAction
        {
            auto cancellation = co_await winrt::get_cancellation_token();
            cancellation.enable_propagation();

            auto applyFallbackIfCurrent = [strongThis, requestIdValue]()
            {
                if (strongThis->m_isShuttingDown || strongThis->m_productiveIconRequestId != requestIdValue)
                    return;

                if (strongThis->ProductiveDetectedAppIcon() && strongThis->ProductiveDetectedAppIconFallback())
                {
                    strongThis->ProductiveDetectedAppIcon().Source(nullptr);
                    strongThis->ProductiveDetectedAppIcon().Visibility(Visibility::Collapsed);
                    strongThis->ProductiveDetectedAppIconFallback().Visibility(Visibility::Visible);
                }

                if (strongThis->HomeProductiveAppIcon() && strongThis->HomeProductiveAppIconFallback())
                {
                    strongThis->HomeProductiveAppIcon().Source(nullptr);
                    strongThis->HomeProductiveAppIcon().Visibility(Visibility::Collapsed);
                    strongThis->HomeProductiveAppIconFallback().Visibility(Visibility::Visible);
                }
            };

            auto isStale = [strongThis, requestIdValue]()
            {
                return strongThis->m_isShuttingDown || strongThis->m_productiveIconRequestId != requestIdValue;
            };

            if (isStale())
                co_return;

            BitmapImage resolvedImage{ nullptr };

            // Primary source: system icon from the running process executable.
            if (!iconInfo.processPath.empty())
            {
                try
                {
                    auto processFile = co_await Windows::Storage::StorageFile::GetFileFromPathAsync(iconInfo.processPath);
                    if (isStale())
                        co_return;

                    auto processThumb = co_await processFile.GetThumbnailAsync(
                        Windows::Storage::FileProperties::ThumbnailMode::SingleItem,
                        128,
                        Windows::Storage::FileProperties::ThumbnailOptions::UseCurrentScale);
                    if (isStale())
                        co_return;

                    IRandomAccessStreamWithContentType processStream = processThumb;
                    if (processStream)
                    {
                        auto candidate = BitmapImage();
                        co_await candidate.SetSourceAsync(processStream);
                        if (isStale())
                            co_return;

                        resolvedImage = candidate;
                    }
                }
                catch (winrt::hresult_canceled const&)
                {
                    co_return;
                }
                catch (...)
                {
                }
            }

            // Backup source: curated local logo file.
            if (!resolvedImage && !curated.empty())
            {
                try
                {
                    auto curatedFile = co_await Windows::Storage::StorageFile::GetFileFromPathAsync(curated);
                    if (isStale())
                        co_return;

                    auto curatedStream = co_await curatedFile.OpenReadAsync();
                    if (isStale())
                        co_return;

                    if (curatedStream)
                    {
                        auto candidate = BitmapImage();
                        co_await candidate.SetSourceAsync(curatedStream);
                        if (isStale())
                            co_return;

                        resolvedImage = candidate;
                    }
                }
                catch (winrt::hresult_canceled const&)
                {
                    co_return;
                }
                catch (...)
                {
                }
            }

            if (!resolvedImage)
            {
                applyFallbackIfCurrent();
                co_return;
            }

            if (isStale())
                co_return;

            if (strongThis->m_lastProductiveIconCacheKey != key)
                co_return;

            if (strongThis->ProductiveDetectedAppIcon() && strongThis->ProductiveDetectedAppIconFallback())
            {
                strongThis->ProductiveDetectedAppIcon().Source(resolvedImage);
                strongThis->ProductiveDetectedAppIcon().Visibility(Visibility::Visible);
                strongThis->ProductiveDetectedAppIconFallback().Visibility(Visibility::Collapsed);
            }

            if (strongThis->HomeProductiveAppIcon() && strongThis->HomeProductiveAppIconFallback())
            {
                strongThis->HomeProductiveAppIcon().Source(resolvedImage);
                strongThis->HomeProductiveAppIcon().Visibility(Visibility::Visible);
                strongThis->HomeProductiveAppIconFallback().Visibility(Visibility::Collapsed);
            }
        };

        m_productiveIconUpdateTask = updateTask(get_strong(), info, m_lastProductiveIconCacheKey, curatedPath, requestId);
    }

    void MainWindow::UpdateCreativeAppIcon(const CreativeActivityInfo& info)
    {
        bool hasCreativeIconControls = CreativeDetectedAppIcon() && CreativeDetectedAppIconFallback();
        bool hasHomeIconControls = HomeCreativeAppIcon() && HomeCreativeAppIconFallback();
        if (!hasCreativeIconControls && !hasHomeIconControls)
            return;

        auto showFallback = [this, hasCreativeIconControls, hasHomeIconControls]()
        {
            if (hasCreativeIconControls)
            {
                CreativeDetectedAppIcon().Source(nullptr);
                CreativeDetectedAppIcon().Visibility(Visibility::Collapsed);
                CreativeDetectedAppIconFallback().Visibility(Visibility::Visible);
            }

            if (hasHomeIconControls)
            {
                HomeCreativeAppIcon().Source(nullptr);
                HomeCreativeAppIcon().Visibility(Visibility::Collapsed);
                HomeCreativeAppIconFallback().Visibility(Visibility::Visible);
            }
        };

        if (!info.active)
        {
            if (m_creativeIconUpdateTask)
            {
                m_creativeIconUpdateTask.Cancel();
                m_creativeIconUpdateTask = nullptr;
            }
            m_lastCreativeIconCacheKey.clear();
            showFallback();
            return;
        }

        auto curatedPath = TryFindCuratedCreativeLogoPath(info);

        std::wstring cacheKey;
        if (!info.processPath.empty())
        {
            cacheKey = L"procpath|" + ToLowerCopy(info.processPath);
            if (!curatedPath.empty())
                cacheKey += L"|backup|" + ToLowerCopy(curatedPath);
        }
        else if (!curatedPath.empty())
        {
            cacheKey = L"backup|" + ToLowerCopy(curatedPath);
        }
        else if (!info.processName.empty())
        {
            cacheKey = L"procname|" + ToLowerCopy(info.processName);
        }
        else
        {
            cacheKey = L"fallback";
        }

        if (cacheKey == m_lastCreativeIconCacheKey)
            return;

        if (m_creativeIconUpdateTask)
        {
            m_creativeIconUpdateTask.Cancel();
            m_creativeIconUpdateTask = nullptr;
        }

        m_lastCreativeIconCacheKey = cacheKey;
        showFallback();

        auto requestId = ++m_creativeIconRequestId;
        auto updateTask = [](auto strongThis,
                             CreativeActivityInfo iconInfo,
                             std::wstring key,
                             std::wstring curated,
                             uint64_t requestIdValue) -> winrt::Windows::Foundation::IAsyncAction
        {
            auto cancellation = co_await winrt::get_cancellation_token();
            cancellation.enable_propagation();

            auto applyFallbackIfCurrent = [strongThis, requestIdValue]()
            {
                if (strongThis->m_isShuttingDown || strongThis->m_creativeIconRequestId != requestIdValue)
                    return;

                if (strongThis->CreativeDetectedAppIcon() && strongThis->CreativeDetectedAppIconFallback())
                {
                    strongThis->CreativeDetectedAppIcon().Source(nullptr);
                    strongThis->CreativeDetectedAppIcon().Visibility(Visibility::Collapsed);
                    strongThis->CreativeDetectedAppIconFallback().Visibility(Visibility::Visible);
                }

                if (strongThis->HomeCreativeAppIcon() && strongThis->HomeCreativeAppIconFallback())
                {
                    strongThis->HomeCreativeAppIcon().Source(nullptr);
                    strongThis->HomeCreativeAppIcon().Visibility(Visibility::Collapsed);
                    strongThis->HomeCreativeAppIconFallback().Visibility(Visibility::Visible);
                }
            };

            auto isStale = [strongThis, requestIdValue]()
            {
                return strongThis->m_isShuttingDown || strongThis->m_creativeIconRequestId != requestIdValue;
            };

            if (isStale())
                co_return;

            BitmapImage resolvedImage{ nullptr };

            // Primary source: system icon from the running process executable.
            if (!iconInfo.processPath.empty())
            {
                try
                {
                    auto processFile = co_await Windows::Storage::StorageFile::GetFileFromPathAsync(iconInfo.processPath);
                    if (isStale())
                        co_return;

                    auto processThumb = co_await processFile.GetThumbnailAsync(
                        Windows::Storage::FileProperties::ThumbnailMode::SingleItem,
                        128,
                        Windows::Storage::FileProperties::ThumbnailOptions::UseCurrentScale);
                    if (isStale())
                        co_return;

                    IRandomAccessStreamWithContentType processStream = processThumb;
                    if (processStream)
                    {
                        auto candidate = BitmapImage();
                        co_await candidate.SetSourceAsync(processStream);
                        if (isStale())
                            co_return;

                        resolvedImage = candidate;
                    }
                }
                catch (winrt::hresult_canceled const&)
                {
                    co_return;
                }
                catch (...)
                {
                }
            }

            // Backup source: curated local logo file.
            if (!resolvedImage && !curated.empty())
            {
                try
                {
                    auto curatedFile = co_await Windows::Storage::StorageFile::GetFileFromPathAsync(curated);
                    if (isStale())
                        co_return;

                    auto curatedStream = co_await curatedFile.OpenReadAsync();
                    if (isStale())
                        co_return;

                    if (curatedStream)
                    {
                        auto candidate = BitmapImage();
                        co_await candidate.SetSourceAsync(curatedStream);
                        if (isStale())
                            co_return;

                        resolvedImage = candidate;
                    }
                }
                catch (winrt::hresult_canceled const&)
                {
                    co_return;
                }
                catch (...)
                {
                }
            }

            if (!resolvedImage)
            {
                applyFallbackIfCurrent();
                co_return;
            }

            if (isStale())
                co_return;

            if (strongThis->m_lastCreativeIconCacheKey != key)
                co_return;

            if (strongThis->CreativeDetectedAppIcon() && strongThis->CreativeDetectedAppIconFallback())
            {
                strongThis->CreativeDetectedAppIcon().Source(resolvedImage);
                strongThis->CreativeDetectedAppIcon().Visibility(Visibility::Visible);
                strongThis->CreativeDetectedAppIconFallback().Visibility(Visibility::Collapsed);
            }

            if (strongThis->HomeCreativeAppIcon() && strongThis->HomeCreativeAppIconFallback())
            {
                strongThis->HomeCreativeAppIcon().Source(resolvedImage);
                strongThis->HomeCreativeAppIcon().Visibility(Visibility::Visible);
                strongThis->HomeCreativeAppIconFallback().Visibility(Visibility::Collapsed);
            }
        };

        m_creativeIconUpdateTask = updateTask(get_strong(), info, m_lastCreativeIconCacheKey, curatedPath, requestId);
    }

    // =====================================================================
    // UI updates
    // =====================================================================

    void MainWindow::UpdateUI(const MediaInfo& info)
    {
        m_lastMedia = info;
        UpdateConnectionStatus();

        bool hasMedia = !info.title.empty();
        auto trackKey = NormalizeForMatch(info.title + L" " + info.artist);
        auto sourceKey = NormalizeForMatch(info.sourceDisplayName + L" " + info.sourceName);
        bool animateTrack = hasMedia && !trackKey.empty() && trackKey != m_lastUiTrackKey;
        bool animateSource = hasMedia && !sourceKey.empty() && sourceKey != m_lastUiSourceKey;

        if (!hasMedia)
        {
            MusicEmptyState().Visibility(Visibility::Visible);
            NowPlayingCard().Visibility(Visibility::Collapsed);
            TransitionElementVisibility(SourceBadge(), false, 6.0);
            HomeMiniPlayer().Visibility(Visibility::Visible);

            SongTitle().Text(L"Nothing playing");
            ArtistName().Text(L"\x2014");
            AlbumName().Text(L"");
            PositionText().Text(L"0:00");
            DurationText().Text(L"0:00");
            SetSongProgress(0);
            SetHomeMiniProgress(0);
            PlayPauseIcon().Glyph(L"\xE768");
            PlaybackStateText().Text(L"Idle");

            HomeSourceValue().Text(L"No active source");
            HomeSourceSubtext().Text(L"Waiting for active session");
            HomeDetectedViaText().Text(L"Detected via --");
            HomeMiniTitle().Text(L"Nothing playing");
            HomeMiniArtist().Text(L"Start playback or activity");
            HomeMiniTimer().Text(L"0:00 / 0:00");
            SetHomeMiniProgress(0);
            HomeMiniPlayIcon().Glyph(L"\xE160");
            HomeMiniStatusText().Text(L"Waiting");
            auto mutedBrush = Application::Current().Resources()
                .Lookup(box_value(L"TextFillColorSecondaryBrush"))
                .as<Brush>();
            HomeMiniPlayIcon().Foreground(mutedBrush);
            HomeMiniStatusText().Foreground(mutedBrush);
            TransitionElementVisibility(HomePausedChip(), false, 4.0);
            SetHomeMiniWaveActive(false);
            SetSongWaveActive(false);
            HideTrackTransitionSkeleton();

            AlbumThumbnail().Source(nullptr);
            HomeMiniThumbnail().Source(nullptr);
            UpdateSourceBadge(info);
            m_lastUiTrackKey.clear();
            m_lastUiSourceKey.clear();
            return;
        }

        // Show now playing
        MusicEmptyState().Visibility(Visibility::Collapsed);
        NowPlayingCard().Visibility(Visibility::Visible);
        HomeMiniPlayer().Visibility(Visibility::Visible);

        HomeMiniTitle().Text(info.title);
        HomeMiniArtist().Text(info.artist.empty() ? L"\x2014" : info.artist);
        HomeSourceValue().Text(info.sourceDisplayName.empty() ? L"Unknown source" : info.sourceDisplayName);
        HomeSourceSubtext().Text(info.sourceName.empty() ? L"Detected via Windows Media Controls" : info.sourceName);
        HomeDetectedViaText().Text(BuildDetectedViaLabel(info, m_lastMergeState));
        TransitionElementVisibility(HomePausedChip(), !info.isPlaying, 4.0);
        SetHomeMiniWaveActive(info.isPlaying);
        SetSongWaveActive(info.isPlaying);

        // Play/Pause icons
        auto glyph = info.isPlaying ? L"\xE768" : L"\xE769";
        PlayPauseIcon().Glyph(glyph);
        PlaybackStateText().Text(info.isPlaying ? L"Playing" : L"Paused");
        HomeMiniPlayIcon().Glyph(glyph);
        HomeMiniStatusText().Text(info.isPlaying ? L"Live" : L"Paused");
        auto statusBrush = Application::Current().Resources()
            .Lookup(box_value(info.isPlaying
                ? L"AccentTextFillColorPrimaryBrush"
                : L"TextFillColorSecondaryBrush"))
            .as<Brush>();
        HomeMiniPlayIcon().Foreground(statusBrush);
        HomeMiniStatusText().Foreground(statusBrush);

        SongTitle().Text(info.title);
        ArtistName().Text(info.artist.empty() ? L"\x2014" : info.artist);
        AlbumName().Text(info.albumTitle);

        auto posSec = static_cast<int>(info.position.count());
        auto durSec = static_cast<int>(info.duration.count());

        int hintPos = 0;
        int hintDur = 0;
        if (TryGetLiveHintTimeline(info, hintPos, hintDur))
        {
            posSec = hintPos;
            durSec = hintDur;
        }

        PositionText().Text(FormatTime(posSec));
        DurationText().Text(FormatTime(durSec));

        if (durSec > 0)
        {
            double progress = (static_cast<double>(posSec) / durSec) * 100.0;
            SetSongProgress(progress);
            SetHomeMiniProgress(progress);
        }
        else
        {
            SetSongProgress(0);
            SetHomeMiniProgress(0);
        }

        auto displayPos = posSec;
        if (displayPos < 0) displayPos = 0;
        if (durSec > 0 && displayPos > durSec) displayPos = durSec;
        HomeMiniTimer().Text(FormatTime(displayPos) + L" / " + FormatTime(durSec > 0 ? durSec : 0));

        UpdateSourceBadge(info);

        UpdateThumbnail(info);

        m_lastUiTrackKey = std::move(trackKey);
        m_lastUiSourceKey = std::move(sourceKey);

        auto motionEnabled = IsMotionEnabled();

        if (animateSource && motionEnabled)
            AnimateOpacityPulse(HomeSourceCard());
        if (animateTrack)
        {
            if (motionEnabled)
            {
                AnimateOpacityPulse(HomeMiniPlayer());
                ShowTrackTransitionSkeleton();
            }
            else
            {
                HideTrackTransitionSkeleton();
            }
        }
    }

    void MainWindow::UpdateSourceBadge(const MediaInfo& info)
    {
        if (SourceToggle().IsOn() && !info.sourceDisplayName.empty())
        {
            SourceText().Text(info.sourceDisplayName);

            if (m_sourceDebugMode)
            {
                SourceDebugText().Visibility(Visibility::Visible);
                SourceDebugText().Text(FormatSourceDebugText(info));
            }
            else
            {
                SourceDebugText().Visibility(Visibility::Collapsed);
                SourceDebugText().Text(L"");
            }

            TransitionElementVisibility(SourceBadge(), true, 6.0);
            return;
        }

        TransitionElementVisibility(SourceBadge(), false, 6.0);
        SourceText().Text(L"No source");
        SourceDebugText().Visibility(Visibility::Collapsed);
        SourceDebugText().Text(L"");
    }

    std::wstring MainWindow::FormatSourceDebugText(const MediaInfo& info) const
    {
        auto appId = info.sourceName.empty() ? L"(none)" : info.sourceName;
        auto detected = info.detectedService.empty() ? L"(none)" : info.detectedService;
        auto rule = info.detectionReason.empty() ? L"(none)" : info.detectionReason;
        auto score = std::to_wstring(info.detectionScore);

        return L"appId: " + appId +
            L"\nservice: " + detected +
            L"\nrule: " + rule +
            L"\nscore: " + score;
    }

    void MainWindow::AppendDiagnosticLog(const std::wstring& level, const std::wstring& component, const std::wstring& message)
    {
        m_diagnosticLog.Append(level, component, message);
        RefreshDiagnosticsPanel();
    }


    void MainWindow::RefreshDiagnosticsPanel()
    {
        try
        {
            SettingsDiagnosticsPanelHost().SetLogText(m_diagnosticLog.JoinLines());
        }
        catch (...) {}
    }

    std::wstring MainWindow::BuildDiagnosticsSnapshotJson() const
    {
        JsonObject root;
        root.Insert(L"schemaVersion", JsonValue::CreateNumberValue(1));
        root.Insert(L"generatedAtUtc", JsonValue::CreateStringValue(BuildUtcTimestampString()));
        root.Insert(L"mergeState", JsonValue::CreateStringValue(m_lastMergeState));

        JsonObject media;
        media.Insert(L"title", JsonValue::CreateStringValue(m_lastMedia.title));
        media.Insert(L"artist", JsonValue::CreateStringValue(m_lastMedia.artist));
        media.Insert(L"albumTitle", JsonValue::CreateStringValue(m_lastMedia.albumTitle));
        media.Insert(L"sourceName", JsonValue::CreateStringValue(m_lastMedia.sourceName));
        media.Insert(L"sourceDisplayName", JsonValue::CreateStringValue(m_lastMedia.sourceDisplayName));
        media.Insert(L"detectedService", JsonValue::CreateStringValue(m_lastMedia.detectedService));
        media.Insert(L"detectionReason", JsonValue::CreateStringValue(m_lastMedia.detectionReason));
        media.Insert(L"detectionScore", JsonValue::CreateNumberValue(m_lastMedia.detectionScore));
        media.Insert(L"isPlaying", JsonValue::CreateBooleanValue(m_lastMedia.isPlaying));
        media.Insert(L"positionSeconds", JsonValue::CreateNumberValue(static_cast<double>(m_lastMedia.position.count())));
        media.Insert(L"durationSeconds", JsonValue::CreateNumberValue(static_cast<double>(m_lastMedia.duration.count())));
        root.Insert(L"lastMedia", media);

        JsonObject bridge;
        {
            auto& state = GetHintServerState();
            std::lock_guard<std::mutex> lock(state.mutex);

            bridge.Insert(L"started", JsonValue::CreateBooleanValue(state.started));
            bridge.Insert(L"binding", JsonValue::CreateBooleanValue(state.binding));
            bridge.Insert(L"extensionSeen", JsonValue::CreateBooleanValue(state.extensionSeen));
            bridge.Insert(L"extensionConnected", JsonValue::CreateBooleanValue(IsExtensionConnected(state)));
            bridge.Insert(L"hasHint", JsonValue::CreateBooleanValue(state.hasHint));
            bridge.Insert(L"lastAcceptedSequence", JsonValue::CreateNumberValue(static_cast<double>(state.lastAcceptedSequence)));

            if (state.hasHint)
            {
                JsonObject hint;
                hint.Insert(L"siteKey", JsonValue::CreateStringValue(state.hint.siteKey));
                hint.Insert(L"pageHost", JsonValue::CreateStringValue(state.hint.pageHost));
                hint.Insert(L"service", JsonValue::CreateStringValue(state.hint.service));
                hint.Insert(L"mediaKind", JsonValue::CreateStringValue(state.hint.mediaKind));
                hint.Insert(L"rule", JsonValue::CreateStringValue(state.hint.rule));
                hint.Insert(L"isPlaying", JsonValue::CreateBooleanValue(state.hint.isPlaying));
                hint.Insert(L"sequence", JsonValue::CreateNumberValue(static_cast<double>(state.hint.sequence)));
                hint.Insert(L"positionSeconds", JsonValue::CreateNumberValue(state.hint.positionSeconds));
                hint.Insert(L"durationSeconds", JsonValue::CreateNumberValue(state.hint.durationSeconds));
                hint.Insert(L"confidence", JsonValue::CreateNumberValue(state.hint.confidence));
                bridge.Insert(L"hint", hint);
            }
        }
        root.Insert(L"bridge", bridge);

        JsonObject discord;
        discord.Insert(L"media", BuildDiscordStatusJson(m_presence ? m_presence->GetTransportStatus() : DiscordRpcStatus{}));
        discord.Insert(L"productive", BuildDiscordStatusJson(
            m_productivePresence ? m_productivePresence->GetTransportStatus() : DiscordRpcStatus{}));
        discord.Insert(L"creative", BuildDiscordStatusJson(
            m_creativePresence ? m_creativePresence->GetTransportStatus() : DiscordRpcStatus{}));
        root.Insert(L"discord", discord);

        JsonObject lanes;
        JsonObject productiveLane;
        productiveLane.Insert(L"lastAction", JsonValue::CreateStringValue(lrp::ToSettingString(m_productiveLaneState.lastAction)));
        productiveLane.Insert(L"lastReason", JsonValue::CreateStringValue(lrp::ToSettingString(m_productiveLaneState.lastReason)));
        productiveLane.Insert(L"lastDecisionKey", JsonValue::CreateStringValue(m_productiveLaneState.lastDecisionKey));
        lanes.Insert(L"productive", productiveLane);

        JsonObject creativeLane;
        creativeLane.Insert(L"lastAction", JsonValue::CreateStringValue(lrp::ToSettingString(m_creativeLaneState.lastAction)));
        creativeLane.Insert(L"lastReason", JsonValue::CreateStringValue(lrp::ToSettingString(m_creativeLaneState.lastReason)));
        creativeLane.Insert(L"lastDecisionKey", JsonValue::CreateStringValue(m_creativeLaneState.lastDecisionKey));
        lanes.Insert(L"creative", creativeLane);
        root.Insert(L"lanes", lanes);

        JsonArray lines;
        for (const auto& line : m_diagnosticLog.Lines())
            lines.Append(JsonValue::CreateStringValue(line));
        root.Insert(L"logs", lines);

        auto json = root.Stringify();
        return std::wstring(json.c_str());
    }

    std::wstring MainWindow::BuildSettingsSnapshotJson()
    {
        JsonObject root;
        root.Insert(L"schemaVersion", JsonValue::CreateNumberValue(2));
        root.Insert(L"generatedAtUtc", JsonValue::CreateStringValue(BuildUtcTimestampString()));

        root.Insert(L"ShowTimestamps", JsonValue::CreateBooleanValue(TimestampToggle().IsOn()));
        root.Insert(L"ShowSourceApp", JsonValue::CreateBooleanValue(SourceToggle().IsOn()));
        root.Insert(L"SourceDebugMode", JsonValue::CreateBooleanValue(SourceDebugToggle().IsOn()));
        root.Insert(L"ShowPaused", JsonValue::CreateBooleanValue(PausedToggle().IsOn()));
        root.Insert(L"ShowAlbumArt", JsonValue::CreateBooleanValue(AlbumArtToggle().IsOn()));
        root.Insert(L"ShowDefaultIdleStatus", JsonValue::CreateBooleanValue(m_showDefaultIdleStatus));

        root.Insert(L"RichPresenceEnabled", JsonValue::CreateBooleanValue(m_enabled));
        root.Insert(L"CloseToTrayOnClose", JsonValue::CreateBooleanValue(m_closeToTrayOnClose));
        root.Insert(L"LaunchOnStartup", JsonValue::CreateBooleanValue(m_launchOnStartup));
        root.Insert(L"StartMinimizedToTray", JsonValue::CreateBooleanValue(m_startMinimizedToTray));
        root.Insert(L"TrayLeftClickToggles", JsonValue::CreateBooleanValue(m_trayLeftClickToggles));

        root.Insert(L"SensitiveKeywordFilter", JsonValue::CreateBooleanValue(m_sensitiveKeywordFilter));
        root.Insert(L"StrictBrowserPrivacy", JsonValue::CreateBooleanValue(m_strictBrowserPrivacy));
        root.Insert(L"SuppressBrowserAlbumArt", JsonValue::CreateBooleanValue(m_suppressBrowserAlbumArt));
        root.Insert(L"ThemeMode", JsonValue::CreateStringValue(ToSettingString(m_themeMode)));
        root.Insert(L"BlockedAppSiteTerms", JsonValue::CreateStringValue(m_blockedAppSiteTermsRaw));
        root.Insert(L"MediaActivityType", JsonValue::CreateStringValue(ToSettingStringActivityTypeOverride(m_mediaActivityTypeOverride)));
        root.Insert(L"CreativeActivityType", JsonValue::CreateStringValue(ToSettingStringActivityTypeOverride(m_creativeActivityTypeOverride)));
        root.Insert(L"ProductiveActivityType", JsonValue::CreateStringValue(ToSettingStringActivityTypeOverride(m_productiveActivityTypeOverride)));

        root.Insert(L"ProductiveEnabled", JsonValue::CreateBooleanValue(m_productiveEnabled));
        root.Insert(L"ProductiveDetectionMode", JsonValue::CreateStringValue(ToSettingString(m_productiveDetectionMode)));
        root.Insert(L"ProductiveShowProjectName", JsonValue::CreateBooleanValue(m_productiveShowProjectName));
        root.Insert(L"ProductiveAppWordEnabled", JsonValue::CreateBooleanValue(m_productiveWordEnabled));
        root.Insert(L"ProductiveAppExcelEnabled", JsonValue::CreateBooleanValue(m_productiveExcelEnabled));
        root.Insert(L"ProductiveAppPowerPointEnabled", JsonValue::CreateBooleanValue(m_productivePowerPointEnabled));
        root.Insert(L"ProductiveAppOneNoteEnabled", JsonValue::CreateBooleanValue(m_productiveOneNoteEnabled));
        root.Insert(L"ProductiveAppAccessEnabled", JsonValue::CreateBooleanValue(m_productiveAccessEnabled));
        root.Insert(L"ProductiveAppPublisherEnabled", JsonValue::CreateBooleanValue(m_productivePublisherEnabled));
        root.Insert(L"ProductiveAppVisioEnabled", JsonValue::CreateBooleanValue(m_productiveVisioEnabled));
        root.Insert(L"ProductiveAppProjectEnabled", JsonValue::CreateBooleanValue(m_productiveProjectEnabled));
        root.Insert(L"ProductiveAppCodexEnabled", JsonValue::CreateBooleanValue(m_productiveCodexEnabled));
        root.Insert(L"CreativeEnabled", JsonValue::CreateBooleanValue(m_creativeEnabled));
        root.Insert(L"CreativePriority", JsonValue::CreateStringValue(ToSettingString(m_creativePriority)));
        root.Insert(L"CreativeDetectionMode", JsonValue::CreateStringValue(ToSettingString(m_creativeDetectionMode)));
        root.Insert(L"CreativeShowProjectName", JsonValue::CreateBooleanValue(m_creativeShowProjectName));
        root.Insert(L"CreativeShowWindowTitle", JsonValue::CreateBooleanValue(m_creativeShowWindowTitle));
        root.Insert(L"CreativeAppPhotoshopEnabled", JsonValue::CreateBooleanValue(m_creativePhotoshopEnabled));
        root.Insert(L"CreativeAppIllustratorEnabled", JsonValue::CreateBooleanValue(m_creativeIllustratorEnabled));
        root.Insert(L"CreativeAppPremiereEnabled", JsonValue::CreateBooleanValue(m_creativePremiereEnabled));
        root.Insert(L"CreativeAppAfterEffectsEnabled", JsonValue::CreateBooleanValue(m_creativeAfterEffectsEnabled));
        root.Insert(L"CreativeAppInDesignEnabled", JsonValue::CreateBooleanValue(m_creativeInDesignEnabled));
        root.Insert(L"CreativeAppAuditionEnabled", JsonValue::CreateBooleanValue(m_creativeAuditionEnabled));
        root.Insert(L"CreativeAppMediaEncoderEnabled", JsonValue::CreateBooleanValue(m_creativeMediaEncoderEnabled));
        root.Insert(L"CreativeAppLightroomEnabled", JsonValue::CreateBooleanValue(m_creativeLightroomEnabled));
        root.Insert(L"CreativeAppLightroomClassicEnabled", JsonValue::CreateBooleanValue(m_creativeLightroomClassicEnabled));
        root.Insert(L"CreativeAppInCopyEnabled", JsonValue::CreateBooleanValue(m_creativeInCopyEnabled));
        root.Insert(L"CreativeAppDreamweaverEnabled", JsonValue::CreateBooleanValue(m_creativeDreamweaverEnabled));
        root.Insert(L"CreativeAppAnimateEnabled", JsonValue::CreateBooleanValue(m_creativeAnimateEnabled));
        root.Insert(L"CreativeAppXdEnabled", JsonValue::CreateBooleanValue(m_creativeXdEnabled));
        root.Insert(L"CreativeAppBridgeEnabled", JsonValue::CreateBooleanValue(m_creativeBridgeEnabled));
        root.Insert(L"CreativeAppCharacterAnimatorEnabled", JsonValue::CreateBooleanValue(m_creativeCharacterAnimatorEnabled));
        root.Insert(L"CreativeAppFrescoEnabled", JsonValue::CreateBooleanValue(m_creativeFrescoEnabled));
        root.Insert(L"CreativeAppDimensionEnabled", JsonValue::CreateBooleanValue(m_creativeDimensionEnabled));
        root.Insert(L"CreativeAppSubstanceEnabled", JsonValue::CreateBooleanValue(m_creativeSubstanceEnabled));
        root.Insert(L"CreativeAppAcrobatEnabled", JsonValue::CreateBooleanValue(m_creativeAcrobatEnabled));
        root.Insert(L"CreativeAppOtherAdobeEnabled", JsonValue::CreateBooleanValue(m_creativeOtherAdobeEnabled));
        root.Insert(L"CreativePrivacyMode", JsonValue::CreateStringValue(ToSettingString(m_creativePrivacyMode)));
        root.Insert(L"CreativeIdleBehavior", JsonValue::CreateStringValue(ToSettingString(m_creativeIdleBehavior)));

        auto json = root.Stringify();
        return std::wstring(json.c_str());
    }

    void MainWindow::UpdateThumbnail(const MediaInfo& info)
    {
        // Cancel any pending thumbnail update
        if (m_thumbnailUpdateTask)
        {
            m_thumbnailUpdateTask.Cancel();
            m_thumbnailUpdateTask = nullptr;
        }

        if (!info.thumbnail)
        {
            AlbumThumbnail().Source(nullptr);
            HomeMiniThumbnail().Source(nullptr);
            return;
        }

        auto updateTask = [](auto strongThis, auto thumbnailRef) -> winrt::Windows::Foundation::IAsyncAction
        {
            auto cancellation = co_await winrt::get_cancellation_token();
            cancellation.enable_propagation();

            if (!thumbnailRef)
            {
                co_return;
            }

            try
            {
                // Check if we're shutting down before starting
                if (strongThis->m_isShuttingDown)
                {
                    co_return;
                }

                auto stream = co_await thumbnailRef.OpenReadAsync();

                // Check again after the first await
                if (strongThis->m_isShuttingDown)
                {
                    co_return;
                }

                auto bitmapImage = BitmapImage();
                co_await bitmapImage.SetSourceAsync(stream);

                // Final check before updating UI
                if (!strongThis->m_isShuttingDown)
                {
                    strongThis->AlbumThumbnail().Source(bitmapImage);
                    strongThis->HomeMiniThumbnail().Source(bitmapImage);
                }
            }
            catch (winrt::hresult_canceled const&)
            {
                // Cancellation is expected, just clear the thumbnails
                if (!strongThis->m_isShuttingDown)
                {
                    strongThis->AlbumThumbnail().Source(nullptr);
                    strongThis->HomeMiniThumbnail().Source(nullptr);
                }
            }
            catch (...)
            {
                // On any other error, clear the thumbnails
                if (!strongThis->m_isShuttingDown)
                {
                    strongThis->AlbumThumbnail().Source(nullptr);
                    strongThis->HomeMiniThumbnail().Source(nullptr);
                }
            }
        };

        m_thumbnailUpdateTask = updateTask(get_strong(), info.thumbnail);
    }

    void MainWindow::SetLivePulseActive(bool active)
    {
        if (m_isShuttingDown)
            active = false;

        auto motionEnabled = IsMotionEnabled();

        if (!active)
        {
            if (m_livePulseStoryboard)
            {
                try { m_livePulseStoryboard.Stop(); } catch (...) {}
                m_livePulseStoryboard = nullptr;
            }

            TransitionElementVisibility(StatusLiveBadge(), false, 4.0);
            StatusLiveDot().Opacity(1.0);
            return;
        }

        TransitionElementVisibility(StatusLiveBadge(), true, 4.0);

        if (!motionEnabled)
        {
            StatusLiveDot().Opacity(1.0);
            return;
        }

        if (m_livePulseStoryboard)
            return;

        auto pulse = Microsoft::UI::Xaml::Media::Animation::DoubleAnimation();
        pulse.From(1.0);
        pulse.To(0.62);
        pulse.AutoReverse(true);
        pulse.Duration(MotionDuration(MotionTokens::LivePulseMs));
        pulse.EasingFunction(MotionEaseInOutSine());
        pulse.RepeatBehavior(Microsoft::UI::Xaml::Media::Animation::RepeatBehaviorHelper::Forever());

        auto storyboard = Microsoft::UI::Xaml::Media::Animation::Storyboard();
        storyboard.Children().Append(pulse);
        Microsoft::UI::Xaml::Media::Animation::Storyboard::SetTarget(pulse, StatusLiveDot());
        Microsoft::UI::Xaml::Media::Animation::Storyboard::SetTargetProperty(pulse, L"Opacity");

        m_livePulseStoryboard = storyboard;
        storyboard.Begin();
    }

    void MainWindow::ShowTrackTransitionSkeleton()
    {
        if (!IsMotionEnabled())
        {
            HideTrackTransitionSkeleton();
            return;
        }

        auto overlay = HomeTrackSkeletonOverlay();
        overlay.Visibility(Visibility::Visible);
        overlay.Opacity(1.0);

        if (!m_trackSkeletonStoryboard)
        {
            auto pulse = Microsoft::UI::Xaml::Media::Animation::DoubleAnimation();
            pulse.From(0.78);
            pulse.To(1.0);
            pulse.AutoReverse(true);
            pulse.Duration(MotionDuration(MotionTokens::SkeletonPulseMs));
            pulse.EasingFunction(MotionEaseInOutSine());
            pulse.RepeatBehavior(Microsoft::UI::Xaml::Media::Animation::RepeatBehaviorHelper::Forever());

            auto storyboard = Microsoft::UI::Xaml::Media::Animation::Storyboard();
            storyboard.Children().Append(pulse);
            Microsoft::UI::Xaml::Media::Animation::Storyboard::SetTarget(pulse, overlay);
            Microsoft::UI::Xaml::Media::Animation::Storyboard::SetTargetProperty(pulse, L"Opacity");

            m_trackSkeletonStoryboard = storyboard;
            storyboard.Begin();
        }

        if (!m_trackSkeletonTimer)
        {
            m_trackSkeletonTimer = DispatcherTimer();
            m_trackSkeletonTimer.Tick([this](IInspectable const&, IInspectable const&)
            {
                if (m_trackSkeletonTimer)
                    m_trackSkeletonTimer.Stop();
                HideTrackTransitionSkeleton();
            });
        }

        m_trackSkeletonTimer.Interval(std::chrono::milliseconds(MotionTokens::SkeletonDwellMs));
        m_trackSkeletonTimer.Start();
    }

    void MainWindow::HideTrackTransitionSkeleton()
    {
        if (m_trackSkeletonTimer)
            m_trackSkeletonTimer.Stop();

        if (m_trackSkeletonStoryboard)
        {
            try { m_trackSkeletonStoryboard.Stop(); } catch (...) {}
            m_trackSkeletonStoryboard = nullptr;
        }

        auto overlay = HomeTrackSkeletonOverlay();
        overlay.Visibility(Visibility::Collapsed);
        overlay.Opacity(1.0);
    }

    // =====================================================================
    // Connection status
    // =====================================================================

    void MainWindow::UpdateConnectionStatus()
    {
        bool connected = m_presence->IsConnected();

        bool bridgeStarted = false;
        bool bridgeBinding = false;
        bool extensionConnected = false;
        bool hasHint = false;
        int64_t hintAgeSeconds = -1;
        {
            auto& state = GetHintServerState();
            std::lock_guard<std::mutex> lock(state.mutex);
            bridgeStarted = state.started;
            bridgeBinding = state.binding;
            extensionConnected = IsExtensionConnected(state);
            hasHint = state.hasHint;

            if (hasHint)
            {
                hintAgeSeconds = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - state.hint.updatedAt).count();
                if (hintAgeSeconds < 0)
                    hintAgeSeconds = 0;
            }
        }

        if (!m_enabled)
        {
            StatusIndicator().Fill(Application::Current().Resources().Lookup(box_value(L"StatusDisconnectedBrush")).as<Brush>());
            StatusText().Text(L"Disabled");
            StatusSubtext().Text(L"Rich Presence is off");
            HomeHealthText().Text(L"Discord: OFF | Bridge: OFF | Hint age: --");
            SetLivePulseActive(false);
            return;
        }

        if (connected)
        {
            StatusIndicator().Fill(Application::Current().Resources().Lookup(box_value(L"StatusConnectedBrush")).as<Brush>());
            StatusText().Text(L"Connected to Discord");
            StatusSubtext().Text(L"Rich Presence is active");
        }
        else
        {
            StatusIndicator().Fill(Application::Current().Resources().Lookup(box_value(L"StatusConnectingBrush")).as<Brush>());
            StatusText().Text(L"Connecting to Discord...");
            StatusSubtext().Text(L"Waiting for Discord client");
        }

        if (extensionConnected)
        {
            HomeExtensionIndicator().Fill(Application::Current().Resources().Lookup(box_value(L"StatusConnectedBrush")).as<Brush>());
            HomeExtensionText().Text(L"Browser extension");
            HomeExtensionSubtext().Text(hasHint
                ? L"Receiving hints"
                : L"Connected and ready");
        }
        else if (bridgeStarted || bridgeBinding)
        {
            HomeExtensionIndicator().Fill(Application::Current().Resources().Lookup(box_value(L"StatusConnectingBrush")).as<Brush>());
            HomeExtensionText().Text(L"Browser bridge waiting");
            HomeExtensionSubtext().Text(L"Open a supported browser tab");
        }
        else
        {
            HomeExtensionIndicator().Fill(Application::Current().Resources().Lookup(box_value(L"StatusDisconnectedBrush")).as<Brush>());
            HomeExtensionText().Text(L"Browser bridge offline");
            HomeExtensionSubtext().Text(L"Restart app to reinitialize bridge");
        }

        auto discordHealth = connected ? L"OK" : L"WAIT";
        auto bridgeHealth = extensionConnected ? L"OK" : ((bridgeStarted || bridgeBinding) ? L"INIT" : L"DOWN");
        auto hintAgeText = (hasHint && hintAgeSeconds >= 0)
            ? (std::to_wstring(hintAgeSeconds) + L"s")
            : std::wstring(L"--");

        HomeHealthText().Text(
            std::wstring(L"Discord: ") + discordHealth +
            L" | Bridge: " + bridgeHealth +
            L" | Hint age: " + hintAgeText);

        bool liveActive = false;
        if (connected && m_enabled)
        {
            const bool activePlayback = !m_lastMedia.title.empty() && m_lastMedia.isPlaying;
            const bool recentPresencePush = m_lastPresencePushPlaying &&
                ((std::chrono::steady_clock::now() - m_lastPresencePushAt) <= std::chrono::seconds(4));
            liveActive = activePlayback || recentPresencePush;
        }
        SetLivePulseActive(liveActive);

        if (connected != m_wasConnected)
        {
            ShowConnectionInfoBar(connected);
            m_wasConnected = connected;
        }
    }

    void MainWindow::ShowConnectionInfoBar(bool connected)
    {
        if (m_connectionInfoBarFadeStoryboard)
        {
            try { m_connectionInfoBarFadeStoryboard.Stop(); } catch (...) {}
            m_connectionInfoBarFadeStoryboard = nullptr;
        }

        auto infoBar = ConnectionInfoBar();
        infoBar.Opacity(1.0);

        if (connected)
        {
            infoBar.Severity(InfoBarSeverity::Success);
            infoBar.Title(L"Connected");
            infoBar.Message(L"Discord Rich Presence is now active.");
            AppendDiagnosticLog(L"INFO", L"discord", L"Connected");
        }
        else
        {
            infoBar.Severity(InfoBarSeverity::Warning);
            infoBar.Title(L"Disconnected");
            infoBar.Message(L"Lost connection to Discord. Reconnecting...");
            AppendDiagnosticLog(L"WARN", L"discord", L"Disconnected, reconnecting");
        }

        infoBar.IsOpen(true);
        ++m_connectionInfoBarVersion;

        if (!m_connectionInfoBarTimer)
        {
            m_connectionInfoBarTimer = DispatcherTimer();
            m_connectionInfoBarTimer.Tick([this](IInspectable const&, IInspectable const&)
            {
                if (m_connectionInfoBarTimer)
                    m_connectionInfoBarTimer.Stop();

                if (m_isShuttingDown)
                    return;

                auto currentBar = ConnectionInfoBar();
                if (!currentBar.IsOpen())
                    return;

                if (!IsMotionEnabled())
                {
                    currentBar.IsOpen(false);
                    currentBar.Opacity(1.0);
                    return;
                }

                const auto version = m_connectionInfoBarVersion;

                auto fadeAnimation = Microsoft::UI::Xaml::Media::Animation::DoubleAnimation();
                fadeAnimation.From(1.0);
                fadeAnimation.To(0.0);
                fadeAnimation.Duration(MotionDuration(MotionTokens::InfoBarFadeMs));
                fadeAnimation.EasingFunction(MotionEaseIn());

                auto storyboard = Microsoft::UI::Xaml::Media::Animation::Storyboard();
                storyboard.Children().Append(fadeAnimation);
                Microsoft::UI::Xaml::Media::Animation::Storyboard::SetTarget(fadeAnimation, currentBar);
                Microsoft::UI::Xaml::Media::Animation::Storyboard::SetTargetProperty(fadeAnimation, L"Opacity");

                auto weak = get_weak();
                storyboard.Completed([weak, version](IInspectable const&, IInspectable const&)
                {
                    auto strong = weak.get();
                    if (!strong) return;
                    if (version != strong->m_connectionInfoBarVersion) return;

                    auto infoBarRef = strong->ConnectionInfoBar();
                    infoBarRef.IsOpen(false);
                    infoBarRef.Opacity(1.0);
                    strong->m_connectionInfoBarFadeStoryboard = nullptr;
                });

                m_connectionInfoBarFadeStoryboard = storyboard;
                storyboard.Begin();
            });
        }

        m_connectionInfoBarTimer.Interval(std::chrono::seconds(4));
        m_connectionInfoBarTimer.Start();
    }

    void MainWindow::SetSongProgress(double progressPercent)
    {
        if (progressPercent < 0.0)
            progressPercent = 0.0;
        if (progressPercent > 100.0)
            progressPercent = 100.0;

        auto current = SongProgressBar().Value();
        auto delta = std::abs(progressPercent - current);
        bool canInterpolate =
            IsMotionEnabled() &&
            m_lastMedia.isPlaying &&
            !m_lastMedia.title.empty() &&
            progressPercent > 0.0 &&
            delta > 0.02 &&
            delta <= 16.0;

        if (!canInterpolate)
        {
            if (m_songProgressStoryboard)
            {
                try { m_songProgressStoryboard.Stop(); } catch (...) {}
                m_songProgressStoryboard = nullptr;
            }

            SongProgressBar().Value(progressPercent);
            UpdateSongWaveClip(progressPercent);
            return;
        }

        if (m_songProgressStoryboard)
        {
            try { m_songProgressStoryboard.Stop(); } catch (...) {}
            m_songProgressStoryboard = nullptr;
        }

        auto progressAnimation = Microsoft::UI::Xaml::Media::Animation::DoubleAnimation();
        progressAnimation.From(current);
        progressAnimation.To(progressPercent);
        progressAnimation.Duration(MotionDuration(MotionTokens::ProgressStepMs));
        progressAnimation.EnableDependentAnimation(true);

        auto storyboard = Microsoft::UI::Xaml::Media::Animation::Storyboard();
        storyboard.Children().Append(progressAnimation);
        Microsoft::UI::Xaml::Media::Animation::Storyboard::SetTarget(progressAnimation, SongProgressBar());
        Microsoft::UI::Xaml::Media::Animation::Storyboard::SetTargetProperty(progressAnimation, L"Value");

        auto weak = get_weak();
        storyboard.Completed([weak](IInspectable const&, IInspectable const&)
        {
            if (auto strong = weak.get())
                strong->m_songProgressStoryboard = nullptr;
        });

        m_songProgressStoryboard = storyboard;
        storyboard.Begin();
    }

    void MainWindow::UpdateSongWaveClip(double progressPercent)
    {
        auto width = SongWaveViewport().ActualWidth();
        auto height = SongWaveViewport().ActualHeight();

        if (width <= 0.0)
        {
            SongWaveFillClip().Rect(Rect{ 0.0f, 0.0f, 0.0f, static_cast<float>(height > 0.0 ? height : 12.0) });
            return;
        }

        if (height <= 0.0)
            height = 12.0;

        auto clamped = progressPercent;
        if (clamped < 0.0)
            clamped = 0.0;
        if (clamped > 100.0)
            clamped = 100.0;

        auto fillWidth = width * (clamped / 100.0);
        if (fillWidth < 0.0)
            fillWidth = 0.0;
        if (fillWidth > width)
            fillWidth = width;

        SongWaveFillClip().Rect(Rect{
            0.0f,
            0.0f,
            static_cast<float>(fillWidth),
            static_cast<float>(height)
            });
    }

    void MainWindow::SetSongWaveActive(bool active)
    {
        if (m_isShuttingDown || !IsMotionEnabled())
            active = false;

        if (!active)
        {
            if (m_songWaveStoryboard)
            {
                try { m_songWaveStoryboard.Stop(); } catch (...) {}
                m_songWaveStoryboard = nullptr;
            }

            SongWaveFillTransform().X(0.0);
            SongWaveFillPath().Opacity(0.95);
            return;
        }

        if (m_songWaveStoryboard)
            return;

        auto shift = Microsoft::UI::Xaml::Media::Animation::DoubleAnimation();
        shift.From(0.0);
        shift.To(-10.0);
        shift.AutoReverse(true);
        shift.Duration(MotionDuration(MotionTokens::WaveShiftMs));
        shift.EasingFunction(MotionEaseInOutSine());
        shift.RepeatBehavior(Microsoft::UI::Xaml::Media::Animation::RepeatBehaviorHelper::Forever());

        auto glow = Microsoft::UI::Xaml::Media::Animation::DoubleAnimation();
        glow.From(0.70);
        glow.To(1.0);
        glow.AutoReverse(true);
        glow.Duration(MotionDuration(MotionTokens::WaveGlowMs));
        glow.EasingFunction(MotionEaseInOutSine());
        glow.RepeatBehavior(Microsoft::UI::Xaml::Media::Animation::RepeatBehaviorHelper::Forever());

        auto storyboard = Microsoft::UI::Xaml::Media::Animation::Storyboard();
        storyboard.Children().Append(shift);
        storyboard.Children().Append(glow);
        Microsoft::UI::Xaml::Media::Animation::Storyboard::SetTarget(shift, SongWaveFillTransform());
        Microsoft::UI::Xaml::Media::Animation::Storyboard::SetTargetProperty(shift, L"X");
        Microsoft::UI::Xaml::Media::Animation::Storyboard::SetTarget(glow, SongWaveFillPath());
        Microsoft::UI::Xaml::Media::Animation::Storyboard::SetTargetProperty(glow, L"Opacity");

        m_songWaveStoryboard = storyboard;
        storyboard.Begin();
    }

    void MainWindow::SetHomeMiniProgress(double progressPercent)
    {
        if (progressPercent < 0.0)
            progressPercent = 0.0;
        if (progressPercent > 100.0)
            progressPercent = 100.0;

        auto current = HomeMiniProgressBar().Value();
        auto delta = std::abs(progressPercent - current);
        bool canInterpolate =
            IsMotionEnabled() &&
            m_lastMedia.isPlaying &&
            !m_lastMedia.title.empty() &&
            progressPercent > 0.0 &&
            delta > 0.02 &&
            delta <= 16.0;

        if (!canInterpolate)
        {
            if (m_homeMiniProgressStoryboard)
            {
                try { m_homeMiniProgressStoryboard.Stop(); } catch (...) {}
                m_homeMiniProgressStoryboard = nullptr;
            }

            HomeMiniProgressBar().Value(progressPercent);
            UpdateHomeMiniWaveClip(progressPercent);
            return;
        }

        if (m_homeMiniProgressStoryboard)
        {
            try { m_homeMiniProgressStoryboard.Stop(); } catch (...) {}
            m_homeMiniProgressStoryboard = nullptr;
        }

        auto progressAnimation = Microsoft::UI::Xaml::Media::Animation::DoubleAnimation();
        progressAnimation.From(current);
        progressAnimation.To(progressPercent);
        progressAnimation.Duration(MotionDuration(MotionTokens::ProgressStepMs));
        progressAnimation.EnableDependentAnimation(true);

        auto storyboard = Microsoft::UI::Xaml::Media::Animation::Storyboard();
        storyboard.Children().Append(progressAnimation);
        Microsoft::UI::Xaml::Media::Animation::Storyboard::SetTarget(progressAnimation, HomeMiniProgressBar());
        Microsoft::UI::Xaml::Media::Animation::Storyboard::SetTargetProperty(progressAnimation, L"Value");

        auto weak = get_weak();
        storyboard.Completed([weak](IInspectable const&, IInspectable const&)
        {
            if (auto strong = weak.get())
                strong->m_homeMiniProgressStoryboard = nullptr;
        });

        m_homeMiniProgressStoryboard = storyboard;
        storyboard.Begin();
    }

    void MainWindow::UpdateHomeMiniWaveClip(double progressPercent)
    {
        auto width = HomeMiniWaveViewport().ActualWidth();
        auto height = HomeMiniWaveViewport().ActualHeight();

        if (width <= 0.0)
        {
            HomeMiniWaveFillClip().Rect(Rect{ 0.0f, 0.0f, 0.0f, static_cast<float>(height > 0.0 ? height : 8.0) });
            return;
        }

        if (height <= 0.0)
            height = 8.0;

        auto clamped = progressPercent;
        if (clamped < 0.0)
            clamped = 0.0;
        if (clamped > 100.0)
            clamped = 100.0;

        auto fillWidth = width * (clamped / 100.0);
        if (fillWidth < 0.0)
            fillWidth = 0.0;
        if (fillWidth > width)
            fillWidth = width;

        HomeMiniWaveFillClip().Rect(Rect{
            0.0f,
            0.0f,
            static_cast<float>(fillWidth),
            static_cast<float>(height)
            });
    }

    void MainWindow::SetHomeMiniWaveActive(bool active)
    {
        if (m_isShuttingDown || !IsMotionEnabled())
            active = false;

        if (!active)
        {
            if (m_homeMiniWaveStoryboard)
            {
                try { m_homeMiniWaveStoryboard.Stop(); } catch (...) {}
                m_homeMiniWaveStoryboard = nullptr;
            }

            HomeMiniWaveFillTransform().X(0.0);
            HomeMiniWaveFillPath().Opacity(0.95);
            return;
        }

        if (m_homeMiniWaveStoryboard)
            return;

        auto shift = Microsoft::UI::Xaml::Media::Animation::DoubleAnimation();
        shift.From(0.0);
        shift.To(-8.0);
        shift.AutoReverse(true);
        shift.Duration(MotionDuration(MotionTokens::HomeWaveShiftMs));
        shift.EasingFunction(MotionEaseInOutSine());
        shift.RepeatBehavior(Microsoft::UI::Xaml::Media::Animation::RepeatBehaviorHelper::Forever());

        auto glow = Microsoft::UI::Xaml::Media::Animation::DoubleAnimation();
        glow.From(0.74);
        glow.To(1.0);
        glow.AutoReverse(true);
        glow.Duration(MotionDuration(MotionTokens::HomeWaveGlowMs));
        glow.EasingFunction(MotionEaseInOutSine());
        glow.RepeatBehavior(Microsoft::UI::Xaml::Media::Animation::RepeatBehaviorHelper::Forever());

        auto storyboard = Microsoft::UI::Xaml::Media::Animation::Storyboard();
        storyboard.Children().Append(shift);
        storyboard.Children().Append(glow);
        Microsoft::UI::Xaml::Media::Animation::Storyboard::SetTarget(shift, HomeMiniWaveFillTransform());
        Microsoft::UI::Xaml::Media::Animation::Storyboard::SetTargetProperty(shift, L"X");
        Microsoft::UI::Xaml::Media::Animation::Storyboard::SetTarget(glow, HomeMiniWaveFillPath());
        Microsoft::UI::Xaml::Media::Animation::Storyboard::SetTargetProperty(glow, L"Opacity");

        m_homeMiniWaveStoryboard = storyboard;
        storyboard.Begin();
    }

    // =====================================================================
    // Progress timer
    // =====================================================================

    void MainWindow::StartProgressTimer()
    {
        m_progressTimer = DispatcherTimer();
        m_progressTimer.Interval(std::chrono::seconds(1));
        m_progressTimer.Tick([this](IInspectable const&, IInspectable const&)
        {
            if (m_isShuttingDown) return;
            RefreshCreativePreviewFromCurrentState();
            if (!m_enabled) return;

            auto info = m_lastMedia;
            auto shouldAnimateWave = !info.title.empty() && info.isPlaying && IsMotionEnabled();
            SetSongWaveActive(shouldAnimateWave);
            SetHomeMiniWaveActive(shouldAnimateWave);

            if (!info.title.empty())
            {
                int liveHintPos = 0;
                int liveHintDur = 0;
                if (TryGetLiveHintTimeline(info, liveHintPos, liveHintDur))
                {
                    auto displayPos = liveHintPos;
                    auto durSec = liveHintDur;

                    PositionText().Text(FormatTime(displayPos));
                    DurationText().Text(FormatTime(durSec));

                    if (durSec > 0)
                    {
                        double progress = (static_cast<double>(displayPos) / durSec) * 100.0;
                        if (progress < 0.0) progress = 0.0;
                        if (progress > 100.0) progress = 100.0;
                        SetSongProgress(progress);
                        SetHomeMiniProgress(progress);
                    }
                    else
                    {
                        SetSongProgress(0);
                        SetHomeMiniProgress(0);
                    }

                    HomeMiniTimer().Text(FormatTime(displayPos) + L" / " + FormatTime(durSec > 0 ? durSec : 0));
                    UpdateConnectionStatus();
                    return;
                }

                auto durSec = static_cast<int>(info.duration.count());
                auto displayPos = static_cast<int>(info.position.count());

                if (info.isPlaying)
                {
                    auto now = std::chrono::system_clock::now();
                    if (info.startTime.time_since_epoch().count() > 0)
                    {
                        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                            now - info.startTime);
                        auto estimatedPos = elapsed.count();

                        if (estimatedPos < 0)
                            estimatedPos = 0;

                        if (durSec > 0)
                        {
                            if (estimatedPos > durSec) estimatedPos = durSec;
                            double progress = (static_cast<double>(estimatedPos) / durSec) * 100.0;
                            SetSongProgress(progress);
                            SetHomeMiniProgress(progress);
                        }
                        else
                        {
                            SetSongProgress(0);
                            SetHomeMiniProgress(0);
                        }

                        displayPos = static_cast<int>(estimatedPos);
                        PositionText().Text(FormatTime(displayPos));
                    }
                    else
                    {
                        auto posSec = static_cast<int>(info.position.count());
                        displayPos = posSec;
                        PositionText().Text(FormatTime(posSec));

                        if (durSec > 0)
                        {
                            double progress = (static_cast<double>(posSec) / durSec) * 100.0;
                            if (progress < 0.0) progress = 0.0;
                            if (progress > 100.0) progress = 100.0;
                            SetSongProgress(progress);
                            SetHomeMiniProgress(progress);
                        }
                        else
                        {
                            SetSongProgress(0);
                            SetHomeMiniProgress(0);
                        }
                    }
                }
                else
                {
                    auto posSec = static_cast<int>(info.position.count());
                    PositionText().Text(FormatTime(posSec));
                    displayPos = posSec;

                    if (durSec > 0)
                    {
                        double progress = (static_cast<double>(posSec) / durSec) * 100.0;
                        if (progress < 0.0) progress = 0.0;
                        if (progress > 100.0) progress = 100.0;
                        SetSongProgress(progress);
                        SetHomeMiniProgress(progress);
                    }
                    else
                    {
                        SetSongProgress(0);
                        SetHomeMiniProgress(0);
                    }
                }

                if (displayPos < 0) displayPos = 0;
                if (durSec > 0 && displayPos > durSec) displayPos = durSec;
                HomeMiniTimer().Text(FormatTime(displayPos) + L" / " + FormatTime(durSec > 0 ? durSec : 0));
            }

            UpdateConnectionStatus();
        });
        m_progressTimer.Start();
    }

    void MainWindow::StopProgressTimer()
    {
        if (m_progressTimer)
        {
            m_progressTimer.Stop();
            m_progressTimer = nullptr;
        }
    }

    // =====================================================================
    // Settings persistence
    // =====================================================================

    lrp::settings::PersistedSettings MainWindow::BuildPersistedSettingsSnapshot()
    {
        lrp::settings::PersistedSettings settings;
        settings.media.showTimestamps = TimestampToggle().IsOn();
        settings.media.showSource = SourceToggle().IsOn();
        settings.media.sourceDebugMode = m_sourceDebugMode;
        settings.media.showPaused = PausedToggle().IsOn();
        settings.media.showAlbumArt = AlbumArtToggle().IsOn();
        settings.media.showDefaultIdleStatus = m_showDefaultIdleStatus;
        settings.media.sensitiveKeywordFilter = m_sensitiveKeywordFilter;
        settings.media.strictBrowserPrivacy = m_strictBrowserPrivacy;
        settings.media.suppressBrowserAlbumArt = m_suppressBrowserAlbumArt;
        settings.media.blockedAppSiteTermsRaw = m_blockedAppSiteTermsRaw;
        settings.media.activityTypeOverride = m_mediaActivityTypeOverride;

        settings.behavior.richPresenceEnabled = m_enabled;
        settings.behavior.closeToTrayOnClose = m_closeToTrayOnClose;
        settings.behavior.launchOnStartup = m_launchOnStartup;
        settings.behavior.startMinimizedToTray = m_startMinimizedToTray;
        settings.behavior.trayLeftClickToggles = m_trayLeftClickToggles;
        settings.behavior.themeMode = m_themeMode;

        settings.productive.enabled = m_productiveEnabled;
        settings.productive.detectionMode = m_productiveDetectionMode;
        settings.productive.showProjectName = m_productiveShowProjectName;
        settings.productive.activityTypeOverride = m_productiveActivityTypeOverride;
        settings.productive.wordEnabled = m_productiveWordEnabled;
        settings.productive.excelEnabled = m_productiveExcelEnabled;
        settings.productive.powerPointEnabled = m_productivePowerPointEnabled;
        settings.productive.oneNoteEnabled = m_productiveOneNoteEnabled;
        settings.productive.accessEnabled = m_productiveAccessEnabled;
        settings.productive.publisherEnabled = m_productivePublisherEnabled;
        settings.productive.visioEnabled = m_productiveVisioEnabled;
        settings.productive.projectEnabled = m_productiveProjectEnabled;
        settings.productive.codexEnabled = m_productiveCodexEnabled;

        settings.creative.enabled = m_creativeEnabled;
        settings.creative.priority = m_creativePriority;
        settings.creative.detectionMode = m_creativeDetectionMode;
        settings.creative.showProjectName = m_creativeShowProjectName;
        settings.creative.showWindowTitle = m_creativeShowWindowTitle;
        settings.creative.activityTypeOverride = m_creativeActivityTypeOverride;
        settings.creative.photoshopEnabled = m_creativePhotoshopEnabled;
        settings.creative.illustratorEnabled = m_creativeIllustratorEnabled;
        settings.creative.premiereEnabled = m_creativePremiereEnabled;
        settings.creative.afterEffectsEnabled = m_creativeAfterEffectsEnabled;
        settings.creative.inDesignEnabled = m_creativeInDesignEnabled;
        settings.creative.auditionEnabled = m_creativeAuditionEnabled;
        settings.creative.mediaEncoderEnabled = m_creativeMediaEncoderEnabled;
        settings.creative.lightroomEnabled = m_creativeLightroomEnabled;
        settings.creative.lightroomClassicEnabled = m_creativeLightroomClassicEnabled;
        settings.creative.inCopyEnabled = m_creativeInCopyEnabled;
        settings.creative.dreamweaverEnabled = m_creativeDreamweaverEnabled;
        settings.creative.animateEnabled = m_creativeAnimateEnabled;
        settings.creative.xdEnabled = m_creativeXdEnabled;
        settings.creative.bridgeEnabled = m_creativeBridgeEnabled;
        settings.creative.characterAnimatorEnabled = m_creativeCharacterAnimatorEnabled;
        settings.creative.frescoEnabled = m_creativeFrescoEnabled;
        settings.creative.dimensionEnabled = m_creativeDimensionEnabled;
        settings.creative.substanceEnabled = m_creativeSubstanceEnabled;
        settings.creative.acrobatEnabled = m_creativeAcrobatEnabled;
        settings.creative.otherAdobeEnabled = m_creativeOtherAdobeEnabled;
        settings.creative.privacyMode = m_creativePrivacyMode;
        settings.creative.idleBehavior = m_creativeIdleBehavior;
        return settings;
    }

    void MainWindow::ApplyPersistedSettingsSnapshot(const lrp::settings::PersistedSettings& settings)
    {
        m_enabled = settings.behavior.richPresenceEnabled;
        EnableToggle().IsOn(m_enabled);

        TimestampToggle().IsOn(settings.media.showTimestamps);
        m_presence->SetShowTimestamps(settings.media.showTimestamps);

        SourceToggle().IsOn(settings.media.showSource);
        m_presence->SetShowSource(settings.media.showSource);

        m_sourceDebugMode = settings.media.sourceDebugMode;
        SourceDebugToggle().IsOn(m_sourceDebugMode);

        PausedToggle().IsOn(settings.media.showPaused);
        m_presence->SetShowPaused(settings.media.showPaused);

        AlbumArtToggle().IsOn(settings.media.showAlbumArt);
        m_presence->SetShowAlbumArt(settings.media.showAlbumArt);

        m_showDefaultIdleStatus = settings.media.showDefaultIdleStatus;
        DefaultIdleStatusToggle().IsOn(m_showDefaultIdleStatus);
        m_presence->SetShowIdleStatus(m_showDefaultIdleStatus);

        m_closeToTrayOnClose = settings.behavior.closeToTrayOnClose;
        CloseToTrayToggle().IsOn(m_closeToTrayOnClose);

        m_launchOnStartup = settings.behavior.launchOnStartup;
        LaunchOnStartupToggle().IsOn(m_launchOnStartup);

        m_startMinimizedToTray = settings.behavior.startMinimizedToTray;
        StartMinimizedToggle().IsOn(m_startMinimizedToTray);

        m_trayLeftClickToggles = settings.behavior.trayLeftClickToggles;
        TrayLeftClickToggle().IsOn(m_trayLeftClickToggles);

        m_sensitiveKeywordFilter = settings.media.sensitiveKeywordFilter;
        SensitiveFilterToggle().IsOn(m_sensitiveKeywordFilter);
        m_presence->SetSensitiveKeywordFilter(m_sensitiveKeywordFilter);

        m_strictBrowserPrivacy = settings.media.strictBrowserPrivacy;
        StrictBrowserPrivacyToggle().IsOn(m_strictBrowserPrivacy);
        m_presence->SetStrictBrowserPrivacy(m_strictBrowserPrivacy);

        m_suppressBrowserAlbumArt = settings.media.suppressBrowserAlbumArt;
        SuppressBrowserArtToggle().IsOn(m_suppressBrowserAlbumArt);
        m_presence->SetSuppressBrowserAlbumArt(m_suppressBrowserAlbumArt);

        m_themeMode = settings.behavior.themeMode;
        ThemeModeCombo().SelectedIndex(ThemeModeToComboIndex(m_themeMode));
        ApplyThemeMode();

        m_blockedAppSiteTermsRaw = settings.media.blockedAppSiteTermsRaw;
        BlockedAppSitesBox().Text(m_blockedAppSiteTermsRaw);
        m_presence->SetBlockedAppSiteTerms(ParseBlockedTerms(m_blockedAppSiteTermsRaw));

        m_mediaActivityTypeOverride = settings.media.activityTypeOverride;
        m_creativeActivityTypeOverride = settings.creative.activityTypeOverride;
        m_productiveActivityTypeOverride = settings.productive.activityTypeOverride;
        MediaActivityTypeCombo().SelectedIndex(ActivityTypeOverrideToComboIndex(m_mediaActivityTypeOverride));
        CreativeActivityTypeCombo().SelectedIndex(ActivityTypeOverrideToComboIndex(m_creativeActivityTypeOverride));
        ProductiveActivityTypeCombo().SelectedIndex(ActivityTypeOverrideToComboIndex(m_productiveActivityTypeOverride));
        ApplyActivityTypeOverrides();

        m_productiveEnabled = settings.productive.enabled;
        ProductiveEnableToggle().IsOn(m_productiveEnabled);
        m_productiveDetectionMode = settings.productive.detectionMode;
        ProductiveDetectionModeCombo().SelectedIndex(ProductiveDetectionModeToComboIndex(m_productiveDetectionMode));
        m_productiveShowProjectName = settings.productive.showProjectName;
        ProductiveShowProjectToggle().IsOn(m_productiveShowProjectName);
        m_productiveWordEnabled = settings.productive.wordEnabled;
        ProductiveAppWordCheck().IsChecked(settings.productive.wordEnabled);
        m_productiveExcelEnabled = settings.productive.excelEnabled;
        ProductiveAppExcelCheck().IsChecked(settings.productive.excelEnabled);
        m_productivePowerPointEnabled = settings.productive.powerPointEnabled;
        ProductiveAppPowerPointCheck().IsChecked(settings.productive.powerPointEnabled);
        m_productiveOneNoteEnabled = settings.productive.oneNoteEnabled;
        ProductiveAppOneNoteCheck().IsChecked(settings.productive.oneNoteEnabled);
        m_productiveAccessEnabled = settings.productive.accessEnabled;
        ProductiveAppAccessCheck().IsChecked(settings.productive.accessEnabled);
        m_productivePublisherEnabled = settings.productive.publisherEnabled;
        ProductiveAppPublisherCheck().IsChecked(settings.productive.publisherEnabled);
        m_productiveVisioEnabled = settings.productive.visioEnabled;
        ProductiveAppVisioCheck().IsChecked(settings.productive.visioEnabled);
        m_productiveProjectEnabled = settings.productive.projectEnabled;
        ProductiveAppProjectCheck().IsChecked(settings.productive.projectEnabled);
        m_productiveCodexEnabled = settings.productive.codexEnabled;
        ProductiveAppCodexCheck().IsChecked(settings.productive.codexEnabled);

        m_creativeEnabled = settings.creative.enabled;
        CreativeEnableToggle().IsOn(m_creativeEnabled);
        m_creativePriority = settings.creative.priority;
        CreativePriorityCombo().SelectedIndex(CreativePriorityModeToComboIndex(m_creativePriority));
        m_creativeDetectionMode = settings.creative.detectionMode;
        CreativeDetectionModeCombo().SelectedIndex(CreativeDetectionModeToComboIndex(m_creativeDetectionMode));
        m_creativeShowProjectName = settings.creative.showProjectName;
        CreativeShowProjectToggle().IsOn(settings.creative.showProjectName);
        m_creativeShowWindowTitle = settings.creative.showWindowTitle;
        CreativeShowWindowTitleToggle().IsOn(settings.creative.showWindowTitle);
        m_creativePhotoshopEnabled = settings.creative.photoshopEnabled;
        CreativeAppPhotoshopCheck().IsChecked(settings.creative.photoshopEnabled);
        m_creativeIllustratorEnabled = settings.creative.illustratorEnabled;
        CreativeAppIllustratorCheck().IsChecked(settings.creative.illustratorEnabled);
        m_creativePremiereEnabled = settings.creative.premiereEnabled;
        CreativeAppPremiereCheck().IsChecked(settings.creative.premiereEnabled);
        m_creativeAfterEffectsEnabled = settings.creative.afterEffectsEnabled;
        CreativeAppAfterEffectsCheck().IsChecked(settings.creative.afterEffectsEnabled);
        m_creativeInDesignEnabled = settings.creative.inDesignEnabled;
        CreativeAppInDesignCheck().IsChecked(settings.creative.inDesignEnabled);
        m_creativeAuditionEnabled = settings.creative.auditionEnabled;
        CreativeAppAuditionCheck().IsChecked(settings.creative.auditionEnabled);
        m_creativeMediaEncoderEnabled = settings.creative.mediaEncoderEnabled;
        CreativeAppMediaEncoderCheck().IsChecked(settings.creative.mediaEncoderEnabled);
        m_creativeLightroomEnabled = settings.creative.lightroomEnabled;
        CreativeAppLightroomCheck().IsChecked(settings.creative.lightroomEnabled);
        m_creativeLightroomClassicEnabled = settings.creative.lightroomClassicEnabled;
        CreativeAppLightroomClassicCheck().IsChecked(settings.creative.lightroomClassicEnabled);
        m_creativeInCopyEnabled = settings.creative.inCopyEnabled;
        CreativeAppInCopyCheck().IsChecked(settings.creative.inCopyEnabled);
        m_creativeDreamweaverEnabled = settings.creative.dreamweaverEnabled;
        CreativeAppDreamweaverCheck().IsChecked(settings.creative.dreamweaverEnabled);
        m_creativeAnimateEnabled = settings.creative.animateEnabled;
        CreativeAppAnimateCheck().IsChecked(settings.creative.animateEnabled);
        m_creativeXdEnabled = settings.creative.xdEnabled;
        CreativeAppXdCheck().IsChecked(settings.creative.xdEnabled);
        m_creativeBridgeEnabled = settings.creative.bridgeEnabled;
        CreativeAppBridgeCheck().IsChecked(settings.creative.bridgeEnabled);
        m_creativeCharacterAnimatorEnabled = settings.creative.characterAnimatorEnabled;
        CreativeAppCharacterAnimatorCheck().IsChecked(settings.creative.characterAnimatorEnabled);
        m_creativeFrescoEnabled = settings.creative.frescoEnabled;
        CreativeAppFrescoCheck().IsChecked(settings.creative.frescoEnabled);
        m_creativeDimensionEnabled = settings.creative.dimensionEnabled;
        CreativeAppDimensionCheck().IsChecked(settings.creative.dimensionEnabled);
        m_creativeSubstanceEnabled = settings.creative.substanceEnabled;
        CreativeAppSubstanceCheck().IsChecked(settings.creative.substanceEnabled);
        m_creativeAcrobatEnabled = settings.creative.acrobatEnabled;
        CreativeAppAcrobatCheck().IsChecked(settings.creative.acrobatEnabled);
        m_creativeOtherAdobeEnabled = settings.creative.otherAdobeEnabled;
        CreativeAppOtherAdobeCheck().IsChecked(settings.creative.otherAdobeEnabled);

        m_creativePrivacyMode = settings.creative.privacyMode;
        CreativePrivacyCombo().SelectedIndex(CreativePrivacyModeToComboIndex(m_creativePrivacyMode));
        m_creativeIdleBehavior = settings.creative.idleBehavior;
        CreativeIdleBehaviorCombo().SelectedIndex(CreativeIdleBehaviorToComboIndex(m_creativeIdleBehavior));
    }

    void MainWindow::AppendSettingsIssues(const std::wstring& operation, const std::vector<lrp::settings::SettingsIssue>& issues)
    {
        if (issues.empty())
            return;

        AppendDiagnosticLog(L"WARN", L"settings", operation + L" warnings: " + JoinIssueMessages(issues));
    }

    void MainWindow::LoadSettings()
    {
        auto loadResult = lrp::settings::LoadPersistedSettingsWithResult();
        ApplyPersistedSettingsSnapshot(loadResult.settings);

        SyncProductiveSettingsFromControls();
        UpdateProductivePreview(m_lastProductiveActivity);
        SyncCreativeSettingsFromControls();
        ApplyCreativeDetectorRuntimeState();
        RefreshCreativePreviewFromCurrentState();
        AppendSettingsIssues(L"Load settings", loadResult.issues);
    }

    lrp::settings::SettingsSaveResult MainWindow::SaveSettings()
    {
        auto saveResult = lrp::settings::SavePersistedSettingsWithResult(BuildPersistedSettingsSnapshot());
        AppendSettingsIssues(L"Save settings", saveResult.issues);
        return saveResult;
    }

    // =====================================================================
    // Utility
    // =====================================================================

    std::wstring MainWindow::FormatTime(int totalSeconds)
    {
        if (totalSeconds < 0) totalSeconds = 0;
        int hours = totalSeconds / 3600;
        int minutes = (totalSeconds % 3600) / 60;
        int seconds = totalSeconds % 60;
        wchar_t buf[16];
        if (hours > 0)
            swprintf_s(buf, L"%d:%02d:%02d", hours, minutes, seconds);
        else
            swprintf_s(buf, L"%d:%02d", minutes, seconds);
        return buf;
    }
}
