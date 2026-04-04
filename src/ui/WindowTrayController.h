#pragma once

#include <windows.h>
#include <shellapi.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace lrp::ui
{
    [[nodiscard]] constexpr bool IsWindowVisibleToUser(bool windowVisible, bool windowIconic, bool hiddenToTray) noexcept
    {
        return windowVisible && !windowIconic && !hiddenToTray;
    }

    enum class TrayMenuCommand
    {
        TogglePresence,
        Exit
    };

    class WindowTrayController
    {
    public:
        using BeforeShowCallback = std::function<void()>;
        using LogCallback = std::function<void(const std::wstring&, const std::wstring&, const std::wstring&)>;
        using MenuCommandCallback = std::function<void(TrayMenuCommand)>;
        using ShouldInterceptCloseCallback = std::function<bool()>;
        using VisibilityChangedCallback = std::function<void(bool)>;

        WindowTrayController() = default;

        ~WindowTrayController()
        {
            Shutdown();
        }

        void SetBeforeShowCallback(BeforeShowCallback callback)
        {
            m_beforeShowCallback = std::move(callback);
        }

        void SetLogCallback(LogCallback callback)
        {
            m_logCallback = std::move(callback);
        }

        void SetMenuCommandCallback(MenuCommandCallback callback)
        {
            m_menuCommandCallback = std::move(callback);
        }

        void SetShouldInterceptCloseCallback(ShouldInterceptCloseCallback callback)
        {
            m_shouldInterceptCloseCallback = std::move(callback);
        }

        void SetVisibilityChangedCallback(VisibilityChangedCallback callback)
        {
            m_visibilityChangedCallback = std::move(callback);
        }

        void SetCloseToTrayOnClose(bool enabled) noexcept
        {
            m_closeToTrayOnClose = enabled;
        }

        void SetTrayLeftClickToggles(bool enabled) noexcept
        {
            m_trayLeftClickToggles = enabled;
        }

        void SetPresenceEnabled(bool enabled) noexcept
        {
            m_presenceEnabled = enabled;
        }

        [[nodiscard]] bool IsReady() const noexcept
        {
            return m_windowHandle && m_trayIconAdded;
        }

        [[nodiscard]] bool Initialize(HWND hwnd, std::filesystem::path const& iconPath)
        {
            if (m_windowHandle)
                return m_windowHandle == hwnd;

            if (!hwnd)
                return false;

            m_windowHandle = hwnd;

            SetLastError(0);
            auto previousWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
                m_windowHandle,
                GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(&WindowTrayController::TrayWindowProc)));
            if (!previousWndProc && GetLastError() != 0)
            {
                m_windowHandle = nullptr;
                Log(L"WARN", L"tray", L"Failed to hook window procedure");
                return false;
            }

            m_originalWndProc = previousWndProc;
            {
                std::lock_guard<std::mutex> lock(WindowStateMutex());
                WindowStateMap()[m_windowHandle] = { this, m_originalWndProc };
            }

            NOTIFYICONDATAW nid{};
            nid.cbSize = sizeof(nid);
            nid.hWnd = m_windowHandle;
            nid.uID = kTrayIconId;
            nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
            nid.uCallbackMessage = kTrayCallbackMessage;
            wcscpy_s(nid.szTip, L"Last Rich Presence");

            if (!iconPath.empty() && std::filesystem::exists(iconPath))
            {
                m_trayIconHandle = static_cast<HICON>(LoadImageW(
                    nullptr,
                    iconPath.c_str(),
                    IMAGE_ICON,
                    0,
                    0,
                    LR_LOADFROMFILE | LR_DEFAULTSIZE));
                m_trayIconOwned = (m_trayIconHandle != nullptr);
            }

            if (!m_trayIconHandle)
            {
                m_trayIconHandle = LoadIconW(nullptr, IDI_APPLICATION);
                m_trayIconOwned = false;
            }

            if (AddTrayIcon())
            {
                Log(L"INFO", L"tray", L"System tray icon initialized");
                return true;
            }

            Log(L"WARN", L"tray", L"Failed to add system tray icon");
            Shutdown();
            return false;
        }

        void Shutdown()
        {
            if (m_trayIconAdded && m_windowHandle)
            {
                NOTIFYICONDATAW nid{};
                nid.cbSize = sizeof(nid);
                nid.hWnd = m_windowHandle;
                nid.uID = kTrayIconId;
                Shell_NotifyIconW(NIM_DELETE, &nid);
                m_trayIconAdded = false;
            }

            if (m_trayIconHandle && m_trayIconOwned)
                DestroyIcon(m_trayIconHandle);

            m_trayIconHandle = nullptr;
            m_trayIconOwned = false;

            if (m_windowHandle && m_originalWndProc)
            {
                SetWindowLongPtrW(
                    m_windowHandle,
                    GWLP_WNDPROC,
                    reinterpret_cast<LONG_PTR>(m_originalWndProc));
            }

            if (m_windowHandle)
            {
                std::lock_guard<std::mutex> lock(WindowStateMutex());
                WindowStateMap().erase(m_windowHandle);
            }

            m_hiddenToTray = false;
            m_originalWndProc = nullptr;
            m_windowHandle = nullptr;
        }

        [[nodiscard]] bool IsHiddenToTray() const noexcept
        {
            return m_hiddenToTray;
        }

        [[nodiscard]] bool IsWindowIconic() const noexcept
        {
            return m_windowHandle && IsIconic(m_windowHandle);
        }

        [[nodiscard]] bool IsWindowVisible() const noexcept
        {
            return IsWindowVisibleToUser(m_windowHandle && ::IsWindowVisible(m_windowHandle), IsWindowIconic(), m_hiddenToTray);
        }

        [[nodiscard]] HWND WindowHandle() const noexcept
        {
            return m_windowHandle;
        }

        void ShowFromTray()
        {
            if (!m_windowHandle)
                return;

            Invoke(m_beforeShowCallback);
            ::ShowWindow(m_windowHandle, SW_SHOW);
            ::ShowWindow(m_windowHandle, SW_RESTORE);
            ::SetForegroundWindow(m_windowHandle);
            m_hiddenToTray = false;
            InvokeVisibilityChanged(true);
        }

        void HideToTray()
        {
            if (!m_windowHandle || m_hiddenToTray)
                return;

            ::ShowWindow(m_windowHandle, SW_HIDE);
            m_hiddenToTray = true;
            InvokeVisibilityChanged(false);
            Log(L"INFO", L"tray", L"Window minimized to system tray");
        }

        void ToggleVisibility()
        {
            auto now = std::chrono::steady_clock::now();
            if (m_lastToggleAt.time_since_epoch().count() > 0)
            {
                auto sinceLastToggle = now - m_lastToggleAt;
                if (sinceLastToggle < std::chrono::milliseconds(250))
                    return;
            }

            m_lastToggleAt = now;
            if (IsWindowVisible())
                HideToTray();
            else
                ShowFromTray();
        }

    private:
        static constexpr UINT kTrayCallbackMessage = WM_APP + 0x52;
        static constexpr UINT kTrayIconId = 1;
        static constexpr uint32_t kTrayMenuShowHide = 31001;
        static constexpr uint32_t kTrayMenuTogglePresence = 31002;
        static constexpr uint32_t kTrayMenuExit = 31003;

        struct WindowState
        {
            WindowTrayController* controller{ nullptr };
            WNDPROC originalWndProc{ nullptr };
        };

        static std::mutex& WindowStateMutex()
        {
            static std::mutex mutex;
            return mutex;
        }

        static std::unordered_map<HWND, WindowState>& WindowStateMap()
        {
            static std::unordered_map<HWND, WindowState> stateMap;
            return stateMap;
        }

        static UINT TaskbarCreatedMessage()
        {
            static const UINT message = RegisterWindowMessageW(L"TaskbarCreated");
            return message;
        }

        [[nodiscard]] bool AddTrayIcon()
        {
            if (!m_windowHandle || !m_trayIconHandle)
                return false;

            NOTIFYICONDATAW nid{};
            nid.cbSize = sizeof(nid);
            nid.hWnd = m_windowHandle;
            nid.uID = kTrayIconId;
            nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
            nid.uCallbackMessage = kTrayCallbackMessage;
            nid.hIcon = m_trayIconHandle;
            wcscpy_s(nid.szTip, L"Last Rich Presence");

            if (!Shell_NotifyIconW(NIM_ADD, &nid))
            {
                m_trayIconAdded = false;
                return false;
            }

            nid.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &nid);
            m_trayIconAdded = true;
            return true;
        }

        static LRESULT CALLBACK TrayWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
        {
            WindowTrayController* controller = nullptr;
            WNDPROC originalWndProc = nullptr;

            {
                std::lock_guard<std::mutex> lock(WindowStateMutex());
                auto it = WindowStateMap().find(hwnd);
                if (it != WindowStateMap().end())
                {
                    controller = it->second.controller;
                    originalWndProc = it->second.originalWndProc;
                }
            }

            LRESULT result = 0;
            if (controller && controller->HandleWindowMessage(message, wParam, lParam, result))
                return result;

            if (originalWndProc)
                return CallWindowProcW(originalWndProc, hwnd, message, wParam, lParam);

            return DefWindowProcW(hwnd, message, wParam, lParam);
        }

        [[nodiscard]] bool HandleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result)
        {
            result = 0;

            if (message == TaskbarCreatedMessage())
            {
                m_trayIconAdded = false;
                if (AddTrayIcon())
                    Log(L"INFO", L"tray", L"Restored system tray icon after Explorer restart");
                else
                    Log(L"WARN", L"tray", L"Failed to restore system tray icon after Explorer restart");
                return true;
            }

            if (message == WM_CLOSE)
            {
                const bool canInterceptClose =
                    m_closeToTrayOnClose &&
                    GetSystemMetrics(SM_SHUTTINGDOWN) == 0 &&
                    (!m_shouldInterceptCloseCallback || m_shouldInterceptCloseCallback());
                if (canInterceptClose)
                {
                    HideToTray();
                    return true;
                }

                return false;
            }

            if (message == WM_COMMAND)
            {
                auto commandId = static_cast<uint32_t>(LOWORD(wParam));
                switch (commandId)
                {
                case kTrayMenuShowHide:
                    ToggleVisibility();
                    return true;
                case kTrayMenuTogglePresence:
                    InvokeMenuCommand(TrayMenuCommand::TogglePresence);
                    return true;
                case kTrayMenuExit:
                    InvokeMenuCommand(TrayMenuCommand::Exit);
                    return true;
                default:
                    return false;
                }
            }

            if (message == kTrayCallbackMessage)
            {
                UINT iconId = 0;
                UINT eventCode = 0;

                auto eventLowWord = LOWORD(static_cast<DWORD_PTR>(lParam));
                auto eventHighWord = HIWORD(static_cast<DWORD_PTR>(lParam));

                const bool looksLikeV4Message =
                    eventLowWord == WM_CONTEXTMENU ||
                    eventLowWord == WM_RBUTTONUP ||
                    eventLowWord == WM_LBUTTONUP ||
                    eventLowWord == WM_LBUTTONDBLCLK ||
                    eventLowWord == NIN_SELECT ||
                    eventLowWord == NIN_KEYSELECT;

                if (looksLikeV4Message)
                {
                    iconId = eventHighWord;
                    eventCode = eventLowWord;
                }
                else
                {
                    iconId = static_cast<UINT>(wParam);
                    eventCode = static_cast<UINT>(lParam);
                }

                if (iconId != kTrayIconId)
                    return false;

                if (eventCode == WM_CONTEXTMENU || eventCode == WM_RBUTTONUP)
                {
                    ShowContextMenu();
                    return true;
                }

                if (eventCode == WM_LBUTTONUP || eventCode == WM_LBUTTONDBLCLK || eventCode == NIN_SELECT || eventCode == NIN_KEYSELECT)
                {
                    if (m_trayLeftClickToggles)
                        ToggleVisibility();
                    else
                        ShowFromTray();
                    return true;
                }
            }

            return false;
        }

        void ShowContextMenu()
        {
            if (!m_windowHandle)
                return;

            auto menu = CreatePopupMenu();
            if (!menu)
                return;

            auto showHideText = IsWindowVisible() ? L"Hide window" : L"Show window";
            auto togglePresenceText = m_presenceEnabled ? L"Disable Rich Presence" : L"Enable Rich Presence";

            AppendMenuW(menu, MF_STRING, kTrayMenuShowHide, showHideText);
            AppendMenuW(menu, MF_STRING, kTrayMenuTogglePresence, togglePresenceText);
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, kTrayMenuExit, L"Exit");

            POINT point{};
            GetCursorPos(&point);
            SetForegroundWindow(m_windowHandle);

            auto command = TrackPopupMenu(
                menu,
                TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                point.x,
                point.y,
                0,
                m_windowHandle,
                nullptr);

            DestroyMenu(menu);

            if (command != 0)
                SendMessageW(m_windowHandle, WM_COMMAND, command, 0);

            PostMessageW(m_windowHandle, WM_NULL, 0, 0);
        }

        template <typename TCallback>
        static void Invoke(TCallback const& callback)
        {
            if (callback)
            {
                try
                {
                    callback();
                }
                catch (...) {}
            }
        }

        void InvokeMenuCommand(TrayMenuCommand command)
        {
            if (m_menuCommandCallback)
            {
                try
                {
                    m_menuCommandCallback(command);
                }
                catch (...) {}
            }
        }

        void InvokeVisibilityChanged(bool visible)
        {
            if (m_visibilityChangedCallback)
            {
                try
                {
                    m_visibilityChangedCallback(visible);
                }
                catch (...) {}
            }
        }

        void Log(const std::wstring& level, const std::wstring& component, const std::wstring& message)
        {
            if (m_logCallback)
            {
                try
                {
                    m_logCallback(level, component, message);
                }
                catch (...) {}
            }
        }

        BeforeShowCallback m_beforeShowCallback;
        LogCallback m_logCallback;
        MenuCommandCallback m_menuCommandCallback;
        ShouldInterceptCloseCallback m_shouldInterceptCloseCallback;
        VisibilityChangedCallback m_visibilityChangedCallback;
        std::chrono::steady_clock::time_point m_lastToggleAt{};
        bool m_hiddenToTray{ false };
        bool m_trayIconAdded{ false };
        bool m_trayIconOwned{ false };
        bool m_closeToTrayOnClose{ true };
        bool m_trayLeftClickToggles{ true };
        bool m_presenceEnabled{ true };
        HWND m_windowHandle{ nullptr };
        WNDPROC m_originalWndProc{ nullptr };
        HICON m_trayIconHandle{ nullptr };
    };
}
