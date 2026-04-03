#include "pch.h"

#include "SettingsStore.h"

#include "SettingsImport.h"
#include "StartupRegistration.h"

namespace
{
    using winrt::Windows::Storage::ApplicationData;
    using winrt::Windows::Storage::ApplicationDataContainer;

    struct StoredBoolValue
    {
        bool present{ false };
        bool valid{ true };
        bool value{ false };
    };

    struct StoredStringValue
    {
        bool present{ false };
        bool valid{ true };
        std::wstring value;
    };

    void AppendIssue(
        std::vector<lrp::settings::SettingsIssue>& issues,
        const wchar_t* key,
        const wchar_t* message)
    {
        issues.push_back({ key, message });
    }

    StoredBoolValue ReadStoredBool(ApplicationDataContainer const& container, const wchar_t* key)
    {
        StoredBoolValue result{};
        auto value = container.Values().TryLookup(key);
        if (!value)
            return result;

        result.present = true;

        try
        {
            result.value = winrt::unbox_value<bool>(value);
        }
        catch (...)
        {
            result.valid = false;
        }

        return result;
    }

    StoredStringValue ReadStoredString(ApplicationDataContainer const& container, const wchar_t* key)
    {
        StoredStringValue result{};
        auto value = container.Values().TryLookup(key);
        if (!value)
            return result;

        result.present = true;

        try
        {
            result.value = std::wstring(winrt::unbox_value<winrt::hstring>(value).c_str());
        }
        catch (...)
        {
            result.valid = false;
        }

        return result;
    }

    void ApplyStoredBoolSetting(
        ApplicationDataContainer const& localSettings,
        const wchar_t* key,
        bool& target,
        std::vector<lrp::settings::SettingsIssue>& issues)
    {
        auto stored = ReadStoredBool(localSettings, key);
        if (!stored.present)
            return;

        if (!stored.valid)
        {
            AppendIssue(issues, key, L"Stored value is not a boolean; kept the current default.");
            return;
        }

        target = stored.value;
    }

    void ApplyStoredStringSetting(
        ApplicationDataContainer const& localSettings,
        const wchar_t* key,
        std::wstring& target,
        std::vector<lrp::settings::SettingsIssue>& issues)
    {
        auto stored = ReadStoredString(localSettings, key);
        if (!stored.present)
            return;

        if (!stored.valid)
        {
            AppendIssue(issues, key, L"Stored value is not a string; kept the current default.");
            return;
        }

        target = std::move(stored.value);
    }

    template <typename TValue, typename TTryParse>
    void ApplyStoredParsedStringSetting(
        ApplicationDataContainer const& localSettings,
        const wchar_t* key,
        TValue& target,
        TTryParse tryParse,
        std::vector<lrp::settings::SettingsIssue>& issues)
    {
        auto stored = ReadStoredString(localSettings, key);
        if (!stored.present)
            return;

        if (!stored.valid)
        {
            AppendIssue(issues, key, L"Stored value is not a string; kept the current default.");
            return;
        }

        TValue parsed{};
        if (!tryParse(stored.value, parsed))
        {
            AppendIssue(issues, key, L"Stored value is not recognized; kept the current default.");
            return;
        }

        target = parsed;
    }
}

namespace lrp::settings
{
    SettingsLoadResult LoadPersistedSettingsWithResult()
    {
        SettingsLoadResult result{};

        RegistryPreferenceOverrides registryOverrides{};
        registryOverrides.hasLaunchOnStartup = startup::TryReadUserPreferenceBool(
            startup::kLaunchOnStartupRegistryValueName,
            registryOverrides.launchOnStartup);
        registryOverrides.hasStartMinimizedToTray = startup::TryReadUserPreferenceBool(
            startup::kStartMinimizedRegistryValueName,
            registryOverrides.startMinimizedToTray);
        registryOverrides.hasShowDefaultIdleStatus = startup::TryReadUserPreferenceBool(
            startup::kShowDefaultIdleStatusRegistryValueName,
            registryOverrides.showDefaultIdleStatus);

        const bool actualRunStartupEnabled = startup::IsRunStartupEnabledForCurrentExecutable();

        try
        {
            auto localSettings = ApplicationData::Current().LocalSettings();

            ApplyStoredBoolSetting(localSettings, L"ShowTimestamps", result.settings.media.showTimestamps, result.issues);
            ApplyStoredBoolSetting(localSettings, L"ShowSourceApp", result.settings.media.showSource, result.issues);
            ApplyStoredBoolSetting(localSettings, L"SourceDebugMode", result.settings.media.sourceDebugMode, result.issues);
            ApplyStoredBoolSetting(localSettings, L"ShowPaused", result.settings.media.showPaused, result.issues);
            ApplyStoredBoolSetting(localSettings, L"ShowAlbumArt", result.settings.media.showAlbumArt, result.issues);
            ApplyStoredBoolSetting(localSettings, L"ShowDefaultIdleStatus", result.settings.media.showDefaultIdleStatus, result.issues);
            ApplyStoredBoolSetting(localSettings, L"SensitiveKeywordFilter", result.settings.media.sensitiveKeywordFilter, result.issues);
            ApplyStoredBoolSetting(localSettings, L"StrictBrowserPrivacy", result.settings.media.strictBrowserPrivacy, result.issues);
            ApplyStoredBoolSetting(localSettings, L"SuppressBrowserAlbumArt", result.settings.media.suppressBrowserAlbumArt, result.issues);
            ApplyStoredStringSetting(localSettings, L"BlockedAppSiteTerms", result.settings.media.blockedAppSiteTermsRaw, result.issues);

            ApplyStoredBoolSetting(localSettings, L"RichPresenceEnabled", result.settings.behavior.richPresenceEnabled, result.issues);
            ApplyStoredBoolSetting(localSettings, L"CloseToTrayOnClose", result.settings.behavior.closeToTrayOnClose, result.issues);
            ApplyStoredBoolSetting(localSettings, L"LaunchOnStartup", result.settings.behavior.launchOnStartup, result.issues);
            ApplyStoredBoolSetting(localSettings, L"StartMinimizedToTray", result.settings.behavior.startMinimizedToTray, result.issues);
            ApplyStoredBoolSetting(localSettings, L"TrayLeftClickToggles", result.settings.behavior.trayLeftClickToggles, result.issues);

            ApplyStoredParsedStringSetting(
                localSettings,
                L"ThemeMode",
                result.settings.behavior.themeMode,
                [](const std::wstring& value, AppThemeMode& parsedOut)
                {
                    return TryParseThemeMode(value, parsedOut);
                },
                result.issues);
            ApplyStoredParsedStringSetting(
                localSettings,
                L"MediaActivityType",
                result.settings.media.activityTypeOverride,
                [](const std::wstring& value, int& parsedOut)
                {
                    return TryParseActivityTypeOverride(value, parsedOut);
                },
                result.issues);

            ApplyStoredBoolSetting(localSettings, L"ProductiveEnabled", result.settings.productive.enabled, result.issues);
            ApplyStoredParsedStringSetting(
                localSettings,
                L"ProductiveDetectionMode",
                result.settings.productive.detectionMode,
                [](const std::wstring& value, ProductiveDetectionMode& parsedOut)
                {
                    return TryParseProductiveDetectionMode(value, parsedOut);
                },
                result.issues);
            ApplyStoredBoolSetting(localSettings, L"ProductiveShowProjectName", result.settings.productive.showProjectName, result.issues);
            ApplyStoredParsedStringSetting(
                localSettings,
                L"ProductiveActivityType",
                result.settings.productive.activityTypeOverride,
                [](const std::wstring& value, int& parsedOut)
                {
                    return TryParseActivityTypeOverride(value, parsedOut);
                },
                result.issues);
            ApplyStoredBoolSetting(localSettings, L"ProductiveAppWordEnabled", result.settings.productive.wordEnabled, result.issues);
            ApplyStoredBoolSetting(localSettings, L"ProductiveAppExcelEnabled", result.settings.productive.excelEnabled, result.issues);
            ApplyStoredBoolSetting(localSettings, L"ProductiveAppPowerPointEnabled", result.settings.productive.powerPointEnabled, result.issues);
            ApplyStoredBoolSetting(localSettings, L"ProductiveAppOneNoteEnabled", result.settings.productive.oneNoteEnabled, result.issues);
            ApplyStoredBoolSetting(localSettings, L"ProductiveAppAccessEnabled", result.settings.productive.accessEnabled, result.issues);
            ApplyStoredBoolSetting(localSettings, L"ProductiveAppPublisherEnabled", result.settings.productive.publisherEnabled, result.issues);
            ApplyStoredBoolSetting(localSettings, L"ProductiveAppVisioEnabled", result.settings.productive.visioEnabled, result.issues);
            ApplyStoredBoolSetting(localSettings, L"ProductiveAppProjectEnabled", result.settings.productive.projectEnabled, result.issues);
            ApplyStoredBoolSetting(localSettings, L"ProductiveAppCodexEnabled", result.settings.productive.codexEnabled, result.issues);

            ApplyStoredBoolSetting(localSettings, L"CreativeEnabled", result.settings.creative.enabled, result.issues);
            ApplyStoredParsedStringSetting(
                localSettings,
                L"CreativePriority",
                result.settings.creative.priority,
                [](const std::wstring& value, CreativePriorityMode& parsedOut)
                {
                    return TryParseCreativePriorityMode(value, parsedOut);
                },
                result.issues);
            ApplyStoredParsedStringSetting(
                localSettings,
                L"CreativeDetectionMode",
                result.settings.creative.detectionMode,
                [](const std::wstring& value, CreativeDetectionMode& parsedOut)
                {
                    return TryParseCreativeDetectionMode(value, parsedOut);
                },
                result.issues);
            ApplyStoredBoolSetting(localSettings, L"CreativeShowProjectName", result.settings.creative.showProjectName, result.issues);
            ApplyStoredBoolSetting(localSettings, L"CreativeShowWindowTitle", result.settings.creative.showWindowTitle, result.issues);
            ApplyStoredParsedStringSetting(
                localSettings,
                L"CreativeActivityType",
                result.settings.creative.activityTypeOverride,
                [](const std::wstring& value, int& parsedOut)
                {
                    return TryParseActivityTypeOverride(value, parsedOut);
                },
                result.issues);
            ApplyStoredBoolSetting(localSettings, L"CreativeAppPhotoshopEnabled", result.settings.creative.photoshopEnabled, result.issues);
            ApplyStoredBoolSetting(localSettings, L"CreativeAppIllustratorEnabled", result.settings.creative.illustratorEnabled, result.issues);
            ApplyStoredBoolSetting(localSettings, L"CreativeAppPremiereEnabled", result.settings.creative.premiereEnabled, result.issues);
            ApplyStoredBoolSetting(localSettings, L"CreativeAppAfterEffectsEnabled", result.settings.creative.afterEffectsEnabled, result.issues);
            ApplyStoredBoolSetting(localSettings, L"CreativeAppInDesignEnabled", result.settings.creative.inDesignEnabled, result.issues);
            ApplyStoredBoolSetting(localSettings, L"CreativeAppAuditionEnabled", result.settings.creative.auditionEnabled, result.issues);
            ApplyStoredBoolSetting(localSettings, L"CreativeAppMediaEncoderEnabled", result.settings.creative.mediaEncoderEnabled, result.issues);
            ApplyStoredBoolSetting(localSettings, L"CreativeAppLightroomEnabled", result.settings.creative.lightroomEnabled, result.issues);
            ApplyStoredBoolSetting(localSettings, L"CreativeAppLightroomClassicEnabled", result.settings.creative.lightroomClassicEnabled, result.issues);
            ApplyStoredBoolSetting(localSettings, L"CreativeAppInCopyEnabled", result.settings.creative.inCopyEnabled, result.issues);
            ApplyStoredBoolSetting(localSettings, L"CreativeAppDreamweaverEnabled", result.settings.creative.dreamweaverEnabled, result.issues);
            ApplyStoredBoolSetting(localSettings, L"CreativeAppAnimateEnabled", result.settings.creative.animateEnabled, result.issues);
            ApplyStoredBoolSetting(localSettings, L"CreativeAppXdEnabled", result.settings.creative.xdEnabled, result.issues);
            ApplyStoredBoolSetting(localSettings, L"CreativeAppBridgeEnabled", result.settings.creative.bridgeEnabled, result.issues);
            ApplyStoredBoolSetting(localSettings, L"CreativeAppCharacterAnimatorEnabled", result.settings.creative.characterAnimatorEnabled, result.issues);
            ApplyStoredBoolSetting(localSettings, L"CreativeAppFrescoEnabled", result.settings.creative.frescoEnabled, result.issues);
            ApplyStoredBoolSetting(localSettings, L"CreativeAppDimensionEnabled", result.settings.creative.dimensionEnabled, result.issues);
            ApplyStoredBoolSetting(localSettings, L"CreativeAppSubstanceEnabled", result.settings.creative.substanceEnabled, result.issues);
            ApplyStoredBoolSetting(localSettings, L"CreativeAppAcrobatEnabled", result.settings.creative.acrobatEnabled, result.issues);
            ApplyStoredBoolSetting(localSettings, L"CreativeAppOtherAdobeEnabled", result.settings.creative.otherAdobeEnabled, result.issues);
            ApplyStoredParsedStringSetting(
                localSettings,
                L"CreativePrivacyMode",
                result.settings.creative.privacyMode,
                [](const std::wstring& value, CreativePrivacyMode& parsedOut)
                {
                    return TryParseCreativePrivacyMode(value, parsedOut);
                },
                result.issues);
            ApplyStoredParsedStringSetting(
                localSettings,
                L"CreativeIdleBehavior",
                result.settings.creative.idleBehavior,
                [](const std::wstring& value, CreativeIdleBehavior& parsedOut)
                {
                    return TryParseCreativeIdleBehavior(value, parsedOut);
                },
                result.issues);
        }
        catch (...)
        {
            AppendIssue(
                result.issues,
                L"LocalSettings",
                L"Failed to access the local settings store; using defaults with any registry-backed overrides.");
        }

        ApplyRegistryPreferenceOverrides(result.settings, registryOverrides, actualRunStartupEnabled);
        return result;
    }

    SettingsSaveResult SavePersistedSettingsWithResult(const PersistedSettings& settings)
    {
        SettingsSaveResult result{};

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
            result.localSettingsSucceeded = false;
            AppendIssue(
                result.issues,
                L"LocalSettings",
                L"Failed to write one or more local settings values; some settings may not persist.");
        }

        if (!result.localSettingsSucceeded)
        {
            result.registrySucceeded = false;
            AppendIssue(
                result.issues,
                L"RegistryBackedPreferences",
                L"Skipped registry-backed preference writes because the local settings save failed.");
            return result;
        }

        auto writeRegistryBool = [&](const wchar_t* key, bool value)
        {
            if (startup::WriteUserPreferenceBool(key, value))
                return;

            result.registrySucceeded = false;
            AppendIssue(
                result.issues,
                key,
                L"Failed to write the registry-backed preference.");
        };

        writeRegistryBool(startup::kLaunchOnStartupRegistryValueName, settings.behavior.launchOnStartup);
        writeRegistryBool(startup::kStartMinimizedRegistryValueName, settings.behavior.startMinimizedToTray);
        writeRegistryBool(startup::kShowDefaultIdleStatusRegistryValueName, settings.media.showDefaultIdleStatus);

        return result;
    }

    PersistedSettings LoadPersistedSettings()
    {
        return LoadPersistedSettingsWithResult().settings;
    }

    void SavePersistedSettings(const PersistedSettings& settings)
    {
        (void)SavePersistedSettingsWithResult(settings);
    }
}
