#include "pch.h"

#include "BrowserNativeMessaging.h"

#include <fcntl.h>
#include <io.h>
#include <shlobj.h>
#include <sddl.h>
#include <tlhelp32.h>

#include <array>
#include <algorithm>
#include <cwctype>
#include <limits>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>

namespace
{
    constexpr wchar_t kNativeHostManifestFileName[] = L"com.lastprojects.lastrichpresence.json";
    constexpr wchar_t kNativeHostManifestDirectory[] = L"LastProjects\\LastRichPresence\\BrowserNativeHost";
    constexpr wchar_t kNativeHostDebugOverrideEnvironmentVariable[] = L"LRP_ALLOW_BROWSER_NATIVE_HOST_DEBUG";
    constexpr DWORD kPipeConnectTimeoutMs = 1500;
    constexpr size_t kMaxTransportMessageBytes = lrp::browser::kMaxBrowserNativeMessageBytes;
    constexpr char kNativeHostNameUtf8[] = "com.lastprojects.lastrichpresence";

    constexpr std::array<std::wstring_view, 1> kAcceptedNativeMessagingOrigins{
        lrp::browser::kBrowserExtensionOrigin
    };

    constexpr std::array<std::wstring_view, 2> kNativeHostRegistryKeys{
        L"Software\\Google\\Chrome\\NativeMessagingHosts\\com.lastprojects.lastrichpresence",
        L"Software\\Microsoft\\Edge\\NativeMessagingHosts\\com.lastprojects.lastrichpresence"
    };

    std::mutex& NativeHostRegistrationErrorMutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    std::wstring& NativeHostRegistrationErrorStorage()
    {
        static std::wstring error;
        return error;
    }

    void SetLastNativeHostRegistrationError(std::wstring_view error)
    {
        std::lock_guard<std::mutex> lock(NativeHostRegistrationErrorMutex());
        NativeHostRegistrationErrorStorage().assign(error);
    }

    std::vector<std::wstring> GetCommandLineArguments()
    {
        int argumentCount = 0;
        auto arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
        if (!arguments)
            return {};

        std::vector<std::wstring> result;
        result.reserve(static_cast<size_t>(argumentCount));
        for (int index = 0; index < argumentCount; ++index)
            result.emplace_back(arguments[index]);

        LocalFree(arguments);
        return result;
    }

    std::wstring ToLowerCopy(std::wstring value)
    {
        for (auto& ch : value)
            ch = static_cast<wchar_t>(towlower(ch));
        return value;
    }

    bool HasNativeHostArgument(const std::vector<std::wstring>& arguments)
    {
        for (size_t index = 1; index < arguments.size(); ++index)
        {
            auto lowered = ToLowerCopy(arguments[index]);
            if (lowered == lrp::browser::kBrowserNativeHostArgument || lowered == L"/browser-native-host")
                return true;
        }

        return false;
    }

    std::optional<std::wstring> TryGetNativeMessagingOrigin(const std::vector<std::wstring>& arguments)
    {
        for (size_t index = 1; index < arguments.size(); ++index)
        {
            auto lowered = ToLowerCopy(arguments[index]);
            if (lowered.rfind(L"chrome-extension://", 0) == 0 || lowered.rfind(L"edge-extension://", 0) == 0)
                return lowered;
        }

        return std::nullopt;
    }

    bool IsAcceptedNativeMessagingOrigin(std::wstring_view origin)
    {
        for (auto acceptedOrigin : kAcceptedNativeMessagingOrigins)
        {
            if (acceptedOrigin == origin)
                return true;
        }

        return false;
    }

    std::wstring GetProcessImagePath(HANDLE processHandle)
    {
        std::wstring buffer(MAX_PATH, L'\0');

        while (true)
        {
            DWORD size = static_cast<DWORD>(buffer.size());
            if (!QueryFullProcessImageNameW(processHandle, 0, buffer.data(), &size))
                return {};

            if (size < buffer.size())
            {
                buffer.resize(size);
                return buffer;
            }

            buffer.resize(buffer.size() * 2);
        }
    }

    std::optional<DWORD> TryGetParentProcessId(DWORD processId)
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return std::nullopt;

        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (!Process32FirstW(snapshot, &entry))
        {
            CloseHandle(snapshot);
            return std::nullopt;
        }

        do
        {
            if (entry.th32ProcessID == processId)
            {
                CloseHandle(snapshot);
                return entry.th32ParentProcessID;
            }
        }
        while (Process32NextW(snapshot, &entry));

        CloseHandle(snapshot);
        return std::nullopt;
    }

    bool IsPipeHandle(HANDLE handle)
    {
        if (!handle || handle == INVALID_HANDLE_VALUE)
            return false;

        return GetFileType(handle) == FILE_TYPE_PIPE;
    }

    bool AreNativeHostStdHandlesPipes()
    {
        return
            IsPipeHandle(GetStdHandle(STD_INPUT_HANDLE)) &&
            IsPipeHandle(GetStdHandle(STD_OUTPUT_HANDLE));
    }

    bool IsTruthyEnvironmentValue(std::wstring value)
    {
        auto lowered = ToLowerCopy(std::move(value));
        return lowered == L"1" || lowered == L"true" || lowered == L"yes" || lowered == L"on";
    }

    bool IsNativeHostDebugOverrideEnabled()
    {
        wchar_t buffer[16]{};
        auto length = GetEnvironmentVariableW(
            kNativeHostDebugOverrideEnvironmentVariable,
            buffer,
            static_cast<DWORD>(std::size(buffer)));
        if (length == 0 || length >= std::size(buffer))
            return false;

        return IsTruthyEnvironmentValue(std::wstring(buffer, length));
    }

    bool IsExplicitNativeHostDebugLaunchAllowed()
    {
        return AreNativeHostStdHandlesPipes() && (IsDebuggerPresent() || IsNativeHostDebugOverrideEnabled());
    }

    bool IsAllowedNativeMessagingWrapperProcessName(std::wstring_view processName) noexcept
    {
        return processName == L"cmd.exe";
    }

    bool TryGetProcessImageName(DWORD processId, std::wstring& imageNameOut)
    {
        imageNameOut.clear();
        if (processId == 0)
            return false;

        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
        if (!process)
            return false;

        auto imagePath = GetProcessImagePath(process);
        CloseHandle(process);
        if (imagePath.empty())
            return false;

        imageNameOut = ToLowerCopy(std::filesystem::path(imagePath).filename().wstring());
        return !imageNameOut.empty();
    }

    bool AreEquivalentPaths(std::wstring_view left, std::wstring_view right)
    {
        if (left.empty() || right.empty())
            return false;

        std::error_code leftError;
        std::error_code rightError;
        auto normalizedLeft = std::filesystem::weakly_canonical(std::filesystem::path(std::wstring(left)), leftError);
        auto normalizedRight = std::filesystem::weakly_canonical(std::filesystem::path(std::wstring(right)), rightError);
        if (!leftError && !rightError)
            return normalizedLeft == normalizedRight;

        return ToLowerCopy(std::wstring(left)) == ToLowerCopy(std::wstring(right));
    }

    bool ValidateNativeHostLaunchOrigin(std::wstring_view origin, std::wstring* errorOut)
    {
        if (!IsAcceptedNativeMessagingOrigin(origin))
        {
            if (errorOut)
                *errorOut = L"native-host-origin-rejected";
            return false;
        }

        if (!AreNativeHostStdHandlesPipes())
        {
            if (errorOut)
                *errorOut = L"native-host-stdio-rejected";
            return false;
        }

        if (errorOut)
            errorOut->clear();
        return true;
    }

    std::wstring GetCurrentExecutablePath()
    {
        std::wstring buffer(MAX_PATH, L'\0');

        while (true)
        {
            auto copied = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (copied == 0)
                return {};

            if (copied < buffer.size() - 1)
            {
                buffer.resize(copied);
                return buffer;
            }

            buffer.resize(buffer.size() * 2);
        }
    }

    std::wstring EscapeJsonString(std::wstring_view value)
    {
        std::wstring escaped;
        escaped.reserve(value.size() + 16);

        for (auto ch : value)
        {
            switch (ch)
            {
            case L'\\':
                escaped += L"\\\\";
                break;
            case L'"':
                escaped += L"\\\"";
                break;
            case L'\b':
                escaped += L"\\b";
                break;
            case L'\f':
                escaped += L"\\f";
                break;
            case L'\n':
                escaped += L"\\n";
                break;
            case L'\r':
                escaped += L"\\r";
                break;
            case L'\t':
                escaped += L"\\t";
                break;
            default:
                if (ch >= 0 && ch < 0x20)
                {
                    wchar_t encoded[7]{};
                    swprintf_s(encoded, L"\\u%04x", static_cast<unsigned>(ch));
                    escaped += encoded;
                }
                else
                {
                    escaped.push_back(ch);
                }
                break;
            }
        }

        return escaped;
    }

    std::string BuildNativeHostManifestJson(const std::wstring& executablePath)
    {
        auto escapedPath = winrt::to_string(EscapeJsonString(executablePath));

        std::ostringstream json;
        json << "{\n"
             << "  \"name\": \"" << kNativeHostNameUtf8 << "\",\n"
             << "  \"description\": \"Last Rich Presence browser companion host\",\n"
             << "  \"path\": \"" << escapedPath << "\",\n"
             << "  \"type\": \"stdio\",\n"
             << "  \"allowed_origins\": [\n";

        for (size_t index = 0; index < kAcceptedNativeMessagingOrigins.size(); ++index)
        {
            json << "    \"" << winrt::to_string(std::wstring(kAcceptedNativeMessagingOrigins[index])) << "\"";
            if (index + 1 < kAcceptedNativeMessagingOrigins.size())
                json << ",";
            json << "\n";
        }

        json << "  ]\n"
             << "}\n";
        return json.str();
    }

    bool TryGetLocalAppDataDirectory(std::filesystem::path& directoryOut, std::wstring* errorOut)
    {
        PWSTR rawPath = nullptr;
        auto result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &rawPath);
        if (FAILED(result) || rawPath == nullptr)
        {
            if (errorOut)
                *errorOut = L"local-appdata-unavailable";
            if (rawPath)
                CoTaskMemFree(rawPath);
            return false;
        }

        directoryOut = std::filesystem::path(rawPath) / kNativeHostManifestDirectory;
        CoTaskMemFree(rawPath);
        return true;
    }

    bool TryGetCurrentUserSidString(std::wstring& sidOut, std::wstring* errorOut)
    {
        HANDLE processToken = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &processToken))
        {
            if (errorOut)
                *errorOut = L"pipe-security-token-open-failed";
            return false;
        }

        DWORD requiredBytes = 0;
        GetTokenInformation(processToken, TokenUser, nullptr, 0, &requiredBytes);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || requiredBytes == 0)
        {
            CloseHandle(processToken);
            if (errorOut)
                *errorOut = L"pipe-security-token-query-failed";
            return false;
        }

        std::vector<BYTE> tokenBuffer(requiredBytes);
        if (!GetTokenInformation(
                processToken,
                TokenUser,
                tokenBuffer.data(),
                requiredBytes,
                &requiredBytes))
        {
            CloseHandle(processToken);
            if (errorOut)
                *errorOut = L"pipe-security-token-read-failed";
            return false;
        }

        CloseHandle(processToken);

        auto* tokenUser = reinterpret_cast<const TOKEN_USER*>(tokenBuffer.data());
        LPWSTR sidString = nullptr;
        if (!ConvertSidToStringSidW(tokenUser->User.Sid, &sidString) || sidString == nullptr)
        {
            if (errorOut)
                *errorOut = L"pipe-security-sid-convert-failed";
            if (sidString)
                LocalFree(sidString);
            return false;
        }

        sidOut.assign(sidString);
        LocalFree(sidString);
        return true;
    }

    std::wstring BuildBrowserHintPipeSecurityDescriptor(std::wstring_view currentUserSid)
    {
        std::wstring descriptor =
            L"D:P" // Protected DACL.
            L"(A;;GA;;;SY)" // LocalSystem.
            L"(A;;GA;;;BA)"; // Built-in administrators.
        descriptor += L"(A;;GA;;;";
        descriptor += currentUserSid;
        descriptor += L")"; // Current interactive user.
        return descriptor;
    }

    bool WriteUtf8FileAtomically(const std::filesystem::path& path, const std::string& contents, std::wstring* errorOut)
    {
        std::error_code filesystemError;
        std::filesystem::create_directories(path.parent_path(), filesystemError);
        if (filesystemError)
        {
            if (errorOut)
                *errorOut = L"manifest-directory-create-failed";
            return false;
        }

        auto tempPath = path;
        tempPath += L".tmp";

        {
            std::ofstream stream(tempPath, std::ios::binary | std::ios::trunc);
            if (!stream)
            {
                if (errorOut)
                    *errorOut = L"manifest-write-open-failed";
                return false;
            }

            stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            if (!stream.good())
            {
                stream.close();
                std::filesystem::remove(tempPath, filesystemError);
                if (errorOut)
                    *errorOut = L"manifest-write-failed";
                return false;
            }
        }

        if (!MoveFileExW(
                tempPath.c_str(),
                path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
        {
            std::error_code cleanupError;
            std::filesystem::remove(tempPath, cleanupError);
            if (errorOut)
                *errorOut = L"manifest-rename-failed";
            return false;
        }

        return true;
    }

    bool WriteRegistryDefaultStringValue(HKEY rootKey, std::wstring_view subKey, const std::wstring& value, std::wstring* errorOut)
    {
        HKEY key = nullptr;
        auto createResult = RegCreateKeyExW(
            rootKey,
            subKey.data(),
            0,
            nullptr,
            0,
            KEY_SET_VALUE,
            nullptr,
            &key,
            nullptr);
        if (createResult != ERROR_SUCCESS)
        {
            if (errorOut)
                *errorOut = L"native-host-registry-create-failed";
            return false;
        }

        auto setResult = RegSetValueExW(
            key,
            nullptr,
            0,
            REG_SZ,
            reinterpret_cast<const BYTE*>(value.c_str()),
            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
        if (setResult != ERROR_SUCCESS)
        {
            if (errorOut)
                *errorOut = L"native-host-registry-write-failed";
            return false;
        }

        return true;
    }

    bool WriteAllRegistryRegistrations(const std::wstring& manifestPath, std::wstring* errorOut)
    {
        for (auto registryKey : kNativeHostRegistryKeys)
        {
            if (!WriteRegistryDefaultStringValue(HKEY_CURRENT_USER, registryKey, manifestPath, errorOut))
                return false;
        }

        return true;
    }

    bool ReadExactBytes(HANDLE handle, void* buffer, size_t bytesToRead)
    {
        auto* destination = static_cast<unsigned char*>(buffer);
        size_t remaining = bytesToRead;

        while (remaining > 0)
        {
            DWORD chunkRead = 0;
            auto chunkSize = static_cast<DWORD>((std::min)(
                remaining,
                static_cast<size_t>((std::numeric_limits<DWORD>::max)())));
            if (!ReadFile(handle, destination, chunkSize, &chunkRead, nullptr) || chunkRead == 0)
                return false;

            destination += chunkRead;
            remaining -= chunkRead;
        }

        return true;
    }

    bool WriteExactBytes(HANDLE handle, const void* buffer, size_t bytesToWrite)
    {
        auto* source = static_cast<const unsigned char*>(buffer);
        size_t remaining = bytesToWrite;

        while (remaining > 0)
        {
            DWORD chunkWritten = 0;
            auto chunkSize = static_cast<DWORD>((std::min)(
                remaining,
                static_cast<size_t>((std::numeric_limits<DWORD>::max)())));
            if (!WriteFile(handle, source, chunkSize, &chunkWritten, nullptr) || chunkWritten == 0)
                return false;

            source += chunkWritten;
            remaining -= chunkWritten;
        }

        return true;
    }

    bool WriteLengthPrefixedPayload(HANDLE handle, const std::string& payload)
    {
        auto length = static_cast<uint32_t>(payload.size());
        return
            WriteExactBytes(handle, &length, sizeof(length)) &&
            WriteExactBytes(handle, payload.data(), payload.size());
    }

    bool ReadLengthPrefixedPayload(HANDLE handle, size_t maxBytes, std::string& payloadOut)
    {
        uint32_t payloadLength = 0;
        if (!ReadExactBytes(handle, &payloadLength, sizeof(payloadLength)))
            return false;

        if (payloadLength > maxBytes)
            return false;

        payloadOut.assign(payloadLength, '\0');
        if (payloadLength == 0)
            return true;

        return ReadExactBytes(handle, payloadOut.data(), payloadLength);
    }

    std::string BuildErrorResponse(std::wstring_view error)
    {
        auto escaped = winrt::to_string(EscapeJsonString(error));
        return "{\"ok\":false,\"error\":\"" + escaped + "\"}";
    }

    bool TryReadNativeHostMessage(std::string& messageOut, bool& endOfStreamOut)
    {
        endOfStreamOut = false;

        std::array<unsigned char, sizeof(uint32_t)> header{};
        auto headerBytesRead = std::fread(header.data(), 1, header.size(), stdin);
        if (headerBytesRead == 0 && std::feof(stdin))
        {
            endOfStreamOut = true;
            return true;
        }

        if (headerBytesRead != header.size())
            return false;

        uint32_t payloadLength =
            static_cast<uint32_t>(header[0]) |
            (static_cast<uint32_t>(header[1]) << 8) |
            (static_cast<uint32_t>(header[2]) << 16) |
            (static_cast<uint32_t>(header[3]) << 24);

        if (payloadLength > kMaxTransportMessageBytes)
            return false;

        messageOut.assign(payloadLength, '\0');
        if (payloadLength == 0)
            return true;

        return std::fread(messageOut.data(), 1, payloadLength, stdin) == payloadLength;
    }

    bool WriteNativeHostMessage(const std::string& message)
    {
        if (message.size() > UINT32_MAX)
            return false;

        uint32_t payloadLength = static_cast<uint32_t>(message.size());
        std::array<unsigned char, sizeof(uint32_t)> header{
            static_cast<unsigned char>(payloadLength & 0xff),
            static_cast<unsigned char>((payloadLength >> 8) & 0xff),
            static_cast<unsigned char>((payloadLength >> 16) & 0xff),
            static_cast<unsigned char>((payloadLength >> 24) & 0xff)
        };

        if (std::fwrite(header.data(), 1, header.size(), stdout) != header.size())
            return false;

        if (!message.empty() && std::fwrite(message.data(), 1, message.size(), stdout) != message.size())
            return false;

        return std::fflush(stdout) == 0;
    }
}

namespace lrp::browser
{
    HANDLE CreateBrowserHintPipeServerHandle(DWORD openMode, DWORD pipeMode, std::wstring* errorOut)
    {
        std::wstring currentUserSid;
        if (!TryGetCurrentUserSidString(currentUserSid, errorOut))
            return INVALID_HANDLE_VALUE;

        auto securityDescriptorSddl = BuildBrowserHintPipeSecurityDescriptor(currentUserSid);
        PSECURITY_DESCRIPTOR securityDescriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                securityDescriptorSddl.c_str(),
                SDDL_REVISION_1,
                &securityDescriptor,
                nullptr) ||
            securityDescriptor == nullptr)
        {
            if (errorOut)
                *errorOut = L"pipe-security-descriptor-create-failed";
            if (securityDescriptor)
                LocalFree(securityDescriptor);
            return INVALID_HANDLE_VALUE;
        }

        SECURITY_ATTRIBUTES securityAttributes{};
        securityAttributes.nLength = sizeof(securityAttributes);
        securityAttributes.lpSecurityDescriptor = securityDescriptor;
        securityAttributes.bInheritHandle = FALSE;

        HANDLE pipeHandle = CreateNamedPipeW(
            kBrowserHintPipeName,
            openMode,
            pipeMode,
            1,
            static_cast<DWORD>(kMaxBrowserNativeMessageBytes),
            static_cast<DWORD>(kMaxBrowserNativeMessageBytes),
            0,
            &securityAttributes);

        LocalFree(securityDescriptor);

        if (pipeHandle == INVALID_HANDLE_VALUE && errorOut)
            *errorOut = L"pipe-create-failed";

        return pipeHandle;
    }

    std::wstring GetLastNativeHostRegistrationError()
    {
        std::lock_guard<std::mutex> lock(NativeHostRegistrationErrorMutex());
        return NativeHostRegistrationErrorStorage();
    }

    bool TryValidateBrowserNativeHostClientProcess(DWORD processId, std::wstring* errorOut)
    {
        if (processId == 0 || processId == GetCurrentProcessId())
        {
            if (errorOut)
                *errorOut = L"pipe-client-process-rejected";
            return false;
        }

        HANDLE clientProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
        if (!clientProcess)
        {
            if (errorOut)
                *errorOut = L"pipe-client-process-open-failed";
            return false;
        }

        auto clientImagePath = GetProcessImagePath(clientProcess);
        CloseHandle(clientProcess);
        if (clientImagePath.empty())
        {
            if (errorOut)
                *errorOut = L"pipe-client-image-unavailable";
            return false;
        }

        auto currentImagePath = GetCurrentExecutablePath();
        if (currentImagePath.empty())
        {
            if (errorOut)
                *errorOut = L"pipe-server-image-unavailable";
            return false;
        }

        if (!AreEquivalentPaths(clientImagePath, currentImagePath))
        {
            if (errorOut)
                *errorOut = L"pipe-client-image-rejected";
            return false;
        }

        auto parentProcessId = TryGetParentProcessId(processId);
        if (!parentProcessId || *parentProcessId == 0)
        {
            if (errorOut)
                *errorOut = L"pipe-client-parent-unavailable";
            return false;
        }

        std::wstring parentImageName;
        if (!TryGetProcessImageName(*parentProcessId, parentImageName))
        {
            if (errorOut)
                *errorOut = L"pipe-client-parent-open-failed";
            return false;
        }

        bool trustedParent = IsAllowedNativeMessagingParentProcessName(parentImageName);
        if (!trustedParent && IsAllowedNativeMessagingWrapperProcessName(parentImageName))
        {
            auto grandParentProcessId = TryGetParentProcessId(*parentProcessId);
            std::wstring grandParentImageName;
            trustedParent =
                grandParentProcessId.has_value() &&
                TryGetProcessImageName(*grandParentProcessId, grandParentImageName) &&
                IsAllowedNativeMessagingParentProcessName(grandParentImageName);
        }

        if (!trustedParent && !IsExplicitNativeHostDebugLaunchAllowed())
        {
            if (errorOut)
                *errorOut = L"pipe-client-parent-rejected";
            return false;
        }

        if (errorOut)
            errorOut->clear();
        return true;
    }

    bool IsBrowserNativeHostLaunch()
    {
        auto arguments = GetCommandLineArguments();
        auto nativeMessagingOrigin = TryGetNativeMessagingOrigin(arguments);
        if (nativeMessagingOrigin)
            return ValidateNativeHostLaunchOrigin(*nativeMessagingOrigin, nullptr);

        return HasNativeHostArgument(arguments) && IsExplicitNativeHostDebugLaunchAllowed();
    }

    int RunBrowserNativeHostFromCurrentProcess()
    {
        auto arguments = GetCommandLineArguments();
        const bool hasExplicitArgument = HasNativeHostArgument(arguments);
        auto nativeMessagingOrigin = TryGetNativeMessagingOrigin(arguments);
        if (!hasExplicitArgument && !nativeMessagingOrigin)
            return 1;

        if (!nativeMessagingOrigin)
        {
            if (!IsExplicitNativeHostDebugLaunchAllowed())
                return 1;
        }
        else
        {
            std::wstring launchError;
            if (!ValidateNativeHostLaunchOrigin(*nativeMessagingOrigin, &launchError))
                return 1;
        }

        if (_setmode(_fileno(stdin), _O_BINARY) == -1)
            return 1;
        if (_setmode(_fileno(stdout), _O_BINARY) == -1)
            return 1;

        while (true)
        {
            bool endOfStream = false;
            std::string requestJson;
            if (!TryReadNativeHostMessage(requestJson, endOfStream))
                return 1;

            if (endOfStream)
                return 0;

            std::string responseJson;
            std::wstring error;
            if (!TrySendBrowserHintToRunningApp(requestJson, responseJson, &error))
                responseJson = BuildErrorResponse(error.empty() ? std::wstring_view(L"app-not-running") : std::wstring_view(error));

            if (!WriteNativeHostMessage(responseJson))
                return 1;
        }
    }

    bool RefreshNativeHostRegistration(std::wstring* errorOut)
    {
        auto executablePath = GetCurrentExecutablePath();
        if (executablePath.empty())
        {
            if (errorOut)
                *errorOut = L"executable-path-unavailable";
            SetLastNativeHostRegistrationError(L"executable-path-unavailable");
            return false;
        }

        std::filesystem::path manifestDirectory;
        if (!TryGetLocalAppDataDirectory(manifestDirectory, errorOut))
        {
            SetLastNativeHostRegistrationError(errorOut && !errorOut->empty()
                ? std::wstring_view(*errorOut)
                : std::wstring_view(L"local-appdata-unavailable"));
            return false;
        }

        auto manifestPath = manifestDirectory / kNativeHostManifestFileName;
        auto manifestJson = BuildNativeHostManifestJson(executablePath);
        if (!WriteUtf8FileAtomically(manifestPath, manifestJson, errorOut))
        {
            SetLastNativeHostRegistrationError(errorOut && !errorOut->empty()
                ? std::wstring_view(*errorOut)
                : std::wstring_view(L"manifest-write-failed"));
            return false;
        }

        if (!WriteAllRegistryRegistrations(manifestPath.wstring(), errorOut))
        {
            SetLastNativeHostRegistrationError(errorOut && !errorOut->empty()
                ? std::wstring_view(*errorOut)
                : std::wstring_view(L"native-host-registry-write-failed"));
            return false;
        }

        SetLastNativeHostRegistrationError(L"");
        return true;
    }

    bool TryReadLengthPrefixedMessage(
        HANDLE handle,
        size_t maxBytes,
        std::string& payloadOut,
        std::wstring& errorOut)
    {
        if (!ReadLengthPrefixedPayload(handle, maxBytes, payloadOut))
        {
            errorOut = L"pipe-read-failed";
            return false;
        }

        errorOut.clear();
        return true;
    }

    bool TryWriteLengthPrefixedMessage(
        HANDLE handle,
        const std::string& payload,
        std::wstring& errorOut)
    {
        if (payload.size() > kMaxBrowserNativeMessageBytes)
        {
            errorOut = L"payload-too-large";
            return false;
        }

        if (!WriteLengthPrefixedPayload(handle, payload))
        {
            errorOut = L"pipe-write-failed";
            return false;
        }

        errorOut.clear();
        return true;
    }

    bool TrySendBrowserHintToRunningApp(
        const std::string& payloadJson,
        std::string& responseJson,
        std::wstring* errorOut)
    {
        if (payloadJson.size() > kMaxTransportMessageBytes)
        {
            if (errorOut)
                *errorOut = L"payload-too-large";
            return false;
        }

        HANDLE pipeHandle = INVALID_HANDLE_VALUE;
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            pipeHandle = CreateFileW(
                kBrowserHintPipeName,
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (pipeHandle != INVALID_HANDLE_VALUE)
                break;

            auto lastError = GetLastError();
            if (lastError == ERROR_PIPE_BUSY)
            {
                if (!WaitNamedPipeW(kBrowserHintPipeName, kPipeConnectTimeoutMs))
                    continue;
                continue;
            }

            if (lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_PATH_NOT_FOUND)
            {
                if (errorOut)
                    *errorOut = L"app-not-running";
                return false;
            }

            if (errorOut)
                *errorOut = L"pipe-open-failed";
            return false;
        }

        if (pipeHandle == INVALID_HANDLE_VALUE)
        {
            if (errorOut)
                *errorOut = L"app-not-running";
            return false;
        }

        if (!WriteLengthPrefixedPayload(pipeHandle, payloadJson))
        {
            CloseHandle(pipeHandle);
            if (errorOut)
                *errorOut = L"pipe-write-failed";
            return false;
        }

        if (!ReadLengthPrefixedPayload(pipeHandle, kMaxTransportMessageBytes, responseJson))
        {
            CloseHandle(pipeHandle);
            if (errorOut)
                *errorOut = L"pipe-read-failed";
            return false;
        }

        CloseHandle(pipeHandle);
        return true;
    }
}
