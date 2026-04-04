#pragma once

#include "PageModels.h"
#include "SettingsPageControl.g.h"

#include <cstdint>
#include <functional>

namespace winrt::Last_Rich_Presence::implementation
{
    struct SettingsPageControl : SettingsPageControlT<SettingsPageControl>
    {
        SettingsPageControl();
        ~SettingsPageControl();

        void ApplyState(lrp::ui::SettingsPageState const& state);
        void ApplySettings(lrp::ui::SettingsPageSettings const& settings);
        lrp::ui::SettingsPageSettings ReadSettings();

        void SetSettingsChangedCallback(std::function<void(lrp::ui::SettingsPageSettings const&)> callback);
        void SetResetSettingsCallback(std::function<void()> callback);
        void SetExportSettingsCallback(std::function<void()> callback);
        void SetImportSettingsCallback(std::function<void()> callback);
        void SetApplyBlockedTermsCallback(std::function<void(winrt::hstring const&)> callback);
        void SetClearDiagnosticsCallback(std::function<void()> callback);
        void SetExportDiagnosticsCallback(std::function<void()> callback);
        void ResetScrollPosition();

        void OnSettingsToggleChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                     winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnSettingsSelectionChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                        winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& e);
        void OnBlockedTermsTextChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                       winrt::Microsoft::UI::Xaml::Controls::TextChangedEventArgs const& e);
        void OnResetSettingsClicked(winrt::Windows::Foundation::IInspectable const& sender,
                                    winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnExportSettingsClicked(winrt::Windows::Foundation::IInspectable const& sender,
                                     winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnImportSettingsClicked(winrt::Windows::Foundation::IInspectable const& sender,
                                     winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnApplyBlockedTermsClicked(winrt::Windows::Foundation::IInspectable const& sender,
                                        winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnDiagnosticsClearRequested(winrt::Windows::Foundation::IInspectable const& sender,
                                         winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnDiagnosticsExportRequested(winrt::Windows::Foundation::IInspectable const& sender,
                                          winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);

    private:
        void NotifySettingsChanged();

        std::function<void(lrp::ui::SettingsPageSettings const&)> m_settingsChangedCallback;
        std::function<void()> m_resetSettingsCallback;
        std::function<void()> m_exportSettingsCallback;
        std::function<void()> m_importSettingsCallback;
        std::function<void(winrt::hstring const&)> m_applyBlockedTermsCallback;
        std::function<void()> m_clearDiagnosticsCallback;
        std::function<void()> m_exportDiagnosticsCallback;
        bool m_suppressSettingsChanged{ false };
        bool m_hasBlockedTermsDraft{ false };
        winrt::hstring m_committedBlockedTermsRaw{};

        winrt::event_token m_diagnosticsClearToken{};
        winrt::event_token m_diagnosticsExportToken{};
    };
}

namespace winrt::Last_Rich_Presence::factory_implementation
{
    struct SettingsPageControl : SettingsPageControlT<SettingsPageControl, implementation::SettingsPageControl>
    {
    };
}
