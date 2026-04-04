#include "pch.h"
#include "App.xaml.h"
#include "BrowserNativeMessaging.h"
#include "MainWindow.xaml.h"

#include <microsoft.ui.xaml.window.h>
#include <objbase.h>
#include <shellapi.h>

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace
{
    constexpr wchar_t kAppSettingsRegistryPath[] = L"Software\\LastProjects\\LastRichPresence";
    constexpr wchar_t kStartMinimizedRegistryValueName[] = L"StartMinimizedToTray";
    constexpr wchar_t kSingleInstanceKey[] = L"LastRichPresence.Primary";

    std::wstring ToLowerTrimmed(std::wstring value)
    {
        auto isWhitespace = [](wchar_t ch)
        {
            return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
        };

        while (!value.empty() && isWhitespace(value.front()))
            value.erase(value.begin());
        while (!value.empty() && isWhitespace(value.back()))
            value.pop_back();

        for (auto& ch : value)
            ch = towlower(ch);

        return value;
    }

    bool HasStartMinimizedArgument(std::wstring arguments)
    {
        if (arguments.empty())
            return false;

        std::wstring syntheticCommandLine = L"LastRichPresence.exe ";
        syntheticCommandLine += arguments;

        int argCount = 0;
        auto argv = CommandLineToArgvW(syntheticCommandLine.c_str(), &argCount);
        if (!argv)
            return false;

        bool requested = false;
        for (int index = 1; index < argCount; ++index)
        {
            auto arg = ToLowerTrimmed(argv[index]);
            if (arg == L"--start-minimized" || arg == L"/start-minimized")
            {
                requested = true;
                break;
            }
        }

        LocalFree(argv);
        return requested;
    }

    bool HasBrowserNativeHostActivationArgument(std::wstring arguments)
    {
        if (arguments.empty())
            return false;

        std::wstring syntheticCommandLine = L"LastRichPresence.exe ";
        syntheticCommandLine += arguments;

        int argCount = 0;
        auto argv = CommandLineToArgvW(syntheticCommandLine.c_str(), &argCount);
        if (!argv)
            return false;

        bool requested = false;
        for (int index = 1; index < argCount; ++index)
        {
            auto arg = ToLowerTrimmed(argv[index]);
            if (arg == lrp::browser::kBrowserNativeHostArgument ||
                arg == L"/browser-native-host" ||
                arg.rfind(L"chrome-extension://", 0) == 0 ||
                arg.rfind(L"edge-extension://", 0) == 0)
            {
                requested = true;
                break;
            }
        }

        LocalFree(argv);
        return requested;
    }

    bool IsStartMinimizedArgumentPresent()
    {
        int argCount = 0;
        auto args = CommandLineToArgvW(GetCommandLineW(), &argCount);
        if (!args)
            return false;

        bool requested = false;
        for (int index = 1; index < argCount; ++index)
        {
            auto arg = ToLowerTrimmed(args[index]);
            if (arg == L"--start-minimized" || arg == L"/start-minimized")
            {
                requested = true;
                break;
            }
        }

        LocalFree(args);
        return requested;
    }

    bool ShouldShowExistingWindowForActivation(Microsoft::Windows::AppLifecycle::AppActivationArguments const& activationArgs)
    {
        using Microsoft::Windows::AppLifecycle::ExtendedActivationKind;

        if (!activationArgs)
            return true;

        switch (activationArgs.Kind())
        {
        case ExtendedActivationKind::StartupTask:
            return false;

        case ExtendedActivationKind::Launch:
        {
            if (auto launchArgs = activationArgs.Data().try_as<Windows::ApplicationModel::Activation::ILaunchActivatedEventArgs>())
            {
                auto arguments = std::wstring(launchArgs.Arguments().c_str());
                return
                    !HasStartMinimizedArgument(arguments) &&
                    !HasBrowserNativeHostActivationArgument(arguments);
            }
            return true;
        }

        case ExtendedActivationKind::CommandLineLaunch:
        {
            if (auto commandLineArgs = activationArgs.Data().try_as<Windows::ApplicationModel::Activation::ICommandLineActivatedEventArgs>())
            {
                auto arguments = std::wstring(commandLineArgs.Operation().Arguments().c_str());
                return
                    !HasStartMinimizedArgument(arguments) &&
                    !HasBrowserNativeHostActivationArgument(arguments);
            }
            return true;
        }

        default:
            return true;
        }
    }

    void RedirectActivationFromStaThread(
        Microsoft::Windows::AppLifecycle::AppInstance const& targetInstance,
        Microsoft::Windows::AppLifecycle::AppActivationArguments const& activationArgs)
    {
        auto completionEvent = winrt::handle{ winrt::check_pointer(CreateEventW(nullptr, true, false, nullptr)) };
        std::exception_ptr redirectError;

        std::thread redirectThread([targetInstance, activationArgs, completionHandle = completionEvent.get(), &redirectError]()
        {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);

            try
            {
                targetInstance.RedirectActivationToAsync(activationArgs).get();
            }
            catch (...)
            {
                redirectError = std::current_exception();
            }

            SetEvent(completionHandle);
        });

        HANDLE waitHandles[] = { completionEvent.get() };
        DWORD signaledIndex = 0;
        auto waitResult = CoWaitForMultipleObjects(
            COWAIT_DISPATCH_CALLS | COWAIT_DISPATCH_WINDOW_MESSAGES,
            INFINITE,
            static_cast<ULONG>(std::size(waitHandles)),
            waitHandles,
            &signaledIndex);

        redirectThread.join();
        winrt::check_hresult(waitResult);

        if (redirectError)
            std::rethrow_exception(redirectError);
    }

    bool TryReadStartMinimizedFromRegistry(bool& valueOut)
    {
        valueOut = false;

        HKEY settingsKey = nullptr;
        auto openResult = RegOpenKeyExW(
            HKEY_CURRENT_USER,
            kAppSettingsRegistryPath,
            0,
            KEY_QUERY_VALUE,
            &settingsKey);
        if (openResult != ERROR_SUCCESS)
            return false;

        DWORD valueType = 0;
        DWORD valueData = 0;
        DWORD valueSize = sizeof(valueData);
        auto queryResult = RegQueryValueExW(
            settingsKey,
            kStartMinimizedRegistryValueName,
            nullptr,
            &valueType,
            reinterpret_cast<LPBYTE>(&valueData),
            &valueSize);
        RegCloseKey(settingsKey);

        if (queryResult != ERROR_SUCCESS || valueType != REG_DWORD || valueSize < sizeof(valueData))
            return false;

        valueOut = (valueData != 0);
        return true;
    }

    bool TryReadStartMinimizedFromLocalSettings(bool& valueOut)
    {
        valueOut = false;

        try
        {
            auto values = Windows::Storage::ApplicationData::Current().LocalSettings().Values();
            auto value = values.TryLookup(L"StartMinimizedToTray");
            if (!value)
                return false;

            valueOut = unbox_value<bool>(value);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool ShouldStartHiddenAtLaunch()
    {
        if (IsStartMinimizedArgumentPresent())
            return true;

        bool startMinimized = false;
        if (TryReadStartMinimizedFromRegistry(startMinimized))
            return startMinimized;

        if (TryReadStartMinimizedFromLocalSettings(startMinimized))
            return startMinimized;

        return false;
    }
}

namespace winrt::Last_Rich_Presence::implementation
{
    App::App()
    {
#if defined _DEBUG && !defined DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION
        UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e)
        {
            if (IsDebuggerPresent())
            {
                auto errorMessage = e.Message();
                __debugbreak();
            }
        });
#endif
    }

    void App::OnLaunched([[maybe_unused]] LaunchActivatedEventArgs const& e)
    {
        if (lrp::browser::IsBrowserNativeHostLaunch())
        {
            auto exitCode = lrp::browser::RunBrowserNativeHostFromCurrentProcess();
            ::ExitProcess(exitCode > 0 ? static_cast<UINT>(exitCode) : 0u);
        }

        std::wstring registrationError;
        if (!lrp::browser::RefreshNativeHostRegistration(&registrationError) && !registrationError.empty())
        {
            auto message = L"Browser native host registration refresh failed: " + registrationError + L"\n";
            OutputDebugStringW(message.c_str());
        }

        m_dispatcherQueue = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();

        auto activationArgs = Microsoft::Windows::AppLifecycle::AppInstance::GetCurrent().GetActivatedEventArgs();
        m_singleInstance = Microsoft::Windows::AppLifecycle::AppInstance::FindOrRegisterForKey(kSingleInstanceKey);
        if (!m_singleInstance.IsCurrent())
        {
            RedirectActivationFromStaThread(m_singleInstance, activationArgs);
            return;
        }

        m_singleInstanceActivatedToken = m_singleInstance.Activated([this](Windows::Foundation::IInspectable const&, Microsoft::Windows::AppLifecycle::AppActivationArguments const& activationArgs)
        {
            if (!ShouldShowExistingWindowForActivation(activationArgs))
                return;

            m_pendingRedirectedActivation.store(true, std::memory_order_relaxed);

            if (m_dispatcherQueue)
            {
                m_dispatcherQueue.TryEnqueue([this]()
                {
                    ProcessPendingRedirectedActivation();
                });
            }
        });

        auto shouldStartHidden = ShouldStartHiddenAtLaunch();
        auto mainWindow = winrt::make<MainWindow>();
        window = mainWindow;

        if (shouldStartHidden)
        {
            // Do not activate the window for minimized startup; activation causes
            // a visible frame on some systems before tray-hide applies.
            mainWindow.InitWindow();
            m_mainWindowReady = true;
            ProcessPendingRedirectedActivation();
            return;
        }

        window.Activate();
        mainWindow.InitWindow();
        m_mainWindowReady = true;
        ProcessPendingRedirectedActivation();
    }

    void App::ProcessPendingRedirectedActivation()
    {
        if (!m_mainWindowReady || !window)
            return;

        if (!m_pendingRedirectedActivation.load(std::memory_order_relaxed))
            return;

        m_pendingRedirectedActivation.store(false, std::memory_order_relaxed);
        if (auto mainWindow = window.try_as<winrt::Last_Rich_Presence::MainWindow>())
            mainWindow.HandleRedirectedActivation();
    }
}
