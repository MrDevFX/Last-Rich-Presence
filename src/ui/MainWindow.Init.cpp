#include "pch.h"
#include "MainWindow.xaml.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::Last_Rich_Presence::implementation
{
    void MainWindow::ConfigurePageControlCallbacks()
    {
        HomePageControlImpl()->SetEnableToggledCallback([this](bool enabled)
        {
            this->HandleHomeEnableToggled(enabled);
        });

        MusicPageControlImpl()->SetSettingsChangedCallback([this](lrp::ui::MusicPageSettings const& settings)
        {
            this->HandleMusicSettingsChanged(settings);
        });

        ProductivityPageControlImpl()->SetSettingsChangedCallback([this](lrp::ui::ProductivityPageSettings const& settings)
        {
            this->HandleProductivitySettingsChanged(settings);
        });
        ProductivityPageControlImpl()->SetSelectAllCallback([this]()
        {
            this->HandleProductivitySelectAll();
        });
        ProductivityPageControlImpl()->SetDeselectAllCallback([this]()
        {
            this->HandleProductivityDeselectAll();
        });

        CreativePageControlImpl()->SetSettingsChangedCallback([this](lrp::ui::CreativePageSettings const& settings)
        {
            this->HandleCreativeSettingsChanged(settings);
        });
        CreativePageControlImpl()->SetSelectAllCallback([this]()
        {
            this->HandleCreativeSelectAll();
        });
        CreativePageControlImpl()->SetDeselectAllCallback([this]()
        {
            this->HandleCreativeDeselectAll();
        });

        SettingsPageControlImpl()->SetSettingsChangedCallback([this](lrp::ui::SettingsPageSettings const& settings)
        {
            this->HandleSettingsPageSettingsChanged(settings);
        });
        SettingsPageControlImpl()->SetResetSettingsCallback([this]()
        {
            OnResetSettingsClicked(IInspectable{ nullptr }, RoutedEventArgs{});
        });
        SettingsPageControlImpl()->SetExportSettingsCallback([this]()
        {
            OnExportSettingsJsonClicked(IInspectable{ nullptr }, RoutedEventArgs{});
        });
        SettingsPageControlImpl()->SetImportSettingsCallback([this]()
        {
            OnImportSettingsJsonClicked(IInspectable{ nullptr }, RoutedEventArgs{});
        });
        SettingsPageControlImpl()->SetApplyBlockedTermsCallback([this](winrt::hstring const& blockedTermsRaw)
        {
            this->HandleApplyBlockedTerms(blockedTermsRaw);
        });
        SettingsPageControlImpl()->SetClearDiagnosticsCallback([this]()
        {
            OnClearDiagnosticsClicked(IInspectable{ nullptr }, RoutedEventArgs{});
        });
        SettingsPageControlImpl()->SetExportDiagnosticsCallback([this]()
        {
            OnExportDiagnosticsJsonClicked(IInspectable{ nullptr }, RoutedEventArgs{});
        });
    }

    void MainWindow::InitializeShellNavigation()
    {
        m_activePage = lrp::ui::AppPage::Home;
        ShowOnlyPage(m_activePage);
        if (auto navItem = NavItemForPage(m_activePage))
            NavView().SelectedItem(navItem);
    }
}
