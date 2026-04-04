#pragma once

#include "PageModels.h"
#include "ProductivityPageControl.g.h"

#include <functional>
#include <string>

namespace winrt::Last_Rich_Presence::implementation
{
    struct ProductivityPageControl : ProductivityPageControlT<ProductivityPageControl>
    {
        ProductivityPageControl();

        void ApplyState(const lrp::ui::ProductivityPageState& state);
        void ApplySettings(const lrp::ui::ProductivityPageSettings& settings);
        lrp::ui::ProductivityPageSettings ReadSettings();

        void SetSettingsChangedCallback(std::function<void(const lrp::ui::ProductivityPageSettings&)> callback);
        void SetSelectAllCallback(std::function<void()> callback);
        void SetDeselectAllCallback(std::function<void()> callback);

        void SetDetectedAppIcon(winrt::Microsoft::UI::Xaml::Media::ImageSource const& source);
        void ClearDetectedAppIcon();
        void ResetScrollPosition();

        void OnSettingsToggleChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                     winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnDetectionModeSelectionChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                             winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& e);
        void OnAppFilterToggled(winrt::Windows::Foundation::IInspectable const& sender,
                                winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnSelectAllAppsClicked(winrt::Windows::Foundation::IInspectable const& sender,
                                    winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnDeselectAllAppsClicked(winrt::Windows::Foundation::IInspectable const& sender,
                                      winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);

    private:
        static bool IsCheckedOrDefault(winrt::Windows::Foundation::IReference<bool> const& value, bool defaultValue) noexcept;
        void SetAllAppFilterChecks(bool enabled);
        void ApplySettingsControlsEnabled(bool enabled);
        void NotifySettingsChanged();

        std::function<void(const lrp::ui::ProductivityPageSettings&)> m_settingsChangedCallback;
        std::function<void()> m_selectAllCallback;
        std::function<void()> m_deselectAllCallback;
        bool m_suppressSettingsChanged{ false };
    };
}

namespace winrt::Last_Rich_Presence::factory_implementation
{
    struct ProductivityPageControl : ProductivityPageControlT<ProductivityPageControl, implementation::ProductivityPageControl>
    {
    };
}
