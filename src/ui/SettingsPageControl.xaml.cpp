#include "pch.h"
#include "SettingsPageControl.xaml.h"
#if __has_include("SettingsPageControl.g.cpp")
#include "SettingsPageControl.g.cpp"
#endif

#include <utility>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Windows::Foundation;

namespace winrt::Last_Rich_Presence::implementation
{
    namespace
    {
        struct ScopedSettingsCallbackBlock
        {
            explicit ScopedSettingsCallbackBlock(bool& blocked)
                : m_blocked(blocked)
                , m_restore(blocked)
            {
                m_blocked = true;
            }

            ~ScopedSettingsCallbackBlock()
            {
                m_blocked = m_restore;
            }

        private:
            bool& m_blocked;
            bool m_restore;
        };
    }

    SettingsPageControl::SettingsPageControl()
    {
        InitializeComponent();

        if (auto diagnosticsPanel = SettingsDiagnosticsPanelHost())
        {
            m_diagnosticsClearToken = diagnosticsPanel.ClearRequested({ this, &SettingsPageControl::OnDiagnosticsClearRequested });
            m_diagnosticsExportToken = diagnosticsPanel.ExportJsonRequested({ this, &SettingsPageControl::OnDiagnosticsExportRequested });
        }
    }

    SettingsPageControl::~SettingsPageControl()
    {
        try
        {
            if (auto diagnosticsPanel = SettingsDiagnosticsPanelHost())
            {
                if (m_diagnosticsClearToken.value != 0)
                    diagnosticsPanel.ClearRequested(m_diagnosticsClearToken);
                if (m_diagnosticsExportToken.value != 0)
                    diagnosticsPanel.ExportJsonRequested(m_diagnosticsExportToken);
            }
        }
        catch (...)
        {
            // Best-effort unhook during teardown.
        }
    }

    void SettingsPageControl::ApplyState(lrp::ui::SettingsPageState const& state)
    {
        SettingsDiagnosticsPanelHost().SetLogText(state.diagnosticsLogText);
    }

    void SettingsPageControl::ApplySettings(lrp::ui::SettingsPageSettings const& settings)
    {
        ScopedSettingsCallbackBlock callbackBlock(m_suppressSettingsChanged);

        CloseToTrayToggle().IsOn(settings.closeToTrayOnClose);
        LaunchOnStartupToggle().IsOn(settings.launchOnStartup);
        StartMinimizedToggle().IsOn(settings.startMinimizedToTray);
        TrayLeftClickToggle().IsOn(settings.trayLeftClickToggles);
        DefaultIdleStatusToggle().IsOn(settings.showDefaultIdleStatus);

        MediaActivityTypeCombo().SelectedIndex(settings.mediaActivityTypeIndex);
        CreativeActivityTypeCombo().SelectedIndex(settings.creativeActivityTypeIndex);
        ProductiveActivityTypeCombo().SelectedIndex(settings.productiveActivityTypeIndex);

        SensitiveFilterToggle().IsOn(settings.sensitiveKeywordFilter);
        StrictBrowserPrivacyToggle().IsOn(settings.strictBrowserPrivacy);
        SuppressBrowserArtToggle().IsOn(settings.suppressBrowserAlbumArt);
        auto currentBlockedTerms = BlockedAppSitesBox().Text();
        m_committedBlockedTermsRaw = settings.blockedAppSitesRaw;
        if (currentBlockedTerms == settings.blockedAppSitesRaw)
            m_hasBlockedTermsDraft = false;
        if (!m_hasBlockedTermsDraft)
            BlockedAppSitesBox().Text(settings.blockedAppSitesRaw);

        ThemeModeCombo().SelectedIndex(settings.themeModeIndex);
    }

    lrp::ui::SettingsPageSettings SettingsPageControl::ReadSettings()
    {
        auto self = const_cast<SettingsPageControl*>(this);
        lrp::ui::SettingsPageSettings settings;
        settings.closeToTrayOnClose = self->CloseToTrayToggle().IsOn();
        settings.launchOnStartup = self->LaunchOnStartupToggle().IsOn();
        settings.startMinimizedToTray = self->StartMinimizedToggle().IsOn();
        settings.trayLeftClickToggles = self->TrayLeftClickToggle().IsOn();
        settings.showDefaultIdleStatus = self->DefaultIdleStatusToggle().IsOn();

        settings.mediaActivityTypeIndex = self->MediaActivityTypeCombo().SelectedIndex();
        settings.creativeActivityTypeIndex = self->CreativeActivityTypeCombo().SelectedIndex();
        settings.productiveActivityTypeIndex = self->ProductiveActivityTypeCombo().SelectedIndex();

        settings.sensitiveKeywordFilter = self->SensitiveFilterToggle().IsOn();
        settings.strictBrowserPrivacy = self->StrictBrowserPrivacyToggle().IsOn();
        settings.suppressBrowserAlbumArt = self->SuppressBrowserArtToggle().IsOn();
        settings.blockedAppSitesRaw = self->BlockedAppSitesBox().Text();

        settings.themeModeIndex = self->ThemeModeCombo().SelectedIndex();
        return settings;
    }

    void SettingsPageControl::SetSettingsChangedCallback(std::function<void(lrp::ui::SettingsPageSettings const&)> callback)
    {
        m_settingsChangedCallback = std::move(callback);
    }

    void SettingsPageControl::SetResetSettingsCallback(std::function<void()> callback)
    {
        m_resetSettingsCallback = std::move(callback);
    }

    void SettingsPageControl::SetExportSettingsCallback(std::function<void()> callback)
    {
        m_exportSettingsCallback = std::move(callback);
    }

    void SettingsPageControl::SetImportSettingsCallback(std::function<void()> callback)
    {
        m_importSettingsCallback = std::move(callback);
    }

    void SettingsPageControl::SetApplyBlockedTermsCallback(std::function<void(winrt::hstring const&)> callback)
    {
        m_applyBlockedTermsCallback = std::move(callback);
    }

    void SettingsPageControl::SetClearDiagnosticsCallback(std::function<void()> callback)
    {
        m_clearDiagnosticsCallback = std::move(callback);
    }

    void SettingsPageControl::SetExportDiagnosticsCallback(std::function<void()> callback)
    {
        m_exportDiagnosticsCallback = std::move(callback);
    }

    void SettingsPageControl::ResetScrollPosition()
    {
        SettingsPage().ChangeView(
            nullptr,
            0.0,
            nullptr,
            true);
    }

    void SettingsPageControl::OnSettingsToggleChanged(IInspectable const&, RoutedEventArgs const&)
    {
        NotifySettingsChanged();
    }

    void SettingsPageControl::OnSettingsSelectionChanged(IInspectable const&, SelectionChangedEventArgs const&)
    {
        NotifySettingsChanged();
    }

    void SettingsPageControl::OnBlockedTermsTextChanged(IInspectable const&, TextChangedEventArgs const&)
    {
        if (m_suppressSettingsChanged)
            return;

        m_hasBlockedTermsDraft = BlockedAppSitesBox().Text() != m_committedBlockedTermsRaw;
    }

    void SettingsPageControl::OnResetSettingsClicked(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_resetSettingsCallback)
            m_resetSettingsCallback();
    }

    void SettingsPageControl::OnExportSettingsClicked(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_exportSettingsCallback)
            m_exportSettingsCallback();
    }

    void SettingsPageControl::OnImportSettingsClicked(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_importSettingsCallback)
            m_importSettingsCallback();
    }

    void SettingsPageControl::OnApplyBlockedTermsClicked(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_applyBlockedTermsCallback)
            m_applyBlockedTermsCallback(BlockedAppSitesBox().Text());
    }

    void SettingsPageControl::OnDiagnosticsClearRequested(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_clearDiagnosticsCallback)
            m_clearDiagnosticsCallback();
    }

    void SettingsPageControl::OnDiagnosticsExportRequested(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_exportDiagnosticsCallback)
            m_exportDiagnosticsCallback();
    }

    void SettingsPageControl::NotifySettingsChanged()
    {
        if (m_suppressSettingsChanged)
            return;

        if (m_settingsChangedCallback)
            m_settingsChangedCallback(ReadSettings());
    }
}
