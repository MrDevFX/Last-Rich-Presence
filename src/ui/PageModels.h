#pragma once

#include "AppEnums.h"

#include <cstdint>
#include <string>

#include <winrt/base.h>

namespace lrp::ui
{
    struct MusicPageSettings
    {
        bool showTimestamps{ true };
        bool showSource{ true };
        bool sourceDebugMode{ false };
        bool showPaused{ true };
        bool showAlbumArt{ true };

        bool operator==(MusicPageSettings const&) const = default;
    };

    struct ProductivityPageSettings
    {
        bool enabled{ true };
        ProductiveDetectionMode detectionMode{ ProductiveDetectionMode::ForegroundPreferredVisibleFallback };
        bool showProjectName{ true };
        bool wordEnabled{ true };
        bool excelEnabled{ true };
        bool powerPointEnabled{ true };
        bool oneNoteEnabled{ true };
        bool accessEnabled{ true };
        bool publisherEnabled{ true };
        bool visioEnabled{ true };
        bool projectEnabled{ true };
        bool codexEnabled{ true };

        bool operator==(ProductivityPageSettings const&) const = default;
    };

    struct CreativePageSettings
    {
        bool enabled{ true };
        CreativePriorityMode priority{ CreativePriorityMode::Auto };
        CreativeDetectionMode detectionMode{ CreativeDetectionMode::ForegroundPreferredVisibleFallback };
        bool showProjectName{ true };
        bool showWindowTitle{ false };
        bool photoshopEnabled{ true };
        bool illustratorEnabled{ true };
        bool premiereEnabled{ true };
        bool afterEffectsEnabled{ true };
        bool inDesignEnabled{ true };
        bool auditionEnabled{ true };
        bool mediaEncoderEnabled{ true };
        bool lightroomEnabled{ true };
        bool lightroomClassicEnabled{ true };
        bool inCopyEnabled{ true };
        bool dreamweaverEnabled{ true };
        bool animateEnabled{ true };
        bool xdEnabled{ true };
        bool bridgeEnabled{ true };
        bool characterAnimatorEnabled{ true };
        bool frescoEnabled{ true };
        bool dimensionEnabled{ true };
        bool substanceEnabled{ true };
        bool acrobatEnabled{ true };
        bool otherAdobeEnabled{ true };
        CreativePrivacyMode privacyMode{ CreativePrivacyMode::Normal };
        CreativeIdleBehavior idleBehavior{ CreativeIdleBehavior::HoldLast5Seconds };

        bool operator==(CreativePageSettings const&) const = default;
    };

    struct SettingsPageSettings
    {
        bool closeToTrayOnClose{ true };
        bool launchOnStartup{ false };
        bool startMinimizedToTray{ false };
        bool trayLeftClickToggles{ true };
        bool showDefaultIdleStatus{ true };
        int32_t mediaActivityTypeIndex{ 0 };
        int32_t creativeActivityTypeIndex{ 0 };
        int32_t productiveActivityTypeIndex{ 0 };
        bool sensitiveKeywordFilter{ true };
        bool strictBrowserPrivacy{ false };
        bool suppressBrowserAlbumArt{ false };
        winrt::hstring blockedAppSitesRaw{};
        int32_t themeModeIndex{ 0 };

        bool operator==(SettingsPageSettings const&) const = default;
    };

    struct MusicPageState
    {
        bool hasMedia{ false };
        bool isPlaying{ false };
        bool motionEnabled{ true };
        double progressPercent{ 0.0 };
        winrt::hstring title{ L"Nothing playing" };
        winrt::hstring artist{ L"\x2014" };
        winrt::hstring albumTitle{};
        winrt::hstring positionText{ L"0:00" };
        winrt::hstring durationText{ L"0:00" };
        winrt::hstring playbackStateText{ L"Idle" };
        bool showSourceBadge{ false };
        winrt::hstring sourceText{ L"No source" };
        bool showSourceDebug{ false };
        winrt::hstring sourceDebugText{};
    };

    struct ProductivityPageState
    {
        winrt::hstring detectedAppText{ L"Awaiting Productive Activity" };
        winrt::hstring detectedProjectText{ L"Launch a supported Office app to update your status." };
        winrt::hstring detectedWindowText{ L"None" };
        winrt::hstring detectedProcessText{ L"None" };
        winrt::hstring runtimeSummaryText{ L"Waiting for supported apps..." };
        bool settingsControlsEnabled{ true };
    };

    struct CreativePageState
    {
        winrt::hstring mvpHeadlineText{ L"Creativity RPC (MVP detector idle)" };
        winrt::hstring mvpSummaryText{};
        winrt::hstring detectedAppText{ L"Awaiting Creative Activity" };
        winrt::hstring detectedProjectText{ L"Launch a supported app to update your status." };
        winrt::hstring detectedWindowText{ L"None" };
        winrt::hstring detectedProcessText{ L"None" };
        bool settingsControlsEnabled{ true };
    };

    struct HomePageState
    {
        bool richPresenceEnabled{ true };
        bool motionEnabled{ true };
        winrt::hstring statusText{ L"Disconnected from Discord" };
        winrt::hstring statusSubtext{ L"Waiting for connection..." };
        winrt::hstring healthText{};
        winrt::hstring statusIndicatorBrushKey{ L"StatusDisconnectedBrush" };
        bool showLiveBadge{ false };

        winrt::hstring sourceValue{ L"No active source" };
        winrt::hstring sourceSubtext{ L"Waiting for active session" };
        winrt::hstring detectedViaText{ L"Detected via --" };
        winrt::hstring extensionText{ L"Native host offline" };
        winrt::hstring extensionSubtext{ L"Restart the app to refresh native-host registration" };
        winrt::hstring extensionIndicatorBrushKey{ L"StatusDisconnectedBrush" };

        winrt::hstring miniTitle{ L"Nothing playing" };
        winrt::hstring miniArtist{ L"Start playback or activity" };
        winrt::hstring miniTimerText{ L"0:00 / 0:00" };
        winrt::hstring miniStatusText{ L"Waiting" };
        winrt::hstring miniStatusGlyph{ L"\xE160" };
        winrt::hstring miniStatusBrushKey{ L"TextFillColorSecondaryBrush" };
        bool showPausedChip{ false };

        winrt::hstring creativeTitle{ L"Awaiting Creative Activity" };
        winrt::hstring creativeSubtitle{ L"Launch a supported app to update your status." };
        winrt::hstring creativeWindowText{ L"None" };
        winrt::hstring creativeProcessText{ L"None" };
        winrt::hstring creativeStatusText{ L"Waiting" };
        winrt::hstring creativeStatusGlyph{ L"\xE160" };
        winrt::hstring creativeStatusBrushKey{ L"TextFillColorSecondaryBrush" };
        winrt::hstring creativeSourceValue{ L"No active creativity source" };
        winrt::hstring creativeSourceSubtext{ L"Waiting for supported apps..." };
        winrt::hstring creativeDetectedViaText{ L"Detected via --" };
        winrt::hstring creativeDetectorText{ L"Creativity detector waiting" };
        winrt::hstring creativeDetectorSubtext{ L"Waiting for supported apps..." };
        winrt::hstring creativeDetectorBrushKey{ L"StatusConnectingBrush" };

        winrt::hstring productiveTitle{ L"Awaiting Productive Activity" };
        winrt::hstring productiveSubtitle{ L"Launch a supported Office app to update your status." };
        winrt::hstring productiveWindowText{ L"None" };
        winrt::hstring productiveProcessText{ L"None" };
        winrt::hstring productiveStatusText{ L"Waiting" };
        winrt::hstring productiveStatusGlyph{ L"\xE160" };
        winrt::hstring productiveStatusBrushKey{ L"TextFillColorSecondaryBrush" };
        winrt::hstring productiveSourceValue{ L"No active productivity source" };
        winrt::hstring productiveSourceSubtext{ L"Waiting for supported apps..." };
        winrt::hstring productiveDetectedViaText{ L"Detected via --" };
        winrt::hstring productiveDetectorText{ L"Productivity detector waiting" };
        winrt::hstring productiveDetectorSubtext{ L"Waiting for supported apps..." };
        winrt::hstring productiveDetectorBrushKey{ L"StatusConnectingBrush" };
    };

    struct SettingsPageState
    {
        winrt::hstring diagnosticsLogText{};
    };
}
