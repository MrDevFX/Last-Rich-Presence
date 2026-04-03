#include "pch.h"

#include "StartupRegistration.h"

#include "TextUtilities.h"

#include <appmodel.h>

namespace
{
    std::wstring GetCurrentExecutablePath()
    {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
        return path;
    }

    std::wstring BuildStartupCommandLine(bool startMinimizedToTray)
    {
        std::wstring commandLine = L"\"" + GetCurrentExecutablePath() + L"\"";
        if (startMinimizedToTray)
            commandLine += L" --start-minimized";
        return commandLine;
    }
}

namespace lrp::startup
{
    bool HasPackageIdentity()
    {
        UINT32 length = 0;
        auto result = GetCurrentPackageFullName(&length, nullptr);
        return result == ERROR_INSUFFICIENT_BUFFER;
    }

    bool IsRunStartupEnabledForCurrentExecutable()
    {
        HKEY runKey = nullptr;
        auto openResult = RegOpenKeyExW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0,
            KEY_QUERY_VALUE,
            &runKey);
        if (openResult != ERROR_SUCCESS)
            return false;

        std::wstring expectedExe = lrp::ToLowerCopy(GetCurrentExecutablePath());
        DWORD valueType = 0;
        wchar_t buffer[2048]{};
        DWORD bufferSize = sizeof(buffer);
        auto queryResult = RegQueryValueExW(
            runKey,
            L"LastRichPresence",
            nullptr,
            &valueType,
            reinterpret_cast<LPBYTE>(buffer),
            &bufferSize);
        RegCloseKey(runKey);

        if (queryResult != ERROR_SUCCESS || valueType != REG_SZ)
            return false;

        std::wstring configured = lrp::ToLowerCopy(buffer);
        return configured.find(expectedExe) != std::wstring::npos;
    }

    bool SetRunStartupEnabledForCurrentExecutable(bool enabled, bool startMinimizedToTray)
    {
        HKEY runKey = nullptr;
        auto createResult = RegCreateKeyExW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0,
            nullptr,
            0,
            KEY_SET_VALUE,
            nullptr,
            &runKey,
            nullptr);
        if (createResult != ERROR_SUCCESS)
            return false;

        bool ok = true;
        if (enabled)
        {
            auto commandLine = BuildStartupCommandLine(startMinimizedToTray);
            auto setResult = RegSetValueExW(
                runKey,
                L"LastRichPresence",
                0,
                REG_SZ,
                reinterpret_cast<const BYTE*>(commandLine.c_str()),
                static_cast<DWORD>((commandLine.size() + 1) * sizeof(wchar_t)));
            ok = (setResult == ERROR_SUCCESS);
        }
        else
        {
            auto deleteResult = RegDeleteValueW(runKey, L"LastRichPresence");
            ok = (deleteResult == ERROR_SUCCESS || deleteResult == ERROR_FILE_NOT_FOUND);
        }

        RegCloseKey(runKey);
        return ok;
    }

    bool TryReadUserPreferenceBool(const wchar_t* valueName, bool& valueOut)
    {
        valueOut = false;

        HKEY settingsKey = nullptr;
        auto openResult = RegOpenKeyExW(
            HKEY_CURRENT_USER,
            kAppSettingsRegistryPath,
            0,
            KEY_QUERY_VALUE,
            &settingsKey);
        if (openResult != ERROR_SUCCESS)
            return false;

        DWORD valueType = 0;
        DWORD valueData = 0;
        DWORD valueSize = sizeof(valueData);
        auto queryResult = RegQueryValueExW(
            settingsKey,
            valueName,
            nullptr,
            &valueType,
            reinterpret_cast<LPBYTE>(&valueData),
            &valueSize);
        RegCloseKey(settingsKey);

        if (queryResult != ERROR_SUCCESS || valueType != REG_DWORD || valueSize < sizeof(valueData))
            return false;

        valueOut = (valueData != 0);
        return true;
    }

    bool WriteUserPreferenceBool(const wchar_t* valueName, bool value)
    {
        HKEY settingsKey = nullptr;
        auto createResult = RegCreateKeyExW(
            HKEY_CURRENT_USER,
            kAppSettingsRegistryPath,
            0,
            nullptr,
            0,
            KEY_SET_VALUE,
            nullptr,
            &settingsKey,
            nullptr);
        if (createResult != ERROR_SUCCESS)
            return false;

        DWORD valueData = value ? 1u : 0u;
        auto setResult = RegSetValueExW(
            settingsKey,
            valueName,
            0,
            REG_DWORD,
            reinterpret_cast<const BYTE*>(&valueData),
            sizeof(valueData));

        RegCloseKey(settingsKey);
        return setResult == ERROR_SUCCESS;
    }
}
