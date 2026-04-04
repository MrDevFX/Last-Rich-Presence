#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "ActivityLaneCoordinator.h"
#include "ActivityPresenceHelpers.h"
#include "AppPage.h"
#include "BrowserNativeMessaging.h"
#include "DiagnosticsLog.h"
#include "DiscordRPC.h"
#include "SettingsImport.h"
#include "SettingsModels.h"
#include "SettingsUiBinder.h"
#include "TextUtilities.h"
#include "WindowTrayController.h"

namespace
{
    using namespace std::chrono_literals;

    void Expect(bool condition, const char* message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    template <typename T>
    void ExpectEqual(const T& actual, const T& expected, const char* message)
    {
        if (!(actual == expected))
            throw std::runtime_error(message);
    }

    template <typename TPredicate>
    void WaitUntil(TPredicate predicate, std::chrono::milliseconds timeout, const char* message)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (predicate())
                return;

            std::this_thread::sleep_for(20ms);
        }

        if (!predicate())
            throw std::runtime_error(message);
    }

    bool Contains(const std::string& value, const std::string& fragment)
    {
        return value.find(fragment) != std::string::npos;
    }

    class FakeDiscordIpcTransport final : public IDiscordIpcTransport
    {
    public:
        struct ReadPlan
        {
            DiscordTransportResult result{ DiscordTransportResult::Ok };
            uint32_t opcode{ 0 };
            std::string data;
        };

        struct WriteRecord
        {
            uint32_t opcode{ 0 };
            std::string data;
        };

        void EnqueueConnectResult(DiscordTransportResult result)
        {
            std::lock_guard lock(m_mutex);
            m_connectResults.push_back(result);
        }

        void EnqueueWriteResult(DiscordTransportResult result)
        {
            std::lock_guard lock(m_mutex);
            m_writeResults.push_back(result);
        }

        void EnqueuePeekResult(DiscordTransportResult result)
        {
            std::lock_guard lock(m_mutex);
            m_peekResults.push_back(result);
        }

        void EnqueueAvailable(uint32_t available)
        {
            std::lock_guard lock(m_mutex);
            m_availableValues.push_back(available);
        }

        void EnqueueRead(DiscordTransportResult result, uint32_t opcode = 0, std::string data = {})
        {
            std::lock_guard lock(m_mutex);
            m_reads.push_back({ result, opcode, std::move(data) });
        }

        bool WaitForWrites(size_t count, std::chrono::milliseconds timeout)
        {
            std::unique_lock lock(m_mutex);
            return m_cv.wait_for(lock, timeout, [&] { return m_writes.size() >= count; });
        }

        size_t WriteCount() const
        {
            std::lock_guard lock(m_mutex);
            return m_writes.size();
        }

        size_t ReadCount() const
        {
            std::lock_guard lock(m_mutex);
            return m_readCount;
        }

        std::vector<WriteRecord> WritesSnapshot() const
        {
            std::lock_guard lock(m_mutex);
            return m_writes;
        }

        DiscordTransportResult Connect() override
        {
            std::lock_guard lock(m_mutex);
            ++m_connectAttempts;
            auto result = PopOrDefault(m_connectResults, DiscordTransportResult::Ok);
            if (result == DiscordTransportResult::Ok)
                m_connected = true;
            return result;
        }

        void Disconnect() override
        {
            std::lock_guard lock(m_mutex);
            m_connected = false;
            ++m_disconnectCount;
        }

        DiscordTransportResult WriteFrame(uint32_t opcode, const std::string& data) override
        {
            std::lock_guard lock(m_mutex);
            m_writes.push_back({ opcode, data });
            auto result = PopOrDefault(m_writeResults, DiscordTransportResult::Ok);
            m_cv.notify_all();
            return result;
        }

        DiscordTransportResult ReadFrame(uint32_t& opcode, std::string& data) override
        {
            std::lock_guard lock(m_mutex);
            if (m_reads.empty())
                return DiscordTransportResult::ReadFailed;

            auto plan = m_reads.front();
            m_reads.pop_front();

            if (plan.result == DiscordTransportResult::Ok)
            {
                opcode = plan.opcode;
                data = std::move(plan.data);
            }
            else
            {
                opcode = 0;
                data.clear();
            }

            ++m_readCount;
            m_cv.notify_all();
            return plan.result;
        }

        DiscordTransportResult PeekAvailable(uint32_t& available) override
        {
            std::lock_guard lock(m_mutex);
            auto result = PopOrDefault(m_peekResults, DiscordTransportResult::Ok);
            if (result != DiscordTransportResult::Ok)
            {
                available = 0;
                return result;
            }

            if (!m_availableValues.empty())
            {
                available = m_availableValues.front();
                m_availableValues.pop_front();
                return DiscordTransportResult::Ok;
            }

            available = m_reads.empty() ? 0u : 1u;
            return DiscordTransportResult::Ok;
        }

    private:
        template <typename T>
        static T PopOrDefault(std::deque<T>& queue, T fallback)
        {
            if (queue.empty())
                return fallback;

            auto value = queue.front();
            queue.pop_front();
            return value;
        }

        mutable std::mutex m_mutex;
        std::condition_variable m_cv;
        std::deque<DiscordTransportResult> m_connectResults;
        std::deque<DiscordTransportResult> m_writeResults;
        std::deque<DiscordTransportResult> m_peekResults;
        std::deque<uint32_t> m_availableValues;
        std::deque<ReadPlan> m_reads;
        std::vector<WriteRecord> m_writes;
        size_t m_readCount{ 0 };
        bool m_connected{ false };
        int m_connectAttempts{ 0 };
        int m_disconnectCount{ 0 };
    };

    std::string DescribeWriteKind(const std::string& payload)
    {
        if (Contains(payload, "\"client_id\""))
            return "handshake";
        if (Contains(payload, "\"activity\":null"))
            return "clear";
        if (Contains(payload, "\"activity\":{"))
            return "update";
        return "other";
    }

    std::string DescribeWrites(const std::vector<FakeDiscordIpcTransport::WriteRecord>& writes)
    {
        std::string description;
        for (size_t index = 0; index < writes.size(); ++index)
        {
            if (!description.empty())
                description += ",";
            description += DescribeWriteKind(writes[index].data);
        }
        return description;
    }

    void TestTextUtilities()
    {
        ExpectEqual(lrp::NormalizeForMatch(L"  Hello, WORLD!  "), std::wstring(L"hello world"),
            "NormalizeForMatch should lowercase and strip punctuation.");
        Expect(lrp::TitlesLikelyMatch(L"Fireflies - Owl City", L"fireflies"),
            "TitlesLikelyMatch should match normalized substrings.");

        auto terms = lrp::ParseDelimitedTerms(L"spotify.exe; youtube.com\r\ntwitch");
        Expect(terms.size() == 3, "ParseDelimitedTerms should split comma/semicolon/newline values.");
        ExpectEqual(terms[0], std::wstring(L"spotify.exe"), "ParseDelimitedTerms should preserve the first term.");
        ExpectEqual(terms[1], std::wstring(L"youtube.com"), "ParseDelimitedTerms should preserve the second term.");
        ExpectEqual(terms[2], std::wstring(L"twitch"), "ParseDelimitedTerms should preserve the last term.");

        auto hint = lrp::ExtractProjectHint(
            L"Poster.psd - Adobe Photoshop 2026",
            [](const std::wstring& value)
            {
                return lrp::NormalizeForMatch(value).find(L"photoshop") != std::wstring::npos;
            });
        ExpectEqual(hint, std::wstring(L"Poster.psd"), "ExtractProjectHint should strip known suffixes.");

        auto codexHint = lrp::ExtractProjectHint(
            L"src\\ui\\MainWindow.xaml.cpp - Codex",
            [](const std::wstring& value)
            {
                auto normalized = lrp::NormalizeForMatch(value);
                return normalized == L"codex" || normalized == L"openai codex";
            });
        ExpectEqual(codexHint, std::wstring(L"src\\ui\\MainWindow.xaml.cpp"),
            "ExtractProjectHint should strip Codex window suffixes.");
    }

    void TestSettingsModels()
    {
        using namespace lrp::settings;

        ExpectEqual(NormalizeSettingValue(L" Prefer_Media "), std::wstring(L"prefer-media"),
            "NormalizeSettingValue should trim, lowercase, and normalize separators.");

        Expect(ParseCreativePriorityMode(L"prefer_media") == CreativePriorityMode::PreferMedia,
            "ParseCreativePriorityMode should accept legacy underscore values.");
        Expect(ParseCreativePriorityMode(L"Prefer-Creative") == CreativePriorityMode::PreferCreative,
            "ParseCreativePriorityMode should accept hyphenated values.");
        ExpectEqual(ToSettingString(CreativePriorityMode::PreferMedia), std::wstring(L"prefer_media"),
            "Creative priority settings should keep the persisted legacy spelling.");

        Expect(ParseCreativePrivacyMode(L"app-only") == CreativePrivacyMode::AppOnly,
            "ParseCreativePrivacyMode should accept hyphenated values.");
        Expect(ParseCreativeIdleBehavior(L"fallback_media") == CreativeIdleBehavior::ClearImmediately,
            "ParseCreativeIdleBehavior should keep the legacy fallback-media alias.");
        Expect(ParseCreativeDetectionMode(L"visible-window-only") == CreativeDetectionMode::VisibleWindowOnly,
            "ParseCreativeDetectionMode should accept normalized values.");
        Expect(ParseProductiveDetectionMode(L"foreground_only") == ProductiveDetectionMode::ForegroundOnly,
            "ParseProductiveDetectionMode should accept legacy underscore values.");

        PersistedSettings defaults;
        Expect(defaults.behavior.richPresenceEnabled,
            "Global rich presence should be enabled by default.");
        Expect(defaults.productive.showProjectName,
            "Productive settings should show project names by default.");
        Expect(defaults.productive.codexEnabled,
            "Productive settings should enable Codex by default.");

        Expect(ThemeModeFromComboIndex(2) == AppThemeMode::FollowSystem,
            "ThemeModeFromComboIndex should map the system option.");
        Expect(ThemeModeToComboIndex(AppThemeMode::Dark) == 1,
            "ThemeModeToComboIndex should map dark mode.");
        ExpectEqual(ThemeModeLabel(AppThemeMode::FollowSystem), std::wstring(L"System"),
            "ThemeModeLabel should describe the system option.");

        Expect(ParseActivityTypeOverride(L"Listening") == 2,
            "ParseActivityTypeOverride should accept named activity types.");
        Expect(ParseActivityTypeOverride(L"5") == 5,
            "ParseActivityTypeOverride should accept numeric activity types.");
        Expect(ActivityTypeOverrideFromComboIndex(4) == 5,
            "ActivityTypeOverrideFromComboIndex should map the competing option.");
        Expect(ActivityTypeOverrideToComboIndex(-1) == 0,
            "ActivityTypeOverrideToComboIndex should map auto to the first option.");
        ExpectEqual(ToSettingStringActivityTypeOverride(3), std::wstring(L"3"),
            "Activity type persistence should remain numeric for compatibility.");
    }

    void TestAppPageHelpers()
    {
        using namespace lrp::ui;

        Expect(kAppPageOrder.size() == 5, "App page order should contain every shell page.");
        ExpectEqual(kAppPageOrder[0], AppPage::Home, "Home should remain the first page.");
        ExpectEqual(kAppPageOrder[1], AppPage::Music, "Music should remain the second page.");
        ExpectEqual(kAppPageOrder[2], AppPage::Creative, "Creative should remain ordered before Productivity.");
        ExpectEqual(kAppPageOrder[3], AppPage::Productivity, "Productivity should remain ordered after Creative.");
        ExpectEqual(kAppPageOrder[4], AppPage::Settings, "Settings should remain the final page.");

        ExpectEqual(AppPageTag(AppPage::Home), std::wstring_view(L"Home"), "AppPageTag should map Home.");
        ExpectEqual(AppPageTag(AppPage::Music), std::wstring_view(L"Music"), "AppPageTag should map Music.");
        ExpectEqual(AppPageTag(AppPage::Creative), std::wstring_view(L"Creative"), "AppPageTag should map Creative.");
        ExpectEqual(AppPageTag(AppPage::Productivity), std::wstring_view(L"Productivity"), "AppPageTag should map Productivity.");
        ExpectEqual(AppPageTag(AppPage::Settings), std::wstring_view(L"Settings"), "AppPageTag should map Settings.");

        ExpectEqual(AppPageIndex(AppPage::Home), 0, "AppPageIndex should map Home to 0.");
        ExpectEqual(AppPageIndex(AppPage::Music), 1, "AppPageIndex should map Music to 1.");
        ExpectEqual(AppPageIndex(AppPage::Creative), 2, "AppPageIndex should map Creative to 2.");
        ExpectEqual(AppPageIndex(AppPage::Productivity), 3, "AppPageIndex should map Productivity to 3.");
        ExpectEqual(AppPageIndex(AppPage::Settings), 4, "AppPageIndex should map Settings to the last slot.");
        Expect(TryAppPageFromTag(L"Home").value_or(AppPage::Settings) == AppPage::Home,
            "TryAppPageFromTag should parse Home.");
        Expect(TryAppPageFromTag(L"Music").value_or(AppPage::Home) == AppPage::Music,
            "TryAppPageFromTag should parse Music.");
        Expect(TryAppPageFromTag(L"Creative").value_or(AppPage::Home) == AppPage::Creative,
            "TryAppPageFromTag should parse Creative.");
        Expect(TryAppPageFromTag(L"Productivity").value_or(AppPage::Home) == AppPage::Productivity,
            "TryAppPageFromTag should parse Productivity.");
        Expect(TryAppPageFromTag(L"Settings").value_or(AppPage::Home) == AppPage::Settings,
            "TryAppPageFromTag should parse Settings.");
        Expect(TryAppPageFromTag(L"Unknown").has_value() == false,
            "TryAppPageFromTag should reject unknown tags.");
        Expect(!IsKnownAppPageTag(L"Unknown"),
            "IsKnownAppPageTag should reject unknown tags.");
        Expect(TryAppPageAtIndex(2).value_or(AppPage::Home) == AppPage::Creative,
            "TryAppPageAtIndex should resolve the shared page ordering.");
        Expect(TryAppPageAtIndex(99).has_value() == false,
            "TryAppPageAtIndex should reject out-of-range indices.");

        for (size_t index = 0; index < kAppPageOrder.size(); ++index)
        {
            auto page = kAppPageOrder[index];
            auto parsedIndex = TryAppPageIndex(page);
            Expect(parsedIndex.has_value(),
                "TryAppPageIndex should resolve every known page.");
            ExpectEqual(parsedIndex.value_or(-1), static_cast<int>(index),
                "TryAppPageIndex should match the canonical page ordering.");

            auto tag = AppPageTag(page);
            auto parsedPage = TryAppPageFromTag(tag);
            Expect(parsedPage.has_value(),
                "Known page tags should round-trip through TryAppPageFromTag.");
            ExpectEqual(parsedPage.value_or(AppPage::Home), page,
                "AppPage tags should round-trip to the same page.");
        }

        Expect(IsForwardAppPageTransition(AppPage::Music, AppPage::Creative),
            "Music-to-Creative should be treated as a forward transition.");
        Expect(IsForwardAppPageTransition(AppPage::Creative, AppPage::Productivity),
            "Creative-to-Productivity should be treated as a forward transition.");
        Expect(!IsForwardAppPageTransition(AppPage::Productivity, AppPage::Music),
            "Productivity-to-Music should be treated as a backward transition.");
        Expect(!IsForwardAppPageTransition(AppPage::Settings, AppPage::Creative),
            "Settings-to-Creative should be treated as a backward transition.");
    }

    void TestBrowserNativeMessagingContract()
    {
        using namespace lrp::browser;

        ExpectEqual(std::wstring(kBrowserNativeHostArgument), std::wstring(L"--browser-native-host"),
            "The browser native host launch argument must remain stable.");
        ExpectEqual(std::wstring(kNativeHostName), std::wstring(L"com.lastprojects.lastrichpresence"),
            "The browser native host name must remain stable for browser registration.");
        ExpectEqual(std::wstring(kBrowserHintPipeName), std::wstring(L"\\\\.\\pipe\\LastRichPresence.BrowserHints"),
            "The browser hint pipe name must remain stable across host and app.");
        ExpectEqual(std::wstring(kBrowserExtensionId), std::wstring(L"hodkjclfknpkaockiingkiijbbjekebj"),
            "The native host allow-list must remain aligned with the extension ID.");
        ExpectEqual(std::wstring(kBrowserExtensionOrigin), std::wstring(L"chrome-extension://hodkjclfknpkaockiingkiijbbjekebj/"),
            "The browser native host manifest origin must remain stable.");
        Expect(kMaxBrowserNativeMessageBytes == 64 * 1024,
            "The browser native messaging payload cap should remain at 64 KiB.");
        Expect(IsAcceptedNativeMessagingOrigin(kBrowserExtensionOrigin),
            "The committed extension origin must remain accepted by the native host.");
        Expect(!IsAcceptedNativeMessagingOrigin(L"chrome-extension://aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/"),
            "Unexpected extension origins must be rejected by the native host.");
        Expect(IsAllowedNativeMessagingParentProcessName(L"chrome.exe"),
            "Chrome should remain an allowed native-host parent process.");
        Expect(IsAllowedNativeMessagingParentProcessName(L"msedge.exe"),
            "Edge should remain an allowed native-host parent process.");
        Expect(!IsAllowedNativeMessagingParentProcessName(L"powershell.exe"),
            "Arbitrary local parent processes must not be trusted for browser hints.");
    }

    void TestWindowTrayVisibilityHelper()
    {
        using namespace lrp::ui;

        Expect(IsWindowVisibleToUser(true, false, false),
            "A shown non-iconic window should be treated as visible to the user.");
        Expect(!IsWindowVisibleToUser(true, true, false),
            "A minimized taskbar window should be treated as restorable, not visible.");
        Expect(!IsWindowVisibleToUser(true, false, true),
            "A hidden-to-tray window should not be treated as visible even if the raw window visibility flag is still set.");
        Expect(!IsWindowVisibleToUser(false, false, false),
            "A hidden window should not be treated as visible.");
    }

    void TestSettingsUiBinder()
    {
        using namespace lrp::settings;

        PersistedSettings settings;
        settings.media.showTimestamps = false;
        settings.media.showSource = false;
        settings.media.sourceDebugMode = true;
        settings.media.showPaused = false;
        settings.media.showAlbumArt = false;
        settings.media.showDefaultIdleStatus = false;
        settings.media.sensitiveKeywordFilter = false;
        settings.media.strictBrowserPrivacy = true;
        settings.media.suppressBrowserAlbumArt = true;
        settings.media.blockedAppSiteTermsRaw = L"teams.exe;meet.google.com";
        settings.media.activityTypeOverride = 2;

        settings.behavior.closeToTrayOnClose = false;
        settings.behavior.launchOnStartup = true;
        settings.behavior.startMinimizedToTray = true;
        settings.behavior.trayLeftClickToggles = false;
        settings.behavior.themeMode = AppThemeMode::Dark;

        settings.productive.enabled = false;
        settings.productive.detectionMode = ProductiveDetectionMode::ForegroundOnly;
        settings.productive.showProjectName = false;
        settings.productive.activityTypeOverride = 5;
        settings.productive.wordEnabled = false;
        settings.productive.codexEnabled = false;

        settings.creative.enabled = false;
        settings.creative.priority = CreativePriorityMode::PreferCreative;
        settings.creative.detectionMode = CreativeDetectionMode::VisibleWindowOnly;
        settings.creative.showProjectName = false;
        settings.creative.showWindowTitle = true;
        settings.creative.activityTypeOverride = 3;
        settings.creative.photoshopEnabled = false;
        settings.creative.bridgeEnabled = false;
        settings.creative.privacyMode = CreativePrivacyMode::Private;
        settings.creative.idleBehavior = CreativeIdleBehavior::ClearImmediately;

        auto musicSettings = lrp::ui::BuildMusicPageSettings(settings);
        Expect(!musicSettings.showTimestamps, "Music UI binder should project showTimestamps.");
        Expect(!musicSettings.showSource, "Music UI binder should project showSource.");
        Expect(musicSettings.sourceDebugMode, "Music UI binder should project sourceDebugMode.");
        Expect(!musicSettings.showPaused, "Music UI binder should project showPaused.");
        Expect(!musicSettings.showAlbumArt, "Music UI binder should project showAlbumArt.");

        auto shellSettings = lrp::ui::BuildSettingsPageSettings(settings);
        Expect(!shellSettings.closeToTrayOnClose, "Settings UI binder should project close-to-tray.");
        Expect(shellSettings.launchOnStartup, "Settings UI binder should project launch-on-startup.");
        Expect(shellSettings.startMinimizedToTray, "Settings UI binder should project start-minimized.");
        Expect(!shellSettings.trayLeftClickToggles, "Settings UI binder should project tray toggle behavior.");
        Expect(!shellSettings.showDefaultIdleStatus, "Settings UI binder should project idle status.");
        ExpectEqual(shellSettings.mediaActivityTypeIndex, 2, "Settings UI binder should map media activity type.");
        ExpectEqual(shellSettings.creativeActivityTypeIndex, 3, "Settings UI binder should map creative activity type.");
        ExpectEqual(shellSettings.productiveActivityTypeIndex, 4, "Settings UI binder should map productive activity type.");
        Expect(!shellSettings.sensitiveKeywordFilter, "Settings UI binder should project keyword filter.");
        Expect(shellSettings.strictBrowserPrivacy, "Settings UI binder should project strict browser privacy.");
        Expect(shellSettings.suppressBrowserAlbumArt, "Settings UI binder should project browser album art suppression.");
        ExpectEqual(std::wstring(shellSettings.blockedAppSitesRaw.c_str()), std::wstring(L"teams.exe;meet.google.com"),
            "Settings UI binder should project blocked app/site terms.");
        ExpectEqual(shellSettings.themeModeIndex, 1, "Settings UI binder should map dark theme mode.");

        auto productiveSettings = lrp::ui::BuildProductivityPageSettings(settings);
        Expect(!productiveSettings.enabled, "Productivity UI binder should project enable state.");
        Expect(productiveSettings.detectionMode == ProductiveDetectionMode::ForegroundOnly,
            "Productivity UI binder should project detection mode.");
        Expect(!productiveSettings.showProjectName, "Productivity UI binder should project project-name toggle.");
        Expect(!productiveSettings.wordEnabled, "Productivity UI binder should project Word filter.");
        Expect(!productiveSettings.codexEnabled, "Productivity UI binder should project Codex filter.");

        auto creativeSettings = lrp::ui::BuildCreativePageSettings(settings);
        Expect(!creativeSettings.enabled, "Creative UI binder should project enable state.");
        Expect(creativeSettings.priority == CreativePriorityMode::PreferCreative,
            "Creative UI binder should project priority mode.");
        Expect(creativeSettings.detectionMode == CreativeDetectionMode::VisibleWindowOnly,
            "Creative UI binder should project detection mode.");
        Expect(!creativeSettings.showProjectName, "Creative UI binder should project project-name toggle.");
        Expect(creativeSettings.showWindowTitle, "Creative UI binder should project window-title toggle.");
        Expect(!creativeSettings.photoshopEnabled, "Creative UI binder should project Photoshop filter.");
        Expect(!creativeSettings.bridgeEnabled, "Creative UI binder should project Bridge filter.");
        Expect(creativeSettings.privacyMode == CreativePrivacyMode::Private,
            "Creative UI binder should project privacy mode.");
        Expect(creativeSettings.idleBehavior == CreativeIdleBehavior::ClearImmediately,
            "Creative UI binder should project idle behavior.");

        PersistedSettings applied;
        applied.media.blockedAppSiteTermsRaw = L"keep-existing";

        lrp::ui::ApplyMusicPageSettings(applied, musicSettings);
        lrp::ui::ApplySettingsPageSettings(applied, shellSettings, false);
        lrp::ui::ApplyProductivityPageSettings(applied, productiveSettings);
        lrp::ui::ApplyCreativePageSettings(applied, creativeSettings);

        Expect(!applied.media.showTimestamps, "Music UI binder should apply showTimestamps.");
        Expect(!applied.media.showSource, "Music UI binder should apply showSource.");
        Expect(applied.media.sourceDebugMode, "Music UI binder should apply sourceDebugMode.");
        Expect(!applied.media.showPaused, "Music UI binder should apply showPaused.");
        Expect(!applied.media.showAlbumArt, "Music UI binder should apply showAlbumArt.");
        ExpectEqual(applied.media.blockedAppSiteTermsRaw, std::wstring(L"keep-existing"),
            "Settings UI binder should preserve blocked terms when Apply-only commit is deferred.");
        Expect(!applied.behavior.closeToTrayOnClose, "Settings UI binder should apply close-to-tray.");
        Expect(applied.behavior.launchOnStartup, "Settings UI binder should apply launch-on-startup.");
        Expect(applied.behavior.startMinimizedToTray, "Settings UI binder should apply start-minimized.");
        Expect(!applied.behavior.trayLeftClickToggles, "Settings UI binder should apply tray toggle behavior.");
        Expect(applied.behavior.themeMode == AppThemeMode::Dark, "Settings UI binder should apply theme mode.");
        Expect(applied.media.activityTypeOverride == 2, "Settings UI binder should apply media activity type.");
        Expect(applied.productive.activityTypeOverride == 5, "Settings UI binder should apply productive activity type.");
        Expect(applied.creative.activityTypeOverride == 3, "Settings UI binder should apply creative activity type.");
        Expect(!applied.productive.enabled, "Productivity UI binder should apply enable state.");
        Expect(!applied.productive.wordEnabled, "Productivity UI binder should apply Word filter.");
        Expect(!applied.creative.enabled, "Creative UI binder should apply enable state.");
        Expect(!applied.creative.photoshopEnabled, "Creative UI binder should apply Photoshop filter.");

        lrp::ui::ApplySettingsPageSettings(applied, shellSettings, true);
        ExpectEqual(applied.media.blockedAppSiteTermsRaw, std::wstring(L"teams.exe;meet.google.com"),
            "Settings UI binder should commit blocked terms when requested.");
    }

    void TestActivityPresenceHelpers()
    {
        Expect(lrp::ResolveRpcTargetPidForDetectedActivity(4242) == 0u,
            "Rich Presence should use the caller PID instead of detected external process IDs.");
    }

    void TestActivityLaneCoordinator()
    {
        lrp::ActivityLaneState state;

        auto setTransition = lrp::ResolveActivityLaneTransition(
            state,
            true,
            true,
            true,
            false,
            false,
            false,
            false,
            L"word|docA",
            lrp::ActivityLaneReason::AppClosed);
        ExpectEqual(setTransition.action, lrp::ActivityLaneAction::Set,
            "First active lane snapshot should publish a set transition.");
        ExpectEqual(setTransition.reason, lrp::ActivityLaneReason::ActiveMatch,
            "Active candidate should report the active-match reason.");
        Expect(setTransition.shouldEnsureRunning,
            "Enabled lanes should request the RPC worker to be running.");
        Expect(!setTransition.duplicate,
            "First active lane snapshot must not be treated as a duplicate.");
        lrp::CommitActivityLaneTransition(state, setTransition);

        auto duplicateActive = lrp::ResolveActivityLaneTransition(
            state,
            true,
            true,
            true,
            false,
            false,
            false,
            false,
            L"word|docA",
            lrp::ActivityLaneReason::AppClosed);
        ExpectEqual(duplicateActive.action, lrp::ActivityLaneAction::Update,
            "Subsequent active snapshots should remain update-class transitions.");
        Expect(duplicateActive.duplicate,
            "Identical active snapshots should be deduped instead of republished.");

        auto heldTransition = lrp::ResolveActivityLaneTransition(
            state,
            true,
            true,
            true,
            true,
            false,
            false,
            false,
            L"word|docA",
            lrp::ActivityLaneReason::DetectorIdle);
        ExpectEqual(heldTransition.action, lrp::ActivityLaneAction::Hold,
            "Held activity should be represented explicitly.");
        ExpectEqual(heldTransition.reason, lrp::ActivityLaneReason::HeldActivity,
            "Held activity should log the held reason.");
        Expect(!heldTransition.duplicate,
            "A held transition should not be deduped against a live active transition.");
        lrp::CommitActivityLaneTransition(state, heldTransition);

        auto clearTransition = lrp::ResolveActivityLaneTransition(
            state,
            true,
            true,
            false,
            false,
            false,
            false,
            false,
            L"",
            lrp::ActivityLaneReason::AppClosed);
        ExpectEqual(clearTransition.action, lrp::ActivityLaneAction::Clear,
            "Missing candidates should clear the lane.");
        ExpectEqual(clearTransition.reason, lrp::ActivityLaneReason::AppClosed,
            "Productive clears should use the app-closed reason.");
        Expect(!clearTransition.duplicate,
            "The first clear after activity should be emitted.");
        lrp::CommitActivityLaneTransition(state, clearTransition);

        auto duplicateClear = lrp::ResolveActivityLaneTransition(
            state,
            true,
            true,
            false,
            false,
            false,
            false,
            false,
            L"",
            lrp::ActivityLaneReason::AppClosed);
        Expect(duplicateClear.duplicate,
            "Repeated clears should be deduped.");

        auto filteredTransition = lrp::ResolveActivityLaneTransition(
            state,
            true,
            true,
            false,
            false,
            true,
            false,
            false,
            L"",
            lrp::ActivityLaneReason::AppClosed);
        ExpectEqual(filteredTransition.action, lrp::ActivityLaneAction::SuppressBySettings,
            "Filtered apps should be suppressed by settings.");
        ExpectEqual(filteredTransition.reason, lrp::ActivityLaneReason::FilteredApp,
            "Filtered apps should carry the filtered-app reason.");

        auto priorityTransition = lrp::ResolveActivityLaneTransition(
            state,
            true,
            true,
            true,
            false,
            false,
            false,
            true,
            L"ps|poster",
            lrp::ActivityLaneReason::DetectorIdle);
        ExpectEqual(priorityTransition.action, lrp::ActivityLaneAction::SuppressByPriority,
            "Priority conflicts should map to the explicit suppress-by-priority action.");
        ExpectEqual(priorityTransition.reason, lrp::ActivityLaneReason::PrioritySuppressed,
            "Priority conflicts should carry the priority-suppressed reason.");
    }

    void TestSettingsImportAndPrecedence()
    {
        using namespace lrp::settings;

        PersistedSettings resolvedDefaults;
        resolvedDefaults.behavior.launchOnStartup = false;
        resolvedDefaults.behavior.startMinimizedToTray = false;
        resolvedDefaults.media.showDefaultIdleStatus = true;

        RegistryPreferenceOverrides overrides;
        ApplyRegistryPreferenceOverrides(resolvedDefaults, overrides, true);
        Expect(resolvedDefaults.behavior.launchOnStartup,
            "The live Run startup state should still enable launch-on-startup when no registry override exists.");

        PersistedSettings overriddenSettings;
        overriddenSettings.behavior.launchOnStartup = true;
        overriddenSettings.behavior.startMinimizedToTray = false;
        overriddenSettings.media.showDefaultIdleStatus = true;
        overrides.hasLaunchOnStartup = true;
        overrides.launchOnStartup = false;
        overrides.hasStartMinimizedToTray = true;
        overrides.startMinimizedToTray = true;
        overrides.hasShowDefaultIdleStatus = true;
        overrides.showDefaultIdleStatus = false;
        ApplyRegistryPreferenceOverrides(overriddenSettings, overrides, true);
        Expect(!overriddenSettings.behavior.launchOnStartup,
            "Registry launch-on-startup overrides should win over both stored and live state.");
        Expect(overriddenSettings.behavior.startMinimizedToTray,
            "Registry start-minimized overrides should be applied.");
        Expect(!overriddenSettings.media.showDefaultIdleStatus,
            "Registry idle-status overrides should be applied.");

        PersistedSettings current;
        current.productive.enabled = true;
        current.behavior.themeMode = AppThemeMode::Dark;
        current.creative.priority = CreativePriorityMode::PreferCreative;

        ImportedSettingMap imported;
        imported[L"ProductiveEnabled"] = std::wstring(L"invalid");
        imported[L"StartMinimizedToTray"] = 0.0;
        imported[L"BlockedAppSiteTerms"] = std::wstring(L"foo;bar");
        imported[L"CreativePriority"] = std::wstring(L"prefer_media");
        imported[L"ThemeMode"] = std::wstring(L"totally-unknown");

        auto parsed = ParseImportedSettings(imported, current);
        Expect(parsed.settings.productive.enabled,
            "Invalid imported booleans should keep the current value.");
        Expect(!parsed.settings.behavior.startMinimizedToTray,
            "Numeric imported booleans should be coerced.");
        ExpectEqual(parsed.settings.media.blockedAppSiteTermsRaw, std::wstring(L"foo;bar"),
            "Imported string settings should be applied when valid.");
        Expect(parsed.settings.creative.priority == CreativePriorityMode::PreferMedia,
            "Imported parsed enums should update when valid.");
        Expect(parsed.settings.behavior.themeMode == AppThemeMode::Dark,
            "Invalid imported enum values should keep the current setting.");
        Expect(parsed.issues.size() == 2,
            "Invalid imported values should be surfaced as issues.");
        ExpectEqual(parsed.issues[0].key, std::wstring(L"ProductiveEnabled"),
            "The first invalid import issue should identify the bad key.");
        ExpectEqual(parsed.issues[1].key, std::wstring(L"ThemeMode"),
            "The second invalid import issue should identify the bad enum key.");
    }

    void TestDiscordRpcStatusAndClearOnShutdown()
    {
        auto transport = std::make_unique<FakeDiscordIpcTransport>();
        auto* transportPtr = transport.get();
        transportPtr->EnqueueRead(DiscordTransportResult::Ok, 1, "{}");
        transportPtr->EnqueueRead(DiscordTransportResult::Ok, 1, "{}");
        transportPtr->EnqueueRead(DiscordTransportResult::Ok, 1, "{}");

        DiscordRPC rpc("transport-test", std::move(transport));
        rpc.Initialize();

        DiscordPresenceData presence;
        presence.name = "LRP";
        presence.details = "Editing";
        rpc.UpdatePresence(presence);

        Expect(transportPtr->WaitForWrites(2, 2500ms),
            "Handshake and first activity update should be written.");
        WaitUntil([&] { return rpc.GetStatus().connected; }, 1500ms,
            "DiscordRPC should report a connected transport after handshake.");

        auto status = rpc.GetStatus();
        Expect(status.connected, "Status should report the transport as connected.");
        Expect(status.lastResult == DiscordTransportResult::Ok,
            "Status should record a successful transport result after handshake.");
        Expect(status.lastSuccessfulHandshakeUnixSeconds > 0,
            "Status should record the handshake timestamp.");

        rpc.Shutdown();

        auto writes = transportPtr->WritesSnapshot();
        Expect(writes.size() >= 3,
            "Shutdown should flush a clear payload before disconnecting.");
        ExpectEqual(writes[0].opcode, 0u, "The first Discord frame should be the handshake.");
        Expect(Contains(writes[0].data, "\"client_id\":\"transport-test\""),
            "The handshake should include the requested app ID.");
        Expect(Contains(writes[1].data, "\"activity\":{"),
            "The second frame should publish the current activity.");
        Expect(Contains(writes.back().data, "\"activity\":null"),
            "Shutdown should clear the activity before disconnecting.");
    }

    void TestDiscordRpcReconnectAndReplay()
    {
        auto transport = std::make_unique<FakeDiscordIpcTransport>();
        auto* transportPtr = transport.get();
        transportPtr->EnqueueConnectResult(DiscordTransportResult::PipeUnavailable);
        transportPtr->EnqueueConnectResult(DiscordTransportResult::Ok);
        transportPtr->EnqueueRead(DiscordTransportResult::Ok, 1, "{}");
        transportPtr->EnqueueRead(DiscordTransportResult::Ok, 1, "{}");
        transportPtr->EnqueueRead(DiscordTransportResult::Ok, 1, "{}");

        DiscordRPC rpc("reconnect-test", std::move(transport));
        rpc.Initialize();

        DiscordPresenceData presence;
        presence.name = "LRP";
        presence.details = "Reconnect";
        rpc.UpdatePresence(presence);

        WaitUntil(
            [&]
            {
                auto status = rpc.GetStatus();
                return status.lastResult == DiscordTransportResult::PipeUnavailable && status.retryCount >= 1;
            },
            1000ms,
            "A failed initial connect should be reflected in transport status.");

        Expect(transportPtr->WaitForWrites(2, 7000ms),
            "The pending presence should be replayed after reconnect.");
        WaitUntil([&] { return rpc.GetStatus().connected; }, 2000ms,
            "DiscordRPC should reconnect after the retry delay.");

        auto status = rpc.GetStatus();
        Expect(status.connected, "The reconnect path should restore the connected state.");
        Expect(status.lastResult == DiscordTransportResult::Ok,
            "A successful reconnect should reset the last transport result to ok.");

        auto writes = transportPtr->WritesSnapshot();
        Expect(writes.size() >= 2,
            "Reconnect should emit a handshake and the replayed activity.");
        Expect(Contains(writes[1].data, "\"activity\":{"),
            "Replayed presence should still be sent after reconnect.");

        rpc.Shutdown();
    }

    void TestDiscordRpcOversizedFrameHandling()
    {
        auto transport = std::make_unique<FakeDiscordIpcTransport>();
        auto* transportPtr = transport.get();
        transportPtr->EnqueueRead(DiscordTransportResult::Ok, 1, "{}");

        DiscordRPC rpc("oversized-test", std::move(transport));
        rpc.Initialize();

        WaitUntil([&] { return rpc.GetStatus().connected; }, 1500ms,
            "Handshake should complete before sending the oversized-frame update.");

        transportPtr->EnqueueRead(DiscordTransportResult::OversizedFrame);
        transportPtr->EnqueueAvailable(0);
        transportPtr->EnqueueAvailable(1);

        DiscordPresenceData presence;
        presence.name = "LRP";
        presence.details = "Oversized";
        rpc.UpdatePresence(presence);
        Expect(transportPtr->WaitForWrites(2, 2500ms),
            "The oversized-frame test should reach the update write path.");

        WaitUntil(
            [&]
            {
                auto status = rpc.GetStatus();
                return !status.connected && status.lastResult == DiscordTransportResult::OversizedFrame;
            },
            1500ms,
            "Oversized frames should disconnect the transport and surface the error.");

        rpc.Shutdown();
    }

    void TestDiscordRpcReplaysClearBeforeUntimedUpdateAfterReconnect()
    {
        auto transport = std::make_unique<FakeDiscordIpcTransport>();
        auto* transportPtr = transport.get();
        transportPtr->EnqueueRead(DiscordTransportResult::Ok, 1, "{}");
        transportPtr->EnqueueRead(DiscordTransportResult::Ok, 1, "{}");
        transportPtr->EnqueueAvailable(0);
        transportPtr->EnqueueAvailable(1);
        transportPtr->EnqueueWriteResult(DiscordTransportResult::Ok);
        transportPtr->EnqueueWriteResult(DiscordTransportResult::Ok);
        transportPtr->EnqueueWriteResult(DiscordTransportResult::WriteFailed);

        DiscordRPC rpc("replay-clear-test", std::move(transport));
        rpc.Initialize();

        DiscordPresenceData timedPresence;
        timedPresence.name = "LRP";
        timedPresence.startTimestamp = 100;
        timedPresence.endTimestamp = 200;
        rpc.UpdatePresence(timedPresence);
        Expect(transportPtr->WaitForWrites(2, 2500ms),
            "The timed presence should be published before the reconnect replay scenario.");
        WaitUntil([&] { return transportPtr->ReadCount() >= 2; }, 1500ms,
            "The timed presence ack should complete before removing timestamps.");

        transportPtr->EnqueueRead(DiscordTransportResult::Ok, 1, "{}");
        transportPtr->EnqueueRead(DiscordTransportResult::Ok, 1, "{}");
        transportPtr->EnqueueRead(DiscordTransportResult::Ok, 1, "{}");
        transportPtr->EnqueueAvailable(0);
        transportPtr->EnqueueAvailable(0);
        transportPtr->EnqueueAvailable(1);
        transportPtr->EnqueueAvailable(1);

        DiscordPresenceData untimedPresence = timedPresence;
        untimedPresence.startTimestamp = 0;
        untimedPresence.endTimestamp = 0;
        rpc.UpdatePresence(untimedPresence);
        if (!transportPtr->WaitForWrites(6, 12000ms))
        {
            auto writes = transportPtr->WritesSnapshot();
            std::string lastWriteKind = writes.empty() ? "none" : DescribeWriteKind(writes.back().data);
            throw std::runtime_error(
                "The untimed replay scenario should reconnect and resend clear plus update. Writes observed: " +
                std::to_string(writes.size()) + ", last write kind: " + lastWriteKind +
                ", sequence: " + DescribeWrites(writes));
        }

        auto writes = transportPtr->WritesSnapshot();
        Expect(Contains(writes[2].data, "\"activity\":null"),
            "The first write after removing timestamps should be a clear.");
        ExpectEqual(writes[3].opcode, 0u,
            "Reconnect replay should perform a fresh handshake.");
        Expect(Contains(writes[4].data, "\"activity\":null"),
            "Reconnect replay should preserve the clear-before-update step.");
        Expect(Contains(writes[5].data, "\"activity\":{"),
            "Reconnect replay should resend the untimed activity after the clear.");

        rpc.Shutdown();
    }

    void TestDiscordRpcClearsTimestampedActivityBeforeUntimedUpdate()
    {
        auto transport = std::make_unique<FakeDiscordIpcTransport>();
        auto* transportPtr = transport.get();
        transportPtr->EnqueueRead(DiscordTransportResult::Ok, 1, "{}");
        transportPtr->EnqueueRead(DiscordTransportResult::Ok, 1, "{}");
        transportPtr->EnqueueRead(DiscordTransportResult::Ok, 1, "{}");
        transportPtr->EnqueueRead(DiscordTransportResult::Ok, 1, "{}");
        transportPtr->EnqueueRead(DiscordTransportResult::Ok, 1, "{}");

        DiscordRPC rpc("timestamp-test", std::move(transport));
        rpc.Initialize();

        DiscordPresenceData timedPresence;
        timedPresence.name = "LRP";
        timedPresence.startTimestamp = 100;
        timedPresence.endTimestamp = 200;
        rpc.UpdatePresence(timedPresence);
        Expect(transportPtr->WaitForWrites(2, 2500ms),
            "Timed presence should be published after the handshake.");

        DiscordPresenceData untimedPresence = timedPresence;
        untimedPresence.startTimestamp = 0;
        untimedPresence.endTimestamp = 0;
        rpc.UpdatePresence(untimedPresence);
        Expect(transportPtr->WaitForWrites(4, 2500ms),
            "Untimed updates after timestamped activity should clear first and then republish.");

        auto writes = transportPtr->WritesSnapshot();
        Expect(Contains(writes[2].data, "\"activity\":null"),
            "Timestamp removal should emit an explicit clear before the next update.");
        Expect(Contains(writes[3].data, "\"activity\":{"),
            "Untimed activity should be sent after the clear.");

        rpc.Shutdown();
    }

    void TestDiagnosticsLog()
    {
        lrp::DiagnosticsLog log;
        for (int index = 0; index < 185; ++index)
            log.Append(L"INFO", L"tests", L"msg" + std::to_wstring(index));

        Expect(log.Lines().size() == 180, "DiagnosticsLog should cap retained entries.");
        Expect(log.Lines().front().find(L"msg5") != std::wstring::npos,
            "DiagnosticsLog should discard the oldest retained entries.");
        Expect(log.Lines().back().find(L"msg184") != std::wstring::npos,
            "DiagnosticsLog should retain the newest entry.");
        Expect(log.JoinLines().find(L"msg184") != std::wstring::npos,
            "DiagnosticsLog should join retained entries.");
    }
}

int main()
{
    try
    {
        TestTextUtilities();
        TestSettingsModels();
        TestAppPageHelpers();
        TestBrowserNativeMessagingContract();
        TestWindowTrayVisibilityHelper();
        TestSettingsUiBinder();
        TestActivityPresenceHelpers();
        TestActivityLaneCoordinator();
        TestSettingsImportAndPrecedence();
        TestDiscordRpcStatusAndClearOnShutdown();
        TestDiscordRpcReconnectAndReplay();
        TestDiscordRpcOversizedFrameHandling();
        TestDiscordRpcReplaysClearBeforeUntimedUpdateAfterReconnect();
        TestDiscordRpcClearsTimestampedActivityBeforeUntimedUpdate();
        TestDiagnosticsLog();
        std::cout << "Refactor guard tests passed.\n";
        return 0;
    }
    catch (const std::exception& ex)
    {
        std::cerr << "Refactor guard tests failed: " << ex.what() << '\n';
        return 1;
    }
}
