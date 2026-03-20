#include "pch.h"
#include "SettingsDiagnosticsPanel.xaml.h"
#if __has_include("SettingsDiagnosticsPanel.g.cpp")
#include "SettingsDiagnosticsPanel.g.cpp"
#endif

namespace winrt::Last_Rich_Presence::implementation
{
    SettingsDiagnosticsPanel::SettingsDiagnosticsPanel()
    {
        InitializeComponent();
    }

    event_token SettingsDiagnosticsPanel::ClearRequested(Microsoft::UI::Xaml::RoutedEventHandler const& handler)
    {
        return m_clearRequested.add(handler);
    }

    void SettingsDiagnosticsPanel::ClearRequested(event_token const& token) noexcept
    {
        m_clearRequested.remove(token);
    }

    event_token SettingsDiagnosticsPanel::ExportJsonRequested(Microsoft::UI::Xaml::RoutedEventHandler const& handler)
    {
        return m_exportJsonRequested.add(handler);
    }

    void SettingsDiagnosticsPanel::ExportJsonRequested(event_token const& token) noexcept
    {
        m_exportJsonRequested.remove(token);
    }

    void SettingsDiagnosticsPanel::SetLogText(winrt::hstring const& value)
    {
        DiagnosticsLogBox().Text(value);
    }

    void SettingsDiagnosticsPanel::OnClearClicked(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e)
    {
        m_clearRequested(*this, e);
    }

    void SettingsDiagnosticsPanel::OnExportJsonClicked(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e)
    {
        m_exportJsonRequested(*this, e);
    }
}
