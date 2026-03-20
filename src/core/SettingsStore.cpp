#include "pch.h"

#include "SettingsStore.h"

#include "StartupRegistration.h"

namespace
{
    using winrt::Windows::Storage::ApplicationData;
    using winrt::Windows::Storage::ApplicationDataContainer;

    bool ReadBool(ApplicationDataContainer const& container, const wchar_t* key, bool fallback)
    {
        auto value = container.Values().TryLookup(key);
        if (!value)
            return fallback;

        try
        {
            return winrt::unbox_value<bool>(value);
        }
        catch (...)
        {
            return fallback;
        }
    }

    std::wstring ReadString(ApplicationDataContainer const& container, const wchar_t* key, const std::wstring& fallback)
    {
        auto value = container.Values().TryLookup(key);
        if (!value)
            return fallback;

        try
        {
            return std::wstring(winrt::unbox_value<winrt::hstring>(value).c_str());
        }
        catch (...)
        {
            return fallback;
        }
    }
}

namespace lrp::settings
{
    PersistedSettings LoadPersistedSettings()
    {
        PersistedSettings settings;

        bool launchOnStartupFromRegistry = false;
        bool hasLaunchOnStartupRegistryValue = startup::TryReadUserPreferenceBool(
            startup::kLaunchOnStartupRegistryValueName,
            launchOnStartupFromRegistry);

        bool startMinimizedFromRegistry = false;
        bool hasStartMinimizedRegistryValue = startup::TryReadUserPreferenceBool(
            startup::kStartMinimizedRegistryValueName,
            startMinimizedFromRegistry);

        bool showDefaultIdleStatusFromRegistry = false;
        bool hasShowDefaultIdleStatusRegistryValue = startup::TryReadUserPreferenceBool(
            startup::kShowDefaultIdleStatusRegistryValueName,
            showDefaultIdleStatusFromRegistry);

        try
        {
            auto localSettings = ApplicationData::Current().LocalSettings();

            settings.media.showTimestamps = ReadBool(localSettings, L"ShowTimestamps", settings.media.showTimestamps);
            settings.media.showSource = ReadBool(localSettings, L"ShowSourceApp", settings.media.showSource);
            settings.media.sourceDebugMode = ReadBool(localSettings, L"SourceDebugMode", settings.media.sourceDebugMode);
            settings.media.showPaused = ReadBool(localSettings, L"ShowPaused", settings.media.showPaused);
            settings.media.showAlbumArt = ReadBool(localSettings, L"ShowAlbumArt", settings.media.showAlbumArt);
            settings.media.showDefaultIdleStatus = ReadBool(localSettings, L"ShowDefaultIdleStatus", settings.media.showDefaultIdleStatus);
            if (hasShowDefaultIdleStatusRegistryValue)
                settings.media.showDefaultIdleStatus = showDefaultIdleStatusFromRegistry;

            settings.behavior.richPresenceEnabled = ReadBool(localSettings, L"RichPresenceEnabled", settings.behavior.richPresenceEnabled);
            settings.behavior.closeToTrayOnClose = ReadBool(localSettings, L"CloseToTrayOnClose", settings.behavior.closeToTrayOnClose);
            auto launchOnStartupDefault = startup::IsRunStartupEnabledForCurrentExecutable();
            auto launchOnStartupStored = ReadBool(localSettings, L"LaunchOnStartup", launchOnStartupDefault);
            settings.behavior.launchOnStartup = hasLaunchOnStartupRegistryValue
                ? launchOnStartupFromRegistry
                : (launchOnStartupStored || launchOnStartupDefault);

            auto startMinimizedDefault = hasStartMinimizedRegistryValue
                ? startMinimizedFromRegistry
                : settings.behavior.startMinimizedToTray;
            settings.behavior.startMinimizedToTray = ReadBool(localSettings, L"StartMinimizedToTray", startMinimizedDefault);
            if (hasStartMinimizedRegistryValue)
                settings.behavior.startMinimizedToTray = startMinimizedFromRegistry;

            settings.behavior.trayLeftClickToggles = ReadBool(localSettings, L"TrayLeftClickToggles", settings.behavior.trayLeftClickToggles);
            settings.behavior.themeMode = ParseThemeMode(ReadString(localSettings, L"ThemeMode", ToSettingString(settings.behavior.themeMode)));

            settings.media.sensitiveKeywordFilter = ReadBool(localSettings, L"SensitiveKeywordFilter", settings.media.sensitiveKeywordFilter);
            settings.media.strictBrowserPrivacy = ReadBool(localSettings, L"StrictBrowserPrivacy", settings.media.strictBrowserPrivacy);
            settings.media.suppressBrowserAlbumArt = ReadBool(localSettings, L"SuppressBrowserAlbumArt", settings.media.suppressBrowserAlbumArt);
            settings.media.blockedAppSiteTermsRaw = ReadString(localSettings, L"BlockedAppSiteTerms", settings.media.blockedAppSiteTermsRaw);
            settings.media.activityTypeOverride = ParseActivityTypeOverride(ReadString(localSettings, L"MediaActivityType", L"auto"));

            settings.productive.enabled = ReadBool(localSettings, L"ProductiveEnabled", settings.productive.enabled);
            settings.productive.detectionMode = ParseProductiveDetectionMode(
                ReadString(localSettings, L"ProductiveDetectionMode", ToSettingString(settings.productive.detectionMode)));
            settings.productive.showProjectName = ReadBool(localSettings, L"ProductiveShowProjectName", settings.productive.showProjectName);
            settings.productive.activityTypeOverride = ParseActivityTypeOverride(ReadString(localSettings, L"ProductiveActivityType", L"auto"));
            settings.productive.wordEnabled = ReadBool(localSettings, L"ProductiveAppWordEnabled", settings.productive.wordEnabled);
            settings.productive.excelEnabled = ReadBool(localSettings, L"ProductiveAppExcelEnabled", settings.productive.excelEnabled);
            settings.productive.powerPointEnabled = ReadBool(localSettings, L"ProductiveAppPowerPointEnabled", settings.productive.powerPointEnabled);
            settings.productive.oneNoteEnabled = ReadBool(localSettings, L"ProductiveAppOneNoteEnabled", settings.productive.oneNoteEnabled);
            settings.productive.accessEnabled = ReadBool(localSettings, L"ProductiveAppAccessEnabled", settings.productive.accessEnabled);
            settings.productive.publisherEnabled = ReadBool(localSettings, L"ProductiveAppPublisherEnabled", settings.productive.publisherEnabled);
            settings.productive.visioEnabled = ReadBool(localSettings, L"ProductiveAppVisioEnabled", settings.productive.visioEnabled);
            settings.productive.projectEnabled = ReadBool(localSettings, L"ProductiveAppProjectEnabled", settings.productive.projectEnabled);
            settings.productive.codexEnabled = ReadBool(localSettings, L"ProductiveAppCodexEnabled", settings.productive.codexEnabled);

            settings.creative.enabled = ReadBool(localSettings, L"CreativeEnabled", settings.creative.enabled);
            settings.creative.priority = ParseCreativePriorityMode(
                ReadString(localSettings, L"CreativePriority", ToSettingString(settings.creative.priority)));
            settings.creative.detectionMode = ParseCreativeDetectionMode(
                ReadString(localSettings, L"CreativeDetectionMode", ToSettingString(settings.creative.detectionMode)));
            settings.creative.showProjectName = ReadBool(localSettings, L"CreativeShowProjectName", settings.creative.showProjectName);
            settings.creative.showWindowTitle = ReadBool(localSettings, L"CreativeShowWindowTitle", settings.creative.showWindowTitle);
            settings.creative.activityTypeOverride = ParseActivityTypeOverride(ReadString(localSettings, L"CreativeActivityType", L"auto"));
            settings.creative.photoshopEnabled = ReadBool(localSettings, L"CreativeAppPhotoshopEnabled", settings.creative.photoshopEnabled);
            settings.creative.illustratorEnabled = ReadBool(localSettings, L"CreativeAppIllustratorEnabled", settings.creative.illustratorEnabled);
            settings.creative.premiereEnabled = ReadBool(localSettings, L"CreativeAppPremiereEnabled", settings.creative.premiereEnabled);
            settings.creative.afterEffectsEnabled = ReadBool(localSettings, L"CreativeAppAfterEffectsEnabled", settings.creative.afterEffectsEnabled);
            settings.creative.inDesignEnabled = ReadBool(localSettings, L"CreativeAppInDesignEnabled", settings.creative.inDesignEnabled);
            settings.creative.auditionEnabled = ReadBool(localSettings, L"CreativeAppAuditionEnabled", settings.creative.auditionEnabled);
            settings.creative.mediaEncoderEnabled = ReadBool(localSettings, L"CreativeAppMediaEncoderEnabled", settings.creative.mediaEncoderEnabled);
            settings.creative.lightroomEnabled = ReadBool(localSettings, L"CreativeAppLightroomEnabled", settings.creative.lightroomEnabled);
            settings.creative.lightroomClassicEnabled = ReadBool(localSettings, L"CreativeAppLightroomClassicEnabled", settings.creative.lightroomClassicEnabled);
            settings.creative.inCopyEnabled = ReadBool(localSettings, L"CreativeAppInCopyEnabled", settings.creative.inCopyEnabled);
            settings.creative.dreamweaverEnabled = ReadBool(localSettings, L"CreativeAppDreamweaverEnabled", settings.creative.dreamweaverEnabled);
            settings.creative.animateEnabled = ReadBool(localSettings, L"CreativeAppAnimateEnabled", settings.creative.animateEnabled);
            settings.creative.xdEnabled = ReadBool(localSettings, L"CreativeAppXdEnabled", settings.creative.xdEnabled);
            settings.creative.bridgeEnabled = ReadBool(localSettings, L"CreativeAppBridgeEnabled", settings.creative.bridgeEnabled);
            settings.creative.characterAnimatorEnabled = ReadBool(localSettings, L"CreativeAppCharacterAnimatorEnabled", settings.creative.characterAnimatorEnabled);
            settings.creative.frescoEnabled = ReadBool(localSettings, L"CreativeAppFrescoEnabled", settings.creative.frescoEnabled);
            settings.creative.dimensionEnabled = ReadBool(localSettings, L"CreativeAppDimensionEnabled", settings.creative.dimensionEnabled);
            settings.creative.substanceEnabled = ReadBool(localSettings, L"CreativeAppSubstanceEnabled", settings.creative.substanceEnabled);
            settings.creative.acrobatEnabled = ReadBool(localSettings, L"CreativeAppAcrobatEnabled", settings.creative.acrobatEnabled);
            settings.creative.otherAdobeEnabled = ReadBool(localSettings, L"CreativeAppOtherAdobeEnabled", settings.creative.otherAdobeEnabled);
            settings.creative.privacyMode = ParseCreativePrivacyMode(
                ReadString(localSettings, L"CreativePrivacyMode", ToSettingString(settings.creative.privacyMode)));
            settings.creative.idleBehavior = ParseCreativeIdleBehavior(
                ReadString(localSettings, L"CreativeIdleBehavior", ToSettingString(settings.creative.idleBehavior)));
        }
        catch (...)
        {
        }

        if (hasLaunchOnStartupRegistryValue)
            settings.behavior.launchOnStartup = launchOnStartupFromRegistry;
        if (hasStartMinimizedRegistryValue)
            settings.behavior.startMinimizedToTray = startMinimizedFromRegistry;
        if (hasShowDefaultIdleStatusRegistryValue)
            settings.media.showDefaultIdleStatus = showDefaultIdleStatusFromRegistry;

        return settings;
    }

    void SavePersistedSettings(const PersistedSettings& settings)
    {
        try
        {
            auto localSettings = ApplicationData::Current().LocalSettings();
            auto values = localSettings.Values();

            values.Insert(L"ShowTimestamps", winrt::box_value(settings.media.showTimestamps));
            values.Insert(L"ShowSourceApp", winrt::box_value(settings.media.showSource));
            values.Insert(L"SourceDebugMode", winrt::box_value(settings.media.sourceDebugMode));
            values.Insert(L"ShowPaused", winrt::box_value(settings.media.showPaused));
            values.Insert(L"ShowAlbumArt", winrt::box_value(settings.media.showAlbumArt));
            values.Insert(L"ShowDefaultIdleStatus", winrt::box_value(settings.media.showDefaultIdleStatus));
            values.Insert(L"RichPresenceEnabled", winrt::box_value(settings.behavior.richPresenceEnabled));
            values.Insert(L"CloseToTrayOnClose", winrt::box_value(settings.behavior.closeToTrayOnClose));
            values.Insert(L"LaunchOnStartup", winrt::box_value(settings.behavior.launchOnStartup));
            values.Insert(L"StartMinimizedToTray", winrt::box_value(settings.behavior.startMinimizedToTray));
            values.Insert(L"TrayLeftClickToggles", winrt::box_value(settings.behavior.trayLeftClickToggles));
            values.Insert(L"SensitiveKeywordFilter", winrt::box_value(settings.media.sensitiveKeywordFilter));
            values.Insert(L"StrictBrowserPrivacy", winrt::box_value(settings.media.strictBrowserPrivacy));
            values.Insert(L"SuppressBrowserAlbumArt", winrt::box_value(settings.media.suppressBrowserAlbumArt));
            values.Insert(L"ThemeMode", winrt::box_value(winrt::hstring(ToSettingString(settings.behavior.themeMode))));
            values.Insert(L"BlockedAppSiteTerms", winrt::box_value(winrt::hstring(settings.media.blockedAppSiteTermsRaw)));
            values.Insert(L"MediaActivityType", winrt::box_value(winrt::hstring(ToSettingStringActivityTypeOverride(settings.media.activityTypeOverride))));
            values.Insert(L"CreativeActivityType", winrt::box_value(winrt::hstring(ToSettingStringActivityTypeOverride(settings.creative.activityTypeOverride))));
            values.Insert(L"ProductiveActivityType", winrt::box_value(winrt::hstring(ToSettingStringActivityTypeOverride(settings.productive.activityTypeOverride))));

            values.Insert(L"ProductiveEnabled", winrt::box_value(settings.productive.enabled));
            values.Insert(L"ProductiveDetectionMode", winrt::box_value(winrt::hstring(ToSettingString(settings.productive.detectionMode))));
            values.Insert(L"ProductiveShowProjectName", winrt::box_value(settings.productive.showProjectName));
            values.Insert(L"ProductiveAppWordEnabled", winrt::box_value(settings.productive.wordEnabled));
            values.Insert(L"ProductiveAppExcelEnabled", winrt::box_value(settings.productive.excelEnabled));
            values.Insert(L"ProductiveAppPowerPointEnabled", winrt::box_value(settings.productive.powerPointEnabled));
            values.Insert(L"ProductiveAppOneNoteEnabled", winrt::box_value(settings.productive.oneNoteEnabled));
            values.Insert(L"ProductiveAppAccessEnabled", winrt::box_value(settings.productive.accessEnabled));
            values.Insert(L"ProductiveAppPublisherEnabled", winrt::box_value(settings.productive.publisherEnabled));
            values.Insert(L"ProductiveAppVisioEnabled", winrt::box_value(settings.productive.visioEnabled));
            values.Insert(L"ProductiveAppProjectEnabled", winrt::box_value(settings.productive.projectEnabled));
            values.Insert(L"ProductiveAppCodexEnabled", winrt::box_value(settings.productive.codexEnabled));

            values.Insert(L"CreativeEnabled", winrt::box_value(settings.creative.enabled));
            values.Insert(L"CreativePriority", winrt::box_value(winrt::hstring(ToSettingString(settings.creative.priority))));
            values.Insert(L"CreativeDetectionMode", winrt::box_value(winrt::hstring(ToSettingString(settings.creative.detectionMode))));
            values.Insert(L"CreativeShowProjectName", winrt::box_value(settings.creative.showProjectName));
            values.Insert(L"CreativeShowWindowTitle", winrt::box_value(settings.creative.showWindowTitle));
            values.Insert(L"CreativeAppPhotoshopEnabled", winrt::box_value(settings.creative.photoshopEnabled));
            values.Insert(L"CreativeAppIllustratorEnabled", winrt::box_value(settings.creative.illustratorEnabled));
            values.Insert(L"CreativeAppPremiereEnabled", winrt::box_value(settings.creative.premiereEnabled));
            values.Insert(L"CreativeAppAfterEffectsEnabled", winrt::box_value(settings.creative.afterEffectsEnabled));
            values.Insert(L"CreativeAppInDesignEnabled", winrt::box_value(settings.creative.inDesignEnabled));
            values.Insert(L"CreativeAppAuditionEnabled", winrt::box_value(settings.creative.auditionEnabled));
            values.Insert(L"CreativeAppMediaEncoderEnabled", winrt::box_value(settings.creative.mediaEncoderEnabled));
            values.Insert(L"CreativeAppLightroomEnabled", winrt::box_value(settings.creative.lightroomEnabled));
            values.Insert(L"CreativeAppLightroomClassicEnabled", winrt::box_value(settings.creative.lightroomClassicEnabled));
            values.Insert(L"CreativeAppInCopyEnabled", winrt::box_value(settings.creative.inCopyEnabled));
            values.Insert(L"CreativeAppDreamweaverEnabled", winrt::box_value(settings.creative.dreamweaverEnabled));
            values.Insert(L"CreativeAppAnimateEnabled", winrt::box_value(settings.creative.animateEnabled));
            values.Insert(L"CreativeAppXdEnabled", winrt::box_value(settings.creative.xdEnabled));
            values.Insert(L"CreativeAppBridgeEnabled", winrt::box_value(settings.creative.bridgeEnabled));
            values.Insert(L"CreativeAppCharacterAnimatorEnabled", winrt::box_value(settings.creative.characterAnimatorEnabled));
            values.Insert(L"CreativeAppFrescoEnabled", winrt::box_value(settings.creative.frescoEnabled));
            values.Insert(L"CreativeAppDimensionEnabled", winrt::box_value(settings.creative.dimensionEnabled));
            values.Insert(L"CreativeAppSubstanceEnabled", winrt::box_value(settings.creative.substanceEnabled));
            values.Insert(L"CreativeAppAcrobatEnabled", winrt::box_value(settings.creative.acrobatEnabled));
            values.Insert(L"CreativeAppOtherAdobeEnabled", winrt::box_value(settings.creative.otherAdobeEnabled));
            values.Insert(L"CreativePrivacyMode", winrt::box_value(winrt::hstring(ToSettingString(settings.creative.privacyMode))));
            values.Insert(L"CreativeIdleBehavior", winrt::box_value(winrt::hstring(ToSettingString(settings.creative.idleBehavior))));
        }
        catch (...)
        {
        }

        startup::WriteUserPreferenceBool(startup::kLaunchOnStartupRegistryValueName, settings.behavior.launchOnStartup);
        startup::WriteUserPreferenceBool(startup::kStartMinimizedRegistryValueName, settings.behavior.startMinimizedToTray);
        startup::WriteUserPreferenceBool(startup::kShowDefaultIdleStatusRegistryValueName, settings.media.showDefaultIdleStatus);
    }
}
