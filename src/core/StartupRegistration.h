#pragma once

namespace lrp::startup
{
    inline constexpr wchar_t kAppSettingsRegistryPath[] = L"Software\\LastProjects\\LastRichPresence";
    inline constexpr wchar_t kLaunchOnStartupRegistryValueName[] = L"LaunchOnStartup";
    inline constexpr wchar_t kStartMinimizedRegistryValueName[] = L"StartMinimizedToTray";
    inline constexpr wchar_t kShowDefaultIdleStatusRegistryValueName[] = L"ShowDefaultIdleStatus";

    bool HasPackageIdentity();
    bool IsRunStartupEnabledForCurrentExecutable();
    bool SetRunStartupEnabledForCurrentExecutable(bool enabled, bool startMinimizedToTray);
    bool TryReadUserPreferenceBool(const wchar_t* valueName, bool& valueOut);
    bool WriteUserPreferenceBool(const wchar_t* valueName, bool value);
}
