#pragma once
#include "pch.h"
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>

struct DiscordPresenceData
{
    std::string name;           // What appears after "Listening to" (e.g., player name)
    std::string details;        // Song title
    std::string state;          // Artist name
    std::string largeImageKey;  // Asset key OR direct HTTPS URL for large image
    std::string largeImageText; // Album name (tooltip on hover)
    std::string smallImageKey;  // Asset key for small image (player icon)
    std::string smallImageText; // Source app name (tooltip on hover)
    int64_t startTimestamp = 0;
    int64_t endTimestamp = 0;
    int activityType = 0;       // 0=Playing, 2=Listening, 3=Watching
    bool playing = false;
    uint32_t targetPid = 0;     // Optional: publish activity for this process PID
};

class DiscordRPC
{
public:
    DiscordRPC();
    explicit DiscordRPC(std::string appId);
    ~DiscordRPC();

    void Initialize();
    void Shutdown();
    void UpdatePresence(const DiscordPresenceData& data);
    void ClearPresence();
    bool IsConnected() const { return m_connected; }

    static constexpr const char* APP_ID = "1469997747187880057";

private:
    void WorkerThread();
    void SendPendingPresence();
    bool ConnectPipe();
    void DisconnectPipe();
    bool WritePipe(uint32_t opcode, const std::string& data);
    bool ReadPipe(uint32_t& opcode, std::string& data);

    std::string m_appId;
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_running{false};
    std::thread m_worker;
    std::mutex m_mutex;
    DiscordPresenceData m_pendingPresence;
    bool m_presenceDirty{false};
    bool m_clearRequested{false};
    bool m_lastPresenceHadTimestamps{false};
    uint32_t m_lastActivityPid{0};
    std::atomic<bool> m_hasPresenceSnapshot{false};
    std::atomic<int> m_nonce{0};

    // Pipe handle — owned by this instance, accessed only from worker thread
    // except during Shutdown() which joins the worker first
    HANDLE m_pipe{INVALID_HANDLE_VALUE};
};
