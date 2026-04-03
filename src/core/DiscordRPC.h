#pragma once

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

struct DiscordPresenceData
{
    std::string name;
    std::string details;
    std::string state;
    std::string largeImageKey;
    std::string largeImageText;
    std::string smallImageKey;
    std::string smallImageText;
    int64_t startTimestamp = 0;
    int64_t endTimestamp = 0;
    int activityType = 0;
    bool playing = false;
    uint32_t targetPid = 0;
};

enum class DiscordTransportResult
{
    None = 0,
    Ok,
    PipeUnavailable,
    WriteFailed,
    ReadFailed,
    PeekFailed,
    OversizedFrame,
    HandshakeFailed,
    ClosedByPeer
};

struct DiscordRpcStatus
{
    bool connected{ false };
    bool running{ false };
    DiscordTransportResult lastResult{ DiscordTransportResult::None };
    uint32_t retryCount{ 0 };
    int64_t lastSuccessfulHandshakeUnixSeconds{ 0 };
    int64_t lastDisconnectUnixSeconds{ 0 };
    std::string lastErrorDetail;
};

class IDiscordIpcTransport
{
public:
    virtual ~IDiscordIpcTransport() = default;

    virtual DiscordTransportResult Connect() = 0;
    virtual void Disconnect() = 0;
    virtual DiscordTransportResult WriteFrame(uint32_t opcode, const std::string& data) = 0;
    virtual DiscordTransportResult ReadFrame(uint32_t& opcode, std::string& data) = 0;
    virtual DiscordTransportResult PeekAvailable(uint32_t& available) = 0;
};

class DiscordRPC
{
public:
    DiscordRPC();
    explicit DiscordRPC(std::string appId);
    DiscordRPC(std::string appId, std::unique_ptr<IDiscordIpcTransport> transport);
    ~DiscordRPC();

    void Initialize();
    void Shutdown();
    void UpdatePresence(const DiscordPresenceData& data);
    void ClearPresence();
    bool IsConnected() const { return m_connected; }
    DiscordRpcStatus GetStatus() const;

    static constexpr const char* APP_ID = "1469997747187880057";
    static const char* TransportResultLabel(DiscordTransportResult result) noexcept;

private:
    void WorkerThread();
    void SendPendingPresence();
    void DisconnectTransport(bool resetPublishedState = false);
    void RecordFailure(DiscordTransportResult result, std::string detail);
    void RecordHandshakeSuccess();
    void UpdateRunningStatus();
    static int64_t GetCurrentUnixTimestamp() noexcept;

    std::string m_appId;
    std::unique_ptr<IDiscordIpcTransport> m_transport;
    std::atomic<bool> m_connected{ false };
    std::atomic<bool> m_running{ false };
    std::thread m_worker;
    std::mutex m_mutex;
    DiscordPresenceData m_pendingPresence;
    bool m_presenceDirty{ false };
    bool m_clearRequested{ false };
    bool m_lastPresenceHadTimestamps{ false };
    bool m_replayRequiresPreClear{ false };
    uint32_t m_lastActivityPid{ 0 };
    std::atomic<bool> m_hasPresenceSnapshot{ false };
    std::atomic<int> m_nonce{ 0 };

    mutable std::mutex m_statusMutex;
    DiscordRpcStatus m_status;
};
