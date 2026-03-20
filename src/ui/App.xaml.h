#pragma once

#include "App.xaml.g.h"

namespace winrt::Last_Rich_Presence::implementation
{
    struct App : AppT<App>
    {
        App();

        void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

    private:
        void ProcessPendingRedirectedActivation();

        winrt::Microsoft::UI::Xaml::Window window{ nullptr };
        winrt::Microsoft::UI::Dispatching::DispatcherQueue m_dispatcherQueue{ nullptr };
        winrt::Microsoft::Windows::AppLifecycle::AppInstance m_singleInstance{ nullptr };
        winrt::event_token m_singleInstanceActivatedToken{};
        std::atomic<bool> m_pendingRedirectedActivation{ false };
        bool m_mainWindowReady{ false };
    };
}
