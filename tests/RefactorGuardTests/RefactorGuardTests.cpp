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
#include "DiagnosticsLog.h"
#include "DiscordRPC.h"
#include "SettingsImport.h"
#include "SettingsModels.h"
#include "TextUtilities.h"

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
