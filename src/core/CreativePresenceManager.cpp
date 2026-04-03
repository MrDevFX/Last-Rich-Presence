#include "pch.h"
#include "CreativePresenceManager.h"
#include "ActivityPresenceHelpers.h"
#include "SettingsModels.h"

#include <algorithm>

CreativePresenceManager::CreativePresenceManager()
    : m_discord(APP_ID)
{
}

CreativePresenceManager::~CreativePresenceManager()
{
    Shutdown();
}

void CreativePresenceManager::Initialize()
{
    std::lock_guard lock(m_mutex);
    if (m_initialized)
        return;

    m_discord.Initialize();
    m_initialized = true;
    // A fresh Discord connection may still need an explicit clear before any
    // activity is published, especially after the app is toggled off/on.
    m_lastWasClear = false;
    m_lastFingerprint.clear();
}

void CreativePresenceManager::Shutdown()
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

bool CreativePresenceManager::IsConnected() const
{
    std::lock_guard lock(m_mutex);
    return m_initialized && m_discord.IsConnected();
}

DiscordRpcStatus CreativePresenceManager::GetTransportStatus() const
{
    std::lock_guard lock(m_mutex);
    return m_discord.GetStatus();
}

std::string CreativePresenceManager::WideToUtf8(const std::wstring& wide)
{
    return lrp::WideToUtf8(wide);
}

std::string CreativePresenceManager::ClampWideField(const std::wstring& wide, size_t maxChars)
{
    return lrp::ClampWideField(wide, maxChars);
}

std::string CreativePresenceManager::BuildAssetKeyForApp(const std::wstring& appKey)
{
    if (appKey == L"PHXS") return "photoshop";
    if (appKey == L"ILST") return "illustrator";
    if (appKey == L"XD") return "xd";
    if (appKey == L"BRDG") return "bridge";
    if (appKey == L"CHAN") return "characteranimator";
    if (appKey == L"FRSC") return "fresco";
    if (appKey == L"DIMN") return "dimension";
    if (appKey == L"SBPT") return "substancepainter";
    if (appKey == L"SBDG") return "substancedesigner";
    if (appKey == L"SBSM") return "substancesampler";
    if (appKey == L"SBST") return "substancestager";
    if (appKey == L"SBMD") return "substancemodeler";
    if (appKey == L"PPRO") return "premiere";
    if (appKey == L"AEFT") return "aftereffects";
    if (appKey == L"IDSN") return "indesign";
    if (appKey == L"AICY") return "incopy";
    if (appKey == L"AUDT") return "audition";
    if (appKey == L"DRWV") return "dreamweaver";
    if (appKey == L"FLPR") return "animate";
    if (appKey == L"LTRM") return "lightroom";
    if (appKey == L"LTRC") return "lightroomclassic";
    if (appKey == L"ACRO") return "acrobat";
    if (appKey == L"AME") return "mediaencoder";
    return {};
}

std::string CreativePresenceManager::BuildPresenceFingerprint(const DiscordPresenceData& data)
{
    return lrp::BuildPresenceFingerprint(data);
}

void CreativePresenceManager::UpdateCreativeActivity(const CreativeActivityInfo& info, const CreativePresenceOptions& options)
{
    if (!info.active || options.privacyMode == CreativePresencePrivacyMode::Private)
    {
        ClearCreativeActivity();
        return;
    }

    std::wstring appName = lrp::TrimCopy(info.appName);
    if (appName.empty())
        appName = L"Adobe Creativity App";

    std::wstring details;
    std::wstring state;

    if (options.privacyMode == CreativePresencePrivacyMode::AppOnly)
    {
        details = L"Creativity workflow";
        state = options.heldActivity ? L"Held activity" : L"Working";
    }
    else
    {
        std::wstring project = options.showProjectName ? lrp::TrimCopy(info.projectHint) : L"";
        std::wstring window = options.showWindowTitle ? lrp::TrimCopy(info.windowTitle) : L"";

        if (!project.empty())
            details = L"Working on " + project;
        else
            details = L"Working in " + appName;

        if (!window.empty() && window != project)
            state = window;
        else if (options.heldActivity)
            state = L"Held activity";
        else
            state = L"Editing";
    }

    if (options.heldActivity && state.empty())
        state = L"Held activity";

    DiscordPresenceData presence{};
    presence.activityType = lrp::settings::IsSupportedActivityType(options.activityTypeOverride)
        ? options.activityTypeOverride
        : 0; // Playing
    presence.playing = true;
    presence.targetPid = lrp::ResolveRpcTargetPidForDetectedActivity(info.processId);
    presence.name = ClampWideField(appName, 64);
    presence.details = ClampWideField(details, 96);
    presence.state = ClampWideField(state, 96);

    auto assetKey = BuildAssetKeyForApp(info.appKey);
    if (!assetKey.empty())
    {
        // Use the Adobe app asset as the main card image (large square).
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

void CreativePresenceManager::ClearCreativeActivity()
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
