#pragma once

#include "PageModels.h"
#include "SettingsModels.h"

namespace lrp::ui
{
    inline MusicPageSettings BuildMusicPageSettings(const lrp::settings::PersistedSettings& settings)
    {
        MusicPageSettings result{};
        result.showTimestamps = settings.media.showTimestamps;
        result.showSource = settings.media.showSource;
        result.sourceDebugMode = settings.media.sourceDebugMode;
        result.showPaused = settings.media.showPaused;
        result.showAlbumArt = settings.media.showAlbumArt;
        return result;
    }

    inline SettingsPageSettings BuildSettingsPageSettings(const lrp::settings::PersistedSettings& settings)
    {
        SettingsPageSettings result{};
        result.closeToTrayOnClose = settings.behavior.closeToTrayOnClose;
        result.launchOnStartup = settings.behavior.launchOnStartup;
        result.startMinimizedToTray = settings.behavior.startMinimizedToTray;
        result.trayLeftClickToggles = settings.behavior.trayLeftClickToggles;
        result.showDefaultIdleStatus = settings.media.showDefaultIdleStatus;
        result.mediaActivityTypeIndex = lrp::settings::ActivityTypeOverrideToComboIndex(settings.media.activityTypeOverride);
        result.creativeActivityTypeIndex = lrp::settings::ActivityTypeOverrideToComboIndex(settings.creative.activityTypeOverride);
        result.productiveActivityTypeIndex = lrp::settings::ActivityTypeOverrideToComboIndex(settings.productive.activityTypeOverride);
        result.sensitiveKeywordFilter = settings.media.sensitiveKeywordFilter;
        result.strictBrowserPrivacy = settings.media.strictBrowserPrivacy;
        result.suppressBrowserAlbumArt = settings.media.suppressBrowserAlbumArt;
        result.blockedAppSitesRaw = winrt::hstring(settings.media.blockedAppSiteTermsRaw);
        result.themeModeIndex = lrp::settings::ThemeModeToComboIndex(settings.behavior.themeMode);
        return result;
    }

    inline ProductivityPageSettings BuildProductivityPageSettings(const lrp::settings::PersistedSettings& settings)
    {
        ProductivityPageSettings result{};
        result.enabled = settings.productive.enabled;
        result.detectionMode = settings.productive.detectionMode;
        result.showProjectName = settings.productive.showProjectName;
        result.wordEnabled = settings.productive.wordEnabled;
        result.excelEnabled = settings.productive.excelEnabled;
        result.powerPointEnabled = settings.productive.powerPointEnabled;
        result.oneNoteEnabled = settings.productive.oneNoteEnabled;
        result.accessEnabled = settings.productive.accessEnabled;
        result.publisherEnabled = settings.productive.publisherEnabled;
        result.visioEnabled = settings.productive.visioEnabled;
        result.projectEnabled = settings.productive.projectEnabled;
        result.codexEnabled = settings.productive.codexEnabled;
        return result;
    }

    inline CreativePageSettings BuildCreativePageSettings(const lrp::settings::PersistedSettings& settings)
    {
        CreativePageSettings result{};
        result.enabled = settings.creative.enabled;
        result.priority = settings.creative.priority;
        result.detectionMode = settings.creative.detectionMode;
        result.showProjectName = settings.creative.showProjectName;
        result.showWindowTitle = settings.creative.showWindowTitle;
        result.photoshopEnabled = settings.creative.photoshopEnabled;
        result.illustratorEnabled = settings.creative.illustratorEnabled;
        result.premiereEnabled = settings.creative.premiereEnabled;
        result.afterEffectsEnabled = settings.creative.afterEffectsEnabled;
        result.inDesignEnabled = settings.creative.inDesignEnabled;
        result.auditionEnabled = settings.creative.auditionEnabled;
        result.mediaEncoderEnabled = settings.creative.mediaEncoderEnabled;
        result.lightroomEnabled = settings.creative.lightroomEnabled;
        result.lightroomClassicEnabled = settings.creative.lightroomClassicEnabled;
        result.inCopyEnabled = settings.creative.inCopyEnabled;
        result.dreamweaverEnabled = settings.creative.dreamweaverEnabled;
        result.animateEnabled = settings.creative.animateEnabled;
        result.xdEnabled = settings.creative.xdEnabled;
        result.bridgeEnabled = settings.creative.bridgeEnabled;
        result.characterAnimatorEnabled = settings.creative.characterAnimatorEnabled;
        result.frescoEnabled = settings.creative.frescoEnabled;
        result.dimensionEnabled = settings.creative.dimensionEnabled;
        result.substanceEnabled = settings.creative.substanceEnabled;
        result.acrobatEnabled = settings.creative.acrobatEnabled;
        result.otherAdobeEnabled = settings.creative.otherAdobeEnabled;
        result.privacyMode = settings.creative.privacyMode;
        result.idleBehavior = settings.creative.idleBehavior;
        return result;
    }

    inline void ApplyMusicPageSettings(lrp::settings::PersistedSettings& settings, MusicPageSettings const& pageSettings)
    {
        settings.media.showTimestamps = pageSettings.showTimestamps;
        settings.media.showSource = pageSettings.showSource;
        settings.media.sourceDebugMode = pageSettings.sourceDebugMode;
        settings.media.showPaused = pageSettings.showPaused;
        settings.media.showAlbumArt = pageSettings.showAlbumArt;
    }

    inline void ApplySettingsPageSettings(
        lrp::settings::PersistedSettings& settings,
        SettingsPageSettings const& pageSettings,
        bool includeBlockedTermsRaw)
    {
        settings.behavior.closeToTrayOnClose = pageSettings.closeToTrayOnClose;
        settings.behavior.launchOnStartup = pageSettings.launchOnStartup;
        settings.behavior.startMinimizedToTray = pageSettings.startMinimizedToTray;
        settings.behavior.trayLeftClickToggles = pageSettings.trayLeftClickToggles;
        settings.behavior.themeMode = lrp::settings::ThemeModeFromComboIndex(pageSettings.themeModeIndex);

        settings.media.showDefaultIdleStatus = pageSettings.showDefaultIdleStatus;
        settings.media.sensitiveKeywordFilter = pageSettings.sensitiveKeywordFilter;
        settings.media.strictBrowserPrivacy = pageSettings.strictBrowserPrivacy;
        settings.media.suppressBrowserAlbumArt = pageSettings.suppressBrowserAlbumArt;
        settings.media.activityTypeOverride = lrp::settings::ActivityTypeOverrideFromComboIndex(pageSettings.mediaActivityTypeIndex);
        if (includeBlockedTermsRaw)
            settings.media.blockedAppSiteTermsRaw = pageSettings.blockedAppSitesRaw.c_str();

        settings.creative.activityTypeOverride = lrp::settings::ActivityTypeOverrideFromComboIndex(pageSettings.creativeActivityTypeIndex);
        settings.productive.activityTypeOverride = lrp::settings::ActivityTypeOverrideFromComboIndex(pageSettings.productiveActivityTypeIndex);
    }

    inline void ApplyProductivityPageSettings(
        lrp::settings::PersistedSettings& settings,
        ProductivityPageSettings const& pageSettings)
    {
        settings.productive.enabled = pageSettings.enabled;
        settings.productive.detectionMode = pageSettings.detectionMode;
        settings.productive.showProjectName = pageSettings.showProjectName;
        settings.productive.wordEnabled = pageSettings.wordEnabled;
        settings.productive.excelEnabled = pageSettings.excelEnabled;
        settings.productive.powerPointEnabled = pageSettings.powerPointEnabled;
        settings.productive.oneNoteEnabled = pageSettings.oneNoteEnabled;
        settings.productive.accessEnabled = pageSettings.accessEnabled;
        settings.productive.publisherEnabled = pageSettings.publisherEnabled;
        settings.productive.visioEnabled = pageSettings.visioEnabled;
        settings.productive.projectEnabled = pageSettings.projectEnabled;
        settings.productive.codexEnabled = pageSettings.codexEnabled;
    }

    inline void ApplyCreativePageSettings(
        lrp::settings::PersistedSettings& settings,
        CreativePageSettings const& pageSettings)
    {
        settings.creative.enabled = pageSettings.enabled;
        settings.creative.priority = pageSettings.priority;
        settings.creative.detectionMode = pageSettings.detectionMode;
        settings.creative.showProjectName = pageSettings.showProjectName;
        settings.creative.showWindowTitle = pageSettings.showWindowTitle;
        settings.creative.photoshopEnabled = pageSettings.photoshopEnabled;
        settings.creative.illustratorEnabled = pageSettings.illustratorEnabled;
        settings.creative.premiereEnabled = pageSettings.premiereEnabled;
        settings.creative.afterEffectsEnabled = pageSettings.afterEffectsEnabled;
        settings.creative.inDesignEnabled = pageSettings.inDesignEnabled;
        settings.creative.auditionEnabled = pageSettings.auditionEnabled;
        settings.creative.mediaEncoderEnabled = pageSettings.mediaEncoderEnabled;
        settings.creative.lightroomEnabled = pageSettings.lightroomEnabled;
        settings.creative.lightroomClassicEnabled = pageSettings.lightroomClassicEnabled;
        settings.creative.inCopyEnabled = pageSettings.inCopyEnabled;
        settings.creative.dreamweaverEnabled = pageSettings.dreamweaverEnabled;
        settings.creative.animateEnabled = pageSettings.animateEnabled;
        settings.creative.xdEnabled = pageSettings.xdEnabled;
        settings.creative.bridgeEnabled = pageSettings.bridgeEnabled;
        settings.creative.characterAnimatorEnabled = pageSettings.characterAnimatorEnabled;
        settings.creative.frescoEnabled = pageSettings.frescoEnabled;
        settings.creative.dimensionEnabled = pageSettings.dimensionEnabled;
        settings.creative.substanceEnabled = pageSettings.substanceEnabled;
        settings.creative.acrobatEnabled = pageSettings.acrobatEnabled;
        settings.creative.otherAdobeEnabled = pageSettings.otherAdobeEnabled;
        settings.creative.privacyMode = pageSettings.privacyMode;
        settings.creative.idleBehavior = pageSettings.idleBehavior;
    }
}
