#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "DiagnosticsLog.h"
#include "SettingsModels.h"
#include "TextUtilities.h"

namespace
{
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
