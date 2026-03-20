#pragma once

#include "AppEnums.h"
#include "TextUtilities.h"

#include <cstdint>
#include <string>

namespace lrp::settings
{
    inline std::wstring NormalizeSettingValue(std::wstring value)
    {
        value = TrimCopy(std::move(value));
        value = ToLowerCopy(std::move(value));

        for (auto& ch : value)
        {
            if (ch == L'_')
                ch = L'-';
        }

        return value;
    }

    struct MediaSettings
    {
        bool showTimestamps{ true };
        bool showSource{ true };
        bool showPaused{ true };
        bool showAlbumArt{ true };
        bool showDefaultIdleStatus{ true };
        bool sourceDebugMode{ false };
        bool sensitiveKeywordFilter{ true };
        bool strictBrowserPrivacy{ false };
        bool suppressBrowserAlbumArt{ false };
        std::wstring blockedAppSiteTermsRaw;
        int activityTypeOverride{ -1 };
    };

    struct BehaviorSettings
    {
        bool richPresenceEnabled{ true };
        bool closeToTrayOnClose{ true };
        bool launchOnStartup{ false };
        bool startMinimizedToTray{ false };
        bool trayLeftClickToggles{ true };
        AppThemeMode themeMode{ AppThemeMode::FollowSystem };
    };

    struct ProductiveSettings
    {
        bool enabled{ true };
        ProductiveDetectionMode detectionMode{ ProductiveDetectionMode::ForegroundPreferredVisibleFallback };
        bool showProjectName{ true };
        int activityTypeOverride{ -1 };
        bool wordEnabled{ true };
        bool excelEnabled{ true };
        bool powerPointEnabled{ true };
        bool oneNoteEnabled{ true };
        bool accessEnabled{ true };
        bool publisherEnabled{ true };
        bool visioEnabled{ true };
        bool projectEnabled{ true };
        bool codexEnabled{ true };
    };

    struct CreativeSettings
    {
        bool enabled{ true };
        CreativePriorityMode priority{ CreativePriorityMode::Auto };
        CreativeDetectionMode detectionMode{ CreativeDetectionMode::ForegroundPreferredVisibleFallback };
        bool showProjectName{ true };
        bool showWindowTitle{ false };
        int activityTypeOverride{ -1 };
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
    };

    struct PersistedSettings
    {
        MediaSettings media;
        BehaviorSettings behavior;
        ProductiveSettings productive;
        CreativeSettings creative;
    };

    inline bool IsSupportedActivityType(int value)
    {
        return value == 0 || value == 2 || value == 3 || value == 5;
    }

    inline int32_t ActivityTypeOverrideToComboIndex(int value)
    {
        switch (value)
        {
        case 0: return 1;
        case 2: return 2;
        case 3: return 3;
        case 5: return 4;
        default: return 0;
        }
    }

    inline std::wstring ToSettingStringActivityTypeOverride(int value)
    {
        switch (value)
        {
        case 0: return L"0";
        case 2: return L"2";
        case 3: return L"3";
        case 5: return L"5";
        default: return L"auto";
        }
    }

    inline int ParseActivityTypeOverride(const std::wstring& value)
    {
        auto normalized = NormalizeSettingValue(value);
        if (normalized == L"playing" || normalized == L"0")
            return 0;
        if (normalized == L"listening" || normalized == L"2")
            return 2;
        if (normalized == L"watching" || normalized == L"3")
            return 3;
        if (normalized == L"competing" || normalized == L"5")
            return 5;
        return -1;
    }

    inline int ActivityTypeOverrideFromComboIndex(int32_t index)
    {
        switch (index)
        {
        case 1: return 0;
        case 2: return 2;
        case 3: return 3;
        case 4: return 5;
        default: return -1;
        }
    }

    inline std::wstring ActivityTypeOverrideLabel(int value)
    {
        switch (value)
        {
        case 0: return L"Playing (0)";
        case 2: return L"Listening (2)";
        case 3: return L"Watching (3)";
        case 5: return L"Competing (5)";
        default: return L"Auto";
        }
    }

    inline CreativePriorityMode CreativePriorityModeFromComboIndex(int32_t index)
    {
        switch (index)
        {
        case 1: return CreativePriorityMode::PreferMedia;
        case 2: return CreativePriorityMode::PreferCreative;
        default: return CreativePriorityMode::Auto;
        }
    }

    inline int32_t CreativePriorityModeToComboIndex(CreativePriorityMode mode)
    {
        switch (mode)
        {
        case CreativePriorityMode::PreferMedia: return 1;
        case CreativePriorityMode::PreferCreative: return 2;
        default: return 0;
        }
    }

    inline std::wstring ToSettingString(CreativePriorityMode mode)
    {
        switch (mode)
        {
        case CreativePriorityMode::PreferMedia: return L"prefer_media";
        case CreativePriorityMode::PreferCreative: return L"prefer_creative";
        default: return L"auto";
        }
    }

    inline CreativePriorityMode ParseCreativePriorityMode(const std::wstring& value)
    {
        auto normalized = NormalizeSettingValue(value);
        if (normalized == L"prefer-media")
            return CreativePriorityMode::PreferMedia;
        if (normalized == L"prefer-creative")
            return CreativePriorityMode::PreferCreative;
        return CreativePriorityMode::Auto;
    }

    inline std::wstring CreativePriorityModeLabel(CreativePriorityMode mode)
    {
        switch (mode)
        {
        case CreativePriorityMode::PreferMedia: return L"Prefer Media";
        case CreativePriorityMode::PreferCreative: return L"Prefer Creativity";
        default: return L"Auto";
        }
    }

    inline CreativePrivacyMode CreativePrivacyModeFromComboIndex(int32_t index)
    {
        switch (index)
        {
        case 1: return CreativePrivacyMode::AppOnly;
        case 2: return CreativePrivacyMode::Private;
        default: return CreativePrivacyMode::Normal;
        }
    }

    inline int32_t CreativePrivacyModeToComboIndex(CreativePrivacyMode mode)
    {
        switch (mode)
        {
        case CreativePrivacyMode::AppOnly: return 1;
        case CreativePrivacyMode::Private: return 2;
        default: return 0;
        }
    }

    inline std::wstring ToSettingString(CreativePrivacyMode mode)
    {
        switch (mode)
        {
        case CreativePrivacyMode::AppOnly: return L"app_only";
        case CreativePrivacyMode::Private: return L"private";
        default: return L"normal";
        }
    }

    inline CreativePrivacyMode ParseCreativePrivacyMode(const std::wstring& value)
    {
        auto normalized = NormalizeSettingValue(value);
        if (normalized == L"app-only")
            return CreativePrivacyMode::AppOnly;
        if (normalized == L"private")
            return CreativePrivacyMode::Private;
        return CreativePrivacyMode::Normal;
    }

    inline std::wstring CreativePrivacyModeLabel(CreativePrivacyMode mode)
    {
        switch (mode)
        {
        case CreativePrivacyMode::AppOnly: return L"App only";
        case CreativePrivacyMode::Private: return L"Private";
        default: return L"Normal";
        }
    }

    inline CreativeIdleBehavior CreativeIdleBehaviorFromComboIndex(int32_t index)
    {
        switch (index)
        {
        case 1: return CreativeIdleBehavior::ClearImmediately;
        default: return CreativeIdleBehavior::HoldLast5Seconds;
        }
    }

    inline int32_t CreativeIdleBehaviorToComboIndex(CreativeIdleBehavior mode)
    {
        switch (mode)
        {
        case CreativeIdleBehavior::ClearImmediately: return 1;
        default: return 0;
        }
    }

    inline std::wstring ToSettingString(CreativeIdleBehavior mode)
    {
        switch (mode)
        {
        case CreativeIdleBehavior::ClearImmediately: return L"clear";
        default: return L"hold_last_5s";
        }
    }

    inline CreativeIdleBehavior ParseCreativeIdleBehavior(const std::wstring& value)
    {
        auto normalized = NormalizeSettingValue(value);
        if (normalized == L"clear-immediately" || normalized == L"clear" || normalized == L"fallback-media")
            return CreativeIdleBehavior::ClearImmediately;
        return CreativeIdleBehavior::HoldLast5Seconds;
    }

    inline std::wstring CreativeIdleBehaviorLabel(CreativeIdleBehavior mode)
    {
        switch (mode)
        {
        case CreativeIdleBehavior::ClearImmediately: return L"Clear immediately";
        default: return L"Hold last (5s)";
        }
    }

    inline CreativeDetectionMode CreativeDetectionModeFromComboIndex(int32_t index)
    {
        switch (index)
        {
        case 1: return CreativeDetectionMode::ForegroundOnly;
        case 2: return CreativeDetectionMode::VisibleWindowOnly;
        default: return CreativeDetectionMode::ForegroundPreferredVisibleFallback;
        }
    }

    inline int32_t CreativeDetectionModeToComboIndex(CreativeDetectionMode mode)
    {
        switch (mode)
        {
        case CreativeDetectionMode::ForegroundOnly: return 1;
        case CreativeDetectionMode::VisibleWindowOnly: return 2;
        default: return 0;
        }
    }

    inline std::wstring ToSettingString(CreativeDetectionMode mode)
    {
        switch (mode)
        {
        case CreativeDetectionMode::ForegroundOnly: return L"foreground_only";
        case CreativeDetectionMode::VisibleWindowOnly: return L"visible_window_only";
        default: return L"foreground_preferred_visible_fallback";
        }
    }

    inline CreativeDetectionMode ParseCreativeDetectionMode(const std::wstring& value)
    {
        auto normalized = NormalizeSettingValue(value);
        if (normalized == L"foreground-only")
            return CreativeDetectionMode::ForegroundOnly;
        if (normalized == L"visible-window-only")
            return CreativeDetectionMode::VisibleWindowOnly;
        return CreativeDetectionMode::ForegroundPreferredVisibleFallback;
    }

    inline std::wstring CreativeDetectionModeLabel(CreativeDetectionMode mode)
    {
        switch (mode)
        {
        case CreativeDetectionMode::ForegroundOnly: return L"Foreground only";
        case CreativeDetectionMode::VisibleWindowOnly: return L"Visible window only";
        default: return L"Foreground preferred + fallback";
        }
    }

    inline ProductiveDetectionMode ProductiveDetectionModeFromComboIndex(int32_t index)
    {
        switch (index)
        {
        case 1: return ProductiveDetectionMode::ForegroundOnly;
        case 2: return ProductiveDetectionMode::VisibleWindowOnly;
        default: return ProductiveDetectionMode::ForegroundPreferredVisibleFallback;
        }
    }

    inline int32_t ProductiveDetectionModeToComboIndex(ProductiveDetectionMode mode)
    {
        switch (mode)
        {
        case ProductiveDetectionMode::ForegroundOnly: return 1;
        case ProductiveDetectionMode::VisibleWindowOnly: return 2;
        default: return 0;
        }
    }

    inline std::wstring ToSettingString(ProductiveDetectionMode mode)
    {
        switch (mode)
        {
        case ProductiveDetectionMode::ForegroundOnly: return L"foreground_only";
        case ProductiveDetectionMode::VisibleWindowOnly: return L"visible_window_only";
        default: return L"foreground_preferred_visible_fallback";
        }
    }

    inline ProductiveDetectionMode ParseProductiveDetectionMode(const std::wstring& value)
    {
        auto normalized = NormalizeSettingValue(value);
        if (normalized == L"foreground-only")
            return ProductiveDetectionMode::ForegroundOnly;
        if (normalized == L"visible-window-only")
            return ProductiveDetectionMode::VisibleWindowOnly;
        return ProductiveDetectionMode::ForegroundPreferredVisibleFallback;
    }

    inline std::wstring ProductiveDetectionModeLabel(ProductiveDetectionMode mode)
    {
        switch (mode)
        {
        case ProductiveDetectionMode::ForegroundOnly: return L"Foreground only";
        case ProductiveDetectionMode::VisibleWindowOnly: return L"Visible window only";
        default: return L"Foreground preferred + fallback";
        }
    }

    inline int32_t ThemeModeToComboIndex(AppThemeMode mode)
    {
        switch (mode)
        {
        case AppThemeMode::Light: return 0;
        case AppThemeMode::Dark: return 1;
        default: return 2;
        }
    }

    inline std::wstring ToSettingString(AppThemeMode mode)
    {
        switch (mode)
        {
        case AppThemeMode::Light: return L"light";
        case AppThemeMode::Dark: return L"dark";
        default: return L"system";
        }
    }

    inline AppThemeMode ParseThemeMode(const std::wstring& value)
    {
        auto normalized = NormalizeSettingValue(value);
        if (normalized == L"light")
            return AppThemeMode::Light;
        if (normalized == L"dark")
            return AppThemeMode::Dark;
        return AppThemeMode::FollowSystem;
    }

    inline AppThemeMode ThemeModeFromComboIndex(int32_t index)
    {
        switch (index)
        {
        case 0: return AppThemeMode::Light;
        case 1: return AppThemeMode::Dark;
        default: return AppThemeMode::FollowSystem;
        }
    }

    inline std::wstring ThemeModeLabel(AppThemeMode mode)
    {
        switch (mode)
        {
        case AppThemeMode::Light: return L"Light";
        case AppThemeMode::Dark: return L"Dark";
        default: return L"System";
        }
    }
}
