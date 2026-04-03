#pragma once
#include "pch.h"

#include "DiscordRPC.h"
#include "ProductiveDetector.h"

enum class ProductivePresencePrivacyMode
{
    Normal = 0,
    AppOnly = 1,
    Private = 2
};

struct ProductivePresenceOptions
{
    ProductivePresencePrivacyMode privacyMode{ ProductivePresencePrivacyMode::Normal };
    bool showProjectName{ true };
    bool showWindowTitle{ false };
    int activityTypeOverride{ -1 };
};

class ProductivePresenceManager
{
public:
    static constexpr const char* APP_ID = "1476916272909258956";

    ProductivePresenceManager();
    ~ProductivePresenceManager();

    void Initialize();
    void Shutdown();
    bool IsConnected() const;
    DiscordRpcStatus GetTransportStatus() const;

    void UpdateProductiveActivity(const ProductiveActivityInfo& info, const ProductivePresenceOptions& options);
    void ClearProductiveActivity();

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
