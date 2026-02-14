#include "pch.h"
#include "MediaDetector.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Media::Control;

namespace
{
    std::wstring ToLowerCopy(std::wstring value)
    {
        for (auto& c : value) c = towlower(c);
        return value;
    }

    void StripSuffixInsensitive(std::wstring& text, const std::wstring& suffix)
    {
        if (text.size() < suffix.size() || suffix.empty()) return;

        auto tail = text.substr(text.size() - suffix.size());
        if (ToLowerCopy(tail) == ToLowerCopy(suffix))
            text.erase(text.size() - suffix.size());
    }

    void NormalizeTitleForService(std::wstring& title, const std::wstring& service)
    {
        if (service == L"YouTube Music")
        {
            StripSuffixInsensitive(title, L" - YouTube Music");
            StripSuffixInsensitive(title, L" | YouTube Music");
            StripSuffixInsensitive(title, L" - YouTube");
            StripSuffixInsensitive(title, L" | YouTube");
            return;
        }

        if (service == L"YouTube")
        {
            StripSuffixInsensitive(title, L" - YouTube");
            StripSuffixInsensitive(title, L" | YouTube");
            return;
        }

        if (service == L"Spotify")
        {
            StripSuffixInsensitive(title, L" - Spotify");
            StripSuffixInsensitive(title, L" | Spotify");
            return;
        }

        if (service == L"Apple Music")
        {
            StripSuffixInsensitive(title, L" - Apple Music");
            StripSuffixInsensitive(title, L" | Apple Music");
        }
    }
}

MediaDetector::MediaDetector()
{
}

MediaDetector::~MediaDetector()
{
    Stop();
}

void MediaDetector::Start()
{
    if (m_running.exchange(true)) return;
    ++m_currentRequestID;
    auto generation = ++m_generation;
    InitializeAsync(generation);
}

void MediaDetector::Stop()
{
    m_running = false;
    ++m_currentRequestID;
    ++m_generation;

    std::lock_guard lock(m_winrtMutex);
    if (m_currentSession)
    {
        try { m_currentSession.MediaPropertiesChanged(m_mediaPropsToken); } catch (...) {}
        try { m_currentSession.PlaybackInfoChanged(m_playbackToken); } catch (...) {}
        try { m_currentSession.TimelinePropertiesChanged(m_timelineToken); } catch (...) {}
        m_currentSession = nullptr;
    }

    if (m_sessionManager)
    {
        try { m_sessionManager.SessionsChanged(m_sessionsChangedToken); } catch (...) {}
        m_sessionManager = nullptr;
    }
}

void MediaDetector::SetCallback(MediaChangedCallback callback)
{
    std::lock_guard lock(m_callbackMutex);
    m_callback = std::move(callback);
}

void MediaDetector::InvokeCallback(const MediaInfo& info)
{
    MediaChangedCallback callback;
    {
        std::lock_guard lock(m_callbackMutex);
        callback = m_callback;
    }

    if (callback)
    {
        try { callback(info); } catch (...) {}
    }
}

MediaInfo MediaDetector::GetCurrentMedia() const
{
    std::lock_guard lock(m_mutex);
    return m_currentMedia;
}

void MediaDetector::InitializeAsync(uint64_t generation)
{
    try
    {
        auto weakSelf = weak_from_this();
        auto managerOp = GlobalSystemMediaTransportControlsSessionManager::RequestAsync();
        managerOp.Completed([weakSelf, generation](auto const& op, AsyncStatus status)
        {
            auto self = weakSelf.lock();
            if (!self || status != AsyncStatus::Completed) return;
            if (!self->m_running || generation != self->m_generation.load()) return;

            std::lock_guard lock(self->m_winrtMutex);
            if (!self->m_running || generation != self->m_generation.load()) return;
            self->m_sessionManager = op.GetResults();
            if (!self->m_sessionManager) return;

            self->m_sessionsChangedToken = self->m_sessionManager.SessionsChanged(
                [weakSelf, generation](auto const& manager, auto const& args)
            {
                if (auto strongSelf = weakSelf.lock())
                {
                    strongSelf->OnSessionChanged(manager, args, generation);
                }
            });

            // Get current session
            auto session = self->m_sessionManager.GetCurrentSession();
            if (session)
            {
                self->m_currentSession = session;
                self->m_mediaPropsToken = session.MediaPropertiesChanged(
                    [weakSelf, generation](auto const& s, auto const& a)
                    {
                        if (auto strongSelf = weakSelf.lock()) strongSelf->OnMediaPropertiesChanged(s, a, generation);
                    });
                self->m_playbackToken = session.PlaybackInfoChanged(
                    [weakSelf, generation](auto const& s, auto const& a)
                    {
                        if (auto strongSelf = weakSelf.lock()) strongSelf->OnPlaybackInfoChanged(s, a, generation);
                    });
                self->m_timelineToken = session.TimelinePropertiesChanged(
                    [weakSelf, generation](auto const& s, auto const& a)
                    {
                        if (auto strongSelf = weakSelf.lock()) strongSelf->OnTimelinePropertiesChanged(s, a, generation);
                    });

                self->UpdateMediaInfo(session, generation);
            }
        });
    }
    catch (...) {}
}

void MediaDetector::OnSessionChanged(
    GlobalSystemMediaTransportControlsSessionManager const& manager,
    SessionsChangedEventArgs const&,
    uint64_t generation)
{
    if (!m_running || generation != m_generation.load()) return;
    ++m_currentRequestID;

    GlobalSystemMediaTransportControlsSession session{ nullptr };
    bool notifyEmpty = false;

    {
        std::lock_guard lock(m_winrtMutex);
        if (!m_running || generation != m_generation.load()) return;

        // Unsubscribe from old session (may throw if session is already dead)
        if (m_currentSession)
        {
            try { m_currentSession.MediaPropertiesChanged(m_mediaPropsToken); } catch (...) {}
            try { m_currentSession.PlaybackInfoChanged(m_playbackToken); } catch (...) {}
            try { m_currentSession.TimelinePropertiesChanged(m_timelineToken); } catch (...) {}
            m_currentSession = nullptr;
        }

        try { session = manager.GetCurrentSession(); } catch (...) {}
        m_currentSession = session;

        if (session)
        {
            try
            {
                auto weakSelf = weak_from_this();
                m_mediaPropsToken = session.MediaPropertiesChanged(
                    [weakSelf, generation](auto const& s, auto const& a)
                    {
                        if (auto strongSelf = weakSelf.lock()) strongSelf->OnMediaPropertiesChanged(s, a, generation);
                    });
                m_playbackToken = session.PlaybackInfoChanged(
                    [weakSelf, generation](auto const& s, auto const& a)
                    {
                        if (auto strongSelf = weakSelf.lock()) strongSelf->OnPlaybackInfoChanged(s, a, generation);
                    });
                m_timelineToken = session.TimelinePropertiesChanged(
                    [weakSelf, generation](auto const& s, auto const& a)
                    {
                        if (auto strongSelf = weakSelf.lock()) strongSelf->OnTimelinePropertiesChanged(s, a, generation);
                    });
            }
            catch (...)
            {
                m_currentSession = nullptr;
                session = nullptr;
                notifyEmpty = true;
            }
        }
        else
        {
            notifyEmpty = true;
        }
    }

    if (session)
    {
        UpdateMediaInfo(session, generation);
        return;
    }

    if (notifyEmpty)
    {
        MediaInfo empty{};
        {
            std::lock_guard dataLock(m_mutex);
            m_currentMedia = empty;
        }
        InvokeCallback(empty);
    }
}

void MediaDetector::OnMediaPropertiesChanged(
    GlobalSystemMediaTransportControlsSession const& session,
    MediaPropertiesChangedEventArgs const&,
    uint64_t generation)
{
    if (!m_running || generation != m_generation.load()) return;
    try { UpdateMediaInfo(session, generation); } catch (...) {}
}

void MediaDetector::OnPlaybackInfoChanged(
    GlobalSystemMediaTransportControlsSession const& session,
    PlaybackInfoChangedEventArgs const&,
    uint64_t generation)
{
    if (!m_running || generation != m_generation.load()) return;
    try { UpdateMediaInfo(session, generation); } catch (...) {}
}

void MediaDetector::OnTimelinePropertiesChanged(
    GlobalSystemMediaTransportControlsSession const& session,
    TimelinePropertiesChangedEventArgs const&,
    uint64_t generation)
{
    if (!m_running || generation != m_generation.load()) return;
    try { UpdateMediaInfo(session, generation); } catch (...) {}
}

void MediaDetector::UpdateMediaInfo(
    GlobalSystemMediaTransportControlsSession const& session,
    uint64_t generation)
{
    if (!session) return;
    if (!m_running || generation != m_generation.load()) return;

    try
    {
        int requestID = ++m_currentRequestID;
        auto weakSelf = weak_from_this();
        auto propsOp = session.TryGetMediaPropertiesAsync();
        propsOp.Completed([weakSelf, requestID, session, generation](auto const& op, AsyncStatus status)
        {
            auto self = weakSelf.lock();
            if (!self || status != AsyncStatus::Completed) return;
            if (!self->m_running || generation != self->m_generation.load()) return;
            if (requestID != self->m_currentRequestID.load()) return;

            try
            {
                auto props = op.GetResults();
                if (!props) return;

                MediaInfo info;
                info.title = std::wstring(props.Title());
                info.artist = std::wstring(props.Artist());
                info.albumTitle = std::wstring(props.AlbumTitle());
                info.thumbnail = props.Thumbnail();

                // Source app info
                try
                {
                    auto appId = std::wstring(session.SourceAppUserModelId());
                    info.sourceName = appId;
                    info.sourceDisplayName = self->ResolveSourceName(appId);
                    bool browserSource = self->IsBrowserSource(info.sourceDisplayName);

                    auto detection = self->DetectMusicService(
                        info.title,
                        info.artist,
                        info.albumTitle,
                        appId,
                        info.sourceDisplayName);

                    if (!detection.service.empty())
                    {
                        NormalizeTitleForService(info.title, detection.service);
                        info.sourceDisplayName = detection.service;
                        info.detectedService = detection.service;
                        info.detectionReason = detection.reason;
                        info.detectionScore = detection.score;
                    }
                    else if (browserSource)
                    {
                        info.sourceDisplayName = L"Web Player";
                        info.detectedService = L"";
                        info.detectionReason = L"browser-fallback:web-player";
                        info.detectionScore = 10;
                    }
                    else
                    {
                        info.detectedService = info.sourceDisplayName;
                        info.detectionReason = L"source-resolver";
                        info.detectionScore = info.sourceDisplayName.empty() ? 0 : 25;
                    }
                }
                catch (...) {}

                // Playback status (session may have died)
                try
                {
                    auto playback = session.GetPlaybackInfo();
                    if (playback)
                    {
                        info.isPlaying = (playback.PlaybackStatus() ==
                            GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing);
                    }
                }
                catch (...) {}

                // Timeline (session may have died)
                try
                {
                    auto timeline = session.GetTimelineProperties();
                    if (timeline)
                    {
                        constexpr int64_t kTicksPerSecond = 10'000'000;

                        int64_t startTicks = timeline.StartTime().count();
                        int64_t positionTicks = timeline.Position().count();
                        int64_t endTicks = timeline.EndTime().count();

                        if (startTicks < 0) startTicks = 0;
                        if (positionTicks < 0) positionTicks = 0;
                        if (endTicks < 0) endTicks = 0;

                        if (startTicks > 0)
                        {
                            positionTicks = (positionTicks >= startTicks) ? (positionTicks - startTicks) : 0;
                            endTicks = (endTicks >= startTicks) ? (endTicks - startTicks) : 0;
                        }

                        info.position = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::duration<int64_t, std::ratio<1, kTicksPerSecond>>(positionTicks));
                        info.duration = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::duration<int64_t, std::ratio<1, kTicksPerSecond>>(endTicks));

                        if (info.duration.count() > 0 && info.position > info.duration)
                            info.position = info.duration;

                        if (info.isPlaying)
                        {
                            auto now = std::chrono::system_clock::now();
                            info.startTime = now - info.position;
                        }
                    }
                }
                catch (...) {}

                {
                    std::lock_guard lock(self->m_mutex);
                    self->m_currentMedia = info;
                }

                self->InvokeCallback(info);
            }
            catch (...) {}
        });
    }
    catch (...) {}
}

std::wstring MediaDetector::ResolveSourceName(const std::wstring& appId) const
{
    if (appId.empty()) return L"";

    // Lowercase the app ID for case-insensitive matching
    std::wstring lower = appId;
    for (auto& c : lower) c = towlower(c);

    // Match known apps by substring (handles full paths, UWP IDs, etc.)
    if (lower.find(L"spotify") != std::wstring::npos)
        return L"Spotify";
    if (lower.find(L"applemusic") != std::wstring::npos || lower.find(L"apple music") != std::wstring::npos)
        return L"Apple Music";
    if (lower.find(L"itunes") != std::wstring::npos)
        return L"iTunes";
    if (lower.find(L"zunemusic") != std::wstring::npos)
        return L"Groove Music";
    if (lower.find(L"tidal") != std::wstring::npos)
        return L"Tidal";
    if (lower.find(L"deezer") != std::wstring::npos)
        return L"Deezer";
    if (lower.find(L"foobar2000") != std::wstring::npos)
        return L"foobar2000";
    if (lower.find(L"musicbee") != std::wstring::npos)
        return L"MusicBee";
    if (lower.find(L"aimp") != std::wstring::npos)
        return L"AIMP";
    if (lower.find(L"winamp") != std::wstring::npos)
        return L"Winamp";
    if (lower.find(L"vlc") != std::wstring::npos)
        return L"VLC";
    if (lower.find(L"amazonmusic") != std::wstring::npos || lower.find(L"amazon music") != std::wstring::npos)
        return L"Amazon Music";

    // Browsers
    if (lower.find(L"chrome") != std::wstring::npos)
        return L"Chrome (Web)";
    if (lower.find(L"firefox") != std::wstring::npos)
        return L"Firefox (Web)";
    if (lower.find(L"msedge") != std::wstring::npos || lower.find(L"edge") != std::wstring::npos)
        return L"Edge (Web)";
    if (lower.find(L"opera") != std::wstring::npos)
        return L"Opera (Web)";
    if (lower.find(L"brave") != std::wstring::npos)
        return L"Brave (Web)";
    if (lower.find(L"vivaldi") != std::wstring::npos)
        return L"Vivaldi (Web)";

    // Fallback: strip path/package prefix and .exe extension
    std::wstring name = appId;

    // Extract name after '!' (UWP package IDs) or '\\' (paths)
    auto bangPos = name.rfind(L'!');
    if (bangPos != std::wstring::npos)
        name = name.substr(bangPos + 1);
    else
    {
        auto slashPos = name.rfind(L'\\');
        if (slashPos != std::wstring::npos)
            name = name.substr(slashPos + 1);
    }

    // Strip .exe extension if present (case-insensitive)
    auto dotPos = name.rfind(L'.');
    if (dotPos != std::wstring::npos)
    {
        auto ext = name.substr(dotPos);
        for (auto& c : ext) c = towlower(c);
        if (ext == L".exe")
            name = name.substr(0, dotPos);
    }

    auto lowerName = ToLowerCopy(name);
    if (lowerName.empty() ||
        lowerName == L"app" ||
        lowerName == L"applicationframehost" ||
        lowerName == L"wwahost")
    {
        return L"";
    }

    return name;
}

bool MediaDetector::IsBrowserSource(const std::wstring& displayName)
{
    // Check if the display name indicates a browser
    return displayName.find(L"Chrome") != std::wstring::npos ||
           displayName.find(L"Edge") != std::wstring::npos ||
           displayName.find(L"Firefox") != std::wstring::npos ||
           displayName.find(L"Brave") != std::wstring::npos ||
           displayName.find(L"Opera") != std::wstring::npos ||
           displayName.find(L"Vivaldi") != std::wstring::npos;
}

MediaDetector::SourceDetection MediaDetector::DetectMusicService(
    const std::wstring& title,
    const std::wstring& artist,
    const std::wstring& album,
    const std::wstring& appId,
    const std::wstring& sourceDisplayName)
{
    SourceDetection best{};

    auto lTitle = ToLowerCopy(title);
    auto lArtist = ToLowerCopy(artist);
    auto lAlbum = ToLowerCopy(album);
    auto lAppId = ToLowerCopy(appId);
    auto lSource = ToLowerCopy(sourceDisplayName);

    auto contains = [](const std::wstring& haystack, const wchar_t* needle)
    {
        return haystack.find(needle) != std::wstring::npos;
    };

    auto consider = [&](const wchar_t* service, int score, const wchar_t* reason)
    {
        if (score > best.score)
        {
            best.service = service;
            best.score = score;
            best.reason = reason;
        }
    };

    auto sourceHint = lAppId + L" " + lSource;

    if (contains(lAppId, L"spotify"))
        consider(L"Spotify", 100, L"app-id:spotify");
    if (contains(lSource, L"spotify"))
        consider(L"Spotify", 90, L"source-name:spotify");
    if (contains(lTitle, L"spotify") || contains(lAlbum, L"spotify"))
        consider(L"Spotify", 60, L"metadata:spotify");

    if (contains(lAppId, L"applemusic") || contains(lAppId, L"music.apple"))
        consider(L"Apple Music", 98, L"app-id:apple-music");
    if (contains(sourceHint, L"apple music"))
        consider(L"Apple Music", 88, L"source-name:apple-music");
    if (contains(lTitle, L"apple music") || contains(lAlbum, L"apple music"))
        consider(L"Apple Music", 58, L"metadata:apple-music");

    if (contains(lAppId, L"music.youtube") || contains(lAppId, L"ytmusic"))
        consider(L"YouTube Music", 99, L"app-id:youtube-music");
    if (contains(sourceHint, L"youtube music") || contains(sourceHint, L"youtubemusic") || contains(sourceHint, L"ytmusic"))
        consider(L"YouTube Music", 87, L"source-name:youtube-music");
    if (contains(lTitle, L"youtube music") || contains(lAlbum, L"youtube music"))
        consider(L"YouTube Music", 62, L"metadata:youtube-music");

    if (contains(lAppId, L"youtube.com") && !contains(lAppId, L"music.youtube"))
        consider(L"YouTube", 97, L"app-id:youtube");
    if (contains(sourceHint, L"youtube") && !contains(sourceHint, L"youtube music") && !contains(sourceHint, L"ytmusic"))
        consider(L"YouTube", 82, L"source-name:youtube");
    if (
        contains(lTitle, L" - youtube") ||
        contains(lTitle, L" | youtube") ||
        contains(lArtist, L"youtube"))
        consider(L"YouTube", 70, L"metadata:youtube");

    if (contains(sourceHint, L"jiosaavn") ||
        contains(sourceHint, L"saavn") ||
        contains(lTitle, L"jiosaavn") ||
        contains(lAlbum, L"jiosaavn") ||
        contains(lTitle, L"saavn") ||
        contains(lAlbum, L"saavn"))
        consider(L"JioSaavn", 85, L"service:jiosaavn");

    if (contains(sourceHint, L"gaana") || contains(lTitle, L"gaana") || contains(lAlbum, L"gaana"))
        consider(L"Gaana", 85, L"service:gaana");

    if (contains(sourceHint, L"wynk") || contains(lTitle, L"wynk") || contains(lAlbum, L"wynk"))
        consider(L"Wynk Music", 85, L"service:wynk");

    if (contains(sourceHint, L"soundcloud") || contains(lAlbum, L"soundcloud") || contains(lTitle, L"soundcloud"))
        consider(L"SoundCloud", 84, L"service:soundcloud");

    if (contains(sourceHint, L"bandcamp") || contains(lAlbum, L"bandcamp"))
        consider(L"Bandcamp", 84, L"service:bandcamp");

    if (contains(sourceHint, L"twitch") || contains(lTitle, L"twitch") || contains(lArtist, L"twitch"))
        consider(L"Twitch", 84, L"service:twitch");

    if (contains(sourceHint, L"amazon") || contains(lAlbum, L"amazon"))
        consider(L"Amazon Music", 84, L"service:amazon-music");

    return best;
}
