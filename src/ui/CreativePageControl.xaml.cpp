#include "pch.h"
#include "CreativePageControl.xaml.h"
#include "SettingsModels.h"
#include "PageModels.h"
#if __has_include("CreativePageControl.g.cpp")
#include "CreativePageControl.g.cpp"
#endif

namespace
{
    using winrt::hstring;
    using winrt::Microsoft::UI::Xaml::Controls::CheckBox;
    using winrt::Windows::Foundation::IReference;

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

    bool ReadCheck(IReference<bool> const& value, bool fallback)
    {
        if (!value)
            return fallback;

        try
        {
            return value.Value();
        }
        catch (...)
        {
            return fallback;
        }
    }

    void SetCheck(CheckBox const& checkBox, bool value)
    {
        if (checkBox)
            checkBox.IsChecked(value);
    }

    int32_t CreativePriorityModeToComboIndex(CreativePriorityMode mode)
    {
        return lrp::settings::CreativePriorityModeToComboIndex(mode);
    }

    CreativePriorityMode CreativePriorityModeFromComboIndex(int32_t index)
    {
        return lrp::settings::CreativePriorityModeFromComboIndex(index);
    }

    int32_t CreativeDetectionModeToComboIndex(CreativeDetectionMode mode)
    {
        return lrp::settings::CreativeDetectionModeToComboIndex(mode);
    }

    CreativeDetectionMode CreativeDetectionModeFromComboIndex(int32_t index)
    {
        return lrp::settings::CreativeDetectionModeFromComboIndex(index);
    }

    int32_t CreativePrivacyModeToComboIndex(CreativePrivacyMode mode)
    {
        return lrp::settings::CreativePrivacyModeToComboIndex(mode);
    }

    CreativePrivacyMode CreativePrivacyModeFromComboIndex(int32_t index)
    {
        return lrp::settings::CreativePrivacyModeFromComboIndex(index);
    }

    int32_t CreativeIdleBehaviorToComboIndex(CreativeIdleBehavior behavior)
    {
        return lrp::settings::CreativeIdleBehaviorToComboIndex(behavior);
    }

    CreativeIdleBehavior CreativeIdleBehaviorFromComboIndex(int32_t index)
    {
        return lrp::settings::CreativeIdleBehaviorFromComboIndex(index);
    }
}

namespace winrt::Last_Rich_Presence::implementation
{
    CreativePageControl::CreativePageControl()
    {
        InitializeComponent();
    }

    void CreativePageControl::ApplyState(lrp::ui::CreativePageState const& state)
    {
        if (CreativeSettingsControlsPanel())
        {
            CreativeSettingsControlsPanel().IsHitTestVisible(state.settingsControlsEnabled);
            CreativeSettingsControlsPanel().Opacity(state.settingsControlsEnabled ? 1.0 : 0.55);
        }

        if (CreativeMvpHeadlineText())
            CreativeMvpHeadlineText().Text(hstring(state.mvpHeadlineText));
        if (CreativeMvpSummaryText())
            CreativeMvpSummaryText().Text(hstring(state.mvpSummaryText));
        if (CreativeDetectedAppText())
            CreativeDetectedAppText().Text(hstring(state.detectedAppText));
        if (CreativeDetectedProjectText())
            CreativeDetectedProjectText().Text(hstring(state.detectedProjectText));
        if (CreativeDetectedWindowText())
            CreativeDetectedWindowText().Text(hstring(state.detectedWindowText));
        if (CreativeDetectedProcessText())
            CreativeDetectedProcessText().Text(hstring(state.detectedProcessText));
    }

    void CreativePageControl::ApplySettings(lrp::ui::CreativePageSettings const& settings)
    {
        ScopedSettingsChangedBlock callbackBlock(m_suppressSettingsChanged);

        if (CreativeEnableToggle())
            CreativeEnableToggle().IsOn(settings.enabled);
        if (CreativePriorityCombo())
            CreativePriorityCombo().SelectedIndex(CreativePriorityModeToComboIndex(settings.priority));
        if (CreativeDetectionModeCombo())
            CreativeDetectionModeCombo().SelectedIndex(CreativeDetectionModeToComboIndex(settings.detectionMode));
        if (CreativeShowProjectToggle())
            CreativeShowProjectToggle().IsOn(settings.showProjectName);
        if (CreativeShowWindowTitleToggle())
            CreativeShowWindowTitleToggle().IsOn(settings.showWindowTitle);

        SetCheck(CreativeAppPhotoshopCheck(), settings.photoshopEnabled);
        SetCheck(CreativeAppIllustratorCheck(), settings.illustratorEnabled);
        SetCheck(CreativeAppPremiereCheck(), settings.premiereEnabled);
        SetCheck(CreativeAppAfterEffectsCheck(), settings.afterEffectsEnabled);
        SetCheck(CreativeAppInDesignCheck(), settings.inDesignEnabled);
        SetCheck(CreativeAppAuditionCheck(), settings.auditionEnabled);
        SetCheck(CreativeAppMediaEncoderCheck(), settings.mediaEncoderEnabled);
        SetCheck(CreativeAppLightroomCheck(), settings.lightroomEnabled);
        SetCheck(CreativeAppLightroomClassicCheck(), settings.lightroomClassicEnabled);
        SetCheck(CreativeAppInCopyCheck(), settings.inCopyEnabled);
        SetCheck(CreativeAppDreamweaverCheck(), settings.dreamweaverEnabled);
        SetCheck(CreativeAppAnimateCheck(), settings.animateEnabled);
        SetCheck(CreativeAppXdCheck(), settings.xdEnabled);
        SetCheck(CreativeAppBridgeCheck(), settings.bridgeEnabled);
        SetCheck(CreativeAppCharacterAnimatorCheck(), settings.characterAnimatorEnabled);
        SetCheck(CreativeAppFrescoCheck(), settings.frescoEnabled);
        SetCheck(CreativeAppDimensionCheck(), settings.dimensionEnabled);
        SetCheck(CreativeAppSubstanceCheck(), settings.substanceEnabled);
        SetCheck(CreativeAppAcrobatCheck(), settings.acrobatEnabled);
        SetCheck(CreativeAppOtherAdobeCheck(), settings.otherAdobeEnabled);

        if (CreativePrivacyCombo())
            CreativePrivacyCombo().SelectedIndex(CreativePrivacyModeToComboIndex(settings.privacyMode));
        if (CreativeIdleBehaviorCombo())
            CreativeIdleBehaviorCombo().SelectedIndex(CreativeIdleBehaviorToComboIndex(settings.idleBehavior));

        if (CreativeSettingsControlsPanel())
        {
            CreativeSettingsControlsPanel().IsHitTestVisible(settings.enabled);
            CreativeSettingsControlsPanel().Opacity(settings.enabled ? 1.0 : 0.55);
        }
    }

    lrp::ui::CreativePageSettings CreativePageControl::ReadSettings()
    {
        lrp::ui::CreativePageSettings settings{};

        settings.enabled = CreativeEnableToggle() ? CreativeEnableToggle().IsOn() : true;
        settings.priority = CreativePriorityModeFromComboIndex(
            CreativePriorityCombo() ? CreativePriorityCombo().SelectedIndex() : 0);
        settings.detectionMode = CreativeDetectionModeFromComboIndex(
            CreativeDetectionModeCombo() ? CreativeDetectionModeCombo().SelectedIndex() : 0);
        settings.showProjectName = CreativeShowProjectToggle() ? CreativeShowProjectToggle().IsOn() : true;
        settings.showWindowTitle = CreativeShowWindowTitleToggle() ? CreativeShowWindowTitleToggle().IsOn() : false;

        settings.photoshopEnabled = ReadCheck(CreativeAppPhotoshopCheck() ? CreativeAppPhotoshopCheck().IsChecked() : nullptr, true);
        settings.illustratorEnabled = ReadCheck(CreativeAppIllustratorCheck() ? CreativeAppIllustratorCheck().IsChecked() : nullptr, true);
        settings.premiereEnabled = ReadCheck(CreativeAppPremiereCheck() ? CreativeAppPremiereCheck().IsChecked() : nullptr, true);
        settings.afterEffectsEnabled = ReadCheck(CreativeAppAfterEffectsCheck() ? CreativeAppAfterEffectsCheck().IsChecked() : nullptr, true);
        settings.inDesignEnabled = ReadCheck(CreativeAppInDesignCheck() ? CreativeAppInDesignCheck().IsChecked() : nullptr, true);
        settings.auditionEnabled = ReadCheck(CreativeAppAuditionCheck() ? CreativeAppAuditionCheck().IsChecked() : nullptr, true);
        settings.mediaEncoderEnabled = ReadCheck(CreativeAppMediaEncoderCheck() ? CreativeAppMediaEncoderCheck().IsChecked() : nullptr, true);
        settings.lightroomEnabled = ReadCheck(CreativeAppLightroomCheck() ? CreativeAppLightroomCheck().IsChecked() : nullptr, true);
        settings.lightroomClassicEnabled = ReadCheck(CreativeAppLightroomClassicCheck() ? CreativeAppLightroomClassicCheck().IsChecked() : nullptr, true);
        settings.inCopyEnabled = ReadCheck(CreativeAppInCopyCheck() ? CreativeAppInCopyCheck().IsChecked() : nullptr, true);
        settings.dreamweaverEnabled = ReadCheck(CreativeAppDreamweaverCheck() ? CreativeAppDreamweaverCheck().IsChecked() : nullptr, true);
        settings.animateEnabled = ReadCheck(CreativeAppAnimateCheck() ? CreativeAppAnimateCheck().IsChecked() : nullptr, true);
        settings.xdEnabled = ReadCheck(CreativeAppXdCheck() ? CreativeAppXdCheck().IsChecked() : nullptr, true);
        settings.bridgeEnabled = ReadCheck(CreativeAppBridgeCheck() ? CreativeAppBridgeCheck().IsChecked() : nullptr, true);
        settings.characterAnimatorEnabled = ReadCheck(CreativeAppCharacterAnimatorCheck() ? CreativeAppCharacterAnimatorCheck().IsChecked() : nullptr, true);
        settings.frescoEnabled = ReadCheck(CreativeAppFrescoCheck() ? CreativeAppFrescoCheck().IsChecked() : nullptr, true);
        settings.dimensionEnabled = ReadCheck(CreativeAppDimensionCheck() ? CreativeAppDimensionCheck().IsChecked() : nullptr, true);
        settings.substanceEnabled = ReadCheck(CreativeAppSubstanceCheck() ? CreativeAppSubstanceCheck().IsChecked() : nullptr, true);
        settings.acrobatEnabled = ReadCheck(CreativeAppAcrobatCheck() ? CreativeAppAcrobatCheck().IsChecked() : nullptr, true);
        settings.otherAdobeEnabled = ReadCheck(CreativeAppOtherAdobeCheck() ? CreativeAppOtherAdobeCheck().IsChecked() : nullptr, true);

        settings.privacyMode = CreativePrivacyModeFromComboIndex(
            CreativePrivacyCombo() ? CreativePrivacyCombo().SelectedIndex() : 0);
        settings.idleBehavior = CreativeIdleBehaviorFromComboIndex(
            CreativeIdleBehaviorCombo() ? CreativeIdleBehaviorCombo().SelectedIndex() : 0);

        return settings;
    }

    void CreativePageControl::SetSettingsChangedCallback(std::function<void(lrp::ui::CreativePageSettings const&)> callback)
    {
        m_settingsChangedCallback = std::move(callback);
    }

    void CreativePageControl::SetSelectAllCallback(std::function<void()> callback)
    {
        m_selectAllCallback = std::move(callback);
    }

    void CreativePageControl::SetDeselectAllCallback(std::function<void()> callback)
    {
        m_deselectAllCallback = std::move(callback);
    }

    void CreativePageControl::SetDetectedAppIcon(winrt::Microsoft::UI::Xaml::Media::ImageSource const& source)
    {
        if (!source)
        {
            ClearDetectedAppIcon();
            return;
        }

        if (!CreativeDetectedAppIcon() || !CreativeDetectedAppIconFallback())
            return;

        CreativeDetectedAppIcon().Source(source);
        CreativeDetectedAppIcon().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
        CreativeDetectedAppIconFallback().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
    }

    void CreativePageControl::ClearDetectedAppIcon()
    {
        if (!CreativeDetectedAppIcon() || !CreativeDetectedAppIconFallback())
            return;

        CreativeDetectedAppIcon().Source(nullptr);
        CreativeDetectedAppIcon().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        CreativeDetectedAppIconFallback().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
    }

    void CreativePageControl::ResetScrollPosition()
    {
        if (CreativePage())
        {
            CreativePage().ChangeView(
                nullptr,
                box_value(0.0).as<winrt::Windows::Foundation::IReference<double>>(),
                nullptr,
                true);
        }
    }

    void CreativePageControl::OnSettingsToggleChanged(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        NotifySettingsChanged();
    }

    void CreativePageControl::OnSettingsSelectionChanged(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {
        NotifySettingsChanged();
    }

    void CreativePageControl::OnAppFilterCheckChanged(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        NotifySettingsChanged();
    }

    void CreativePageControl::OnSelectAllClicked(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        SetAllAppFilterChecks(true);

        if (m_selectAllCallback)
            m_selectAllCallback();
    }

    void CreativePageControl::OnDeselectAllClicked(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        SetAllAppFilterChecks(false);

        if (m_deselectAllCallback)
            m_deselectAllCallback();
    }

    void CreativePageControl::SetAllAppFilterChecks(bool enabled)
    {
        ScopedSettingsChangedBlock callbackBlock(m_suppressSettingsChanged);

        SetCheck(CreativeAppPhotoshopCheck(), enabled);
        SetCheck(CreativeAppIllustratorCheck(), enabled);
        SetCheck(CreativeAppPremiereCheck(), enabled);
        SetCheck(CreativeAppAfterEffectsCheck(), enabled);
        SetCheck(CreativeAppInDesignCheck(), enabled);
        SetCheck(CreativeAppAuditionCheck(), enabled);
        SetCheck(CreativeAppMediaEncoderCheck(), enabled);
        SetCheck(CreativeAppLightroomCheck(), enabled);
        SetCheck(CreativeAppLightroomClassicCheck(), enabled);
        SetCheck(CreativeAppInCopyCheck(), enabled);
        SetCheck(CreativeAppDreamweaverCheck(), enabled);
        SetCheck(CreativeAppAnimateCheck(), enabled);
        SetCheck(CreativeAppXdCheck(), enabled);
        SetCheck(CreativeAppBridgeCheck(), enabled);
        SetCheck(CreativeAppCharacterAnimatorCheck(), enabled);
        SetCheck(CreativeAppFrescoCheck(), enabled);
        SetCheck(CreativeAppDimensionCheck(), enabled);
        SetCheck(CreativeAppSubstanceCheck(), enabled);
        SetCheck(CreativeAppAcrobatCheck(), enabled);
        SetCheck(CreativeAppOtherAdobeCheck(), enabled);
    }

    void CreativePageControl::NotifySettingsChanged()
    {
        if (m_suppressSettingsChanged)
            return;

        if (m_settingsChangedCallback)
            m_settingsChangedCallback(ReadSettings());
    }
}
