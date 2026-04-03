#include "DiscordRPC.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>
#include <vector>

static constexpr uint32_t OP_HANDSHAKE = 0;
static constexpr uint32_t OP_FRAME = 1;
static constexpr uint32_t OP_CLOSE = 2;

namespace json
{
    static std::string escape(const std::string& s)
    {
        std::string out;
        out.reserve(s.size() + 8);
        for (char c : s)
        {
            switch (c)
            {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
            }
        }
        return out;
    }
}

namespace
{
    class WindowsDiscordIpcTransport final : public IDiscordIpcTransport
    {
    public:
        ~WindowsDiscordIpcTransport() override
        {
            Disconnect();
        }

        DiscordTransportResult Connect() override
        {
            if (m_pipe != INVALID_HANDLE_VALUE)
                return DiscordTransportResult::Ok;

            for (int index = 0; index < 10; ++index)
            {
                wchar_t pipeName[64];
                swprintf_s(pipeName, L"\\\\.\\pipe\\discord-ipc-%d", index);

                m_pipe = CreateFileW(
                    pipeName,
                    GENERIC_READ | GENERIC_WRITE,
                    0,
                    nullptr,
                    OPEN_EXISTING,
                    0,
                    nullptr);

                if (m_pipe != INVALID_HANDLE_VALUE)
                {
                    DWORD mode = PIPE_READMODE_BYTE;
                    SetNamedPipeHandleState(m_pipe, &mode, nullptr, nullptr);
                    return DiscordTransportResult::Ok;
                }
            }

            return DiscordTransportResult::PipeUnavailable;
        }

        void Disconnect() override
        {
            if (m_pipe == INVALID_HANDLE_VALUE)
                return;

            CloseHandle(m_pipe);
            m_pipe = INVALID_HANDLE_VALUE;
        }

        DiscordTransportResult WriteFrame(uint32_t opcode, const std::string& data) override
        {
            if (m_pipe == INVALID_HANDLE_VALUE)
                return DiscordTransportResult::WriteFailed;

            struct Header
            {
                uint32_t opcode;
                uint32_t length;
            };

            std::vector<uint8_t> buffer(sizeof(Header) + data.size());
            auto* header = reinterpret_cast<Header*>(buffer.data());
            header->opcode = opcode;
            header->length = static_cast<uint32_t>(data.size());

            if (!data.empty())
                std::memcpy(buffer.data() + sizeof(Header), data.data(), data.size());

            DWORD written = 0;
            if (!WriteFile(m_pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &written, nullptr))
                return DiscordTransportResult::WriteFailed;

            return written == buffer.size()
                ? DiscordTransportResult::Ok
                : DiscordTransportResult::WriteFailed;
        }

        DiscordTransportResult ReadFrame(uint32_t& opcode, std::string& data) override
        {
            if (m_pipe == INVALID_HANDLE_VALUE)
                return DiscordTransportResult::ReadFailed;

            struct Header
            {
                uint32_t opcode;
                uint32_t length;
            };

            Header header{};
            DWORD bytesRead = 0;
            if (!ReadFile(m_pipe, &header, sizeof(header), &bytesRead, nullptr) ||
                bytesRead < sizeof(header))
            {
                return DiscordTransportResult::ReadFailed;
            }

            opcode = header.opcode;
            data.clear();

            if (header.length >= 65536)
            {
                DWORD remaining = header.length;
                char discard[4096];
                while (remaining > 0)
                {
                    DWORD toRead = (std::min)(remaining, static_cast<DWORD>(sizeof(discard)));
                    DWORD chunkRead = 0;
                    if (!ReadFile(m_pipe, discard, toRead, &chunkRead, nullptr) || chunkRead == 0)
                        return DiscordTransportResult::ReadFailed;

                    remaining -= chunkRead;
                }

                return DiscordTransportResult::OversizedFrame;
            }

            if (header.length == 0)
                return DiscordTransportResult::Ok;

            data.resize(header.length);
            DWORD totalRead = 0;
            while (totalRead < header.length)
            {
                DWORD chunkRead = 0;
                if (!ReadFile(
                    m_pipe,
                    data.data() + totalRead,
                    header.length - totalRead,
                    &chunkRead,
                    nullptr) ||
                    chunkRead == 0)
                {
                    return DiscordTransportResult::ReadFailed;
                }

                totalRead += chunkRead;
            }

            return DiscordTransportResult::Ok;
        }

        DiscordTransportResult PeekAvailable(uint32_t& available) override
        {
            available = 0;
            if (m_pipe == INVALID_HANDLE_VALUE)
                return DiscordTransportResult::PeekFailed;

            DWORD availableBytes = 0;
            if (!PeekNamedPipe(m_pipe, nullptr, 0, nullptr, &availableBytes, nullptr))
                return DiscordTransportResult::PeekFailed;

            available = static_cast<uint32_t>(availableBytes);
            return DiscordTransportResult::Ok;
        }

    private:
        HANDLE m_pipe{ INVALID_HANDLE_VALUE };
    };

    std::unique_ptr<IDiscordIpcTransport> CreateDefaultTransport()
    {
        return std::make_unique<WindowsDiscordIpcTransport>();
    }
}

DiscordRPC::DiscordRPC()
    : DiscordRPC(std::string(APP_ID))
{
}

DiscordRPC::DiscordRPC(std::string appId)
    : DiscordRPC(std::move(appId), CreateDefaultTransport())
{
}

DiscordRPC::DiscordRPC(std::string appId, std::unique_ptr<IDiscordIpcTransport> transport)
    : m_appId(std::move(appId))
    , m_transport(std::move(transport))
{
    if (m_appId.empty())
        m_appId = APP_ID;

    if (!m_transport)
        m_transport = CreateDefaultTransport();
}

DiscordRPC::~DiscordRPC()
{
    Shutdown();
}

const char* DiscordRPC::TransportResultLabel(DiscordTransportResult result) noexcept
{
    switch (result)
    {
    case DiscordTransportResult::Ok: return "ok";
    case DiscordTransportResult::PipeUnavailable: return "pipe_unavailable";
    case DiscordTransportResult::WriteFailed: return "write_failed";
    case DiscordTransportResult::ReadFailed: return "read_failed";
    case DiscordTransportResult::PeekFailed: return "peek_failed";
    case DiscordTransportResult::OversizedFrame: return "oversized_frame";
    case DiscordTransportResult::HandshakeFailed: return "handshake_failed";
    case DiscordTransportResult::ClosedByPeer: return "closed_by_peer";
    default: return "none";
    }
}

int64_t DiscordRPC::GetCurrentUnixTimestamp() noexcept
{
    using clock = std::chrono::system_clock;
    return std::chrono::duration_cast<std::chrono::seconds>(
        clock::now().time_since_epoch()).count();
}

void DiscordRPC::UpdateRunningStatus()
{
    std::lock_guard lock(m_statusMutex);
    m_status.running = m_running.load();
    m_status.connected = m_connected.load();
}

DiscordRpcStatus DiscordRPC::GetStatus() const
{
    std::lock_guard lock(m_statusMutex);
    auto status = m_status;
    status.running = m_running.load();
    status.connected = m_connected.load();
    return status;
}

void DiscordRPC::DisconnectTransport(bool resetPublishedState)
{
    if (m_transport)
        m_transport->Disconnect();

    if (resetPublishedState)
    {
        m_lastPresenceHadTimestamps = false;
        m_lastActivityPid = 0;
    }

    m_connected = false;
}

void DiscordRPC::RecordFailure(DiscordTransportResult result, std::string detail)
{
    m_connected = false;

    std::lock_guard lock(m_statusMutex);
    m_status.connected = false;
    m_status.running = m_running.load();
    m_status.lastResult = result;
    m_status.lastErrorDetail = detail.empty() ? TransportResultLabel(result) : std::move(detail);
    ++m_status.retryCount;
    if (m_status.lastSuccessfulHandshakeUnixSeconds > 0)
        m_status.lastDisconnectUnixSeconds = GetCurrentUnixTimestamp();
}

void DiscordRPC::RecordHandshakeSuccess()
{
    m_connected = true;

    std::lock_guard lock(m_statusMutex);
    m_status.connected = true;
    m_status.running = m_running.load();
    m_status.lastResult = DiscordTransportResult::Ok;
    m_status.lastErrorDetail.clear();
    m_status.retryCount = 0;
    m_status.lastSuccessfulHandshakeUnixSeconds = GetCurrentUnixTimestamp();
}

void DiscordRPC::Initialize()
{
    if (m_worker.joinable())
    {
        m_running = false;
        m_worker.join();
    }

    m_running = true;
    UpdateRunningStatus();
    m_worker = std::thread(&DiscordRPC::WorkerThread, this);
}

void DiscordRPC::Shutdown()
{
    if (m_connected)
        ClearPresence();

    m_running = false;
    UpdateRunningStatus();

    if (m_worker.joinable())
        m_worker.join();

    DisconnectTransport(true);

    std::lock_guard lock(m_statusMutex);
    m_status.running = false;
    m_status.connected = false;
}

void DiscordRPC::UpdatePresence(const DiscordPresenceData& data)
{
    std::lock_guard lock(m_mutex);
    m_pendingPresence = data;
    m_presenceDirty = true;
    m_clearRequested = false;
    m_hasPresenceSnapshot = true;
}

void DiscordRPC::ClearPresence()
{
    std::lock_guard lock(m_mutex);
    m_pendingPresence = {};
    m_presenceDirty = true;
    m_clearRequested = true;
    m_hasPresenceSnapshot = true;
}

void DiscordRPC::WorkerThread()
{
    while (m_running)
    {
        if (!m_connected)
        {
            auto connectResult = m_transport->Connect();
            if (connectResult == DiscordTransportResult::Ok)
            {
                std::string handshake = "{\"v\":1,\"client_id\":\"" + json::escape(m_appId) + "\"}";
                auto writeResult = m_transport->WriteFrame(OP_HANDSHAKE, handshake);
                if (writeResult == DiscordTransportResult::Ok)
                {
                    uint32_t op = 0;
                    std::string response;
                    auto readResult = m_transport->ReadFrame(op, response);
                    if (readResult == DiscordTransportResult::Ok && op == OP_FRAME)
                    {
                        RecordHandshakeSuccess();
                        if (m_hasPresenceSnapshot)
                        {
                            std::lock_guard lock(m_mutex);
                            m_presenceDirty = true;
                        }
                    }
                    else
                    {
                        DisconnectTransport();
                        RecordFailure(
                            readResult == DiscordTransportResult::Ok
                                ? DiscordTransportResult::HandshakeFailed
                                : readResult,
                            "discord_handshake_failed");
                    }
                }
                else
                {
                    DisconnectTransport();
                    RecordFailure(writeResult, "discord_handshake_write_failed");
                }
            }
            else
            {
                RecordFailure(connectResult, "discord_pipe_unavailable");
            }

            if (!m_connected)
            {
                for (int index = 0; index < 50 && m_running; ++index)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
        }

        uint32_t available = 0;
        auto peekResult = m_transport->PeekAvailable(available);
        if (peekResult != DiscordTransportResult::Ok)
        {
            DisconnectTransport();
            RecordFailure(peekResult, "discord_health_check_failed");
            continue;
        }

        while (available > 0 && m_connected)
        {
            uint32_t op = 0;
            std::string response;
            auto readResult = m_transport->ReadFrame(op, response);
            if (readResult != DiscordTransportResult::Ok)
            {
                DisconnectTransport();
                RecordFailure(readResult, "discord_passive_read_failed");
                break;
            }

            if (op == OP_CLOSE)
            {
                DisconnectTransport();
                RecordFailure(DiscordTransportResult::ClosedByPeer, "discord_closed_by_peer");
                break;
            }

            peekResult = m_transport->PeekAvailable(available);
            if (peekResult != DiscordTransportResult::Ok)
            {
                DisconnectTransport();
                RecordFailure(peekResult, "discord_health_check_failed");
                break;
            }
        }

        if (!m_connected)
            continue;

        SendPendingPresence();

        for (int index = 0; index < 10 && m_running; ++index)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (m_connected)
        SendPendingPresence();
}

void DiscordRPC::SendPendingPresence()
{
    bool dirty = false;
    bool clearRequested = false;
    DiscordPresenceData presence;
    {
        std::lock_guard lock(m_mutex);
        dirty = m_presenceDirty;
        if (dirty)
        {
            clearRequested = m_clearRequested;
            presence = m_pendingPresence;
            m_presenceDirty = false;
            m_clearRequested = false;
        }
    }

    if (!dirty || !m_connected)
        return;

    auto requeuePending = [&]()
    {
        std::lock_guard lock(m_mutex);
        if (!m_presenceDirty)
        {
            m_pendingPresence = presence;
            m_clearRequested = clearRequested;
            m_presenceDirty = true;
        }
    };

    auto sendFrameAndPoll = [&](const std::string& payload) -> bool
    {
        auto writeResult = m_transport->WriteFrame(OP_FRAME, payload);
        if (writeResult != DiscordTransportResult::Ok)
        {
            DisconnectTransport();
            RecordFailure(writeResult, "discord_write_failed");
            requeuePending();
            return false;
        }

        uint32_t op = 0;
        std::string response;
        for (int waitIndex = 0; waitIndex < 10; ++waitIndex)
        {
            uint32_t available = 0;
            auto peekResult = m_transport->PeekAvailable(available);
            if (peekResult != DiscordTransportResult::Ok)
            {
                DisconnectTransport();
                RecordFailure(peekResult, "discord_poll_failed");
                requeuePending();
                return false;
            }

            if (available == 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            auto readResult = m_transport->ReadFrame(op, response);
            if (readResult != DiscordTransportResult::Ok)
            {
                DisconnectTransport();
                RecordFailure(readResult, "discord_ack_read_failed");
                requeuePending();
                return false;
            }

            if (op == OP_CLOSE)
            {
                DisconnectTransport();
                RecordFailure(DiscordTransportResult::ClosedByPeer, "discord_closed_by_peer");
                requeuePending();
                return false;
            }

            break;
        }

        return true;
    };

    const uint32_t fallbackPid = GetCurrentProcessId();
    const uint32_t activityPid =
        (presence.targetPid != 0) ? presence.targetPid :
        (m_lastActivityPid != 0 ? m_lastActivityPid : fallbackPid);

    if (clearRequested)
    {
        std::ostringstream clearPayload;
        clearPayload << "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" << activityPid
                     << ",\"activity\":null},\"nonce\":\"" << ++m_nonce << "\"}";

        if (sendFrameAndPoll(clearPayload.str()))
        {
            m_replayRequiresPreClear = false;
            m_lastPresenceHadTimestamps = false;
            m_lastActivityPid = 0;
        }
        return;
    }

    const bool hasTimestamps = (presence.startTimestamp > 0 || presence.endTimestamp > 0);

    if (!hasTimestamps && (m_lastPresenceHadTimestamps || m_replayRequiresPreClear))
    {
        m_replayRequiresPreClear = true;
        std::ostringstream clearPayload;
        clearPayload << "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" << activityPid
                     << ",\"activity\":null},\"nonce\":\"" << ++m_nonce << "\"}";

        if (!sendFrameAndPoll(clearPayload.str()))
            return;

        m_replayRequiresPreClear = false;
    }

    std::ostringstream payload;
    payload << "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" << activityPid;
    payload << ",\"activity\":{";
    payload << "\"type\":" << presence.activityType << ",";

    if (!presence.name.empty())
        payload << "\"name\":\"" << json::escape(presence.name) << "\",";
    if (!presence.details.empty())
        payload << "\"details\":\"" << json::escape(presence.details) << "\",";
    if (!presence.state.empty())
        payload << "\"state\":\"" << json::escape(presence.state) << "\",";

    if (hasTimestamps)
    {
        payload << "\"timestamps\":{";
        if (presence.startTimestamp > 0)
            payload << "\"start\":" << presence.startTimestamp;
        if (presence.endTimestamp > 0)
        {
            if (presence.startTimestamp > 0)
                payload << ",";
            payload << "\"end\":" << presence.endTimestamp;
        }
        payload << "},";
    }

    payload << "\"assets\":{";
    bool hasAsset = false;

    auto writeAssetField = [&](const char* key, const std::string& value)
    {
        if (value.empty())
            return;

        if (hasAsset)
            payload << ",";
        payload << "\"" << key << "\":\"" << json::escape(value) << "\"";
        hasAsset = true;
    };

    writeAssetField("large_image", presence.largeImageKey);
    writeAssetField("large_text", presence.largeImageText);
    writeAssetField("small_image", presence.smallImageKey);
    writeAssetField("small_text", presence.smallImageText);

    payload << "}";
    payload << "}";
    payload << "},\"nonce\":\"" << ++m_nonce << "\"}";

    if (sendFrameAndPoll(payload.str()))
    {
        m_replayRequiresPreClear = false;
        m_lastPresenceHadTimestamps = hasTimestamps;
        m_lastActivityPid = activityPid;
    }
}
