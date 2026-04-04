#include "pch.h"
#include "MainWindow.xaml.h"
#include "BrowserNativeMessaging.h"
#include "SettingsImport.h"
#include "SettingsModels.h"
#include "SettingsStore.h"
#include "SettingsUiBinder.h"
#include "StartupRegistration.h"
#include "TextUtilities.h"
#include "WindowTrayController.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

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
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>

#pragma comment(lib, "shell32.lib")

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media;
using namespace Microsoft::UI::Xaml::Media::Imaging;
using namespace Windows::Foundation;
using namespace Windows::Data::Json;
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

    constexpr wchar_t kSettingsExportFileName[] = L"settings-export.json";
    constexpr wchar_t kStartupTaskId[] = L"LastRichPresenceStartupTask";
    constexpr wchar_t kStartMinimizedArgument[] = L"--start-minimized";
    constexpr int32_t kDefaultWindowWidth = 1230;
    constexpr int32_t kDefaultWindowHeight = 845;
    constexpr DWORD kBrowserHintPipeReadTimeoutMs = 3500;
    constexpr DWORD kBrowserHintPipePollIntervalMs = 20;

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
        std::thread worker;
        HANDLE stopEvent{ nullptr };
        bool started{ false };
        bool binding{ false };
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

    bool TryReadExactPipeBytesWithTimeout(
        HANDLE pipe,
        HANDLE stopEvent,
        void* buffer,
        size_t bytesToRead,
        DWORD timeoutMs,
        std::wstring& errorOut)
    {
        auto* destination = static_cast<unsigned char*>(buffer);
        size_t bytesRemaining = bytesToRead;
        const auto deadline = GetTickCount64() + timeoutMs;

        while (bytesRemaining > 0)
        {
            if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0)
            {
                errorOut = L"server-stopping";
                return false;
            }

            DWORD bytesRead = 0;
            auto chunkSize = static_cast<DWORD>((std::min)(
                bytesRemaining,
                static_cast<size_t>((std::numeric_limits<DWORD>::max)())));
            if (ReadFile(pipe, destination, chunkSize, &bytesRead, nullptr))
            {
                if (bytesRead == 0)
                {
                    errorOut = L"pipe-client-disconnected";
                    return false;
                }

                destination += bytesRead;
                bytesRemaining -= bytesRead;
                continue;
            }

            auto lastError = GetLastError();
            if (lastError == ERROR_MORE_DATA && bytesRead > 0)
            {
                destination += bytesRead;
                bytesRemaining -= bytesRead;
                continue;
            }

            if (lastError == ERROR_NO_DATA)
            {
                if (GetTickCount64() >= deadline)
                {
                    errorOut = L"pipe-read-timeout";
                    return false;
                }

                auto waitResult = WaitForSingleObject(stopEvent, kBrowserHintPipePollIntervalMs);
                if (waitResult == WAIT_OBJECT_0)
                {
                    errorOut = L"server-stopping";
                    return false;
                }

                continue;
            }

            if (lastError == ERROR_BROKEN_PIPE || lastError == ERROR_PIPE_NOT_CONNECTED)
            {
                errorOut = L"pipe-client-disconnected";
                return false;
            }

            errorOut = L"pipe-read-failed";
            return false;
        }

        errorOut.clear();
        return true;
    }

    bool TryReadPipePayloadWithTimeout(
        HANDLE pipe,
        HANDLE stopEvent,
        size_t maxBytes,
        std::string& payloadOut,
        std::wstring& errorOut)
    {
        uint32_t payloadLength = 0;
        if (!TryReadExactPipeBytesWithTimeout(pipe, stopEvent, &payloadLength, sizeof(payloadLength), kBrowserHintPipeReadTimeoutMs, errorOut))
            return false;

        if (payloadLength > maxBytes)
        {
            errorOut = L"payload-too-large";
            return false;
        }

        payloadOut.assign(payloadLength, '\0');
        if (payloadLength == 0)
        {
            errorOut.clear();
            return true;
        }

        return TryReadExactPipeBytesWithTimeout(pipe, stopEvent, payloadOut.data(), payloadLength, kBrowserHintPipeReadTimeoutMs, errorOut);
    }

    bool IsTrustedBrowserHintPipeClient(HANDLE pipe, std::wstring& errorOut)
    {
        DWORD clientProcessId = 0;
        if (!GetNamedPipeClientProcessId(pipe, &clientProcessId) || clientProcessId == 0)
        {
            errorOut = L"pipe-client-unidentified";
            return false;
        }

        return lrp::browser::TryValidateBrowserNativeHostClientProcess(clientProcessId, &errorOut);
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

    using MainWindowImpl = winrt::Last_Rich_Presence::implementation::MainWindow;

    struct AppPageRoute
    {
        FrameworkElement (*pageHost)(MainWindowImpl&);
        NavigationViewItem (*navItem)(MainWindowImpl&);
        void (*resetScroll)(MainWindowImpl&);
    };

    FrameworkElement ResolveHomePageHost(MainWindowImpl& window)
    {
        return window.HomePage();
    }

    FrameworkElement ResolveMusicPageHost(MainWindowImpl& window)
    {
        return window.MusicPage();
    }

    FrameworkElement ResolveCreativePageHost(MainWindowImpl& window)
    {
        return window.CreativePage();
    }

    FrameworkElement ResolveProductivityPageHost(MainWindowImpl& window)
    {
        return window.ProductivityPage();
    }

    FrameworkElement ResolveSettingsPageHost(MainWindowImpl& window)
    {
        return window.SettingsPage();
    }

    NavigationViewItem ResolveHomeNavItem(MainWindowImpl& window)
    {
        return window.HomeNavItem();
    }

    NavigationViewItem ResolveMusicNavItem(MainWindowImpl& window)
    {
        return window.MusicNavItem();
    }

    NavigationViewItem ResolveCreativeNavItem(MainWindowImpl& window)
    {
        return window.CreativeNavItem();
    }

    NavigationViewItem ResolveProductivityNavItem(MainWindowImpl& window)
    {
        return window.ProductivityNavItem();
    }

    NavigationViewItem ResolveSettingsNavItem(MainWindowImpl& window)
    {
        return window.SettingsNavItem();
    }

    void ResetHomePageScroll(MainWindowImpl& window)
    {
        winrt::get_self<winrt::Last_Rich_Presence::implementation::HomePageControl>(
            window.HomePage())->ResetScrollPosition();
    }

    void ResetMusicPageScroll(MainWindowImpl& window)
    {
        winrt::get_self<winrt::Last_Rich_Presence::implementation::MusicPageControl>(
            window.MusicPage())->ResetScrollPosition();
    }

    void ResetCreativePageScroll(MainWindowImpl& window)
    {
        winrt::get_self<winrt::Last_Rich_Presence::implementation::CreativePageControl>(
            window.CreativePage())->ResetScrollPosition();
    }

    void ResetProductivityPageScroll(MainWindowImpl& window)
    {
        winrt::get_self<winrt::Last_Rich_Presence::implementation::ProductivityPageControl>(
            window.ProductivityPage())->ResetScrollPosition();
    }

    void ResetSettingsPageScroll(MainWindowImpl& window)
    {
        winrt::get_self<winrt::Last_Rich_Presence::implementation::SettingsPageControl>(
            window.SettingsPage())->ResetScrollPosition();
    }

    auto const& AppPageRoutes()
    {
        static const std::array<AppPageRoute, lrp::ui::kAppPageOrder.size()> routes{ {
            { ResolveHomePageHost, ResolveHomeNavItem, ResetHomePageScroll },
            { ResolveMusicPageHost, ResolveMusicNavItem, ResetMusicPageScroll },
            { ResolveCreativePageHost, ResolveCreativeNavItem, ResetCreativePageScroll },
            { ResolveProductivityPageHost, ResolveProductivityNavItem, ResetProductivityPageScroll },
            { ResolveSettingsPageHost, ResolveSettingsNavItem, ResetSettingsPageScroll }
        } };
        return routes;
    }

    AppPageRoute const* TryFindAppPageRoute(lrp::ui::AppPage page) noexcept
    {
        auto index = lrp::ui::TryAppPageIndex(page);
        WINRT_ASSERT(index.has_value());
        if (!index)
            return nullptr;

        auto const& routes = AppPageRoutes();
        return &routes[static_cast<size_t>(*index)];
    }

    void InitializePageRouting(MainWindowImpl& window)
    {
        auto const& routes = AppPageRoutes();
        for (size_t index = 0; index < routes.size(); ++index)
        {
            auto const page = lrp::ui::kAppPageOrder[index];
            auto const& route = routes[index];

            auto navItem = route.navItem(window);
            WINRT_ASSERT(static_cast<bool>(navItem));
            if (!navItem)
                continue;

            navItem.Tag(box_value(lrp::ui::AppPageTagHString(page)));
        }
    }

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

    std::string BuildBrowserHintBridgeResponse(bool ok, std::wstring_view error = {})
    {
        if (ok)
            return "{\"ok\":true}";

        auto errorUtf8 = winrt::to_string(std::wstring(error.empty() ? L"bridge-failed" : error));
        return std::string("{\"ok\":false,\"error\":\"") + errorUtf8 + "\"}";
    }

    void ProcessBrowserHintPipeClient(HANDLE pipe, HANDLE stopEvent)
    {
        std::wstring trustError;
        if (!IsTrustedBrowserHintPipeClient(pipe, trustError))
        {
            std::wstring writeError;
            lrp::browser::TryWriteLengthPrefixedMessage(
                pipe,
                BuildBrowserHintBridgeResponse(false, trustError),
                writeError);
            return;
        }

        std::string request;
        std::wstring readError;
        if (!TryReadPipePayloadWithTimeout(
                pipe,
                stopEvent,
                lrp::browser::kMaxBrowserNativeMessageBytes,
                request,
                readError))
        {
            if (readError == L"server-stopping")
                return;

            std::wstring writeError;
            lrp::browser::TryWriteLengthPrefixedMessage(
                pipe,
                BuildBrowserHintBridgeResponse(false, readError),
                writeError);
            return;
        }

        std::wstring parseError;
        auto response = UpdateStoredBrowserHint(request, &parseError)
            ? BuildBrowserHintBridgeResponse(true)
            : BuildBrowserHintBridgeResponse(false, parseError.empty() ? L"invalid-payload" : parseError);

        std::wstring writeError;
        lrp::browser::TryWriteLengthPrefixedMessage(pipe, response, writeError);
    }

    void BrowserHintPipeServerWorker(HANDLE stopEvent)
    {
        {
            auto& state = GetHintServerState();
            std::lock_guard<std::mutex> lock(state.mutex);
            state.started = true;
            state.binding = false;
        }

        for (;;)
        {
            if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0)
                break;

            std::wstring pipeError;
            HANDLE pipe = lrp::browser::CreateBrowserHintPipeServerHandle(
                PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                &pipeError);
            if (pipe == INVALID_HANDLE_VALUE)
                break;

            BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
            if (!connected)
            {
                CloseHandle(pipe);
                if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0)
                    break;
                continue;
            }

            DWORD pipeMode = PIPE_READMODE_MESSAGE | PIPE_NOWAIT;
            if (!SetNamedPipeHandleState(pipe, &pipeMode, nullptr, nullptr))
            {
                CloseHandle(pipe);
                if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0)
                    break;
                continue;
            }

            if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0)
            {
                DisconnectNamedPipe(pipe);
                CloseHandle(pipe);
                break;
            }

            ProcessBrowserHintPipeClient(pipe, stopEvent);
            // Ensure the native-host client can read the full response before the
            // server tears down this one-shot pipe instance.
            FlushFileBuffers(pipe);
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
        }

        auto& state = GetHintServerState();
        std::lock_guard<std::mutex> lock(state.mutex);
        state.started = false;
        state.binding = false;
    }

    void WakeBrowserHintPipeServer()
    {
        HANDLE pipe = CreateFileW(
            lrp::browser::kBrowserHintPipeName,
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);
        if (pipe != INVALID_HANDLE_VALUE)
            CloseHandle(pipe);
    }

    void StartBrowserHintServer()
    {
        auto& state = GetHintServerState();

        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.started || state.binding)
            return;

        auto stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stopEvent)
            return;

        try
        {
            state.stopEvent = stopEvent;
            state.binding = true;
            state.worker = std::thread([stopEvent]()
            {
                BrowserHintPipeServerWorker(stopEvent);
            });
        }
        catch (...)
        {
            CloseHandle(stopEvent);
            state.stopEvent = nullptr;
            state.started = false;
            state.binding = false;
        }
    }

    void StopBrowserHintServer()
    {
        std::thread worker;
        HANDLE stopEvent = nullptr;

        {
            auto& state = GetHintServerState();
            std::lock_guard<std::mutex> lock(state.mutex);

            stopEvent = state.stopEvent;
            if (stopEvent)
                SetEvent(stopEvent);

            worker = std::move(state.worker);
            state.stopEvent = nullptr;
            state.binding = false;
        }

        if (stopEvent)
        {
            WakeBrowserHintPipeServer();

            if (worker.joinable())
                worker.join();

            CloseHandle(stopEvent);
        }
        else if (worker.joinable())
        {
            worker.join();
        }

        auto& state = GetHintServerState();
        std::lock_guard<std::mutex> lock(state.mutex);
        state.started = false;
        state.binding = false;
        state.extensionSeen = false;
        state.extensionSeenAt = {};
        state.hasHint = false;
        state.lastAcceptedSequence = -1;
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

    HomePageControl* MainWindow::HomePageControlImpl()
    {
        return winrt::get_self<implementation::HomePageControl>(HomePage());
    }

    MusicPageControl* MainWindow::MusicPageControlImpl()
    {
        return winrt::get_self<implementation::MusicPageControl>(MusicPage());
    }

    ProductivityPageControl* MainWindow::ProductivityPageControlImpl()
    {
        return winrt::get_self<implementation::ProductivityPageControl>(ProductivityPage());
    }

    CreativePageControl* MainWindow::CreativePageControlImpl()
    {
        return winrt::get_self<implementation::CreativePageControl>(CreativePage());
    }

    SettingsPageControl* MainWindow::SettingsPageControlImpl()
    {
        return winrt::get_self<implementation::SettingsPageControl>(SettingsPage());
    }

    FrameworkElement MainWindow::PageHost(lrp::ui::AppPage page)
    {
        auto route = TryFindAppPageRoute(page);
        WINRT_ASSERT(route != nullptr);
        if (!route)
            return FrameworkElement{ nullptr };

        return route->pageHost(*this);
    }

    NavigationViewItem MainWindow::NavItemForPage(lrp::ui::AppPage page)
    {
        auto route = TryFindAppPageRoute(page);
        WINRT_ASSERT(route != nullptr);
        if (!route)
            return NavigationViewItem{ nullptr };

        return route->navItem(*this);
    }

    void MainWindow::ShowOnlyPage(lrp::ui::AppPage page)
    {
        auto const& routes = AppPageRoutes();
        for (size_t index = 0; index < routes.size(); ++index)
        {
            auto const pageForRoute = lrp::ui::kAppPageOrder[index];
            auto const& route = routes[index];
            auto host = route.pageHost(*this);
            WINRT_ASSERT(static_cast<bool>(host));
            if (!host)
                continue;

            host.Visibility(pageForRoute == page ? Visibility::Visible : Visibility::Collapsed);
        }
    }

    void MainWindow::ResetPageScrollPosition(lrp::ui::AppPage page)
    {
        auto route = TryFindAppPageRoute(page);
        WINRT_ASSERT(route != nullptr);
        if (!route)
            return;

        route->resetScroll(*this);
    }

    void MainWindow::SyncMusicSettingsFromControls()
    {
        m_sourceDebugMode = m_settings.media.sourceDebugMode;

        if (!m_presence)
            return;

        m_presence->SetShowTimestamps(m_settings.media.showTimestamps);
        m_presence->SetShowSource(m_settings.media.showSource);
        m_presence->SetShowPaused(m_settings.media.showPaused);
        m_presence->SetShowAlbumArt(m_settings.media.showAlbumArt);
    }

    void MainWindow::SyncShellSettingsFromControls()
    {
        m_closeToTrayOnClose = m_settings.behavior.closeToTrayOnClose;
        m_launchOnStartup = m_settings.behavior.launchOnStartup;
        m_startMinimizedToTray = m_settings.behavior.startMinimizedToTray;
        m_trayLeftClickToggles = m_settings.behavior.trayLeftClickToggles;
        m_showDefaultIdleStatus = m_settings.media.showDefaultIdleStatus;
        m_sensitiveKeywordFilter = m_settings.media.sensitiveKeywordFilter;
        m_strictBrowserPrivacy = m_settings.media.strictBrowserPrivacy;
        m_suppressBrowserAlbumArt = m_settings.media.suppressBrowserAlbumArt;
        m_blockedAppSiteTermsRaw = m_settings.media.blockedAppSiteTermsRaw;
        m_themeMode = m_settings.behavior.themeMode;
        m_mediaActivityTypeOverride = m_settings.media.activityTypeOverride;
        m_creativeActivityTypeOverride = m_settings.creative.activityTypeOverride;
        m_productiveActivityTypeOverride = m_settings.productive.activityTypeOverride;

        m_trayController.SetCloseToTrayOnClose(m_closeToTrayOnClose);
        m_trayController.SetTrayLeftClickToggles(m_trayLeftClickToggles);
        m_trayController.SetPresenceEnabled(m_enabled);
    }

    lrp::ui::MusicPageState MainWindow::BuildMusicPageState(const MediaInfo& info, int positionSeconds, int durationSeconds)
    {
        lrp::ui::MusicPageState state{};
        state.hasMedia = !info.title.empty();
        state.isPlaying = info.isPlaying;
        state.motionEnabled = IsMotionEnabled();

        if (positionSeconds < 0)
            positionSeconds = static_cast<int>(info.position.count());
        if (durationSeconds < 0)
            durationSeconds = static_cast<int>(info.duration.count());

        if (positionSeconds < 0)
            positionSeconds = 0;
        if (durationSeconds < 0)
            durationSeconds = 0;
        if (durationSeconds > 0 && positionSeconds > durationSeconds)
            positionSeconds = durationSeconds;

        state.title = state.hasMedia ? hstring(info.title) : hstring(L"Nothing playing");
        state.artist = state.hasMedia
            ? hstring(info.artist.empty() ? std::wstring(L"\x2014") : info.artist)
            : hstring(L"\x2014");
        state.albumTitle = hstring(info.albumTitle);
        state.positionText = hstring(FormatTime(positionSeconds));
        state.durationText = hstring(FormatTime(durationSeconds));
        state.playbackStateText = !state.hasMedia ? hstring(L"Idle") : hstring(info.isPlaying ? L"Playing" : L"Paused");
        state.showSourceBadge = m_settings.media.showSource && !info.sourceDisplayName.empty();
        state.sourceText = info.sourceDisplayName.empty() ? hstring(L"No source") : hstring(info.sourceDisplayName);
        state.showSourceDebug = state.showSourceBadge && m_settings.media.sourceDebugMode;
        state.sourceDebugText = state.showSourceDebug ? hstring(FormatSourceDebugText(info)) : hstring();

        if (durationSeconds > 0)
        {
            auto progress = (static_cast<double>(positionSeconds) / static_cast<double>(durationSeconds)) * 100.0;
            if (progress < 0.0)
                progress = 0.0;
            if (progress > 100.0)
                progress = 100.0;
            state.progressPercent = progress;
        }

        return state;
    }

    lrp::ui::ProductivityPageState MainWindow::BuildProductivityPageState(const ProductiveActivityInfo& info)
    {
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

        lrp::ui::ProductivityPageState state{};
        state.settingsControlsEnabled = m_productiveEnabled;

        bool rawActive = info.active;
        bool rawAllowed = !rawActive || IsProductiveAppEnabled(info);
        bool rawFiltered = rawActive && !rawAllowed;

        if (!m_productiveEnabled)
        {
            state.detectedAppText = L"Productive RPC disabled";
            state.detectedProjectText = L"Enable Productive RPC to publish Office activity.";
            state.detectedWindowText = L"None";
            state.detectedProcessText = L"None";
            state.runtimeSummaryText = L"Productive pipeline is disabled.";
            return state;
        }

        if (rawFiltered)
        {
            state.detectedAppText = hstring(buildAppLabel(info) + L" (filtered)");
            state.detectedProjectText = L"Enable this app in the per-app list to allow Productivity RPC.";
            state.detectedWindowText = hstring(info.windowTitle.empty() ? std::wstring(L"None") : info.windowTitle);
            state.detectedProcessText = hstring(buildProcessLabel(info));
            state.runtimeSummaryText = hstring(
                L"Detected app is blocked by Productivity per-app filters. Detection mode: " +
                ProductiveDetectionModeLabel(m_productiveDetectionMode) + L".");
            return state;
        }

        if (!info.active)
        {
            state.detectedAppText = L"Awaiting Productive Activity";
            state.detectedProjectText = L"Launch a supported Office app to update your status.";
            state.detectedWindowText = L"None";
            state.detectedProcessText = L"None";
            state.runtimeSummaryText = hstring(
                L"Waiting for supported apps (Word, Excel, PowerPoint, OneNote, Access, Publisher, Visio, Project). "
                L"Detection mode: " + ProductiveDetectionModeLabel(m_productiveDetectionMode) + L".");
            return state;
        }

        std::wstring subtitle;
        if (m_productiveShowProjectName && !info.projectHint.empty())
            subtitle = L"Working on " + info.projectHint;
        else
            subtitle = L"Working in " + (info.appName.empty() ? std::wstring(L"Microsoft Office") : info.appName);

        state.detectedAppText = hstring(buildAppLabel(info));
        state.detectedProjectText = hstring(subtitle);
        state.detectedWindowText = hstring(info.windowTitle.empty() ? std::wstring(L"None") : info.windowTitle);
        state.detectedProcessText = hstring(buildProcessLabel(info));
        state.runtimeSummaryText = hstring(
            L"Productive detector active. Detection mode: " + ProductiveDetectionModeLabel(m_productiveDetectionMode) +
            L". Outlook, Teams, and background Office helpers are excluded.");
        return state;
    }

    lrp::ui::CreativePageState MainWindow::BuildCreativePageState(const CreativeActivityInfo& info)
    {
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

        lrp::ui::CreativePageState state{};
        state.settingsControlsEnabled = m_creativeEnabled;

        std::wstring policySummary =
            L"Priority: " + CreativePriorityModeLabel(m_creativePriority) +
            L" | Detection: " + CreativeDetectionModeLabel(m_creativeDetectionMode) +
            L" | Privacy: " + CreativePrivacyModeLabel(m_creativePrivacyMode) +
            L" | Idle: " + CreativeIdleBehaviorLabel(m_creativeIdleBehavior) + L". ";

        if (!m_creativeEnabled)
        {
            state.mvpHeadlineText = L"Creativity RPC (disabled)";
            state.mvpSummaryText = hstring(
                policySummary +
                L"Creativity detector/pipeline is disabled. Settings are preserved and can be re-enabled later.");
            state.detectedAppText = L"Disabled";
            state.detectedProjectText = L"Creativity detection is turned off.";
            state.detectedWindowText = L"--";
            state.detectedProcessText = L"--";
            return state;
        }

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
                    state.detectedAppText = L"Creativity activity hidden";
                    state.detectedProjectText = L"(Filtered and hidden by privacy mode)";
                    state.detectedWindowText = L"--";
                    state.detectedProcessText = L"--";
                }
                else if (m_creativePrivacyMode == CreativePrivacyMode::AppOnly)
                {
                    state.detectedAppText = hstring(buildAppLabel(info) + L" (filtered)");
                    state.detectedProjectText = L"(Hidden by privacy mode)";
                    state.detectedWindowText = L"(Hidden by privacy mode)";
                    state.detectedProcessText = L"(Hidden by privacy mode)";
                }
                else
                {
                    state.detectedAppText = hstring(buildAppLabel(info) + L" (filtered)");
                    state.detectedProjectText = L"Enable this app in the per-app list to allow Creativity RPC.";
                    state.detectedWindowText = hstring(
                        m_creativeShowWindowTitle && !info.windowTitle.empty()
                            ? info.windowTitle
                            : std::wstring(L"(Hidden by setting)"));
                    state.detectedProcessText = hstring(buildProcessLabel(info));
                }
            }
            else if (clearImmediatelyMode)
            {
                headline = L"Creativity RPC (idle - clear mode)";
                summary += L"No supported app detected. Creativity activity clears immediately.";
                state.detectedAppText = L"Awaiting Creative Activity";
                state.detectedProjectText = L"Creativity preview is idle.";
                state.detectedWindowText = L"None";
                state.detectedProcessText = L"None";
            }
            else
            {
                summary += L"Listening for supported apps. Detector remains isolated from the current media pipeline.";
                state.detectedAppText = L"Awaiting Creative Activity";
                state.detectedProjectText = L"Launch a supported app to update your status.";
                state.detectedWindowText = L"None";
                state.detectedProcessText = L"None";
            }

            state.mvpHeadlineText = hstring(headline);
            state.mvpSummaryText = hstring(summary);
            return state;
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
                if (elapsed < 0)
                    elapsed = 0;
                secondsLeft = static_cast<int>(kCreativeIdleHoldSeconds - elapsed);
                if (secondsLeft < 0)
                    secondsLeft = 0;
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

        state.mvpHeadlineText = hstring(headline);
        state.mvpSummaryText = hstring(summary);

        if (m_creativePrivacyMode == CreativePrivacyMode::Private)
        {
            state.detectedAppText = L"Creativity activity hidden";
            state.detectedProjectText = L"(Private mode)";
            state.detectedWindowText = L"(Private mode)";
            state.detectedProcessText = L"(Private mode)";
            return state;
        }

        std::wstring appLabel = buildAppLabel(displayInfo);
        if (showingHeld)
            appLabel += L" (held)";

        if (m_creativePrivacyMode == CreativePrivacyMode::AppOnly)
        {
            state.detectedAppText = hstring(appLabel);
            state.detectedProjectText = L"(Hidden by privacy mode)";
            state.detectedWindowText = L"(Hidden by privacy mode)";
            state.detectedProcessText = L"(Hidden by privacy mode)";
            return state;
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

        state.detectedAppText = hstring(appLabel);
        state.detectedProjectText = hstring(projectText);
        state.detectedWindowText = hstring(windowText);
        state.detectedProcessText = hstring(buildProcessLabel(displayInfo));
        return state;
    }

    lrp::ui::HomePageState MainWindow::BuildHomePageState(int positionSeconds, int durationSeconds)
    {
        auto buildProductiveAppLabel = [](const ProductiveActivityInfo& activity)
        {
            std::wstring label = activity.appName.empty() ? std::wstring(L"Microsoft Office") : activity.appName;
            if (!activity.appKey.empty())
                label += L" (" + activity.appKey + L")";
            return label;
        };

        auto buildProductiveProcessLabel = [](const ProductiveActivityInfo& activity)
        {
            std::wstring label = activity.processName.empty() ? std::wstring(L"None") : activity.processName;
            if (activity.processId != 0)
                label += L" (PID " + std::to_wstring(activity.processId) + L")";
            return label;
        };

        auto buildCreativeAppLabel = [](const CreativeActivityInfo& activity)
        {
            std::wstring label = activity.appName.empty() ? std::wstring(L"Adobe creativity app") : activity.appName;
            if (!activity.appKey.empty())
                label += L" (" + activity.appKey + L")";
            return label;
        };

        auto buildCreativeProcessLabel = [](const CreativeActivityInfo& activity)
        {
            std::wstring label = activity.processName.empty() ? std::wstring(L"--") : activity.processName;
            if (activity.processId != 0)
                label += L" (PID " + std::to_wstring(activity.processId) + L")";
            return label;
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

        lrp::ui::HomePageState state{};
        state.richPresenceEnabled = m_enabled;
        state.motionEnabled = IsMotionEnabled();

        bool connected = m_presence && m_presence->IsConnected();
        bool bridgeStarted = false;
        bool bridgeBinding = false;
        bool extensionConnected = false;
        bool hasHint = false;
        int64_t hintAgeSeconds = -1;
        {
            auto& hintState = GetHintServerState();
            std::lock_guard<std::mutex> lock(hintState.mutex);
            bridgeStarted = hintState.started;
            bridgeBinding = hintState.binding;
            extensionConnected = IsExtensionConnected(hintState);
            hasHint = hintState.hasHint;

            if (hasHint)
            {
                hintAgeSeconds = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - hintState.hint.updatedAt).count();
                if (hintAgeSeconds < 0)
                    hintAgeSeconds = 0;
            }
        }

        if (!m_enabled)
        {
            state.statusText = L"Disabled";
            state.statusSubtext = L"Rich Presence is off";
            state.healthText = L"Discord: OFF | Extension: OFF | Hint age: --";
            state.statusIndicatorBrushKey = L"StatusDisconnectedBrush";
            state.showLiveBadge = false;
            state.extensionText = L"Browser hints paused";
            state.extensionSubtext = L"Rich Presence is disabled.";
            state.extensionIndicatorBrushKey = L"StatusDisconnectedBrush";
        }
        else
        {
            state.statusIndicatorBrushKey = connected ? L"StatusConnectedBrush" : L"StatusConnectingBrush";
            state.statusText = connected ? L"Connected to Discord" : L"Connecting to Discord...";
            state.statusSubtext = connected ? L"Rich Presence is active" : L"Waiting for Discord client";
            auto nativeHostRegistrationError = lrp::browser::GetLastNativeHostRegistrationError();

            if (extensionConnected)
            {
                state.extensionIndicatorBrushKey = L"StatusConnectedBrush";
                state.extensionText = L"Browser extension";
                state.extensionSubtext = hasHint ? L"Receiving hints" : L"Connected and ready";
            }
            else if (bridgeStarted || bridgeBinding)
            {
                state.extensionIndicatorBrushKey = L"StatusConnectingBrush";
                state.extensionText = L"Native host waiting";
                state.extensionSubtext = L"Open a supported browser tab";
            }
            else
            {
                state.extensionIndicatorBrushKey = L"StatusDisconnectedBrush";
                state.extensionText = L"Native host offline";
                state.extensionSubtext = nativeHostRegistrationError.empty()
                    ? L"Restart the app to refresh native-host registration"
                    : (L"Registration refresh failed: " + nativeHostRegistrationError);
            }

            auto discordHealth = connected ? L"OK" : L"WAIT";
            auto bridgeHealth = extensionConnected ? L"OK" : ((bridgeStarted || bridgeBinding) ? L"INIT" : L"DOWN");
            auto hintAgeText = (hasHint && hintAgeSeconds >= 0)
                ? (std::to_wstring(hintAgeSeconds) + L"s")
                : std::wstring(L"--");
            state.healthText = hstring(
                std::wstring(L"Discord: ") + discordHealth +
                L" | Extension: " + bridgeHealth +
                L" | Hint age: " + hintAgeText);

            const bool activePlayback = !m_lastMedia.title.empty() && m_lastMedia.isPlaying;
            const bool recentPresencePush = m_lastPresencePushPlaying &&
                ((std::chrono::steady_clock::now() - m_lastPresencePushAt) <= std::chrono::seconds(4));
            state.showLiveBadge = connected && (activePlayback || recentPresencePush);
        }

        if (!m_lastMedia.title.empty())
        {
            if (positionSeconds < 0 || durationSeconds < 0)
            {
                int liveHintPos = 0;
                int liveHintDur = 0;
                if (TryGetLiveHintTimeline(m_lastMedia, liveHintPos, liveHintDur))
                {
                    if (positionSeconds < 0)
                        positionSeconds = liveHintPos;
                    if (durationSeconds < 0)
                        durationSeconds = liveHintDur;
                }
            }

            if (positionSeconds < 0)
                positionSeconds = static_cast<int>(m_lastMedia.position.count());
            if (durationSeconds < 0)
                durationSeconds = static_cast<int>(m_lastMedia.duration.count());
            if (positionSeconds < 0)
                positionSeconds = 0;
            if (durationSeconds < 0)
                durationSeconds = 0;
            if (durationSeconds > 0 && positionSeconds > durationSeconds)
                positionSeconds = durationSeconds;

            state.sourceValue = hstring(m_lastMedia.sourceDisplayName.empty() ? std::wstring(L"Unknown source") : m_lastMedia.sourceDisplayName);
            state.sourceSubtext = hstring(m_lastMedia.sourceName.empty() ? std::wstring(L"Detected via Windows Media Controls") : m_lastMedia.sourceName);
            state.detectedViaText = hstring(BuildDetectedViaLabel(m_lastMedia, m_lastMergeState));
            state.miniTitle = hstring(m_lastMedia.title);
            state.miniArtist = hstring(m_lastMedia.artist.empty() ? std::wstring(L"\x2014") : m_lastMedia.artist);
            state.miniTimerText = hstring(FormatTime(positionSeconds) + L" / " + FormatTime(durationSeconds > 0 ? durationSeconds : 0));
            state.miniStatusGlyph = m_lastMedia.isPlaying ? L"\xE768" : L"\xE769";
            state.miniStatusText = m_lastMedia.isPlaying ? L"Live" : L"Paused";
            state.miniStatusBrushKey = m_lastMedia.isPlaying ? L"AccentTextFillColorPrimaryBrush" : L"TextFillColorSecondaryBrush";
            state.showPausedChip = !m_lastMedia.isPlaying;
        }
        else if (!m_enabled)
        {
            state.miniStatusGlyph = L"\xE711";
            state.miniStatusText = L"Off";
            state.miniStatusBrushKey = L"TextFillColorSecondaryBrush";
        }

        const auto& rawProductive = m_lastProductiveActivity;
        bool productiveRawActive = rawProductive.active;
        bool productiveRawAllowed = !productiveRawActive || IsProductiveAppEnabled(rawProductive);
        bool productiveRawFiltered = productiveRawActive && !productiveRawAllowed;

        if (!m_enabled)
        {
            state.productiveTitle = L"Rich Presence is off";
            state.productiveSubtitle = L"Enable Rich Presence to run the Productive detector.";
            state.productiveWindowText = L"None";
            state.productiveProcessText = L"None";
            state.productiveStatusText = L"Paused";
            state.productiveStatusGlyph = L"\xE769";
            state.productiveStatusBrushKey = L"TextFillColorSecondaryBrush";
            state.productiveSourceValue = L"Productivity pipeline paused";
            state.productiveSourceSubtext = L"Rich Presence is disabled.";
            state.productiveDetectedViaText = L"Detected via --";
            state.productiveDetectorText = L"Productivity detector paused";
            state.productiveDetectorSubtext = L"Enable Rich Presence to resume.";
            state.productiveDetectorBrushKey = L"StatusDisconnectedBrush";
        }
        else if (!m_productiveEnabled)
        {
            state.productiveTitle = L"Productive RPC disabled";
            state.productiveSubtitle = L"Turn on Productive RPC in the Productive section.";
            state.productiveWindowText = L"None";
            state.productiveProcessText = L"None";
            state.productiveStatusText = L"Disabled";
            state.productiveStatusGlyph = L"\xE711";
            state.productiveStatusBrushKey = L"TextFillColorSecondaryBrush";
            state.productiveSourceValue = L"Productivity source disabled";
            state.productiveSourceSubtext = L"Turn on Productive RPC in Productive settings.";
            state.productiveDetectedViaText = L"Detected via --";
            state.productiveDetectorText = L"Productivity detector off";
            state.productiveDetectorSubtext = L"Productive pipeline toggle is off.";
            state.productiveDetectorBrushKey = L"StatusDisconnectedBrush";
        }
        else if (productiveRawFiltered)
        {
            auto filteredLabel = buildProductiveAppLabel(rawProductive);
            state.productiveTitle = hstring(filteredLabel + L" (filtered)");
            state.productiveSubtitle = L"Enable this app in Productivity filters to allow activity.";
            state.productiveWindowText = hstring(rawProductive.windowTitle.empty() ? std::wstring(L"None") : rawProductive.windowTitle);
            state.productiveProcessText = hstring(buildProductiveProcessLabel(rawProductive));
            state.productiveStatusText = L"Filtered";
            state.productiveStatusGlyph = L"\xE711";
            state.productiveStatusBrushKey = L"TextFillColorSecondaryBrush";
            state.productiveSourceValue = hstring(filteredLabel + L" (filtered)");
            state.productiveSourceSubtext = L"Blocked by your Productivity per-app filters.";
            state.productiveDetectedViaText = hstring(productiveDetectedViaLabel());
            state.productiveDetectorText = L"Productivity detector filtering";
            state.productiveDetectorSubtext = L"App blocked by your Productivity app filter.";
            state.productiveDetectorBrushKey = L"StatusConnectingBrush";
        }
        else if (!productiveRawActive)
        {
            state.productiveTitle = L"Awaiting Productive Activity";
            state.productiveSubtitle = L"Launch a supported Office app to update your status.";
            state.productiveWindowText = L"None";
            state.productiveProcessText = L"None";
            state.productiveStatusText = L"Waiting";
            state.productiveStatusGlyph = L"\xE160";
            state.productiveStatusBrushKey = L"TextFillColorSecondaryBrush";
            state.productiveSourceValue = L"No active productivity source";
            state.productiveSourceSubtext = L"Waiting for supported apps...";
            state.productiveDetectedViaText = hstring(productiveDetectedViaLabel());
            state.productiveDetectorText = L"Productivity detector waiting";
            state.productiveDetectorSubtext = L"Waiting for supported apps...";
            state.productiveDetectorBrushKey = L"StatusConnectingBrush";
        }
        else
        {
            std::wstring subtitle;
            if (m_productiveShowProjectName && !rawProductive.projectHint.empty())
                subtitle = L"Working on " + rawProductive.projectHint;
            else
                subtitle = L"Working in " + (rawProductive.appName.empty() ? std::wstring(L"Microsoft Office") : rawProductive.appName);

            auto appLabel = buildProductiveAppLabel(rawProductive);
            state.productiveTitle = hstring(appLabel);
            state.productiveSubtitle = hstring(subtitle);
            state.productiveWindowText = hstring(rawProductive.windowTitle.empty() ? std::wstring(L"None") : rawProductive.windowTitle);
            state.productiveProcessText = hstring(buildProductiveProcessLabel(rawProductive));
            state.productiveStatusText = L"Active";
            state.productiveStatusGlyph = L"\xE768";
            state.productiveStatusBrushKey = L"AccentTextFillColorPrimaryBrush";
            state.productiveSourceValue = hstring(appLabel);
            state.productiveSourceSubtext = L"Publishing current Office app.";
            state.productiveDetectedViaText = hstring(productiveDetectedViaLabel());
            state.productiveDetectorText = L"Productivity detector active";
            state.productiveDetectorSubtext = hstring(L"Mode: " + ProductiveDetectionModeLabel(m_productiveDetectionMode));
            state.productiveDetectorBrushKey = L"StatusConnectedBrush";
        }

        const auto& rawCreative = m_lastCreativeActivity;
        bool creativeRawActive = rawCreative.active;
        bool creativeRawAllowed = !creativeRawActive || IsCreativeAppEnabled(rawCreative);
        bool creativeRawFiltered = creativeRawActive && !creativeRawAllowed;
        bool clearImmediatelyMode = (m_creativeIdleBehavior == CreativeIdleBehavior::ClearImmediately);

        CreativeActivityInfo displayCreative{};
        bool showingHeld = false;
        bool showDisplay = TryGetEffectiveCreativeActivityForRpc(displayCreative, showingHeld);

        if (!m_enabled)
        {
            state.creativeTitle = L"Rich Presence is off";
            state.creativeSubtitle = L"Enable Rich Presence to run the Creativity detector.";
            state.creativeWindowText = L"--";
            state.creativeProcessText = L"--";
            state.creativeStatusText = L"Paused";
            state.creativeStatusGlyph = L"\xE769";
            state.creativeStatusBrushKey = L"TextFillColorSecondaryBrush";
            state.creativeSourceValue = L"Creativity pipeline paused";
            state.creativeSourceSubtext = L"Rich Presence is disabled.";
            state.creativeDetectedViaText = L"Detected via --";
            state.creativeDetectorText = L"Creativity detector paused";
            state.creativeDetectorSubtext = L"Enable Rich Presence to resume.";
            state.creativeDetectorBrushKey = L"StatusDisconnectedBrush";
        }
        else if (!m_creativeEnabled)
        {
            state.creativeTitle = L"Creativity RPC disabled";
            state.creativeSubtitle = L"Turn on Creativity RPC in the Creativity section.";
            state.creativeWindowText = L"--";
            state.creativeProcessText = L"--";
            state.creativeStatusText = L"Disabled";
            state.creativeStatusGlyph = L"\xE711";
            state.creativeStatusBrushKey = L"TextFillColorSecondaryBrush";
            state.creativeSourceValue = L"Creativity source disabled";
            state.creativeSourceSubtext = L"Turn on Creativity RPC in Creativity settings.";
            state.creativeDetectedViaText = L"Detected via --";
            state.creativeDetectorText = L"Creativity detector off";
            state.creativeDetectorSubtext = L"Creativity pipeline toggle is off.";
            state.creativeDetectorBrushKey = L"StatusDisconnectedBrush";
        }
        else if (!showDisplay)
        {
            if (creativeRawFiltered)
            {
                state.creativeStatusText = L"Filtered";
                state.creativeStatusGlyph = L"\xE711";
                state.creativeStatusBrushKey = L"TextFillColorSecondaryBrush";
                if (m_creativePrivacyMode == CreativePrivacyMode::Private)
                {
                    state.creativeTitle = L"Creativity activity hidden";
                    state.creativeSubtitle = L"(Filtered and hidden by privacy mode)";
                    state.creativeWindowText = L"--";
                    state.creativeProcessText = L"--";
                    state.creativeSourceValue = L"Filtered creativity app";
                    state.creativeSourceSubtext = L"Hidden by privacy mode.";
                }
                else if (m_creativePrivacyMode == CreativePrivacyMode::AppOnly)
                {
                    auto appLabel = buildCreativeAppLabel(rawCreative) + L" (filtered)";
                    state.creativeTitle = hstring(appLabel);
                    state.creativeSubtitle = L"Filtered by your per-app Creativity settings.";
                    state.creativeWindowText = L"(Hidden by privacy mode)";
                    state.creativeProcessText = L"(Hidden by privacy mode)";
                    state.creativeSourceValue = hstring(appLabel);
                    state.creativeSourceSubtext = L"Blocked by your per-app Creativity filters.";
                }
                else
                {
                    std::wstring windowText =
                        (m_creativeShowWindowTitle && !rawCreative.windowTitle.empty())
                            ? rawCreative.windowTitle
                            : std::wstring(L"(Hidden by setting)");
                    state.creativeTitle = hstring(buildCreativeAppLabel(rawCreative) + L" (filtered)");
                    state.creativeSubtitle = L"Enable this app in Creativity filters to allow activity.";
                    state.creativeWindowText = hstring(windowText);
                    state.creativeProcessText = hstring(buildCreativeProcessLabel(rawCreative));
                    state.creativeSourceValue = hstring(buildCreativeAppLabel(rawCreative) + L" (filtered)");
                    state.creativeSourceSubtext = L"Enable this app in Creativity filters.";
                }
                state.creativeDetectedViaText = hstring(creativeDetectedViaLabel());
                state.creativeDetectorText = L"Creativity detector filtering";
                state.creativeDetectorSubtext = L"App blocked by your Creativity app filter.";
                state.creativeDetectorBrushKey = L"StatusConnectingBrush";
            }
            else if (clearImmediatelyMode)
            {
                state.creativeTitle = L"Awaiting Creative Activity";
                state.creativeSubtitle = L"No supported app detected (clear mode).";
                state.creativeWindowText = L"None";
                state.creativeProcessText = L"None";
                state.creativeStatusText = L"Idle";
                state.creativeStatusGlyph = L"\xE160";
                state.creativeStatusBrushKey = L"TextFillColorSecondaryBrush";
                state.creativeSourceValue = L"No active creativity source";
                state.creativeSourceSubtext = L"Clear mode: idle source is cleared immediately.";
                state.creativeDetectedViaText = hstring(creativeDetectedViaLabel());
                state.creativeDetectorText = L"Creativity detector idle";
                state.creativeDetectorSubtext = L"Waiting for supported apps...";
                state.creativeDetectorBrushKey = L"StatusDisconnectedBrush";
            }
            else
            {
                state.creativeTitle = L"Awaiting Creative Activity";
                state.creativeSubtitle = L"Launch a supported app to update your status.";
                state.creativeWindowText = L"None";
                state.creativeProcessText = L"None";
                state.creativeStatusText = L"Waiting";
                state.creativeStatusGlyph = L"\xE160";
                state.creativeStatusBrushKey = L"TextFillColorSecondaryBrush";
                state.creativeSourceValue = L"No active creativity source";
                state.creativeSourceSubtext = L"Hold mode: waits before clearing recent app.";
                state.creativeDetectedViaText = hstring(creativeDetectedViaLabel());
                state.creativeDetectorText = L"Creativity detector waiting";
                state.creativeDetectorSubtext = L"Waiting for supported apps...";
                state.creativeDetectorBrushKey = L"StatusConnectingBrush";
            }
        }
        else if (m_creativePrivacyMode == CreativePrivacyMode::Private)
        {
            state.creativeTitle = L"Creativity activity hidden";
            state.creativeSubtitle = L"Private mode is enabled.";
            state.creativeWindowText = L"(Private mode)";
            state.creativeProcessText = L"(Private mode)";
            state.creativeStatusText = L"Private";
            state.creativeStatusGlyph = L"\xE72E";
            state.creativeStatusBrushKey = L"TextFillColorSecondaryBrush";
            state.creativeSourceValue = L"Creativity source hidden";
            state.creativeSourceSubtext = L"Private mode is enabled.";
            state.creativeDetectedViaText = hstring(creativeDetectedViaLabel());
            state.creativeDetectorText = L"Creativity detector active";
            state.creativeDetectorSubtext = L"Activity captured, details hidden.";
            state.creativeDetectorBrushKey = L"StatusConnectedBrush";
        }
        else
        {
            auto appLabel = buildCreativeAppLabel(displayCreative);
            if (showingHeld)
                appLabel += L" (held)";

            state.creativeStatusText = showingHeld ? L"Held" : L"Active";
            state.creativeStatusGlyph = showingHeld ? L"\xE823" : L"\xE768";
            state.creativeStatusBrushKey = L"AccentTextFillColorPrimaryBrush";
            state.creativeDetectedViaText = hstring(creativeDetectedViaLabel());

            if (m_creativePrivacyMode == CreativePrivacyMode::AppOnly)
            {
                state.creativeTitle = hstring(appLabel);
                state.creativeSubtitle = L"(Details hidden by privacy mode)";
                state.creativeWindowText = L"(Hidden by privacy mode)";
                state.creativeProcessText = L"(Hidden by privacy mode)";
                state.creativeSourceValue = hstring(appLabel);
                state.creativeSourceSubtext = L"App-only privacy hides project and window details.";
            }
            else
            {
                std::wstring projectText;
                if (!m_creativeShowProjectName)
                    projectText = L"(Hidden by setting)";
                else if (displayCreative.projectHint.empty())
                    projectText = L"(No project title heuristic)";
                else
                    projectText = displayCreative.projectHint;

                std::wstring windowText;
                if (!m_creativeShowWindowTitle)
                    windowText = L"(Hidden by debug setting)";
                else if (displayCreative.windowTitle.empty())
                    windowText = L"--";
                else
                    windowText = displayCreative.windowTitle;

                state.creativeTitle = hstring(appLabel);
                state.creativeSubtitle = hstring(projectText);
                state.creativeWindowText = hstring(windowText);
                state.creativeProcessText = hstring(buildCreativeProcessLabel(displayCreative));
                state.creativeSourceValue = hstring(appLabel);
                state.creativeSourceSubtext = hstring(
                    showingHeld ? std::wstring(L"Holding last detected Adobe app.") : std::wstring(L"Publishing current Adobe app."));
            }

            state.creativeDetectorText = showingHeld ? L"Creativity detector holding" : L"Creativity detector active";
            state.creativeDetectorSubtext = hstring(
                L"Mode: " + CreativeDetectionModeLabel(m_creativeDetectionMode) +
                L" | Idle: " + CreativeIdleBehaviorLabel(m_creativeIdleBehavior));
            state.creativeDetectorBrushKey = showingHeld ? L"StatusConnectingBrush" : L"StatusConnectedBrush";
        }

        return state;
    }

    void MainWindow::InitializeRuntimeComponents()
    {
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
    }

    void MainWindow::InitializeWindowChromeAndTray()
    {
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
        Closed([this](auto const&, auto const&)
        {
            ShutdownWindow();
        });
    }

    void MainWindow::ApplyInitialSettingsAndVisibility()
    {
        LoadSettings();

        m_isInitializing = false;

        ApplyLaunchOnStartupState(m_launchOnStartup, false, true);

        if (m_startMinimizedToTray || IsStartMinimizedLaunchRequested())
        {
            if (!m_isShuttingDown)
            {
                if (m_trayController.IsReady())
                {
                    HideWindowToTray();
                }
                else
                {
                    Activate();
                    AppendDiagnosticLog(
                        L"WARN",
                        L"tray",
                        L"Start hidden requested, but tray initialization failed. Showing the main window instead.");
                }
            }
        }
    }

    void MainWindow::RegisterRuntimeCallbacks()
    {
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
    }

    void MainWindow::StartRuntimeServices()
    {
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
        AppendDiagnosticLog(L"INFO", L"bridge", L"Browser native-host transport started");
        auto nativeHostRegistrationError = lrp::browser::GetLastNativeHostRegistrationError();
        if (!nativeHostRegistrationError.empty())
        {
            AppendDiagnosticLog(
                L"WARN",
                L"bridge",
                L"Browser native-host registration refresh failed: " + nativeHostRegistrationError);
        }
        SyncProductiveRpcOutput();
        SyncCreativeRpcOutput();
        if (!m_enabled)
            ApplyGlobalEnableRuntimeState();
        else
            UpdateConnectionStatus();
    }

    // =====================================================================
    // Initialization
    // =====================================================================

    void MainWindow::InitWindow()
    {
        m_isInitializing = true;
        InitializeComponent();
        InitializePageRouting(*this);
        m_windowInitialized = true;
        m_dispatcherQueue = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
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

        ConfigurePageControlCallbacks();
        InitializeShellNavigation();
        InitializeRuntimeComponents();
        InitializeWindowChromeAndTray();
        ApplyInitialSettingsAndVisibility();
        RegisterRuntimeCallbacks();
        StartRuntimeServices();
    }

    void MainWindow::HandleRedirectedActivation()
    {
        AppendDiagnosticLog(L"INFO", L"app", L"Received redirected activation");
        ShowWindowFromTray();
    }

    void MainWindow::InitializeSystemTray()
    {
        auto windowNative = this->try_as<IWindowNative>();
        if (!windowNative)
            return;

        HWND hwnd{};
        if (FAILED(windowNative->get_WindowHandle(&hwnd)) || !hwnd)
            return;

        auto iconPath = GetExecutableDirectory() / L"Assets" / L"logo.ico";
        m_trayController.SetBeforeShowCallback([this]()
        {
            Activate();
        });
        m_trayController.SetLogCallback([this](std::wstring const& level,
                                               std::wstring const& component,
                                               std::wstring const& message)
        {
            AppendDiagnosticLog(level, component, message);
        });
        m_trayController.SetMenuCommandCallback([this](lrp::ui::TrayMenuCommand command)
        {
            HandleTrayMenuCommand(command);
        });
        m_trayController.SetShouldInterceptCloseCallback([this]()
        {
            return !m_isShuttingDown && !m_exitRequested;
        });
        m_trayController.SetVisibilityChangedCallback([this](bool visible)
        {
            if (visible)
            {
                bool resumeWave = !m_lastMedia.title.empty() && m_lastMedia.isPlaying;
                SetSongWaveActive(resumeWave);
                SetHomeMiniWaveActive(resumeWave);
                UpdateConnectionStatus();
                return;
            }

            SetSongWaveActive(false);
            SetHomeMiniWaveActive(false);
            SetLivePulseActive(false);
            HideTrackTransitionSkeleton();
        });
        m_trayController.SetCloseToTrayOnClose(m_closeToTrayOnClose);
        m_trayController.SetTrayLeftClickToggles(m_trayLeftClickToggles);
        m_trayController.SetPresenceEnabled(m_enabled);
        const bool trayInitialized = m_trayController.Initialize(hwnd, iconPath);
        if (!trayInitialized)
            AppendDiagnosticLog(L"WARN", L"tray", L"Tray controller failed to initialize");
    }

    void MainWindow::CleanupSystemTray()
    {
        m_trayController.Shutdown();
    }

    void MainWindow::ShowWindowFromTray()
    {
        m_trayController.ShowFromTray();
        Activate();
    }

    void MainWindow::HideWindowToTray()
    {
        m_trayController.HideToTray();
    }

    void MainWindow::ToggleWindowVisibilityFromTray()
    {
        m_trayController.ToggleVisibility();
    }

    void MainWindow::HandleHomeEnableToggled(bool enabled)
    {
        if (m_enabled == enabled)
            return;

        m_enabled = enabled;
        m_settings.behavior.richPresenceEnabled = enabled;

        ApplyProductiveRuntimeState();
        ApplyCreativeDetectorRuntimeState();
        ApplyGlobalEnableRuntimeState();
        SaveSettings();
        UpdateConnectionStatus();
    }

    void MainWindow::HandleMusicSettingsChanged(lrp::ui::MusicPageSettings const& settings)
    {
        if (settings == lrp::ui::BuildMusicPageSettings(m_settings))
            return;

        lrp::ui::ApplyMusicPageSettings(m_settings, settings);
        SyncMusicSettingsFromControls();
        UpdateUI(m_lastMedia);
        RefreshMediaPresenceOutput();
        SaveSettings();
    }

    void MainWindow::HandleProductivitySettingsChanged(lrp::ui::ProductivityPageSettings const& settings)
    {
        if (settings == lrp::ui::BuildProductivityPageSettings(m_settings))
            return;

        lrp::ui::ApplyProductivityPageSettings(m_settings, settings);
        SyncProductiveSettingsFromControls();
        ApplyProductiveRuntimeState();
        UpdateProductivePreview(m_lastProductiveActivity);
        SaveSettings();
    }

    void MainWindow::HandleProductivitySelectAll()
    {
        auto settings = lrp::ui::BuildProductivityPageSettings(m_settings);
        settings.wordEnabled = true;
        settings.excelEnabled = true;
        settings.powerPointEnabled = true;
        settings.oneNoteEnabled = true;
        settings.accessEnabled = true;
        settings.publisherEnabled = true;
        settings.visioEnabled = true;
        settings.projectEnabled = true;
        settings.codexEnabled = true;
        HandleProductivitySettingsChanged(settings);
    }

    void MainWindow::HandleProductivityDeselectAll()
    {
        auto settings = lrp::ui::BuildProductivityPageSettings(m_settings);
        settings.wordEnabled = false;
        settings.excelEnabled = false;
        settings.powerPointEnabled = false;
        settings.oneNoteEnabled = false;
        settings.accessEnabled = false;
        settings.publisherEnabled = false;
        settings.visioEnabled = false;
        settings.projectEnabled = false;
        settings.codexEnabled = false;
        HandleProductivitySettingsChanged(settings);
    }

    void MainWindow::HandleCreativeSettingsChanged(lrp::ui::CreativePageSettings const& settings)
    {
        if (settings == lrp::ui::BuildCreativePageSettings(m_settings))
            return;

        lrp::ui::ApplyCreativePageSettings(m_settings, settings);
        SyncCreativeSettingsFromControls();
        ApplyCreativeDetectorRuntimeState();
        RefreshCreativePreviewFromCurrentState();
        SaveSettings();
    }

    void MainWindow::HandleCreativeSelectAll()
    {
        auto settings = lrp::ui::BuildCreativePageSettings(m_settings);
        settings.photoshopEnabled = true;
        settings.illustratorEnabled = true;
        settings.premiereEnabled = true;
        settings.afterEffectsEnabled = true;
        settings.inDesignEnabled = true;
        settings.auditionEnabled = true;
        settings.mediaEncoderEnabled = true;
        settings.lightroomEnabled = true;
        settings.lightroomClassicEnabled = true;
        settings.inCopyEnabled = true;
        settings.dreamweaverEnabled = true;
        settings.animateEnabled = true;
        settings.xdEnabled = true;
        settings.bridgeEnabled = true;
        settings.characterAnimatorEnabled = true;
        settings.frescoEnabled = true;
        settings.dimensionEnabled = true;
        settings.substanceEnabled = true;
        settings.acrobatEnabled = true;
        settings.otherAdobeEnabled = true;
        HandleCreativeSettingsChanged(settings);
    }

    void MainWindow::HandleCreativeDeselectAll()
    {
        auto settings = lrp::ui::BuildCreativePageSettings(m_settings);
        settings.photoshopEnabled = false;
        settings.illustratorEnabled = false;
        settings.premiereEnabled = false;
        settings.afterEffectsEnabled = false;
        settings.inDesignEnabled = false;
        settings.auditionEnabled = false;
        settings.mediaEncoderEnabled = false;
        settings.lightroomEnabled = false;
        settings.lightroomClassicEnabled = false;
        settings.inCopyEnabled = false;
        settings.dreamweaverEnabled = false;
        settings.animateEnabled = false;
        settings.xdEnabled = false;
        settings.bridgeEnabled = false;
        settings.characterAnimatorEnabled = false;
        settings.frescoEnabled = false;
        settings.dimensionEnabled = false;
        settings.substanceEnabled = false;
        settings.acrobatEnabled = false;
        settings.otherAdobeEnabled = false;
        HandleCreativeSettingsChanged(settings);
    }

    void MainWindow::HandleSettingsPageSettingsChanged(lrp::ui::SettingsPageSettings const& settings)
    {
        auto currentSettings = lrp::ui::BuildSettingsPageSettings(m_settings);
        currentSettings.blockedAppSitesRaw = settings.blockedAppSitesRaw;
        if (settings == currentSettings)
            return;

        auto const launchOnStartupChanged = currentSettings.launchOnStartup != settings.launchOnStartup;
        auto const startupLaunchModeChanged =
            currentSettings.startMinimizedToTray != settings.startMinimizedToTray &&
            settings.launchOnStartup;

        lrp::ui::ApplySettingsPageSettings(m_settings, settings, false);
        SyncShellSettingsFromControls();

        if (m_presence)
        {
            m_presence->SetShowIdleStatus(m_settings.media.showDefaultIdleStatus);
            m_presence->SetSensitiveKeywordFilter(m_settings.media.sensitiveKeywordFilter);
            m_presence->SetStrictBrowserPrivacy(m_settings.media.strictBrowserPrivacy);
            m_presence->SetSuppressBrowserAlbumArt(m_settings.media.suppressBrowserAlbumArt);
        }

        ApplyThemeMode();
        ApplyActivityTypeOverrides();
        RefreshMediaPresenceOutput();
        SyncProductiveRpcOutput();
        RefreshCreativePreviewFromCurrentState();

        if (launchOnStartupChanged)
        {
            ApplyLaunchOnStartupState(m_settings.behavior.launchOnStartup, true, true);
            return;
        }

        if (startupLaunchModeChanged)
        {
            ApplyLaunchOnStartupState(m_settings.behavior.launchOnStartup, false, true);
            return;
        }

        SaveSettings();
    }

    void MainWindow::HandleApplyBlockedTerms(winrt::hstring const& blockedTermsRaw)
    {
        auto const blockedTerms = std::wstring(blockedTermsRaw);
        if (m_settings.media.blockedAppSiteTermsRaw == blockedTerms)
        {
            SettingsPageControlImpl()->ApplySettings(lrp::ui::BuildSettingsPageSettings(m_settings));
            return;
        }

        m_settings.media.blockedAppSiteTermsRaw = blockedTerms;
        SyncShellSettingsFromControls();
        SettingsPageControlImpl()->ApplySettings(lrp::ui::BuildSettingsPageSettings(m_settings));

        if (m_presence)
            m_presence->SetBlockedAppSiteTerms(ParseBlockedTerms(m_blockedAppSiteTermsRaw));

        SaveSettings();

        if (m_enabled && m_mediaDetector)
            OnMediaChanged(m_mediaDetector->GetCurrentMedia());
    }

    void MainWindow::ApplyLaunchOnStartupState(bool enabled, bool userInitiated, bool persistSettings)
    {
        auto requestVersion = ++m_launchOnStartupRequestVersion;

        auto applyTask = [](winrt::weak_ref<MainWindow> weak,
                            bool enabledValue,
                            bool userInitiatedValue,
                            bool persistValue,
                            uint64_t requestVersionValue) -> winrt::fire_and_forget
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

            if (requestVersionValue != strong->m_launchOnStartupRequestVersion)
            {
                strong->ApplyLaunchOnStartupState(strong->m_settings.behavior.launchOnStartup, false, true);
                co_return;
            }

            auto const shouldPersistResolvedState =
                persistValue &&
                strong->m_settings.behavior.launchOnStartup != applied;

            strong->m_launchOnStartup = applied;
            strong->m_settings.behavior.launchOnStartup = applied;
            strong->SyncShellSettingsFromControls();

            bool initBefore = strong->m_isInitializing;
            strong->m_isInitializing = true;
            strong->SettingsPageControlImpl()->ApplySettings(lrp::ui::BuildSettingsPageSettings(strong->m_settings));
            strong->m_isInitializing = initBefore;

            if (shouldPersistResolvedState)
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

        applyTask(get_weak(), enabled, userInitiated, persistSettings, requestVersion);
    }

    void MainWindow::HandleTrayMenuCommand(lrp::ui::TrayMenuCommand command)
    {
        switch (command)
        {
        case lrp::ui::TrayMenuCommand::TogglePresence:
            HandleHomeEnableToggled(!m_enabled);
            break;
        case lrp::ui::TrayMenuCommand::Exit:
            m_exitRequested = true;
            AppendDiagnosticLog(L"INFO", L"tray", L"Exit requested from system tray");
            CleanupSystemTray();
            Close();
            break;
        default:
            break;
        }
    }

    // =====================================================================
    // Navigation
    // =====================================================================

    bool MainWindow::IsMotionEnabled() const
    {
        if (m_isShuttingDown)
            return false;

        bool reduceMotionRequested = m_reduceMotionRequested;
        try
        {
            auto uiSettings = Windows::UI::ViewManagement::UISettings();
            reduceMotionRequested = !uiSettings.AnimationsEnabled();
        }
        catch (...)
        {
        }

        if (reduceMotionRequested)
            return false;

        if (m_trayController.IsHiddenToTray())
            return false;

        if (m_trayController.IsWindowIconic())
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
        if (!m_windowInitialized)
            return;

        auto selectedItem = args.SelectedItem().try_as<NavigationViewItem>();
        if (!selectedItem)
            return;

        auto tag = unbox_value_or<hstring>(selectedItem.Tag(), L"");
        auto page = lrp::ui::TryAppPageFromTag(tag.c_str());
        if (!page)
        {
            auto tagText = std::wstring(tag.c_str());
            if (tagText.empty())
                tagText = L"<empty>";

            AppendDiagnosticLog(L"WARN", L"navigation", L"Ignored unknown navigation tag: " + tagText);
            if (auto activeNavItem = NavItemForPage(m_activePage))
                NavView().SelectedItem(activeNavItem);
            return;
        }

        auto incoming = PageHost(*page);
        if (!incoming)
        {
            if (auto activeNavItem = NavItemForPage(m_activePage))
                NavView().SelectedItem(activeNavItem);
            return;
        }

        if (m_pageTransitionInProgress)
        {
            if (*page != m_activePage)
                m_queuedPage = *page;
            return;
        }

        if (*page == m_activePage)
            return;

        auto outgoing = PageHost(m_activePage);
        bool forward = lrp::ui::IsForwardAppPageTransition(m_activePage, *page);
        bool involvesSettings = (*page == lrp::ui::AppPage::Settings || m_activePage == lrp::ui::AppPage::Settings);
        double incomingOffset = involvesSettings ? 10.0 : 16.0;
        double outgoingOffset = involvesSettings ? 6.0 : 10.0;

        if (m_pageTransitionStoryboard)
        {
            try { m_pageTransitionStoryboard.Stop(); } catch (...) {}
            m_pageTransitionStoryboard = nullptr;
            m_pageTransitionInProgress = false;
            ShowOnlyPage(m_activePage);
        }

        ResetPageScrollPosition(*page);

        if (!IsMotionEnabled() || !outgoing || outgoing == incoming)
        {
            ShowOnlyPage(*page);
            m_activePage = *page;
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
        auto outgoingPage = m_activePage;
        auto incomingPage = *page;
        storyboard.Completed([weak, outgoingPage, incomingPage](IInspectable const&, IInspectable const&)
        {
            auto strong = weak.get();
            if (!strong)
                return;

            auto outgoingHost = strong->PageHost(outgoingPage);
            auto incomingHost = strong->PageHost(incomingPage);

            if (outgoingHost)
            {
                outgoingHost.Visibility(Visibility::Collapsed);
                outgoingHost.Opacity(1.0);
                if (auto transform = outgoingHost.RenderTransform().try_as<TranslateTransform>())
                    transform.X(0.0);
            }

            if (incomingHost)
            {
                incomingHost.Visibility(Visibility::Visible);
                incomingHost.Opacity(1.0);
                if (auto transform = incomingHost.RenderTransform().try_as<TranslateTransform>())
                    transform.X(0.0);
            }

            strong->m_pageTransitionStoryboard = nullptr;
            strong->m_pageTransitionInProgress = false;

            auto queuedPage = strong->m_queuedPage;
            strong->m_queuedPage.reset();
            if (!queuedPage || *queuedPage == incomingPage)
                return;

            auto queuedHost = strong->PageHost(*queuedPage);
            if (!queuedHost)
                return;

            auto queuedItem = strong->NavItemForPage(*queuedPage);
            if (!queuedItem)
                return;

            auto navView = strong->NavView();
            auto selected = navView.SelectedItem().try_as<NavigationViewItem>();
            auto selectedTag = selected ? unbox_value_or<hstring>(selected.Tag(), L"") : hstring{};
            if (selectedTag != lrp::ui::AppPageTagHString(*queuedPage))
            {
                navView.SelectedItem(queuedItem);
                return;
            }

            auto currentItem = strong->NavItemForPage(incomingPage);
            if (currentItem)
                navView.SelectedItem(currentItem);
            navView.SelectedItem(queuedItem);
        });

        m_queuedPage.reset();
        m_pageTransitionInProgress = true;
        m_activePage = *page;
        m_pageTransitionStoryboard = storyboard;
        storyboard.Begin();
    }

    // =====================================================================
    // Shell runtime helpers
    // =====================================================================

    void MainWindow::ApplyGlobalEnableRuntimeState()
    {
        m_trayController.SetPresenceEnabled(m_enabled);

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
            UpdateUI({});
        }

        UpdateProductivePreview(m_lastProductiveActivity);
        UpdateCreativePreview(m_lastCreativeActivity);
        UpdateHomeCreativePreview();
        UpdateConnectionStatus();
    }

    void MainWindow::ApplyActivityTypeOverrides()
    {
        if (m_presence)
            m_presence->SetActivityTypeOverride(m_mediaActivityTypeOverride);
    }

    void MainWindow::ApplyProductiveRuntimeState()
    {
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
        m_productiveEnabled = m_settings.productive.enabled;
        m_productiveDetectionMode = m_settings.productive.detectionMode;
        m_productiveShowProjectName = m_settings.productive.showProjectName;
        m_productiveWordEnabled = m_settings.productive.wordEnabled;
        m_productiveExcelEnabled = m_settings.productive.excelEnabled;
        m_productivePowerPointEnabled = m_settings.productive.powerPointEnabled;
        m_productiveOneNoteEnabled = m_settings.productive.oneNoteEnabled;
        m_productiveAccessEnabled = m_settings.productive.accessEnabled;
        m_productivePublisherEnabled = m_settings.productive.publisherEnabled;
        m_productiveVisioEnabled = m_settings.productive.visioEnabled;
        m_productiveProjectEnabled = m_settings.productive.projectEnabled;
        m_productiveCodexEnabled = m_settings.productive.codexEnabled;
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
        m_creativeEnabled = m_settings.creative.enabled;
        m_creativePriority = m_settings.creative.priority;
        m_creativeDetectionMode = m_settings.creative.detectionMode;
        m_creativeShowProjectName = m_settings.creative.showProjectName;
        m_creativeShowWindowTitle = m_settings.creative.showWindowTitle;
        m_creativePhotoshopEnabled = m_settings.creative.photoshopEnabled;
        m_creativeIllustratorEnabled = m_settings.creative.illustratorEnabled;
        m_creativePremiereEnabled = m_settings.creative.premiereEnabled;
        m_creativeAfterEffectsEnabled = m_settings.creative.afterEffectsEnabled;
        m_creativeInDesignEnabled = m_settings.creative.inDesignEnabled;
        m_creativeAuditionEnabled = m_settings.creative.auditionEnabled;
        m_creativeMediaEncoderEnabled = m_settings.creative.mediaEncoderEnabled;
        m_creativeLightroomEnabled = m_settings.creative.lightroomEnabled;
        m_creativeLightroomClassicEnabled = m_settings.creative.lightroomClassicEnabled;
        m_creativeInCopyEnabled = m_settings.creative.inCopyEnabled;
        m_creativeDreamweaverEnabled = m_settings.creative.dreamweaverEnabled;
        m_creativeAnimateEnabled = m_settings.creative.animateEnabled;
        m_creativeXdEnabled = m_settings.creative.xdEnabled;
        m_creativeBridgeEnabled = m_settings.creative.bridgeEnabled;
        m_creativeCharacterAnimatorEnabled = m_settings.creative.characterAnimatorEnabled;
        m_creativeFrescoEnabled = m_settings.creative.frescoEnabled;
        m_creativeDimensionEnabled = m_settings.creative.dimensionEnabled;
        m_creativeSubstanceEnabled = m_settings.creative.substanceEnabled;
        m_creativeAcrobatEnabled = m_settings.creative.acrobatEnabled;
        m_creativeOtherAdobeEnabled = m_settings.creative.otherAdobeEnabled;
        m_creativePrivacyMode = m_settings.creative.privacyMode;
        m_creativeIdleBehavior = m_settings.creative.idleBehavior;

        if (m_lastCreativeAcceptedActivity.active && !IsCreativeAppEnabled(m_lastCreativeAcceptedActivity))
        {
            m_lastCreativeAcceptedActivity = {};
            m_lastCreativeActiveSeenAt = {};
        }
    }

    void MainWindow::ApplyCreativeDetectorRuntimeState()
    {
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

    void MainWindow::OnResetSettingsClicked(IInspectable const&, RoutedEventArgs const&)
    {
        const lrp::settings::PersistedSettings defaults;
        BoolValueGuard initializingGuard(m_isInitializing, true);
        ApplyPersistedSettingsSnapshot(defaults);

        ApplyThemeMode();
        ApplyLaunchOnStartupState(defaults.behavior.launchOnStartup, false, true);
        ApplyProductiveRuntimeState();
        ApplyCreativeDetectorRuntimeState();
        SaveSettings();
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
            }

            ApplyLaunchOnStartupState(m_launchOnStartup, false, true);
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

        if (!m_windowInitialized)
        {
            if (m_lifetimeToken)
                m_lifetimeToken->store(false);

            SetBrowserHintUpdateCallback({});
            return;
        }

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
        m_queuedPage.reset();

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
        AppendDiagnosticLog(L"INFO", L"bridge", L"Browser native-host transport stopped");
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
        ProductivityPageControlImpl()->ApplyState(BuildProductivityPageState(info));
        UpdateProductiveAppIcon(m_productiveEnabled && info.active && IsProductiveAppEnabled(info) ? info : ProductiveActivityInfo{});
        UpdateHomeProductivePreview();
    }

    void MainWindow::UpdateCreativePreview(const CreativeActivityInfo& info)
    {
        CreativePageControlImpl()->ApplyState(BuildCreativePageState(info));

        CreativeActivityInfo iconInfo{};
        bool showingHeld = false;
        CreativeActivityInfo displayInfo{};
        if (TryGetEffectiveCreativeActivityForRpc(displayInfo, showingHeld) &&
            m_creativePrivacyMode != CreativePrivacyMode::Private)
        {
            iconInfo = displayInfo;
        }
        else if (info.active && IsCreativeAppEnabled(info) && m_creativePrivacyMode != CreativePrivacyMode::Private)
        {
            iconInfo = info;
        }

        UpdateCreativeAppIcon(iconInfo);
        UpdateHomeCreativePreview();
    }

    void MainWindow::UpdateHomeProductivePreview()
    {
        HomePageControlImpl()->ApplyState(BuildHomePageState());
    }

    void MainWindow::UpdateHomeCreativePreview()
    {
        HomePageControlImpl()->ApplyState(BuildHomePageState());
    }

    void MainWindow::UpdateProductiveAppIcon(const ProductiveActivityInfo& info)
    {
        auto showFallback = [this]()
        {
            ProductivityPageControlImpl()->ClearDetectedAppIcon();
            HomePageControlImpl()->ClearProductiveAppIcon();
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

                strongThis->ProductivityPageControlImpl()->ClearDetectedAppIcon();
                strongThis->HomePageControlImpl()->ClearProductiveAppIcon();
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

            strongThis->ProductivityPageControlImpl()->SetDetectedAppIcon(resolvedImage);
            strongThis->HomePageControlImpl()->SetProductiveAppIcon(resolvedImage);
        };

        m_productiveIconUpdateTask = updateTask(get_strong(), info, m_lastProductiveIconCacheKey, curatedPath, requestId);
    }

    void MainWindow::UpdateCreativeAppIcon(const CreativeActivityInfo& info)
    {
        auto showFallback = [this]()
        {
            CreativePageControlImpl()->ClearDetectedAppIcon();
            HomePageControlImpl()->ClearCreativeAppIcon();
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

                strongThis->CreativePageControlImpl()->ClearDetectedAppIcon();
                strongThis->HomePageControlImpl()->ClearCreativeAppIcon();
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

            strongThis->CreativePageControlImpl()->SetDetectedAppIcon(resolvedImage);
            strongThis->HomePageControlImpl()->SetCreativeAppIcon(resolvedImage);
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
        int posSec = static_cast<int>(info.position.count());
        int durSec = static_cast<int>(info.duration.count());
        int hintPos = 0;
        int hintDur = 0;
        if (TryGetLiveHintTimeline(info, hintPos, hintDur))
        {
            posSec = hintPos;
            durSec = hintDur;
        }

        auto musicState = BuildMusicPageState(info, posSec, durSec);
        MusicPageControlImpl()->ApplyState(musicState);
        HomePageControlImpl()->ApplyState(BuildHomePageState(posSec, durSec));

        SetHomeMiniProgress(musicState.progressPercent);
        SetHomeMiniWaveActive(hasMedia && info.isPlaying);

        if (!hasMedia)
        {
            UpdateThumbnail(info);
            HideTrackTransitionSkeleton();
            m_lastUiTrackKey.clear();
            m_lastUiSourceKey.clear();
            return;
        }

        UpdateThumbnail(info);

        m_lastUiTrackKey = std::move(trackKey);
        m_lastUiSourceKey = std::move(sourceKey);

        auto motionEnabled = IsMotionEnabled();

        if (animateSource && motionEnabled)
            HomePageControlImpl()->PulseSourceCard();
        if (animateTrack)
        {
            if (motionEnabled)
            {
                HomePageControlImpl()->PulseMiniPlayer();
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
        MusicPageControlImpl()->ApplyState(BuildMusicPageState(info));
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
            lrp::ui::SettingsPageState state{};
            state.diagnosticsLogText = hstring(m_diagnosticLog.JoinLines());
            SettingsPageControlImpl()->ApplyState(state);
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
        auto settings = BuildPersistedSettingsSnapshot();

        JsonObject root;
        root.Insert(L"schemaVersion", JsonValue::CreateNumberValue(2));
        root.Insert(L"generatedAtUtc", JsonValue::CreateStringValue(BuildUtcTimestampString()));

        root.Insert(L"ShowTimestamps", JsonValue::CreateBooleanValue(settings.media.showTimestamps));
        root.Insert(L"ShowSourceApp", JsonValue::CreateBooleanValue(settings.media.showSource));
        root.Insert(L"SourceDebugMode", JsonValue::CreateBooleanValue(settings.media.sourceDebugMode));
        root.Insert(L"ShowPaused", JsonValue::CreateBooleanValue(settings.media.showPaused));
        root.Insert(L"ShowAlbumArt", JsonValue::CreateBooleanValue(settings.media.showAlbumArt));
        root.Insert(L"ShowDefaultIdleStatus", JsonValue::CreateBooleanValue(settings.media.showDefaultIdleStatus));

        root.Insert(L"RichPresenceEnabled", JsonValue::CreateBooleanValue(settings.behavior.richPresenceEnabled));
        root.Insert(L"CloseToTrayOnClose", JsonValue::CreateBooleanValue(settings.behavior.closeToTrayOnClose));
        root.Insert(L"LaunchOnStartup", JsonValue::CreateBooleanValue(settings.behavior.launchOnStartup));
        root.Insert(L"StartMinimizedToTray", JsonValue::CreateBooleanValue(settings.behavior.startMinimizedToTray));
        root.Insert(L"TrayLeftClickToggles", JsonValue::CreateBooleanValue(settings.behavior.trayLeftClickToggles));

        root.Insert(L"SensitiveKeywordFilter", JsonValue::CreateBooleanValue(settings.media.sensitiveKeywordFilter));
        root.Insert(L"StrictBrowserPrivacy", JsonValue::CreateBooleanValue(settings.media.strictBrowserPrivacy));
        root.Insert(L"SuppressBrowserAlbumArt", JsonValue::CreateBooleanValue(settings.media.suppressBrowserAlbumArt));
        root.Insert(L"ThemeMode", JsonValue::CreateStringValue(ToSettingString(settings.behavior.themeMode)));
        root.Insert(L"BlockedAppSiteTerms", JsonValue::CreateStringValue(settings.media.blockedAppSiteTermsRaw));
        root.Insert(L"MediaActivityType", JsonValue::CreateStringValue(ToSettingStringActivityTypeOverride(settings.media.activityTypeOverride)));
        root.Insert(L"CreativeActivityType", JsonValue::CreateStringValue(ToSettingStringActivityTypeOverride(settings.creative.activityTypeOverride)));
        root.Insert(L"ProductiveActivityType", JsonValue::CreateStringValue(ToSettingStringActivityTypeOverride(settings.productive.activityTypeOverride)));

        root.Insert(L"ProductiveEnabled", JsonValue::CreateBooleanValue(settings.productive.enabled));
        root.Insert(L"ProductiveDetectionMode", JsonValue::CreateStringValue(ToSettingString(settings.productive.detectionMode)));
        root.Insert(L"ProductiveShowProjectName", JsonValue::CreateBooleanValue(settings.productive.showProjectName));
        root.Insert(L"ProductiveAppWordEnabled", JsonValue::CreateBooleanValue(settings.productive.wordEnabled));
        root.Insert(L"ProductiveAppExcelEnabled", JsonValue::CreateBooleanValue(settings.productive.excelEnabled));
        root.Insert(L"ProductiveAppPowerPointEnabled", JsonValue::CreateBooleanValue(settings.productive.powerPointEnabled));
        root.Insert(L"ProductiveAppOneNoteEnabled", JsonValue::CreateBooleanValue(settings.productive.oneNoteEnabled));
        root.Insert(L"ProductiveAppAccessEnabled", JsonValue::CreateBooleanValue(settings.productive.accessEnabled));
        root.Insert(L"ProductiveAppPublisherEnabled", JsonValue::CreateBooleanValue(settings.productive.publisherEnabled));
        root.Insert(L"ProductiveAppVisioEnabled", JsonValue::CreateBooleanValue(settings.productive.visioEnabled));
        root.Insert(L"ProductiveAppProjectEnabled", JsonValue::CreateBooleanValue(settings.productive.projectEnabled));
        root.Insert(L"ProductiveAppCodexEnabled", JsonValue::CreateBooleanValue(settings.productive.codexEnabled));
        root.Insert(L"CreativeEnabled", JsonValue::CreateBooleanValue(settings.creative.enabled));
        root.Insert(L"CreativePriority", JsonValue::CreateStringValue(ToSettingString(settings.creative.priority)));
        root.Insert(L"CreativeDetectionMode", JsonValue::CreateStringValue(ToSettingString(settings.creative.detectionMode)));
        root.Insert(L"CreativeShowProjectName", JsonValue::CreateBooleanValue(settings.creative.showProjectName));
        root.Insert(L"CreativeShowWindowTitle", JsonValue::CreateBooleanValue(settings.creative.showWindowTitle));
        root.Insert(L"CreativeAppPhotoshopEnabled", JsonValue::CreateBooleanValue(settings.creative.photoshopEnabled));
        root.Insert(L"CreativeAppIllustratorEnabled", JsonValue::CreateBooleanValue(settings.creative.illustratorEnabled));
        root.Insert(L"CreativeAppPremiereEnabled", JsonValue::CreateBooleanValue(settings.creative.premiereEnabled));
        root.Insert(L"CreativeAppAfterEffectsEnabled", JsonValue::CreateBooleanValue(settings.creative.afterEffectsEnabled));
        root.Insert(L"CreativeAppInDesignEnabled", JsonValue::CreateBooleanValue(settings.creative.inDesignEnabled));
        root.Insert(L"CreativeAppAuditionEnabled", JsonValue::CreateBooleanValue(settings.creative.auditionEnabled));
        root.Insert(L"CreativeAppMediaEncoderEnabled", JsonValue::CreateBooleanValue(settings.creative.mediaEncoderEnabled));
        root.Insert(L"CreativeAppLightroomEnabled", JsonValue::CreateBooleanValue(settings.creative.lightroomEnabled));
        root.Insert(L"CreativeAppLightroomClassicEnabled", JsonValue::CreateBooleanValue(settings.creative.lightroomClassicEnabled));
        root.Insert(L"CreativeAppInCopyEnabled", JsonValue::CreateBooleanValue(settings.creative.inCopyEnabled));
        root.Insert(L"CreativeAppDreamweaverEnabled", JsonValue::CreateBooleanValue(settings.creative.dreamweaverEnabled));
        root.Insert(L"CreativeAppAnimateEnabled", JsonValue::CreateBooleanValue(settings.creative.animateEnabled));
        root.Insert(L"CreativeAppXdEnabled", JsonValue::CreateBooleanValue(settings.creative.xdEnabled));
        root.Insert(L"CreativeAppBridgeEnabled", JsonValue::CreateBooleanValue(settings.creative.bridgeEnabled));
        root.Insert(L"CreativeAppCharacterAnimatorEnabled", JsonValue::CreateBooleanValue(settings.creative.characterAnimatorEnabled));
        root.Insert(L"CreativeAppFrescoEnabled", JsonValue::CreateBooleanValue(settings.creative.frescoEnabled));
        root.Insert(L"CreativeAppDimensionEnabled", JsonValue::CreateBooleanValue(settings.creative.dimensionEnabled));
        root.Insert(L"CreativeAppSubstanceEnabled", JsonValue::CreateBooleanValue(settings.creative.substanceEnabled));
        root.Insert(L"CreativeAppAcrobatEnabled", JsonValue::CreateBooleanValue(settings.creative.acrobatEnabled));
        root.Insert(L"CreativeAppOtherAdobeEnabled", JsonValue::CreateBooleanValue(settings.creative.otherAdobeEnabled));
        root.Insert(L"CreativePrivacyMode", JsonValue::CreateStringValue(ToSettingString(settings.creative.privacyMode)));
        root.Insert(L"CreativeIdleBehavior", JsonValue::CreateStringValue(ToSettingString(settings.creative.idleBehavior)));

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
            MusicPageControlImpl()->ClearAlbumThumbnail();
            HomePageControlImpl()->ClearMiniThumbnail();
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
                    strongThis->MusicPageControlImpl()->SetAlbumThumbnail(bitmapImage);
                    strongThis->HomePageControlImpl()->SetMiniThumbnail(bitmapImage);
                }
            }
            catch (winrt::hresult_canceled const&)
            {
                // Cancellation is expected, just clear the thumbnails
                if (!strongThis->m_isShuttingDown)
                {
                    strongThis->MusicPageControlImpl()->ClearAlbumThumbnail();
                    strongThis->HomePageControlImpl()->ClearMiniThumbnail();
                }
            }
            catch (...)
            {
                // On any other error, clear the thumbnails
                if (!strongThis->m_isShuttingDown)
                {
                    strongThis->MusicPageControlImpl()->ClearAlbumThumbnail();
                    strongThis->HomePageControlImpl()->ClearMiniThumbnail();
                }
            }
        };

        m_thumbnailUpdateTask = updateTask(get_strong(), info.thumbnail);
    }

    void MainWindow::SetLivePulseActive(bool active)
    {
        HomePageControlImpl()->SetLivePulseActive(active && !m_isShuttingDown, IsMotionEnabled());
    }

    void MainWindow::ShowTrackTransitionSkeleton()
    {
        HomePageControlImpl()->ShowTrackTransitionSkeleton(IsMotionEnabled());
    }

    void MainWindow::HideTrackTransitionSkeleton()
    {
        HomePageControlImpl()->HideTrackTransitionSkeleton();
    }

    // =====================================================================
    // Connection status
    // =====================================================================

    void MainWindow::UpdateConnectionStatus()
    {
        bool connected = m_presence && m_presence->IsConnected();

        bool liveActive = false;
        if (connected && m_enabled)
        {
            const bool activePlayback = !m_lastMedia.title.empty() && m_lastMedia.isPlaying;
            const bool recentPresencePush = m_lastPresencePushPlaying &&
                ((std::chrono::steady_clock::now() - m_lastPresencePushAt) <= std::chrono::seconds(4));
            liveActive = activePlayback || recentPresencePush;
        }

        HomePageControlImpl()->ApplyState(BuildHomePageState());
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
        MusicPageControlImpl()->SetSongProgress(
            progressPercent,
            IsMotionEnabled(),
            m_lastMedia.isPlaying,
            !m_lastMedia.title.empty());
    }

    void MainWindow::UpdateSongWaveClip(double progressPercent)
    {
        MusicPageControlImpl()->SetSongProgress(
            progressPercent,
            IsMotionEnabled(),
            m_lastMedia.isPlaying,
            !m_lastMedia.title.empty());
    }

    void MainWindow::SetSongWaveActive(bool active)
    {
        MusicPageControlImpl()->SetSongWaveActive(active && !m_isShuttingDown, IsMotionEnabled());
    }

    void MainWindow::SetHomeMiniProgress(double progressPercent)
    {
        HomePageControlImpl()->SetMiniProgress(
            progressPercent,
            IsMotionEnabled(),
            m_lastMedia.isPlaying,
            !m_lastMedia.title.empty());
    }

    void MainWindow::UpdateHomeMiniWaveClip(double progressPercent)
    {
        HomePageControlImpl()->SetMiniProgress(
            progressPercent,
            IsMotionEnabled(),
            m_lastMedia.isPlaying,
            !m_lastMedia.title.empty());
    }

    void MainWindow::SetHomeMiniWaveActive(bool active)
    {
        HomePageControlImpl()->SetMiniWaveActive(active && !m_isShuttingDown, IsMotionEnabled());
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
                int durSec = static_cast<int>(info.duration.count());
                int displayPos = static_cast<int>(info.position.count());

                int liveHintPos = 0;
                int liveHintDur = 0;
                if (TryGetLiveHintTimeline(info, liveHintPos, liveHintDur))
                {
                    displayPos = liveHintPos;
                    durSec = liveHintDur;
                }
                else if (info.isPlaying)
                {
                    auto now = std::chrono::system_clock::now();
                    if (info.startTime.time_since_epoch().count() > 0)
                    {
                        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                            now - info.startTime);
                        auto estimatedPos = elapsed.count();

                        if (estimatedPos < 0)
                            estimatedPos = 0;

                        displayPos = static_cast<int>(estimatedPos);
                    }
                }

                if (displayPos < 0)
                    displayPos = 0;
                if (durSec > 0 && displayPos > durSec)
                    displayPos = durSec;

                auto musicState = BuildMusicPageState(info, displayPos, durSec);
                MusicPageControlImpl()->ApplyState(musicState);
                HomePageControlImpl()->ApplyState(BuildHomePageState(displayPos, durSec));
                SetHomeMiniProgress(musicState.progressPercent);
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
        auto settings = m_settings;
        settings.behavior.richPresenceEnabled = m_enabled;
        return settings;
    }

    void MainWindow::ApplyPersistedSettingsSnapshot(const lrp::settings::PersistedSettings& settings)
    {
        m_settings = settings;
        m_enabled = settings.behavior.richPresenceEnabled;
        SyncShellSettingsFromControls();
        SyncMusicSettingsFromControls();
        SyncProductiveSettingsFromControls();
        SyncCreativeSettingsFromControls();

        MusicPageControlImpl()->ApplySettings(lrp::ui::BuildMusicPageSettings(m_settings));
        SettingsPageControlImpl()->ApplySettings(lrp::ui::BuildSettingsPageSettings(m_settings));
        ProductivityPageControlImpl()->ApplySettings(lrp::ui::BuildProductivityPageSettings(m_settings));
        CreativePageControlImpl()->ApplySettings(lrp::ui::BuildCreativePageSettings(m_settings));

        if (m_presence)
        {
            m_presence->SetShowTimestamps(m_settings.media.showTimestamps);
            m_presence->SetShowSource(m_settings.media.showSource);
            m_presence->SetShowPaused(m_settings.media.showPaused);
            m_presence->SetShowAlbumArt(m_settings.media.showAlbumArt);
            m_presence->SetShowIdleStatus(m_settings.media.showDefaultIdleStatus);
            m_presence->SetSensitiveKeywordFilter(m_settings.media.sensitiveKeywordFilter);
            m_presence->SetStrictBrowserPrivacy(m_settings.media.strictBrowserPrivacy);
            m_presence->SetSuppressBrowserAlbumArt(m_settings.media.suppressBrowserAlbumArt);
            m_presence->SetBlockedAppSiteTerms(ParseBlockedTerms(m_blockedAppSiteTermsRaw));
        }

        ApplyThemeMode();
        ApplyActivityTypeOverrides();
        UpdateConnectionStatus();
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

        UpdateProductivePreview(m_lastProductiveActivity);
        ApplyCreativeDetectorRuntimeState();
        RefreshCreativePreviewFromCurrentState();
        UpdateUI(m_lastMedia);
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
