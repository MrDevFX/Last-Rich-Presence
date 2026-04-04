#pragma once

#include <windows.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace lrp::browser
{
    inline constexpr wchar_t kBrowserNativeHostArgument[] = L"--browser-native-host";
    inline constexpr wchar_t kNativeHostName[] = L"com.lastprojects.lastrichpresence";
    inline constexpr wchar_t kBrowserHintPipeName[] = L"\\\\.\\pipe\\LastRichPresence.BrowserHints";
    inline constexpr wchar_t kBrowserExtensionId[] = L"hodkjclfknpkaockiingkiijbbjekebj";
    inline constexpr wchar_t kBrowserExtensionOrigin[] = L"chrome-extension://hodkjclfknpkaockiingkiijbbjekebj/";
    inline constexpr wchar_t kBrowserExtensionOriginWithoutSlash[] = L"chrome-extension://hodkjclfknpkaockiingkiijbbjekebj";
    inline constexpr wchar_t kBrowserEdgeExtensionOrigin[] = L"edge-extension://hodkjclfknpkaockiingkiijbbjekebj/";
    inline constexpr wchar_t kBrowserEdgeExtensionOriginWithoutSlash[] = L"edge-extension://hodkjclfknpkaockiingkiijbbjekebj";
    inline constexpr size_t kMaxBrowserNativeMessageBytes = 64 * 1024;

    inline bool IsAcceptedNativeMessagingOrigin(std::wstring_view origin) noexcept
    {
        return
            origin == kBrowserExtensionOrigin ||
            origin == kBrowserExtensionOriginWithoutSlash ||
            origin == kBrowserEdgeExtensionOrigin ||
            origin == kBrowserEdgeExtensionOriginWithoutSlash;
    }

    inline bool IsAllowedNativeMessagingParentProcessName(std::wstring_view processName) noexcept
    {
        return
            processName == L"chrome.exe" ||
            processName == L"msedge.exe";
    }

    bool IsBrowserNativeHostLaunch();
    int RunBrowserNativeHostFromCurrentProcess();
    bool RefreshNativeHostRegistration(std::wstring* errorOut = nullptr);
    HANDLE CreateBrowserHintPipeServerHandle(DWORD openMode, DWORD pipeMode, std::wstring* errorOut = nullptr);
    std::wstring GetLastNativeHostRegistrationError();
    bool TryValidateBrowserNativeHostClientProcess(DWORD processId, std::wstring* errorOut = nullptr);
    bool TryReadLengthPrefixedMessage(
        HANDLE handle,
        size_t maxBytes,
        std::string& payloadOut,
        std::wstring& errorOut);
    bool TryWriteLengthPrefixedMessage(
        HANDLE handle,
        const std::string& payload,
        std::wstring& errorOut);
    bool TrySendBrowserHintToRunningApp(
        const std::string& payloadJson,
        std::string& responseJson,
        std::wstring* errorOut = nullptr);
}
