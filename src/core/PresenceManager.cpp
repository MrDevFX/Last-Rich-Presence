#include "pch.h"
#include "PresenceManager.h"

#include <sstream>
#include <cstdint>

namespace
{
    bool IsSupportedActivityType(int value)
    {
        return value == 0 || value == 2 || value == 3 || value == 5;
    }

    int ResolveActivityTypeOrDefault(int overrideType, int fallbackType)
    {
        return IsSupportedActivityType(overrideType) ? overrideType : fallbackType;
    }

    std::wstring ToLowerCopy(std::wstring value)
    {
        for (auto& c : value) c = towlower(c);
        return value;
    }

    bool Contains(const std::wstring& haystack, const wchar_t* needle)
    {
        return haystack.find(needle) != std::wstring::npos;
    }

    bool ContainsAnyKeyword(
        const std::wstring& haystack,
        std::initializer_list<const wchar_t*> keywords)
    {
        for (auto keyword : keywords)
        {
            if (haystack.find(keyword) != std::wstring::npos)
                return true;
        }
        return false;
    }

    bool IsBrowserContext(const MediaInfo& info)
    {
        auto sourceName = ToLowerCopy(info.sourceName);
        auto sourceDisplay = ToLowerCopy(info.sourceDisplayName);

        return
            Contains(sourceName, L"chrome") ||
            Contains(sourceName, L"msedge") ||
            Contains(sourceName, L"edge") ||
            Contains(sourceName, L"firefox") ||
            Contains(sourceName, L"brave") ||
            Contains(sourceName, L"opera") ||
            Contains(sourceName, L"vivaldi") ||
            Contains(sourceDisplay, L"chrome") ||
            Contains(sourceDisplay, L"edge") ||
            Contains(sourceDisplay, L"firefox") ||
            Contains(sourceDisplay, L"brave") ||
            Contains(sourceDisplay, L"opera") ||
            Contains(sourceDisplay, L"vivaldi") ||
            Contains(sourceDisplay, L"web player");
    }

    std::wstring BuildMediaSearchBlob(const MediaInfo& info)
    {
        std::wstring blob;
        blob.reserve(
            info.title.size() + info.artist.size() + info.albumTitle.size() +
            info.sourceName.size() + info.sourceDisplayName.size() +
            info.detectedService.size() + info.detectionReason.size() + 16);

        blob += info.title;
        blob += L"\n";
        blob += info.artist;
        blob += L"\n";
        blob += info.albumTitle;
        blob += L"\n";
        blob += info.sourceName;
        blob += L"\n";
        blob += info.sourceDisplayName;
        blob += L"\n";
        blob += info.detectedService;
        blob += L"\n";
        blob += info.detectionReason;

        return ToLowerCopy(std::move(blob));
    }

    bool MatchesAnyBlockedTerm(const std::wstring& lowerBlob, const std::vector<std::wstring>& blockedTerms)
    {
        for (const auto& rawTerm : blockedTerms)
        {
            auto term = ToLowerCopy(rawTerm);
            if (term.empty())
                continue;

            if (lowerBlob.find(term) != std::wstring::npos)
                return true;
        }

        return false;
    }

    bool IsTrustedWebService(const MediaInfo& info)
    {
        auto detected = ToLowerCopy(info.detectedService);
        auto display = ToLowerCopy(info.sourceDisplayName);
        auto service = !detected.empty() ? detected : display;

        return
            service == L"youtube" ||
            service == L"youtube music" ||
            service == L"spotify" ||
            service == L"apple music" ||
            service == L"soundcloud" ||
            service == L"deezer" ||
            service == L"tidal" ||
            service == L"bandcamp" ||
            service == L"mixcloud" ||
            service == L"twitch" ||
            service == L"jiosaavn" ||
            service == L"gaana" ||
            service == L"wynk music" ||
            service == L"amazon music";
    }

    bool IsSensitiveMedia(const MediaInfo& info, bool keywordFilterEnabled, bool strictBrowserPrivacy)
    {
        auto lower = BuildMediaSearchBlob(info);
        bool extensionManaged =
            lower.find(L"extension:") != std::wstring::npos &&
            lower.find(L"extension:pending") == std::wstring::npos;

        if (keywordFilterEnabled && ContainsAnyKeyword(lower,
        {
            L"pornhub", L"xvideos", L"xnxx", L"xhamster", L"redtube", L"youporn",
            L"xnxx.com", L"xvideos.com", L"xhamster.com", L"redtube.com", L"youporn.com",
            L"youporn", L"spankbang", L"tube8", L"beeg", L"tnaflix", L"sunporno",
            L"eporner", L"hclips", L"drtuber", L"porn.com", L"pornhd", L"youjizz",
            L"hqporner", L"camsoda", L"chaturbate", L"stripchat", L"cam4", L"myfreecams",
            L"onlyfans", L"fansly", L"manyvids", L"brazzers", L"bangbros", L"naughtyamerica",
            L"realitykings", L"mofos", L"evilangel", L"vixen", L"hentai", L"rule34",
            L"nsfw", L"adult video", L"adult site", L"sex video", L"porn", L"xxx"
        }))
        {
            return true;
        }

        if (IsBrowserContext(info) && !IsTrustedWebService(info) && !extensionManaged)
        {
            if (strictBrowserPrivacy)
                return true;

            bool likelyRealTrack = !info.title.empty() && (!info.artist.empty() || !info.albumTitle.empty());
            return !likelyRealTrack;
        }

        return false;
    }
}

PresenceManager::PresenceManager() {}

PresenceManager::~PresenceManager()
{
    Shutdown();
}

void PresenceManager::Initialize()
{
    m_discord.Initialize();
    StartArtWorker();
    BuildAndSendIdlePresence();
}

void PresenceManager::Shutdown()
{
    ++m_currentRequestID;
    StopArtWorker();

    m_discord.ClearPresence();
    m_discord.Shutdown();
}

bool PresenceManager::IsConnected() const
{
    return m_discord.IsConnected();
}

// --- Settings ---

void PresenceManager::SetShowTimestamps(bool show) { m_showTimestamps = show; }
void PresenceManager::SetShowPaused(bool show)     { m_showPaused = show; }
void PresenceManager::SetShowSource(bool show)     { m_showSource = show; }
void PresenceManager::SetSensitiveKeywordFilter(bool enabled) { m_sensitiveKeywordFilter = enabled; }
void PresenceManager::SetStrictBrowserPrivacy(bool enabled) { m_strictBrowserPrivacy = enabled; }

void PresenceManager::SetActivityTypeOverride(int type)
{
    m_activityTypeOverride = IsSupportedActivityType(type) ? type : -1;
}

void PresenceManager::SetBlockedAppSiteTerms(std::vector<std::wstring> terms)
{
    std::lock_guard<std::mutex> lock(m_blockedTermsMutex);
    m_blockedAppSiteTerms = std::move(terms);
}

void PresenceManager::SetSuppressBrowserAlbumArt(bool enabled)
{
    m_suppressBrowserAlbumArt = enabled;
    if (enabled)
    {
        ++m_currentRequestID;
        {
            std::lock_guard lock(m_artMutex);
            m_artUrl.clear();
            m_artCacheKey.clear();
        }

        {
            std::lock_guard lock(m_artWorkerMutex);
            m_hasPendingArtJob = false;
        }
        m_artWorkerCv.notify_one();
    }
}

void PresenceManager::SetShowAlbumArt(bool show)
{
    m_showAlbumArt = show;
    if (!show)
    {
        ++m_currentRequestID;
        {
            std::lock_guard lock(m_artMutex);
            m_artUrl.clear();
            m_artCacheKey.clear();
        }

        {
            std::lock_guard workerLock(m_artWorkerMutex);
            m_hasPendingArtJob = false;
        }
        m_artWorkerCv.notify_one();
    }
}

// --- Media updates ---

void PresenceManager::UpdateMedia(const MediaInfo& info)
{
    {
        std::lock_guard lock(m_mediaMutex);
        m_lastMedia = info;
    }

    if (info.title.empty())
    {
        ClearMedia();
        return;
    }

    bool customBlocked = false;
    {
        std::lock_guard<std::mutex> lock(m_blockedTermsMutex);
        if (!m_blockedAppSiteTerms.empty())
        {
            auto lower = BuildMediaSearchBlob(info);
            customBlocked = MatchesAnyBlockedTerm(lower, m_blockedAppSiteTerms);
        }
    }

    bool sensitive = customBlocked || IsSensitiveMedia(
        info,
        m_sensitiveKeywordFilter.load(),
        m_strictBrowserPrivacy.load());

    bool suppressBrowserArt = m_suppressBrowserAlbumArt.load() && IsBrowserContext(info);

    if (sensitive)
    {
        ++m_currentRequestID;

        {
            std::lock_guard lock(m_artMutex);
            m_artUrl.clear();
            m_artCacheKey.clear();
        }

        {
            std::lock_guard lock(m_artWorkerMutex);
            m_hasPendingArtJob = false;
        }

        m_artWorkerCv.notify_one();
    }
    else if (suppressBrowserArt)
    {
        ++m_currentRequestID;

        {
            std::lock_guard lock(m_artMutex);
            m_artUrl.clear();
            m_artCacheKey.clear();
        }

        {
            std::lock_guard lock(m_artWorkerMutex);
            m_hasPendingArtJob = false;
        }

        m_artWorkerCv.notify_one();
    }
    else
    {
        // Fetch album art in background (de-duped by cache key)
        FetchAlbumArtAsync(info);
    }

    // Build and send presence immediately (with whatever art URL we have cached)
    BuildAndSendPresence(info);
}

void PresenceManager::ClearMedia()
{
    ++m_currentRequestID;

    {
        std::lock_guard lock(m_mediaMutex);
        m_lastMedia = {};
    }
    {
        std::lock_guard lock(m_artMutex);
        m_artUrl.clear();
        m_artCacheKey.clear();
    }

    {
        std::lock_guard lock(m_artWorkerMutex);
        m_hasPendingArtJob = false;
    }

    BuildAndSendIdlePresence();
}

void PresenceManager::RefreshPresence()
{
    MediaInfo info;
    {
        std::lock_guard lock(m_mediaMutex);
        info = m_lastMedia;
    }
    if (!info.title.empty())
        BuildAndSendPresence(info);
    else
        BuildAndSendIdlePresence();
}

// --- Core presence builder ---

void PresenceManager::BuildAndSendIdlePresence()
{
    // Media idle should clear the media app activity entirely so separate
    // app IDs (Creativity/Productive) can surface without an always-on
    // placeholder media card.
    m_discord.ClearPresence();
}

void PresenceManager::BuildAndSendPresence(const MediaInfo& info)
{
    if (info.title.empty())
    {
        BuildAndSendIdlePresence();
        return;
    }

    // If paused and "Show Paused" is off, clear presence
    if (!info.isPlaying && !m_showPaused)
    {
        BuildAndSendIdlePresence();
        return;
    }

    DiscordPresenceData presence;
    bool customBlocked = false;
    {
        std::lock_guard<std::mutex> lock(m_blockedTermsMutex);
        if (!m_blockedAppSiteTerms.empty())
        {
            auto lower = BuildMediaSearchBlob(info);
            customBlocked = MatchesAnyBlockedTerm(lower, m_blockedAppSiteTerms);
        }
    }

    bool sensitive = customBlocked || IsSensitiveMedia(
        info,
        m_sensitiveKeywordFilter.load(),
        m_strictBrowserPrivacy.load());

    if (sensitive)
    {
        presence.details = "Hidden content";
        presence.playing = info.isPlaying;
        presence.activityType = ResolveActivityTypeOrDefault(m_activityTypeOverride.load(), 3);
        presence.name = "Private Media";

        std::string state = info.isPlaying ? "Private session" : "Private session - Paused";
        presence.state = ClampDiscordField(state);

        presence.largeImageKey = "music";
        presence.largeImageText = "Private Media";

        if (m_showSource)
        {
            presence.smallImageKey = "music";
            presence.smallImageText = "Private";
        }

        m_discord.UpdatePresence(presence);
        return;
    }

    presence.details = ClampDiscordField(WideToUtf8(info.title));
    presence.playing = info.isPlaying;

    std::wstring sourceDisplay = info.sourceDisplayName;
    std::wstring lowerSource = sourceDisplay;
    for (auto& c : lowerSource) c = towlower(c);
    bool isVideoSource =
        (lowerSource == L"youtube") ||
        (lowerSource == L"twitch");

    // Activity type: 2 = Listening, 3 = Watching, or user override.
    int defaultActivityType = isVideoSource ? 3 : 2;
    presence.activityType = ResolveActivityTypeOrDefault(m_activityTypeOverride.load(), defaultActivityType);

    // Name: what appears after "Listening to" / "Watching"
    if (!sourceDisplay.empty())
        presence.name = ClampDiscordField(WideToUtf8(sourceDisplay));
    else
        presence.name = ClampDiscordField(isVideoSource ? "Video" : "Music");

    // State: artist, with paused indicator when paused
    std::wstring stateText = info.artist.empty() ? L"Unknown Artist" : info.artist;
    if (!info.isPlaying)
        stateText += L" \x2022 Paused";
    presence.state = ClampDiscordField(WideToUtf8(stateText));

    // Album tooltip on large image
    if (!info.albumTitle.empty())
        presence.largeImageText = ClampDiscordField(WideToUtf8(info.albumTitle));

    // Source player icon (small image) — only when toggle is on
    if (m_showSource && !info.sourceDisplayName.empty())
    {
        presence.smallImageText = ClampDiscordField(WideToUtf8(info.sourceDisplayName));
        presence.smallImageKey = GetPlayerAssetKey(info.sourceDisplayName);
    }

    // Large image: fallback to player icon asset
    presence.largeImageKey = GetPlayerAssetKey(info.sourceDisplayName);

    // Album art URL overrides large image
    if (m_showAlbumArt && !(m_suppressBrowserAlbumArt && IsBrowserContext(info)))
    {
        std::lock_guard lock(m_artMutex);
        if (!m_artUrl.empty())
        {
            presence.largeImageKey = m_artUrl;
            if (!info.albumTitle.empty())
                presence.largeImageText = ClampDiscordField(WideToUtf8(info.albumTitle));
            else if (!info.sourceDisplayName.empty())
                presence.largeImageText = ClampDiscordField(WideToUtf8(info.sourceDisplayName));
        }
    }

    // Timestamps — only when playing, toggle on, and duration known
    if (m_showTimestamps && info.isPlaying && info.duration.count() > 0 &&
        info.startTime.time_since_epoch().count() > 0)
    {
        auto startEpoch = std::chrono::duration_cast<std::chrono::seconds>(
            info.startTime.time_since_epoch()).count();
        auto endEpoch = startEpoch + info.duration.count();

        presence.startTimestamp = startEpoch;
        presence.endTimestamp = endEpoch;
    }

    m_discord.UpdatePresence(presence);
}

// --- Album art fetching ---

void PresenceManager::FetchAlbumArtAsync(const MediaInfo& info)
{
    if (!m_showAlbumArt) return;
    if (info.title.empty()) return;

    // Build cache key
    std::string artKey = WideToUtf8(info.title) + "|" +
                         WideToUtf8(info.artist) + "|" +
                         WideToUtf8(info.albumTitle);

    {
        std::lock_guard lock(m_artMutex);
        if (artKey == m_artCacheKey && !m_artUrl.empty())
            return; // Already have art for this track

        if (artKey != m_artCacheKey)
        {
            // Avoid briefly showing stale art while a new track's art is resolving.
            m_artUrl.clear();
            m_artCacheKey.clear();
        }
    }

    ArtFetchJob job;
    job.requestID = ++m_currentRequestID;
    job.thumbnail = info.thumbnail;
    job.artKey = std::move(artKey);
    job.artist = WideToUtf8(info.artist);
    job.title = WideToUtf8(info.title);

    {
        std::lock_guard lock(m_artWorkerMutex);
        m_pendingArtJob = std::move(job);
        m_hasPendingArtJob = true;
    }

    m_artWorkerCv.notify_one();
}

void PresenceManager::StartArtWorker()
{
    std::lock_guard lock(m_artWorkerMutex);
    if (m_artWorkerRunning) return;

    m_hasPendingArtJob = false;
    m_artWorkerExit = false;

    m_artWorker = std::thread([this]()
    {
        try
        {
            winrt::init_apartment();
        }
        catch (...)
        {
            std::lock_guard lock(m_artWorkerMutex);
            m_artWorkerRunning = false;
            m_artWorkerExit = false;
            return;
        }

        while (true)
        {
            ArtFetchJob job;

            {
                std::unique_lock waitLock(m_artWorkerMutex);
                m_artWorkerCv.wait(waitLock, [this]()
                {
                    return m_artWorkerExit || m_hasPendingArtJob;
                });

                if (m_artWorkerExit) break;

                job = std::move(m_pendingArtJob);
                m_hasPendingArtJob = false;
            }

            ProcessArtFetchJob(job);
        }
    });

    m_artWorkerRunning = true;
}

void PresenceManager::StopArtWorker()
{
    {
        std::lock_guard lock(m_artWorkerMutex);
        if (!m_artWorkerRunning)
        {
            m_hasPendingArtJob = false;
            return;
        }

        m_hasPendingArtJob = false;
        m_artWorkerExit = true;
    }

    m_artWorkerCv.notify_all();

    if (m_artWorker.joinable())
        m_artWorker.join();

    {
        std::lock_guard lock(m_artWorkerMutex);
        m_artWorkerRunning = false;
        m_artWorkerExit = false;
    }
}

void PresenceManager::ProcessArtFetchJob(const ArtFetchJob& job)
{
    if (job.requestID != m_currentRequestID.load()) return;

    try
    {
        auto url = FetchFromiTunes(job.artist, job.title);
        if (job.requestID != m_currentRequestID.load()) return;

        if (url.empty() && job.thumbnail)
        {
            try
            {
                auto stream = job.thumbnail.OpenReadAsync().get();
                if (job.requestID != m_currentRequestID.load()) return;

                auto size = static_cast<uint32_t>(stream.Size());
                if (size > 0 && size < 10 * 1024 * 1024)
                {
                    auto reader = winrt::Windows::Storage::Streams::DataReader(stream);
                    reader.LoadAsync(size).get();
                    if (job.requestID != m_currentRequestID.load()) return;

                    std::vector<uint8_t> imageData(size);
                    reader.ReadBytes(imageData);
                    reader.Close();

                    url = UploadToImgur(imageData);
                }
            }
            catch (...) {}
        }

        if (job.requestID != m_currentRequestID.load()) return;

        if (!url.empty())
        {
            {
                std::lock_guard lock(m_artMutex);
                m_artUrl = url;
                m_artCacheKey = job.artKey;
            }

            RefreshPresence();
        }
    }
    catch (...) {}
}

// --- HTTP: iTunes Search API ---

std::string PresenceManager::FetchFromiTunes(const std::string& artist, const std::string& title)
{
    if (artist.empty() && title.empty()) return "";

    std::string searchTerm = artist.empty() ? title : (artist + " " + title);
    std::string encodedTerm = UrlEncode(searchTerm);

    std::wstring path = L"/search?term=";
    path += std::wstring(encodedTerm.begin(), encodedTerm.end());
    path += L"&entity=song&limit=1";

    HINTERNET hSession = WinHttpOpen(L"LastRichPresence/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "";

    WinHttpSetTimeouts(hSession, 5000, 5000, 5000, 5000);

    HINTERNET hConnect = WinHttpConnect(hSession, L"itunes.apple.com",
        INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return ""; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return ""; }

    std::string artUrl;

    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, nullptr))
    {
        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

        if (statusCode == 200)
        {
            std::string responseBody;
            DWORD bytesAvailable = 0;
            while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0)
            {
                std::vector<char> buffer(bytesAvailable);
                DWORD bytesRead = 0;
                if (!WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead) || bytesRead == 0)
                    break;
                responseBody.append(buffer.data(), bytesRead);
            }

            // Parse artworkUrl100
            auto artPos = responseBody.find("\"artworkUrl100\":");
            if (artPos != std::string::npos)
            {
                auto urlStart = responseBody.find("\"", artPos + 16);
                auto urlEnd = responseBody.find("\"", urlStart + 1);
                if (urlStart != std::string::npos && urlEnd != std::string::npos)
                {
                    artUrl = responseBody.substr(urlStart + 1, urlEnd - urlStart - 1);

                    // Unescape JSON forward slashes
                    std::string cleaned;
                    cleaned.reserve(artUrl.size());
                    for (size_t i = 0; i < artUrl.size(); i++)
                    {
                        if (artUrl[i] == '\\' && i + 1 < artUrl.size() && artUrl[i + 1] == '/')
                        {
                            cleaned += '/';
                            i++;
                        }
                        else
                        {
                            cleaned += artUrl[i];
                        }
                    }
                    artUrl = cleaned;

                    // Upscale from 100x100 to 600x600
                    auto sizePos = artUrl.find("100x100");
                    if (sizePos != std::string::npos)
                        artUrl.replace(sizePos, 7, "600x600");
                }
            }
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return artUrl;
}

// --- HTTP: Imgur upload ---

std::string PresenceManager::UploadToImgur(const std::vector<uint8_t>& imageData)
{
    static const wchar_t* IMGUR_HOST = L"api.imgur.com";
    static const wchar_t* IMGUR_PATH = L"/3/image";
    static const wchar_t* IMGUR_CLIENT_ID = L"Client-ID 546c25a59c58ad7";

    HINTERNET hSession = WinHttpOpen(L"LastRichPresence/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "";

    WinHttpSetTimeouts(hSession, 5000, 5000, 10000, 10000);

    HINTERNET hConnect = WinHttpConnect(hSession, IMGUR_HOST,
        INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return ""; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", IMGUR_PATH,
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return ""; }

    // Authorization
    WinHttpAddRequestHeaders(hRequest, (std::wstring(L"Authorization: ") + IMGUR_CLIENT_ID).c_str(),
        (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

    // Multipart body
    std::string boundary = "----LastRPBoundary7ma4d9abcdef";
    std::wstring contentTypeHeader = L"Content-Type: multipart/form-data; boundary=" +
        std::wstring(boundary.begin(), boundary.end());
    WinHttpAddRequestHeaders(hRequest, contentTypeHeader.c_str(),
        (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    std::string body;
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"image\"; filename=\"cover.png\"\r\n";
    body += "Content-Type: image/png\r\n\r\n";
    body.insert(body.end(), imageData.begin(), imageData.end());
    body += "\r\n--" + boundary + "--\r\n";

    BOOL result = WinHttpSendRequest(hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        (LPVOID)body.data(), static_cast<DWORD>(body.size()),
        static_cast<DWORD>(body.size()), 0);

    std::string url;
    if (result && WinHttpReceiveResponse(hRequest, nullptr))
    {
        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

        if (statusCode == 200)
        {
            std::string responseBody;
            DWORD bytesAvailable = 0;
            while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0)
            {
                std::vector<char> buffer(bytesAvailable);
                DWORD bytesRead = 0;
                if (!WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead) || bytesRead == 0)
                    break;
                responseBody.append(buffer.data(), bytesRead);
            }

            auto linkPos = responseBody.find("\"link\":");
            if (linkPos != std::string::npos)
            {
                auto urlStart = responseBody.find("\"", linkPos + 7);
                auto urlEnd = responseBody.find("\"", urlStart + 1);
                if (urlStart != std::string::npos && urlEnd != std::string::npos)
                {
                    url = responseBody.substr(urlStart + 1, urlEnd - urlStart - 1);
                    // Unescape JSON forward slashes
                    std::string cleaned;
                    cleaned.reserve(url.size());
                    for (size_t i = 0; i < url.size(); i++)
                    {
                        if (url[i] == '\\' && i + 1 < url.size() && url[i + 1] == '/')
                        {
                            cleaned += '/';
                            i++;
                        }
                        else
                        {
                            cleaned += url[i];
                        }
                    }
                    url = cleaned;
                }
            }
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return url;
}

// --- Utilities ---

std::string PresenceManager::ClampDiscordField(std::string s)
{
    if (s.empty()) return s;

    // Pad to minimum 2 characters
    if (s.size() == 1) s += ' ';

    // Truncate to maximum 128 characters (UTF-8 safe: don't split multi-byte sequences)
    if (s.size() > 128)
    {
        size_t cut = 125;
        // Walk back to avoid splitting a multi-byte UTF-8 character
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
            cut--;
        s.resize(cut);
        s += "...";
    }
    return s;
}

std::string PresenceManager::WideToUtf8(const std::wstring& wide)
{
    if (wide.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                   nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

std::string PresenceManager::UrlEncode(const std::string& value)
{
    std::string encoded;
    encoded.reserve(value.size() * 3);
    for (unsigned char c : value)
    {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            encoded += c;
        else
        {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", c);
            encoded += buf;
        }
    }
    return encoded;
}

std::string PresenceManager::GetPlayerAssetKey(const std::wstring& sourceDisplayName)
{
    std::wstring lower = sourceDisplayName;
    for (auto& c : lower) c = towlower(c);

    if (lower == L"spotify") return "spotify";
    if (lower == L"apple music") return "applemusic";
    if (lower == L"itunes") return "itunes";
    if (lower == L"youtube music") return "youtubemusic";
    if (lower == L"youtube") return "youtube";
    if (lower == L"twitch") return "twitch";
    if (lower == L"soundcloud") return "soundcloud";
    if (lower == L"bandcamp") return "bandcamp";
    if (lower == L"vlc") return "vlc";
    if (lower == L"foobar2000") return "foobar";
    if (lower == L"musicbee") return "musicbee";
    if (lower == L"aimp") return "aimp";
    if (lower == L"groove music") return "groove";
    if (lower == L"tidal") return "tidal";
    if (lower == L"deezer") return "deezer";
    if (lower == L"amazon music") return "amazon";
    if (lower.find(L"chrome") != std::wstring::npos) return "chrome";
    if (lower.find(L"edge") != std::wstring::npos) return "edge";
    if (lower.find(L"firefox") != std::wstring::npos) return "firefox";
    if (lower.find(L"brave") != std::wstring::npos) return "brave";
    if (lower.find(L"opera") != std::wstring::npos) return "opera";
    if (lower.find(L"vivaldi") != std::wstring::npos) return "vivaldi";

    return "music";
}
