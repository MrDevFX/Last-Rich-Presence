#pragma once

#include "SettingsModels.h"

#include <vector>

namespace lrp::settings
{
    struct SettingsIssue
    {
        std::wstring key;
        std::wstring message;
    };

    struct SettingsLoadResult
    {
        PersistedSettings settings;
        std::vector<SettingsIssue> issues;
    };

    struct SettingsSaveResult
    {
        bool localSettingsSucceeded{ true };
        bool registrySucceeded{ true };
        std::vector<SettingsIssue> issues;
    };

    SettingsLoadResult LoadPersistedSettingsWithResult();
    SettingsSaveResult SavePersistedSettingsWithResult(const PersistedSettings& settings);
    PersistedSettings LoadPersistedSettings();
    void SavePersistedSettings(const PersistedSettings& settings);
}
