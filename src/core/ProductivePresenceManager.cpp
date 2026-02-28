#include "pch.h"
#include "ProductivePresenceManager.h"

#include <algorithm>

namespace
{
    bool IsSupportedActivityType(int value)
    {
        return value == 0 || value == 2 || value == 3 || value == 5;
    }

    std::wstring TrimCopy(std::wstring value)
    {
        auto isWs = [](wchar_t ch)
        {
            return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
        };

        while (!value.empty() && isWs(value.front()))
            value.erase(value.begin());
        while (!value.empty() && isWs(value.back()))
            value.pop_back();
        return value;
    }

    std::wstring ClampWide(std::wstring value, size_t maxChars)
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
}

ProductivePresenceManager::ProductivePresenceManager()
    : m_discord(APP_ID)
{
}

ProductivePresenceManager::~ProductivePresenceManager()
{
    Shutdown();
}

void ProductivePresenceManager::Initialize()
{
    std::lock_guard lock(m_mutex);
    if (m_initialized)
        return;

    m_discord.Initialize();
    m_initialized = true;
    m_lastWasClear = true;
    m_lastFingerprint.clear();
}

void ProductivePresenceManager::Shutdown()
{
    std::lock_guard lock(m_mutex);
    if (!m_initialized)
        return;

    m_discord.ClearPresence();
    m_discord.Shutdown();
    m_initialized = false;
    m_lastWasClear = true;
    m_lastFingerprint.clear();
}

bool ProductivePresenceManager::IsConnected() const
{
    std::lock_guard lock(m_mutex);
    return m_initialized && m_discord.IsConnected();
}

std::string ProductivePresenceManager::WideToUtf8(const std::wstring& wide)
{
    if (wide.empty())
        return {};
    return winrt::to_string(winrt::hstring(wide));
}

std::string ProductivePresenceManager::ClampWideField(const std::wstring& wide, size_t maxChars)
{
    auto clamped = ClampWide(wide, maxChars);
    return WideToUtf8(clamped);
}

std::string ProductivePresenceManager::BuildAssetKeyForApp(const std::wstring& appKey)
{
    if (appKey == L"WORD") return "word";
    if (appKey == L"XCEL") return "excel";
    if (appKey == L"PPT") return "powerpoint";
    if (appKey == L"ONEN") return "onenote";
    if (appKey == L"ACCS") return "access";
    if (appKey == L"PUBR") return "publisher";
    if (appKey == L"VISI") return "visio";
    if (appKey == L"PROJ") return "project";
    return {};
}

std::string ProductivePresenceManager::BuildPresenceFingerprint(const DiscordPresenceData& data)
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

void ProductivePresenceManager::UpdateProductiveActivity(const ProductiveActivityInfo& info, const ProductivePresenceOptions& options)
{
    if (!info.active || options.privacyMode == ProductivePresencePrivacyMode::Private)
    {
        ClearProductiveActivity();
        return;
    }

    std::wstring appName = TrimCopy(info.appName);
    if (appName.empty())
        appName = L"Microsoft Office";

    std::wstring details;
    std::wstring state;

    if (options.privacyMode == ProductivePresencePrivacyMode::AppOnly)
    {
        details = L"Productive workflow";
        state = L"Working";
    }
    else
    {
        std::wstring project = options.showProjectName ? TrimCopy(info.projectHint) : L"";
        std::wstring window = options.showWindowTitle ? TrimCopy(info.windowTitle) : L"";

        if (!project.empty())
            details = L"Working on " + project;
        else
            details = L"Working in " + appName;

        if (!window.empty() && window != project)
            state = window;
        else
            state = L"Editing";
    }

    DiscordPresenceData presence{};
    // Default is Competing (5) to coexist with Creativity + Media cards,
    // with optional per-section override from settings.
    presence.activityType = IsSupportedActivityType(options.activityTypeOverride)
        ? options.activityTypeOverride
        : 5;
    presence.playing = true;
    presence.targetPid = info.processId;
    presence.name = "Work Session";
    presence.details = ClampWideField(details, 96);
    presence.state = ClampWideField(state, 96);

    auto assetKey = BuildAssetKeyForApp(info.appKey);
    if (!assetKey.empty())
    {
        presence.largeImageKey = assetKey;
        presence.largeImageText = ClampWideField(appName, 64);
    }

    std::string fingerprint = BuildPresenceFingerprint(presence);

    std::lock_guard lock(m_mutex);
    if (!m_initialized)
        return;

    if (!m_lastWasClear && fingerprint == m_lastFingerprint)
        return;

    m_discord.UpdatePresence(presence);
    m_lastFingerprint = std::move(fingerprint);
    m_lastWasClear = false;
}

void ProductivePresenceManager::ClearProductiveActivity()
{
    std::lock_guard lock(m_mutex);
    if (!m_initialized)
        return;

    if (m_lastWasClear)
        return;

    m_discord.ClearPresence();
    m_lastWasClear = true;
    m_lastFingerprint.clear();
}
