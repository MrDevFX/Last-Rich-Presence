#pragma once
#include "pch.h"
#include <string>
#include <functional>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <chrono>

namespace GSMTC = winrt::Windows::Media::Control;

struct MediaInfo
{
    std::wstring title;
    std::wstring artist;
    std::wstring albumTitle;
    std::wstring sourceName;      // e.g. "Spotify.exe", "chrome.exe"
    std::wstring sourceDisplayName; // e.g. "Spotify", "YouTube Music"
    std::wstring detectedService;   // service picked by scoring rules
    std::wstring detectionReason;   // winning rule id/name
    int detectionScore = 0;
    bool isPlaying = false;
    std::chrono::seconds position{0};
    std::chrono::seconds duration{0};
    std::chrono::system_clock::time_point startTime;
    winrt::Windows::Storage::Streams::IRandomAccessStreamReference thumbnail{nullptr};
};

class MediaDetector : public std::enable_shared_from_this<MediaDetector>
{
public:
    using MediaChangedCallback = std::function<void(const MediaInfo&)>;

    MediaDetector();
    ~MediaDetector();

    void Start();
    void Stop();
    void SetCallback(MediaChangedCallback callback);
    MediaInfo GetCurrentMedia() const;

private:
    struct SourceDetection
    {
        std::wstring service;
        std::wstring reason;
        int score = 0;
    };

    void InitializeAsync(uint64_t generation);
    void OnSessionChanged(
        GSMTC::GlobalSystemMediaTransportControlsSessionManager const& manager,
        GSMTC::SessionsChangedEventArgs const& args,
        uint64_t generation);
    void OnMediaPropertiesChanged(
        GSMTC::GlobalSystemMediaTransportControlsSession const& session,
        GSMTC::MediaPropertiesChangedEventArgs const& args,
        uint64_t generation);
    void OnPlaybackInfoChanged(
        GSMTC::GlobalSystemMediaTransportControlsSession const& session,
        GSMTC::PlaybackInfoChangedEventArgs const& args,
        uint64_t generation);
    void OnTimelinePropertiesChanged(
        GSMTC::GlobalSystemMediaTransportControlsSession const& session,
        GSMTC::TimelinePropertiesChangedEventArgs const& args,
        uint64_t generation);
    void UpdateMediaInfo(
        GSMTC::GlobalSystemMediaTransportControlsSession const& session,
        uint64_t generation);
    void InvokeCallback(const MediaInfo& info);
    std::wstring ResolveSourceName(const std::wstring& appId) const;
    static bool IsBrowserSource(const std::wstring& displayName);
    static SourceDetection DetectMusicService(
        const std::wstring& title,
        const std::wstring& artist,
        const std::wstring& album,
        const std::wstring& appId,
        const std::wstring& sourceDisplayName);

    GSMTC::GlobalSystemMediaTransportControlsSessionManager m_sessionManager{nullptr};
    GSMTC::GlobalSystemMediaTransportControlsSession m_currentSession{nullptr};
    winrt::event_token m_sessionsChangedToken;
    winrt::event_token m_mediaPropsToken;
    winrt::event_token m_playbackToken;
    winrt::event_token m_timelineToken;

    MediaInfo m_currentMedia;
    mutable std::mutex m_mutex;
    mutable std::mutex m_winrtMutex;
    mutable std::mutex m_callbackMutex;
    MediaChangedCallback m_callback;
    std::atomic<bool> m_running{false};
    std::atomic<int> m_currentRequestID{0};
    std::atomic<uint64_t> m_generation{0};
};
