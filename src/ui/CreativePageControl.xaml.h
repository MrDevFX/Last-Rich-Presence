#pragma once

#include "PageModels.h"
#include "CreativePageControl.g.h"
#include <functional>

namespace winrt::Last_Rich_Presence::implementation
{
    struct CreativePageControl : CreativePageControlT<CreativePageControl>
    {
        CreativePageControl();

        void ApplyState(lrp::ui::CreativePageState const& state);
        void ApplySettings(lrp::ui::CreativePageSettings const& settings);
        lrp::ui::CreativePageSettings ReadSettings();
        void SetSettingsChangedCallback(std::function<void(lrp::ui::CreativePageSettings const&)> callback);
        void SetSelectAllCallback(std::function<void()> callback);
        void SetDeselectAllCallback(std::function<void()> callback);
        void SetDetectedAppIcon(winrt::Microsoft::UI::Xaml::Media::ImageSource const& source);
        void ClearDetectedAppIcon();
        void ResetScrollPosition();

        void OnSettingsToggleChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                     winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnSettingsSelectionChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                        winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& e);
        void OnAppFilterCheckChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                     winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnSelectAllClicked(winrt::Windows::Foundation::IInspectable const& sender,
                                winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnDeselectAllClicked(winrt::Windows::Foundation::IInspectable const& sender,
                                  winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);

    private:
        void SetAllAppFilterChecks(bool enabled);
        void NotifySettingsChanged();

        bool m_suppressSettingsChanged{false};
        std::function<void(lrp::ui::CreativePageSettings const&)> m_settingsChangedCallback;
        std::function<void()> m_selectAllCallback;
        std::function<void()> m_deselectAllCallback;
    };
}

namespace winrt::Last_Rich_Presence::factory_implementation
{
    struct CreativePageControl : CreativePageControlT<CreativePageControl, implementation::CreativePageControl>
    {
    };
}
