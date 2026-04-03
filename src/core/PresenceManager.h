#pragma once
#include "pch.h"
#include "DiscordRPC.h"
#include "MediaDetector.h"
#include <string>
#include <atomic>
#include <mutex>
#include <vector>
#include <thread>
#include <condition_variable>

class PresenceManager : public std::enable_shared_from_this<PresenceManager>
{
public:
    PresenceManager();
    ~PresenceManager();

    void Initialize();
    void Shutdown();

    // Call when media changes — handles art fetching + Discord update
    void UpdateMedia(const MediaInfo& info);
    void ClearPresence();
    void ClearMedia();

    // Re-send presence with current settings (e.g., after toggle change)
    void RefreshPresence();

    // Settings
    void SetShowTimestamps(bool show);
    void SetShowPaused(bool show);
    void SetShowAlbumArt(bool show);
    void SetShowSource(bool show);
    void SetShowIdleStatus(bool show);
    void SetSensitiveKeywordFilter(bool enabled);
    void SetStrictBrowserPrivacy(bool enabled);
    void SetSuppressBrowserAlbumArt(bool enabled);
    void SetBlockedAppSiteTerms(std::vector<std::wstring> terms);
    void SetActivityTypeOverride(int type);

    bool IsConnected() const;
    DiscordRpcStatus GetTransportStatus() const;

private:
    struct ArtFetchJob
    {
        int requestID = 0;
        winrt::Windows::Storage::Streams::IRandomAccessStreamReference thumbnail{nullptr};
        std::string artKey;
        std::string artist;
        std::string title;
    };

    void BuildAndSendPresence(const MediaInfo& info);
    void BuildAndSendIdlePresence();
    void FetchAlbumArtAsync(const MediaInfo& info);
    void StartArtWorker();
    void StopArtWorker();
    void ProcessArtFetchJob(const ArtFetchJob& job);

    // HTTP helpers (static — no instance state needed)
    static std::string FetchFromiTunes(const std::string& artist, const std::string& title);
    static std::string UploadToImgur(const std::vector<uint8_t>& data);

    // Mapping helpers
    static std::string GetPlayerAssetKey(const std::wstring& sourceDisplayName);
    static std::string WideToUtf8(const std::wstring& wide);
    static std::string UrlEncode(const std::string& value);

    // Clamp a string to Discord's 2-128 character requirement (UTF-8 safe)
    static std::string ClampDiscordField(std::string s);

    DiscordRPC m_discord;

    std::atomic<int> m_currentRequestID{0};

    // Current media snapshot (thread-safe)
    MediaInfo m_lastMedia;
    std::mutex m_mediaMutex;

    // Album art URL cache (thread-safe)
    std::mutex m_artMutex;
    std::string m_artUrl;
    std::string m_artCacheKey;

    // Album art worker (single latest-job queue)
    std::mutex m_artWorkerMutex;
    std::condition_variable m_artWorkerCv;
    ArtFetchJob m_pendingArtJob;
    bool m_hasPendingArtJob{false};
    std::thread m_artWorker;
    bool m_artWorkerRunning{false};
    bool m_artWorkerExit{false};

    // Settings (atomic for cross-thread reads)
    std::atomic<bool> m_showTimestamps{true};
    std::atomic<bool> m_showPaused{true};
    std::atomic<bool> m_showAlbumArt{true};
    std::atomic<bool> m_showSource{true};
    std::atomic<bool> m_showIdleStatus{true};
    std::atomic<bool> m_sensitiveKeywordFilter{true};
    std::atomic<bool> m_strictBrowserPrivacy{false};
    std::atomic<bool> m_suppressBrowserAlbumArt{false};
    std::atomic<int> m_activityTypeOverride{-1};
    std::mutex m_blockedTermsMutex;
    std::vector<std::wstring> m_blockedAppSiteTerms;
};
