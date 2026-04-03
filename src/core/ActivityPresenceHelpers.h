#pragma once

#include "TextUtilities.h"

#include <sstream>
#include <string>

#include <windows.h>

namespace lrp
{
    inline uint32_t ResolveRpcTargetPidForDetectedActivity(uint32_t detectedProcessId) noexcept
    {
        (void)detectedProcessId;
        // Discord RPC expects the PID of the application connected to the RPC
        // socket. Returning 0 lets DiscordRPC fall back to the current Last Rich
        // Presence process ID instead of forwarding a foreign app PID.
        return 0;
    }

    inline std::wstring ClampWide(std::wstring value, size_t maxChars)
    {
        value = TrimCopy(std::move(value));
        if (maxChars == 0)
            return {};

        if (value.size() <= maxChars)
            return value;

        if (maxChars <= 3)
            return value.substr(0, maxChars);

        value.resize(maxChars - 3);
        value += L"...";
        return value;
    }

    inline std::string WideToUtf8(const std::wstring& wide)
    {
        if (wide.empty())
            return {};

        int size = WideCharToMultiByte(
            CP_UTF8,
            0,
            wide.c_str(),
            static_cast<int>(wide.size()),
            nullptr,
            0,
            nullptr,
            nullptr);
        if (size <= 0)
            return {};

        std::string result(static_cast<size_t>(size), '\0');
        WideCharToMultiByte(
            CP_UTF8,
            0,
            wide.c_str(),
            static_cast<int>(wide.size()),
            result.data(),
            size,
            nullptr,
            nullptr);
        return result;
    }

    inline std::string ClampWideField(const std::wstring& wide, size_t maxChars)
    {
        return WideToUtf8(ClampWide(wide, maxChars));
    }

    template <typename PresenceData>
    std::string BuildPresenceFingerprint(const PresenceData& data)
    {
        std::ostringstream oss;
        oss << data.name << '\n'
            << data.details << '\n'
            << data.state << '\n'
            << data.largeImageKey << '\n'
            << data.largeImageText << '\n'
            << data.smallImageKey << '\n'
            << data.smallImageText << '\n'
            << data.startTimestamp << '\n'
            << data.endTimestamp << '\n'
            << data.activityType << '\n'
            << (data.playing ? 1 : 0);
        return oss.str();
    }
}
