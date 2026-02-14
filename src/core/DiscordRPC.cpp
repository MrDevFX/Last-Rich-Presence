#include "pch.h"
#include "DiscordRPC.h"

// Discord RPC via IPC (named pipe) on Windows.
// Uses Discord's documented IPC protocol with the extended fields
// (large_url, small_url) that Discord's client supports natively.

#include <sstream>
#include <cstring>
#include <cstdint>

// Discord IPC opcodes
static constexpr uint32_t OP_HANDSHAKE = 0;
static constexpr uint32_t OP_FRAME     = 1;
static constexpr uint32_t OP_CLOSE     = 2;

// Simple JSON string escaper
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
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:   out += c; break;
            }
        }
        return out;
    }
}

// --- Pipe methods (instance-level, no global state) ---

bool DiscordRPC::ConnectPipe()
{
    if (m_pipe != INVALID_HANDLE_VALUE)
        return true;

    for (int i = 0; i < 10; i++)
    {
        wchar_t pipeName[64];
        swprintf_s(pipeName, L"\\\\.\\pipe\\discord-ipc-%d", i);

        m_pipe = CreateFileW(
            pipeName,
            GENERIC_READ | GENERIC_WRITE,
            0, nullptr,
            OPEN_EXISTING,
            0, nullptr);

        if (m_pipe != INVALID_HANDLE_VALUE)
        {
            DWORD mode = PIPE_READMODE_BYTE;
            SetNamedPipeHandleState(m_pipe, &mode, nullptr, nullptr);
            return true;
        }
    }
    return false;
}

void DiscordRPC::DisconnectPipe()
{
    if (m_pipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }
    m_lastPresenceHadTimestamps = false;
}

bool DiscordRPC::WritePipe(uint32_t opcode, const std::string& data)
{
    if (m_pipe == INVALID_HANDLE_VALUE)
        return false;

    struct Header { uint32_t opcode; uint32_t length; };
    size_t totalSize = sizeof(Header) + data.size();
    std::vector<uint8_t> buffer(totalSize);

    Header* header = reinterpret_cast<Header*>(buffer.data());
    header->opcode = opcode;
    header->length = static_cast<uint32_t>(data.size());

    if (!data.empty())
    {
        std::memcpy(buffer.data() + sizeof(Header), data.data(), data.size());
    }

    DWORD written = 0;
    if (!WriteFile(m_pipe, buffer.data(), static_cast<DWORD>(totalSize), &written, nullptr))
        return false;

    return written == totalSize;
}

bool DiscordRPC::ReadPipe(uint32_t& opcode, std::string& data)
{
    if (m_pipe == INVALID_HANDLE_VALUE)
        return false;

    struct Header { uint32_t opcode; uint32_t length; };
    Header header{};
    DWORD bytesRead = 0;
    if (!ReadFile(m_pipe, &header, sizeof(header), &bytesRead, nullptr) ||
        bytesRead < sizeof(header))
        return false;

    opcode = header.opcode;

    if (header.length > 0 && header.length < 65536)
    {
        data.resize(header.length);
        DWORD totalRead = 0;
        while (totalRead < header.length)
        {
            DWORD chunkRead = 0;
            if (!ReadFile(m_pipe, data.data() + totalRead,
                          header.length - totalRead, &chunkRead, nullptr) || chunkRead == 0)
                return false;
            totalRead += chunkRead;
        }
    }
    else if (header.length >= 65536)
    {
        // Message too large — drain and discard to keep pipe in sync
        DWORD remaining = header.length;
        char discard[4096];
        while (remaining > 0)
        {
            DWORD toRead = min(remaining, (DWORD)sizeof(discard));
            DWORD chunkRead = 0;
            if (!ReadFile(m_pipe, discard, toRead, &chunkRead, nullptr) || chunkRead == 0)
                return false;
            remaining -= chunkRead;
        }
        return false;  // Signal failure for oversized messages
    }
    return true;
}

// --- DiscordRPC implementation ---

DiscordRPC::DiscordRPC() {}

DiscordRPC::~DiscordRPC()
{
    Shutdown();
}

void DiscordRPC::Initialize()
{
    // Join any previous thread first to avoid std::terminate
    if (m_worker.joinable())
    {
        m_running = false;
        m_worker.join();
    }

    m_appId = APP_ID;
    m_running = true;
    m_worker = std::thread(&DiscordRPC::WorkerThread, this);
}

void DiscordRPC::Shutdown()
{
    // Signal the worker to stop, then let it drain pending data
    m_running = false;
    if (m_worker.joinable())
        m_worker.join();

    // Safe to touch pipe here — worker has exited
    DisconnectPipe();
    m_connected = false;
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
        // Connect if needed
        if (!m_connected)
        {
            if (ConnectPipe())
            {
                std::string handshake = "{\"v\":1,\"client_id\":\"" +
                    json::escape(m_appId) + "\"}";

                if (WritePipe(OP_HANDSHAKE, handshake))
                {
                    uint32_t op;
                    std::string response;
                    if (ReadPipe(op, response) && op == OP_FRAME)
                    {
                        m_connected = true;
                        if (m_hasPresenceSnapshot)
                        {
                            std::lock_guard lock(m_mutex);
                            m_presenceDirty = true;
                        }
                    }
                    else
                        DisconnectPipe();
                }
                else
                    DisconnectPipe();
            }

            if (!m_connected)
            {
                for (int i = 0; i < 50 && m_running; i++)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
        }

        // Passive health check: detect Discord quit even without presence changes
        DWORD available = 0;
        if (!PeekNamedPipe(m_pipe, nullptr, 0, nullptr, &available, nullptr))
        {
            DisconnectPipe();
            m_connected = false;
            continue;
        }

        while (available > 0 && m_connected)
        {
            uint32_t op = 0;
            std::string response;
            if (!ReadPipe(op, response))
            {
                DisconnectPipe();
                m_connected = false;
                break;
            }

            if (op == OP_CLOSE)
            {
                DisconnectPipe();
                m_connected = false;
                break;
            }

            if (!PeekNamedPipe(m_pipe, nullptr, 0, nullptr, &available, nullptr))
            {
                DisconnectPipe();
                m_connected = false;
                break;
            }
        }

        if (!m_connected)
            continue;

        // Send pending presence
        SendPendingPresence();

        // Sleep ~1 second between updates
        for (int i = 0; i < 10 && m_running; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Drain: send any final pending data (e.g., ClearPresence before shutdown)
    if (m_connected)
        SendPendingPresence();
}

void DiscordRPC::SendPendingPresence()
{
    bool dirty = false;
    bool clearReq = false;
    DiscordPresenceData presence;
    {
        std::lock_guard lock(m_mutex);
        dirty = m_presenceDirty;
        if (dirty)
        {
            clearReq = m_clearRequested;
            presence = m_pendingPresence;
            m_presenceDirty = false;
            m_clearRequested = false;
        }
    }

    if (!dirty || !m_connected) return;

    auto requeuePending = [&]()
    {
        std::lock_guard lock(m_mutex);
        if (!m_presenceDirty)
        {
            m_pendingPresence = presence;
            m_clearRequested = clearReq;
            m_presenceDirty = true;
        }
    };

    auto sendFrameAndPoll = [&](const std::string& payload) -> bool
    {
        if (!WritePipe(OP_FRAME, payload))
        {
            DisconnectPipe();
            m_connected = false;
            requeuePending();
            return false;
        }

        uint32_t op;
        std::string response;
        for (int w = 0; w < 10; w++)
        {
            DWORD available = 0;
            if (PeekNamedPipe(m_pipe, nullptr, 0, nullptr, &available, nullptr) && available > 0)
            {
                ReadPipe(op, response);
                if (op == OP_CLOSE)
                {
                    DisconnectPipe();
                    m_connected = false;
                    requeuePending();
                    return false;
                }
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return true;
    };

    if (clearReq)
    {
        std::ostringstream clearPayload;
        clearPayload << "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" << GetCurrentProcessId()
                     << ",\"activity\":null},\"nonce\":\"" << ++m_nonce << "\"}";

        if (sendFrameAndPoll(clearPayload.str()))
            m_lastPresenceHadTimestamps = false;
        return;
    }

    const bool hasTimestamps = (presence.startTimestamp > 0 || presence.endTimestamp > 0);

    if (!hasTimestamps && m_lastPresenceHadTimestamps)
    {
        std::ostringstream clearPayload;
        clearPayload << "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" << GetCurrentProcessId()
                     << ",\"activity\":null},\"nonce\":\"" << ++m_nonce << "\"}";

        if (!sendFrameAndPoll(clearPayload.str()))
            return;
    }

    std::ostringstream p;
    p << "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" << GetCurrentProcessId();

    p << ",\"activity\":{";

    // Activity type
    p << "\"type\":" << presence.activityType << ",";

    // Name: controls what appears after "Listening to" on Discord profile
    if (!presence.name.empty())
        p << "\"name\":\"" << json::escape(presence.name) << "\",";

    if (!presence.details.empty())
        p << "\"details\":\"" << json::escape(presence.details) << "\",";
    if (!presence.state.empty())
        p << "\"state\":\"" << json::escape(presence.state) << "\",";

    // Timestamps
    if (hasTimestamps)
    {
        p << "\"timestamps\":{";
        if (presence.startTimestamp > 0)
            p << "\"start\":" << presence.startTimestamp;
        if (presence.endTimestamp > 0)
        {
            if (presence.startTimestamp > 0) p << ",";
            p << "\"end\":" << presence.endTimestamp;
        }
        p << "},";
    }

    // Assets
    p << "\"assets\":{";
    bool hasAsset = false;

    auto writeField = [&](const char* key, const std::string& val) {
        if (val.empty()) return;
        if (hasAsset) p << ",";
        p << "\"" << key << "\":\"" << json::escape(val) << "\"";
        hasAsset = true;
    };

    writeField("large_image", presence.largeImageKey);
    writeField("large_text", presence.largeImageText);
    writeField("small_image", presence.smallImageKey);
    writeField("small_text", presence.smallImageText);

    p << "}";

    p << "}";

    p << "},\"nonce\":\"" << ++m_nonce << "\"}";

    if (sendFrameAndPoll(p.str()))
        m_lastPresenceHadTimestamps = hasTimestamps;
}
