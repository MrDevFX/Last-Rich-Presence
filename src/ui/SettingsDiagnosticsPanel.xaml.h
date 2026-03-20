#pragma once

#include "SettingsDiagnosticsPanel.g.h"

namespace winrt::Last_Rich_Presence::implementation
{
    struct SettingsDiagnosticsPanel : SettingsDiagnosticsPanelT<SettingsDiagnosticsPanel>
    {
        SettingsDiagnosticsPanel();

        event_token ClearRequested(Microsoft::UI::Xaml::RoutedEventHandler const& handler);
        void ClearRequested(event_token const& token) noexcept;
        event_token ExportJsonRequested(Microsoft::UI::Xaml::RoutedEventHandler const& handler);
        void ExportJsonRequested(event_token const& token) noexcept;
        void SetLogText(winrt::hstring const& value);

        void OnClearClicked(winrt::Windows::Foundation::IInspectable const& sender,
                            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnExportJsonClicked(winrt::Windows::Foundation::IInspectable const& sender,
                                 winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);

    private:
        winrt::event<Microsoft::UI::Xaml::RoutedEventHandler> m_clearRequested;
        winrt::event<Microsoft::UI::Xaml::RoutedEventHandler> m_exportJsonRequested;
    };
}

namespace winrt::Last_Rich_Presence::factory_implementation
{
    struct SettingsDiagnosticsPanel : SettingsDiagnosticsPanelT<SettingsDiagnosticsPanel, implementation::SettingsDiagnosticsPanel>
    {
    };
}
