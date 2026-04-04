#include "pch.h"
#include "ProductivityPageControl.xaml.h"
#include "SettingsModels.h"
#if __has_include("ProductivityPageControl.g.cpp")
#include "ProductivityPageControl.g.cpp"
#endif

#include <utility>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media;
using namespace Windows::Foundation;

namespace
{
    struct ScopedSettingsChangedBlock
    {
        explicit ScopedSettingsChangedBlock(bool& blocked)
            : m_blocked(blocked)
            , m_restore(blocked)
        {
            m_blocked = true;
        }

        ~ScopedSettingsChangedBlock()
        {
            m_blocked = m_restore;
        }

    private:
        bool& m_blocked;
        bool m_restore;
    };

    int32_t ProductiveDetectionModeToComboIndex(ProductiveDetectionMode mode)
    {
        return lrp::settings::ProductiveDetectionModeToComboIndex(mode);
    }

    ProductiveDetectionMode ProductiveDetectionModeFromComboIndex(int32_t index)
    {
        return lrp::settings::ProductiveDetectionModeFromComboIndex(index);
    }

    hstring CoalesceText(hstring const& value, wchar_t const* fallback)
    {
        if (!value.empty())
        {
            return value;
        }

        return hstring(fallback);
    }
}

namespace winrt::Last_Rich_Presence::implementation
{
    ProductivityPageControl::ProductivityPageControl()
    {
        InitializeComponent();
        ApplySettingsControlsEnabled(ProductiveEnableToggle().IsOn());
    }

    void ProductivityPageControl::ApplyState(const lrp::ui::ProductivityPageState& state)
    {
        ProductiveDetectedAppText().Text(CoalesceText(state.detectedAppText, L"Awaiting Productive Activity"));
        ProductiveDetectedProjectText().Text(CoalesceText(state.detectedProjectText, L"Launch a supported Office app to update your status."));
        ProductiveDetectedWindowText().Text(CoalesceText(state.detectedWindowText, L"None"));
        ProductiveDetectedProcessText().Text(CoalesceText(state.detectedProcessText, L"None"));
        ProductiveRuntimeSummaryText().Text(CoalesceText(state.runtimeSummaryText, L"Waiting for supported apps..."));
        ApplySettingsControlsEnabled(state.settingsControlsEnabled);
    }

    void ProductivityPageControl::ApplySettings(const lrp::ui::ProductivityPageSettings& settings)
    {
        ScopedSettingsChangedBlock callbackBlock(m_suppressSettingsChanged);

        ProductiveEnableToggle().IsOn(settings.enabled);
        ProductiveDetectionModeCombo().SelectedIndex(ProductiveDetectionModeToComboIndex(settings.detectionMode));
        ProductiveShowProjectToggle().IsOn(settings.showProjectName);
        ProductiveAppWordCheck().IsChecked(settings.wordEnabled);
        ProductiveAppExcelCheck().IsChecked(settings.excelEnabled);
        ProductiveAppPowerPointCheck().IsChecked(settings.powerPointEnabled);
        ProductiveAppOneNoteCheck().IsChecked(settings.oneNoteEnabled);
        ProductiveAppAccessCheck().IsChecked(settings.accessEnabled);
        ProductiveAppPublisherCheck().IsChecked(settings.publisherEnabled);
        ProductiveAppVisioCheck().IsChecked(settings.visioEnabled);
        ProductiveAppProjectCheck().IsChecked(settings.projectEnabled);
        ProductiveAppCodexCheck().IsChecked(settings.codexEnabled);
        ApplySettingsControlsEnabled(settings.enabled);
    }

    lrp::ui::ProductivityPageSettings ProductivityPageControl::ReadSettings()
    {
        auto self = const_cast<ProductivityPageControl*>(this);
        lrp::ui::ProductivityPageSettings settings{};
        settings.enabled = self->ProductiveEnableToggle().IsOn();
        settings.detectionMode = ProductiveDetectionModeFromComboIndex(self->ProductiveDetectionModeCombo().SelectedIndex());
        settings.showProjectName = self->ProductiveShowProjectToggle().IsOn();
        settings.wordEnabled = IsCheckedOrDefault(self->ProductiveAppWordCheck().IsChecked(), false);
        settings.excelEnabled = IsCheckedOrDefault(self->ProductiveAppExcelCheck().IsChecked(), false);
        settings.powerPointEnabled = IsCheckedOrDefault(self->ProductiveAppPowerPointCheck().IsChecked(), false);
        settings.oneNoteEnabled = IsCheckedOrDefault(self->ProductiveAppOneNoteCheck().IsChecked(), false);
        settings.accessEnabled = IsCheckedOrDefault(self->ProductiveAppAccessCheck().IsChecked(), false);
        settings.publisherEnabled = IsCheckedOrDefault(self->ProductiveAppPublisherCheck().IsChecked(), false);
        settings.visioEnabled = IsCheckedOrDefault(self->ProductiveAppVisioCheck().IsChecked(), false);
        settings.projectEnabled = IsCheckedOrDefault(self->ProductiveAppProjectCheck().IsChecked(), false);
        settings.codexEnabled = IsCheckedOrDefault(self->ProductiveAppCodexCheck().IsChecked(), false);
        return settings;
    }

    void ProductivityPageControl::SetSettingsChangedCallback(std::function<void(const lrp::ui::ProductivityPageSettings&)> callback)
    {
        m_settingsChangedCallback = std::move(callback);
    }

    void ProductivityPageControl::SetSelectAllCallback(std::function<void()> callback)
    {
        m_selectAllCallback = std::move(callback);
    }

    void ProductivityPageControl::SetDeselectAllCallback(std::function<void()> callback)
    {
        m_deselectAllCallback = std::move(callback);
    }

    void ProductivityPageControl::SetDetectedAppIcon(ImageSource const& source)
    {
        if (!source)
        {
            ClearDetectedAppIcon();
            return;
        }

        ProductiveDetectedAppIcon().Source(source);
        ProductiveDetectedAppIcon().Visibility(Visibility::Visible);
        ProductiveDetectedAppIconFallback().Visibility(Visibility::Collapsed);
    }

    void ProductivityPageControl::ClearDetectedAppIcon()
    {
        ProductiveDetectedAppIcon().Source(nullptr);
        ProductiveDetectedAppIcon().Visibility(Visibility::Collapsed);
        ProductiveDetectedAppIconFallback().Visibility(Visibility::Visible);
    }

    void ProductivityPageControl::ResetScrollPosition()
    {
        auto scrollViewer = ProductivityPage();
        if (!scrollViewer || scrollViewer.VerticalOffset() <= 0.5)
        {
            return;
        }

        try
        {
            scrollViewer.ChangeView(
                nullptr,
                box_value(0.0).as<Windows::Foundation::IReference<double>>(),
                nullptr,
                true);
        }
        catch (...) {}
    }

    void ProductivityPageControl::OnSettingsToggleChanged(IInspectable const&, RoutedEventArgs const&)
    {
        ApplySettingsControlsEnabled(ProductiveEnableToggle().IsOn());
        NotifySettingsChanged();
    }

    void ProductivityPageControl::OnDetectionModeSelectionChanged(IInspectable const&, SelectionChangedEventArgs const&)
    {
        NotifySettingsChanged();
    }

    void ProductivityPageControl::OnAppFilterToggled(IInspectable const&, RoutedEventArgs const&)
    {
        NotifySettingsChanged();
    }

    void ProductivityPageControl::OnSelectAllAppsClicked(IInspectable const&, RoutedEventArgs const&)
    {
        SetAllAppFilterChecks(true);

        if (m_selectAllCallback)
        {
            m_selectAllCallback();
        }
    }

    void ProductivityPageControl::OnDeselectAllAppsClicked(IInspectable const&, RoutedEventArgs const&)
    {
        SetAllAppFilterChecks(false);

        if (m_deselectAllCallback)
        {
            m_deselectAllCallback();
        }
    }

    bool ProductivityPageControl::IsCheckedOrDefault(IReference<bool> const& value, bool defaultValue) noexcept
    {
        return value ? value.Value() : defaultValue;
    }

    void ProductivityPageControl::SetAllAppFilterChecks(bool enabled)
    {
        ScopedSettingsChangedBlock callbackBlock(m_suppressSettingsChanged);

        ProductiveAppWordCheck().IsChecked(enabled);
        ProductiveAppExcelCheck().IsChecked(enabled);
        ProductiveAppPowerPointCheck().IsChecked(enabled);
        ProductiveAppOneNoteCheck().IsChecked(enabled);
        ProductiveAppAccessCheck().IsChecked(enabled);
        ProductiveAppPublisherCheck().IsChecked(enabled);
        ProductiveAppVisioCheck().IsChecked(enabled);
        ProductiveAppProjectCheck().IsChecked(enabled);
        ProductiveAppCodexCheck().IsChecked(enabled);
    }

    void ProductivityPageControl::ApplySettingsControlsEnabled(bool enabled)
    {
        if (auto panel = ProductiveSettingsControlsPanel())
        {
            panel.IsHitTestVisible(enabled);
            panel.Opacity(enabled ? 1.0 : 0.55);
        }
    }

    void ProductivityPageControl::NotifySettingsChanged()
    {
        if (m_suppressSettingsChanged || !m_settingsChangedCallback)
        {
            return;
        }

        m_settingsChangedCallback(ReadSettings());
    }
}
