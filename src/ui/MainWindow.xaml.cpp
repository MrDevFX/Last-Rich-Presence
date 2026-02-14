#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <winrt/Windows.Networking.h>
#include <winrt/Windows.Networking.Sockets.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.UI.ViewManagement.h>

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
        static constexpr int PageTransitionMs = 260;
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

    std::wstring NormalizeForMatch(const std::wstring& value)
    {
        std::wstring out;
        out.reserve(value.size());

        for (wchar_t ch : value)
        {
            auto c = towlower(ch);
            if (iswalnum(c) || c == L' ')
                out.push_back(c);
        }

        return TrimCopy(out);
    }

    std::vector<std::wstring> ParseBlockedTerms(const std::wstring& raw)
    {
        std::vector<std::wstring> terms;
        std::wstring current;

        for (wchar_t ch : raw)
        {
            if (ch == L',' || ch == L';' || ch == L'\n' || ch == L'\r')
            {
                auto term = TrimCopy(current);
                if (!term.empty())
                    terms.push_back(term);
                current.clear();
                continue;
            }

            current.push_back(ch);
        }

        auto tail = TrimCopy(current);
        if (!tail.empty())
            terms.push_back(tail);

        return terms;
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
        m_lifetimeToken = std::make_shared<std::atomic<bool>>(true);
        AppendDiagnosticLog(L"INFO", L"app", L"Main window initialized");

        // Window size and title bar
        auto appWindow = this->AppWindow();
        if (appWindow)
        {
            Windows::Graphics::SizeInt32 size{ 1000, 700 };
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
                presenter.IsResizable(true);
                presenter.IsMinimizable(true);
                presenter.IsMaximizable(true);
            }

            // Enforce minimum window size
            appWindow.Changed([](Microsoft::UI::Windowing::AppWindow const& sender, Microsoft::UI::Windowing::AppWindowChangedEventArgs const& args)
            {
                if (args.DidSizeChange())
                {
                    auto currentSize = sender.Size();
                    bool needsResize = false;
                    int32_t newWidth = currentSize.Width;
                    int32_t newHeight = currentSize.Height;

                    if (currentSize.Width < 600)  { newWidth = 600;  needsResize = true; }
                    if (currentSize.Height < 450) { newHeight = 450; needsResize = true; }
                    if (needsResize)
                        sender.Resize({ newWidth, newHeight });
                }
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

        if (m_startMinimizedToTray && m_dispatcherQueue)
        {
            m_dispatcherQueue.TryEnqueue([this]()
            {
                if (!m_isShuttingDown)
                    HideWindowToTray();
            });
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
        m_mediaDetector->Start();
        m_presence->Initialize();
        StartProgressTimer();
        StartBrowserHintServer();
        AppendDiagnosticLog(L"INFO", L"bridge", L"Browser hint bridge started");
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
        if (!m_windowHandle || !IsWindowVisible(m_windowHandle))
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
                        strong->AppendDiagnosticLog(L"WARN", L"settings", L"Startup enable request was denied by system policy/user setting");
                }
                else
                {
                    startupTask.Disable();
                    applied = false;
                }
            }
            catch (...)
            {
                applied = false;
                if (userInitiatedValue)
                    strong->AppendDiagnosticLog(L"WARN", L"settings", L"Launch on startup is not available on this install");
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

        auto incoming = pageForTag(tag);
        if (!incoming)
            return;

        if (tag == hstring(m_activePageTag))
            return;

        auto outgoing = pageForTag(hstring(m_activePageTag));

        auto pageOrder = [](hstring const& value)
        {
            if (value == L"Home") return 0;
            if (value == L"Music") return 1;
            if (value == L"Productivity") return 2;
            if (value == L"Creative") return 3;
            if (value == L"Settings") return 4;
            return 0;
        };

        bool forward = pageOrder(tag) >= pageOrder(hstring(m_activePageTag));
        bool involvesSettings = (tag == L"Settings" || hstring(m_activePageTag) == L"Settings");
        double incomingOffset = involvesSettings ? 14.0 : 28.0;
        double outgoingOffset = involvesSettings ? 8.0 : 16.0;

        if (m_pageTransitionStoryboard)
        {
            try { m_pageTransitionStoryboard.Stop(); } catch (...) {}
            m_pageTransitionStoryboard = nullptr;

            HomePage().Visibility(m_activePageTag == L"Home" ? Visibility::Visible : Visibility::Collapsed);
            MusicPage().Visibility(m_activePageTag == L"Music" ? Visibility::Visible : Visibility::Collapsed);
            ProductivityPage().Visibility(m_activePageTag == L"Productivity" ? Visibility::Visible : Visibility::Collapsed);
            CreativePage().Visibility(m_activePageTag == L"Creative" ? Visibility::Visible : Visibility::Collapsed);
            SettingsPage().Visibility(m_activePageTag == L"Settings" ? Visibility::Visible : Visibility::Collapsed);
        }

        if (!IsMotionEnabled() || !outgoing || outgoing == incoming)
        {
            HomePage().Visibility(tag == L"Home" ? Visibility::Visible : Visibility::Collapsed);
            MusicPage().Visibility(tag == L"Music" ? Visibility::Visible : Visibility::Collapsed);
            ProductivityPage().Visibility(tag == L"Productivity" ? Visibility::Visible : Visibility::Collapsed);
            CreativePage().Visibility(tag == L"Creative" ? Visibility::Visible : Visibility::Collapsed);
            SettingsPage().Visibility(tag == L"Settings" ? Visibility::Visible : Visibility::Collapsed);
            m_activePageTag = tag.c_str();
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
        });

        m_activePageTag = tag.c_str();
        m_pageTransitionStoryboard = storyboard;
        storyboard.Begin();
    }

    // =====================================================================
    // Toggle handlers
    // =====================================================================

    void MainWindow::OnEnableToggled(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_isInitializing) return;

        m_enabled = EnableToggle().IsOn();

        if (m_enabled)
        {
            m_mediaDetector->Start();
            if (!m_presence->IsConnected())
                m_presence->Initialize();
        }
        else
        {
            m_mediaDetector->Stop();
            m_presence->Shutdown();
            m_lastPresencePushPlaying = false;

            // Reset UI
            StatusIndicator().Fill(Application::Current().Resources().Lookup(box_value(L"StatusDisconnectedBrush")).as<Brush>());
            StatusText().Text(L"Disabled");
            StatusSubtext().Text(L"Rich Presence is off");

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
            HomeMiniPlayIcon().Glyph(L"\xE768");
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

        UpdateConnectionStatus();
    }

    void MainWindow::OnTimestampToggled(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_isInitializing) return;
        m_presence->SetShowTimestamps(TimestampToggle().IsOn());
        SaveSettings();
        m_presence->RefreshPresence();
    }

    void MainWindow::OnSourceToggled(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_isInitializing) return;
        m_presence->SetShowSource(SourceToggle().IsOn());
        SaveSettings();

        UpdateSourceBadge(m_lastMedia);

        m_presence->RefreshPresence();
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
        m_presence->RefreshPresence();
    }

    void MainWindow::OnAlbumArtToggled(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_isInitializing) return;
        m_presence->SetShowAlbumArt(AlbumArtToggle().IsOn());
        SaveSettings();
        m_presence->RefreshPresence();
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
        m_presence->RefreshPresence();
    }

    void MainWindow::OnStrictBrowserPrivacyToggled(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_isInitializing) return;
        m_strictBrowserPrivacy = StrictBrowserPrivacyToggle().IsOn();
        m_presence->SetStrictBrowserPrivacy(m_strictBrowserPrivacy);
        SaveSettings();
        m_presence->RefreshPresence();
    }

    void MainWindow::OnSuppressBrowserArtToggled(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_isInitializing) return;
        m_suppressBrowserAlbumArt = SuppressBrowserArtToggle().IsOn();
        m_presence->SetSuppressBrowserAlbumArt(m_suppressBrowserAlbumArt);
        SaveSettings();
        m_presence->RefreshPresence();
    }

    void MainWindow::OnApplyBlockedAppSitesClicked(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_isInitializing) return;

        m_blockedAppSiteTermsRaw = BlockedAppSitesBox().Text().c_str();
        auto terms = ParseBlockedTerms(m_blockedAppSiteTermsRaw);
        m_presence->SetBlockedAppSiteTerms(std::move(terms));
        SaveSettings();
        m_presence->RefreshPresence();
        AppendDiagnosticLog(L"INFO", L"settings", L"Updated blocked apps/sites list");
    }

    void MainWindow::OnResetSettingsClicked(IInspectable const&, RoutedEventArgs const&)
    {
        bool initBefore = m_isInitializing;
        m_isInitializing = true;

        TimestampToggle().IsOn(true);
        SourceToggle().IsOn(true);
        SourceDebugToggle().IsOn(false);
        PausedToggle().IsOn(true);
        AlbumArtToggle().IsOn(true);

        CloseToTrayToggle().IsOn(true);
        LaunchOnStartupToggle().IsOn(false);
        StartMinimizedToggle().IsOn(false);
        TrayLeftClickToggle().IsOn(true);

        SensitiveFilterToggle().IsOn(true);
        StrictBrowserPrivacyToggle().IsOn(false);
        SuppressBrowserArtToggle().IsOn(false);
        BlockedAppSitesBox().Text(L"");

        m_sourceDebugMode = false;
        m_closeToTrayOnClose = true;
        m_launchOnStartup = false;
        m_startMinimizedToTray = false;
        m_trayLeftClickToggles = true;
        m_sensitiveKeywordFilter = true;
        m_strictBrowserPrivacy = false;
        m_suppressBrowserAlbumArt = false;
        m_blockedAppSiteTermsRaw.clear();

        m_presence->SetShowTimestamps(true);
        m_presence->SetShowSource(true);
        m_presence->SetShowPaused(true);
        m_presence->SetShowAlbumArt(true);
        m_presence->SetSensitiveKeywordFilter(true);
        m_presence->SetStrictBrowserPrivacy(false);
        m_presence->SetSuppressBrowserAlbumArt(false);
        m_presence->SetBlockedAppSiteTerms({});

        m_isInitializing = initBefore;
        ApplyLaunchOnStartupState(false, false);
        SaveSettings();
        UpdateSourceBadge(m_lastMedia);
        m_presence->RefreshPresence();
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

            auto readBool = [&](const wchar_t* key, bool fallback)
            {
                if (!root.HasKey(key)) return fallback;
                auto value = root.Lookup(key);
                if (value.ValueType() == JsonValueType::Boolean) return value.GetBoolean();
                if (value.ValueType() == JsonValueType::Number) return value.GetNumber() != 0.0;
                return fallback;
            };

            auto readString = [&](const wchar_t* key, const std::wstring& fallback)
            {
                if (!root.HasKey(key)) return fallback;
                auto value = root.Lookup(key);
                if (value.ValueType() == JsonValueType::String)
                    return std::wstring(value.GetString().c_str());
                return fallback;
            };

            bool initBefore = m_isInitializing;
            m_isInitializing = true;

            TimestampToggle().IsOn(readBool(L"ShowTimestamps", TimestampToggle().IsOn()));
            SourceToggle().IsOn(readBool(L"ShowSourceApp", SourceToggle().IsOn()));
            SourceDebugToggle().IsOn(readBool(L"SourceDebugMode", SourceDebugToggle().IsOn()));
            PausedToggle().IsOn(readBool(L"ShowPaused", PausedToggle().IsOn()));
            AlbumArtToggle().IsOn(readBool(L"ShowAlbumArt", AlbumArtToggle().IsOn()));

            CloseToTrayToggle().IsOn(readBool(L"CloseToTrayOnClose", CloseToTrayToggle().IsOn()));
            LaunchOnStartupToggle().IsOn(readBool(L"LaunchOnStartup", LaunchOnStartupToggle().IsOn()));
            StartMinimizedToggle().IsOn(readBool(L"StartMinimizedToTray", StartMinimizedToggle().IsOn()));
            TrayLeftClickToggle().IsOn(readBool(L"TrayLeftClickToggles", TrayLeftClickToggle().IsOn()));

            SensitiveFilterToggle().IsOn(readBool(L"SensitiveKeywordFilter", SensitiveFilterToggle().IsOn()));
            StrictBrowserPrivacyToggle().IsOn(readBool(L"StrictBrowserPrivacy", StrictBrowserPrivacyToggle().IsOn()));
            SuppressBrowserArtToggle().IsOn(readBool(L"SuppressBrowserAlbumArt", SuppressBrowserArtToggle().IsOn()));

            m_blockedAppSiteTermsRaw = readString(L"BlockedAppSiteTerms", BlockedAppSitesBox().Text().c_str());
            BlockedAppSitesBox().Text(m_blockedAppSiteTermsRaw);

            m_sourceDebugMode = SourceDebugToggle().IsOn();
            m_closeToTrayOnClose = CloseToTrayToggle().IsOn();
            m_launchOnStartup = LaunchOnStartupToggle().IsOn();
            m_startMinimizedToTray = StartMinimizedToggle().IsOn();
            m_trayLeftClickToggles = TrayLeftClickToggle().IsOn();
            m_sensitiveKeywordFilter = SensitiveFilterToggle().IsOn();
            m_strictBrowserPrivacy = StrictBrowserPrivacyToggle().IsOn();
            m_suppressBrowserAlbumArt = SuppressBrowserArtToggle().IsOn();

            m_presence->SetShowTimestamps(TimestampToggle().IsOn());
            m_presence->SetShowSource(SourceToggle().IsOn());
            m_presence->SetShowPaused(PausedToggle().IsOn());
            m_presence->SetShowAlbumArt(AlbumArtToggle().IsOn());
            m_presence->SetSensitiveKeywordFilter(m_sensitiveKeywordFilter);
            m_presence->SetStrictBrowserPrivacy(m_strictBrowserPrivacy);
            m_presence->SetSuppressBrowserAlbumArt(m_suppressBrowserAlbumArt);
            m_presence->SetBlockedAppSiteTerms(ParseBlockedTerms(m_blockedAppSiteTermsRaw));

            m_isInitializing = initBefore;

            ApplyLaunchOnStartupState(m_launchOnStartup, false);

            SaveSettings();
            UpdateSourceBadge(m_lastMedia);
            m_presence->RefreshPresence();
            AppendDiagnosticLog(L"INFO", L"settings", L"Imported settings from " + inputPath.wstring());
        }
        catch (...)
        {
            AppendDiagnosticLog(L"ERROR", L"settings", L"Failed to import settings");
        }
    }

    void MainWindow::OnClearDiagnosticsClicked(IInspectable const&, RoutedEventArgs const&)
    {
        m_diagnosticLines.clear();
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
            HomeMiniPlayIcon().Glyph(L"\xE768");
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
        std::time_t now = std::time(nullptr);
        std::tm localTime{};
        localtime_s(&localTime, &now);

        wchar_t timeBuf[16]{};
        wcsftime(timeBuf, 16, L"%H:%M:%S", &localTime);

        std::wstring line = L"[" + std::wstring(timeBuf) + L"] [" + level + L"] [" + component + L"] " + message;

        m_diagnosticLines.push_back(std::move(line));
        constexpr size_t kMaxLogLines = 180;
        while (m_diagnosticLines.size() > kMaxLogLines)
            m_diagnosticLines.pop_front();

        RefreshDiagnosticsPanel();
    }


    void MainWindow::RefreshDiagnosticsPanel()
    {
        std::wstring combined;
        for (const auto& line : m_diagnosticLines)
        {
            if (!combined.empty()) combined += L"\n";
            combined += line;
        }

        try
        {
            DiagnosticsLogBox().Text(combined);
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

        JsonArray lines;
        for (const auto& line : m_diagnosticLines)
            lines.Append(JsonValue::CreateStringValue(line));
        root.Insert(L"logs", lines);

        auto json = root.Stringify();
        return std::wstring(json.c_str());
    }

    std::wstring MainWindow::BuildSettingsSnapshotJson()
    {
        JsonObject root;
        root.Insert(L"schemaVersion", JsonValue::CreateNumberValue(1));
        root.Insert(L"generatedAtUtc", JsonValue::CreateStringValue(BuildUtcTimestampString()));

        root.Insert(L"ShowTimestamps", JsonValue::CreateBooleanValue(TimestampToggle().IsOn()));
        root.Insert(L"ShowSourceApp", JsonValue::CreateBooleanValue(SourceToggle().IsOn()));
        root.Insert(L"SourceDebugMode", JsonValue::CreateBooleanValue(SourceDebugToggle().IsOn()));
        root.Insert(L"ShowPaused", JsonValue::CreateBooleanValue(PausedToggle().IsOn()));
        root.Insert(L"ShowAlbumArt", JsonValue::CreateBooleanValue(AlbumArtToggle().IsOn()));

        root.Insert(L"CloseToTrayOnClose", JsonValue::CreateBooleanValue(m_closeToTrayOnClose));
        root.Insert(L"LaunchOnStartup", JsonValue::CreateBooleanValue(m_launchOnStartup));
        root.Insert(L"StartMinimizedToTray", JsonValue::CreateBooleanValue(m_startMinimizedToTray));
        root.Insert(L"TrayLeftClickToggles", JsonValue::CreateBooleanValue(m_trayLeftClickToggles));

        root.Insert(L"SensitiveKeywordFilter", JsonValue::CreateBooleanValue(m_sensitiveKeywordFilter));
        root.Insert(L"StrictBrowserPrivacy", JsonValue::CreateBooleanValue(m_strictBrowserPrivacy));
        root.Insert(L"SuppressBrowserAlbumArt", JsonValue::CreateBooleanValue(m_suppressBrowserAlbumArt));
        root.Insert(L"BlockedAppSiteTerms", JsonValue::CreateStringValue(m_blockedAppSiteTermsRaw));

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
            if (m_isShuttingDown || !m_enabled) return;

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

    void MainWindow::LoadSettings()
    {
        try
        {
            auto localSettings = Windows::Storage::ApplicationData::Current().LocalSettings();
            auto values = localSettings.Values();

            auto readBool = [&](const wchar_t* key, bool fallback)
            {
                auto value = values.TryLookup(key);
                if (!value) return fallback;
                try { return unbox_value<bool>(value); }
                catch (...) { return fallback; }
            };

            auto readString = [&](const wchar_t* key, const std::wstring& fallback)
            {
                auto value = values.TryLookup(key);
                if (!value) return fallback;
                try { return std::wstring(unbox_value<hstring>(value).c_str()); }
                catch (...) { return fallback; }
            };

            auto ts = readBool(L"ShowTimestamps", TimestampToggle().IsOn());
            TimestampToggle().IsOn(ts);
            m_presence->SetShowTimestamps(ts);

            auto src = readBool(L"ShowSourceApp", SourceToggle().IsOn());
            SourceToggle().IsOn(src);
            m_presence->SetShowSource(src);

            m_sourceDebugMode = readBool(L"SourceDebugMode", SourceDebugToggle().IsOn());
            SourceDebugToggle().IsOn(m_sourceDebugMode);

            auto paused = readBool(L"ShowPaused", PausedToggle().IsOn());
            PausedToggle().IsOn(paused);
            m_presence->SetShowPaused(paused);

            auto art = readBool(L"ShowAlbumArt", AlbumArtToggle().IsOn());
            AlbumArtToggle().IsOn(art);
            m_presence->SetShowAlbumArt(art);

            m_closeToTrayOnClose = readBool(L"CloseToTrayOnClose", CloseToTrayToggle().IsOn());
            CloseToTrayToggle().IsOn(m_closeToTrayOnClose);

            m_launchOnStartup = readBool(L"LaunchOnStartup", LaunchOnStartupToggle().IsOn());
            LaunchOnStartupToggle().IsOn(m_launchOnStartup);

            m_startMinimizedToTray = readBool(L"StartMinimizedToTray", StartMinimizedToggle().IsOn());
            StartMinimizedToggle().IsOn(m_startMinimizedToTray);

            m_trayLeftClickToggles = readBool(L"TrayLeftClickToggles", TrayLeftClickToggle().IsOn());
            TrayLeftClickToggle().IsOn(m_trayLeftClickToggles);

            m_sensitiveKeywordFilter = readBool(L"SensitiveKeywordFilter", SensitiveFilterToggle().IsOn());
            SensitiveFilterToggle().IsOn(m_sensitiveKeywordFilter);
            m_presence->SetSensitiveKeywordFilter(m_sensitiveKeywordFilter);

            m_strictBrowserPrivacy = readBool(L"StrictBrowserPrivacy", StrictBrowserPrivacyToggle().IsOn());
            StrictBrowserPrivacyToggle().IsOn(m_strictBrowserPrivacy);
            m_presence->SetStrictBrowserPrivacy(m_strictBrowserPrivacy);

            m_suppressBrowserAlbumArt = readBool(L"SuppressBrowserAlbumArt", SuppressBrowserArtToggle().IsOn());
            SuppressBrowserArtToggle().IsOn(m_suppressBrowserAlbumArt);
            m_presence->SetSuppressBrowserAlbumArt(m_suppressBrowserAlbumArt);

            m_blockedAppSiteTermsRaw = readString(L"BlockedAppSiteTerms", L"");
            BlockedAppSitesBox().Text(m_blockedAppSiteTermsRaw);
            m_presence->SetBlockedAppSiteTerms(ParseBlockedTerms(m_blockedAppSiteTermsRaw));
        }
        catch (...) {}
    }

    void MainWindow::SaveSettings()
    {
        try
        {
            auto localSettings = Windows::Storage::ApplicationData::Current().LocalSettings();
            auto values = localSettings.Values();
            values.Insert(L"ShowTimestamps", box_value(TimestampToggle().IsOn()));
            values.Insert(L"ShowSourceApp", box_value(SourceToggle().IsOn()));
            values.Insert(L"SourceDebugMode", box_value(SourceDebugToggle().IsOn()));
            values.Insert(L"ShowPaused", box_value(PausedToggle().IsOn()));
            values.Insert(L"ShowAlbumArt", box_value(AlbumArtToggle().IsOn()));

            values.Insert(L"CloseToTrayOnClose", box_value(m_closeToTrayOnClose));
            values.Insert(L"LaunchOnStartup", box_value(m_launchOnStartup));
            values.Insert(L"StartMinimizedToTray", box_value(m_startMinimizedToTray));
            values.Insert(L"TrayLeftClickToggles", box_value(m_trayLeftClickToggles));

            values.Insert(L"SensitiveKeywordFilter", box_value(m_sensitiveKeywordFilter));
            values.Insert(L"StrictBrowserPrivacy", box_value(m_strictBrowserPrivacy));
            values.Insert(L"SuppressBrowserAlbumArt", box_value(m_suppressBrowserAlbumArt));
            values.Insert(L"BlockedAppSiteTerms", box_value(hstring(m_blockedAppSiteTermsRaw)));
        }
        catch (...) {}
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
