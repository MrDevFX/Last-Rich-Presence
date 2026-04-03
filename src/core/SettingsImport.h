#pragma once

#include "SettingsStore.h"

#include <map>
#include <variant>

namespace lrp::settings
{
    using ImportedSettingValue = std::variant<std::monostate, bool, double, std::wstring>;
    using ImportedSettingMap = std::map<std::wstring, ImportedSettingValue>;

    struct RegistryPreferenceOverrides
    {
        bool hasLaunchOnStartup{ false };
        bool launchOnStartup{ false };
        bool hasStartMinimizedToTray{ false };
        bool startMinimizedToTray{ false };
        bool hasShowDefaultIdleStatus{ false };
        bool showDefaultIdleStatus{ false };
    };

    struct ImportedSettingsResult
    {
        PersistedSettings settings;
        std::vector<SettingsIssue> issues;
    };

    inline bool ResolveLaunchOnStartupValue(
        bool localStoredValue,
        bool actualRunStartupEnabled,
        bool hasRegistryOverride,
        bool registryValue)
    {
        if (hasRegistryOverride)
            return registryValue;

        return localStoredValue || actualRunStartupEnabled;
    }

    inline bool ResolveRegistryBackedBoolValue(
        bool localStoredValue,
        bool hasRegistryOverride,
        bool registryValue)
    {
        if (hasRegistryOverride)
            return registryValue;

        return localStoredValue;
    }

    inline void ApplyRegistryPreferenceOverrides(
        PersistedSettings& settings,
        const RegistryPreferenceOverrides& overrides,
        bool actualRunStartupEnabled)
    {
        settings.behavior.launchOnStartup = ResolveLaunchOnStartupValue(
            settings.behavior.launchOnStartup,
            actualRunStartupEnabled,
            overrides.hasLaunchOnStartup,
            overrides.launchOnStartup);
        settings.behavior.startMinimizedToTray = ResolveRegistryBackedBoolValue(
            settings.behavior.startMinimizedToTray,
            overrides.hasStartMinimizedToTray,
            overrides.startMinimizedToTray);
        settings.media.showDefaultIdleStatus = ResolveRegistryBackedBoolValue(
            settings.media.showDefaultIdleStatus,
            overrides.hasShowDefaultIdleStatus,
            overrides.showDefaultIdleStatus);
    }

    inline const ImportedSettingValue* FindImportedSettingValue(
        const ImportedSettingMap& values,
        const wchar_t* key)
    {
        auto it = values.find(key);
        if (it == values.end())
            return nullptr;

        return &it->second;
    }

    inline void AddImportedSettingIssue(
        std::vector<SettingsIssue>& issues,
        const wchar_t* key,
        const wchar_t* message)
    {
        issues.push_back({ key, message });
    }

    inline bool TryReadImportedBool(
        const ImportedSettingMap& values,
        const wchar_t* key,
        bool& valueOut,
        std::vector<SettingsIssue>& issues)
    {
        auto value = FindImportedSettingValue(values, key);
        if (!value)
            return false;

        if (auto boolValue = std::get_if<bool>(value))
        {
            valueOut = *boolValue;
            return true;
        }

        if (auto numberValue = std::get_if<double>(value))
        {
            valueOut = (*numberValue != 0.0);
            return true;
        }

        AddImportedSettingIssue(issues, key, L"Expected a boolean-compatible value; kept the current setting.");
        return false;
    }

    inline bool TryReadImportedString(
        const ImportedSettingMap& values,
        const wchar_t* key,
        std::wstring& valueOut,
        std::vector<SettingsIssue>& issues)
    {
        auto value = FindImportedSettingValue(values, key);
        if (!value)
            return false;

        if (auto stringValue = std::get_if<std::wstring>(value))
        {
            valueOut = *stringValue;
            return true;
        }

        AddImportedSettingIssue(issues, key, L"Expected a string value; kept the current setting.");
        return false;
    }

    template <typename TValue, typename TTryParse>
    inline void ApplyImportedParsedString(
        const ImportedSettingMap& values,
        const wchar_t* key,
        TValue& target,
        TTryParse tryParse,
        std::vector<SettingsIssue>& issues)
    {
        std::wstring raw;
        if (!TryReadImportedString(values, key, raw, issues))
            return;

        TValue parsed{};
        if (!tryParse(raw, parsed))
        {
            AddImportedSettingIssue(issues, key, L"Value is not recognized; kept the current setting.");
            return;
        }

        target = parsed;
    }

    inline ImportedSettingsResult ParseImportedSettings(
        const ImportedSettingMap& values,
        const PersistedSettings& current)
    {
        ImportedSettingsResult result{};
        result.settings = current;

        auto applyBool = [&](const wchar_t* key, bool& target)
        {
            bool parsed = target;
            if (TryReadImportedBool(values, key, parsed, result.issues))
                target = parsed;
        };

        applyBool(L"ShowTimestamps", result.settings.media.showTimestamps);
        applyBool(L"ShowSourceApp", result.settings.media.showSource);
        applyBool(L"SourceDebugMode", result.settings.media.sourceDebugMode);
        applyBool(L"ShowPaused", result.settings.media.showPaused);
        applyBool(L"ShowAlbumArt", result.settings.media.showAlbumArt);
        applyBool(L"ShowDefaultIdleStatus", result.settings.media.showDefaultIdleStatus);
        applyBool(L"SensitiveKeywordFilter", result.settings.media.sensitiveKeywordFilter);
        applyBool(L"StrictBrowserPrivacy", result.settings.media.strictBrowserPrivacy);
        applyBool(L"SuppressBrowserAlbumArt", result.settings.media.suppressBrowserAlbumArt);

        applyBool(L"RichPresenceEnabled", result.settings.behavior.richPresenceEnabled);
        applyBool(L"CloseToTrayOnClose", result.settings.behavior.closeToTrayOnClose);
        applyBool(L"LaunchOnStartup", result.settings.behavior.launchOnStartup);
        applyBool(L"StartMinimizedToTray", result.settings.behavior.startMinimizedToTray);
        applyBool(L"TrayLeftClickToggles", result.settings.behavior.trayLeftClickToggles);

        applyBool(L"ProductiveEnabled", result.settings.productive.enabled);
        applyBool(L"ProductiveShowProjectName", result.settings.productive.showProjectName);
        applyBool(L"ProductiveAppWordEnabled", result.settings.productive.wordEnabled);
        applyBool(L"ProductiveAppExcelEnabled", result.settings.productive.excelEnabled);
        applyBool(L"ProductiveAppPowerPointEnabled", result.settings.productive.powerPointEnabled);
        applyBool(L"ProductiveAppOneNoteEnabled", result.settings.productive.oneNoteEnabled);
        applyBool(L"ProductiveAppAccessEnabled", result.settings.productive.accessEnabled);
        applyBool(L"ProductiveAppPublisherEnabled", result.settings.productive.publisherEnabled);
        applyBool(L"ProductiveAppVisioEnabled", result.settings.productive.visioEnabled);
        applyBool(L"ProductiveAppProjectEnabled", result.settings.productive.projectEnabled);
        applyBool(L"ProductiveAppCodexEnabled", result.settings.productive.codexEnabled);

        applyBool(L"CreativeEnabled", result.settings.creative.enabled);
        applyBool(L"CreativeShowProjectName", result.settings.creative.showProjectName);
        applyBool(L"CreativeShowWindowTitle", result.settings.creative.showWindowTitle);
        applyBool(L"CreativeAppPhotoshopEnabled", result.settings.creative.photoshopEnabled);
        applyBool(L"CreativeAppIllustratorEnabled", result.settings.creative.illustratorEnabled);
        applyBool(L"CreativeAppPremiereEnabled", result.settings.creative.premiereEnabled);
        applyBool(L"CreativeAppAfterEffectsEnabled", result.settings.creative.afterEffectsEnabled);
        applyBool(L"CreativeAppInDesignEnabled", result.settings.creative.inDesignEnabled);
        applyBool(L"CreativeAppAuditionEnabled", result.settings.creative.auditionEnabled);
        applyBool(L"CreativeAppMediaEncoderEnabled", result.settings.creative.mediaEncoderEnabled);
        applyBool(L"CreativeAppLightroomEnabled", result.settings.creative.lightroomEnabled);
        applyBool(L"CreativeAppLightroomClassicEnabled", result.settings.creative.lightroomClassicEnabled);
        applyBool(L"CreativeAppInCopyEnabled", result.settings.creative.inCopyEnabled);
        applyBool(L"CreativeAppDreamweaverEnabled", result.settings.creative.dreamweaverEnabled);
        applyBool(L"CreativeAppAnimateEnabled", result.settings.creative.animateEnabled);
        applyBool(L"CreativeAppXdEnabled", result.settings.creative.xdEnabled);
        applyBool(L"CreativeAppBridgeEnabled", result.settings.creative.bridgeEnabled);
        applyBool(L"CreativeAppCharacterAnimatorEnabled", result.settings.creative.characterAnimatorEnabled);
        applyBool(L"CreativeAppFrescoEnabled", result.settings.creative.frescoEnabled);
        applyBool(L"CreativeAppDimensionEnabled", result.settings.creative.dimensionEnabled);
        applyBool(L"CreativeAppSubstanceEnabled", result.settings.creative.substanceEnabled);
        applyBool(L"CreativeAppAcrobatEnabled", result.settings.creative.acrobatEnabled);
        applyBool(L"CreativeAppOtherAdobeEnabled", result.settings.creative.otherAdobeEnabled);

        std::wstring blockedTerms;
        if (TryReadImportedString(values, L"BlockedAppSiteTerms", blockedTerms, result.issues))
            result.settings.media.blockedAppSiteTermsRaw = std::move(blockedTerms);

        ApplyImportedParsedString(
            values,
            L"MediaActivityType",
            result.settings.media.activityTypeOverride,
            [](const std::wstring& value, int& parsedOut)
            {
                return TryParseActivityTypeOverride(value, parsedOut);
            },
            result.issues);
        ApplyImportedParsedString(
            values,
            L"CreativeActivityType",
            result.settings.creative.activityTypeOverride,
            [](const std::wstring& value, int& parsedOut)
            {
                return TryParseActivityTypeOverride(value, parsedOut);
            },
            result.issues);
        ApplyImportedParsedString(
            values,
            L"ProductiveActivityType",
            result.settings.productive.activityTypeOverride,
            [](const std::wstring& value, int& parsedOut)
            {
                return TryParseActivityTypeOverride(value, parsedOut);
            },
            result.issues);
        ApplyImportedParsedString(
            values,
            L"ThemeMode",
            result.settings.behavior.themeMode,
            [](const std::wstring& value, AppThemeMode& parsedOut)
            {
                return TryParseThemeMode(value, parsedOut);
            },
            result.issues);
        ApplyImportedParsedString(
            values,
            L"ProductiveDetectionMode",
            result.settings.productive.detectionMode,
            [](const std::wstring& value, ProductiveDetectionMode& parsedOut)
            {
                return TryParseProductiveDetectionMode(value, parsedOut);
            },
            result.issues);
        ApplyImportedParsedString(
            values,
            L"CreativePriority",
            result.settings.creative.priority,
            [](const std::wstring& value, CreativePriorityMode& parsedOut)
            {
                return TryParseCreativePriorityMode(value, parsedOut);
            },
            result.issues);
        ApplyImportedParsedString(
            values,
            L"CreativeDetectionMode",
            result.settings.creative.detectionMode,
            [](const std::wstring& value, CreativeDetectionMode& parsedOut)
            {
                return TryParseCreativeDetectionMode(value, parsedOut);
            },
            result.issues);
        ApplyImportedParsedString(
            values,
            L"CreativePrivacyMode",
            result.settings.creative.privacyMode,
            [](const std::wstring& value, CreativePrivacyMode& parsedOut)
            {
                return TryParseCreativePrivacyMode(value, parsedOut);
            },
            result.issues);
        ApplyImportedParsedString(
            values,
            L"CreativeIdleBehavior",
            result.settings.creative.idleBehavior,
            [](const std::wstring& value, CreativeIdleBehavior& parsedOut)
            {
                return TryParseCreativeIdleBehavior(value, parsedOut);
            },
            result.issues);

        return result;
    }
}
