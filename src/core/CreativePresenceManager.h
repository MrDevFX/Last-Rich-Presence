#pragma once
#include "pch.h"

#include "DiscordRPC.h"
#include "CreativeDetector.h"

enum class CreativePresencePrivacyMode
{
    Normal = 0,
    AppOnly = 1,
    Private = 2
};

struct CreativePresenceOptions
{
    CreativePresencePrivacyMode privacyMode{ CreativePresencePrivacyMode::Normal };
    bool showProjectName{ true };
    bool showWindowTitle{ false };
    bool heldActivity{ false };
    int activityTypeOverride{ -1 };
};

class CreativePresenceManager
{
public:
    static constexpr const char* APP_ID = "1476655689580412948";

    CreativePresenceManager();
    ~CreativePresenceManager();

    void Initialize();
    void Shutdown();
    bool IsConnected() const;

    void UpdateCreativeActivity(const CreativeActivityInfo& info, const CreativePresenceOptions& options);
    void ClearCreativeActivity();

private:
    static std::string WideToUtf8(const std::wstring& wide);
    static std::string ClampWideField(const std::wstring& wide, size_t maxChars);
    static std::string BuildAssetKeyForApp(const std::wstring& appKey);
    static std::string BuildPresenceFingerprint(const DiscordPresenceData& data);

    DiscordRPC m_discord;
    mutable std::mutex m_mutex;
    bool m_initialized{ false };
    bool m_lastWasClear{ true };
    std::string m_lastFingerprint;
};
