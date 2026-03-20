#pragma once

#include "SettingsModels.h"

namespace lrp::settings
{
    PersistedSettings LoadPersistedSettings();
    void SavePersistedSettings(const PersistedSettings& settings);
}
