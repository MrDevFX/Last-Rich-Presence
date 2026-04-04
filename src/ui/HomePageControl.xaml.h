#pragma once

#include "PageModels.h"
#include "HomePageControl.g.h"

#include <functional>

namespace lrp::ui
{
    struct HomePageState;
}

namespace winrt::Last_Rich_Presence::implementation
{
    struct HomePageControl : HomePageControlT<HomePageControl>
    {
        HomePageControl();
        ~HomePageControl();

        void ApplyState(lrp::ui::HomePageState const& state);
        bool IsRichPresenceEnabled();
        void SetEnableToggledCallback(std::function<void(bool)> callback);

        void SetMiniThumbnail(winrt::Microsoft::UI::Xaml::Media::ImageSource const& source);
        void ClearMiniThumbnail();
        void SetCreativeAppIcon(winrt::Microsoft::UI::Xaml::Media::ImageSource const& source);
        void ClearCreativeAppIcon();
        void SetProductiveAppIcon(winrt::Microsoft::UI::Xaml::Media::ImageSource const& source);
        void ClearProductiveAppIcon();

        void SetLivePulseActive(bool active, bool motionEnabled);
        void SetMiniProgress(double progressPercent, bool motionEnabled, bool isPlaying, bool hasMedia);
        void SetMiniWaveActive(bool active, bool motionEnabled);
        void ShowTrackTransitionSkeleton(bool motionEnabled);
        void HideTrackTransitionSkeleton();
        void PulseSourceCard();
        void PulseMiniPlayer();
        void ResetScrollPosition();

        void OnEnableToggled(winrt::Windows::Foundation::IInspectable const& sender,
                             winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);

    private:
        void UpdateMiniWaveClip(double progressPercent);

        std::function<void(bool)> m_enableToggledCallback;
        winrt::Microsoft::UI::Xaml::DispatcherTimer m_trackSkeletonTimer{ nullptr };
        winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard m_trackSkeletonStoryboard{ nullptr };
        winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard m_livePulseStoryboard{ nullptr };
        winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard m_homeMiniProgressStoryboard{ nullptr };
        winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard m_homeMiniWaveStoryboard{ nullptr };
        bool m_suppressEnableToggled{ false };
    };
}

namespace winrt::Last_Rich_Presence::factory_implementation
{
    struct HomePageControl : HomePageControlT<HomePageControl, implementation::HomePageControl>
    {
    };
}
