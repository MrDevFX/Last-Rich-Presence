#pragma once

#include "MusicPageControl.g.h"

namespace lrp::ui
{
    struct MusicPageState;
    struct MusicPageSettings;
}

namespace winrt::Last_Rich_Presence::implementation
{
    struct MusicPageControl : MusicPageControlT<MusicPageControl>
    {
        MusicPageControl();

        void ApplyState(lrp::ui::MusicPageState const& state);
        void ApplySettings(lrp::ui::MusicPageSettings const& settings);
        lrp::ui::MusicPageSettings ReadSettings();
        void SetSettingsChangedCallback(std::function<void(lrp::ui::MusicPageSettings const&)> callback);
        void SetAlbumThumbnail(winrt::Microsoft::UI::Xaml::Media::ImageSource const& source);
        void ClearAlbumThumbnail();
        void SetSongProgress(double progressPercent, bool motionEnabled, bool isPlaying, bool hasMedia);
        void SetSongWaveActive(bool active, bool motionEnabled);
        void ResetScrollPosition();

        void OnTimestampToggled(winrt::Windows::Foundation::IInspectable const& sender,
                                winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnSourceToggled(winrt::Windows::Foundation::IInspectable const& sender,
                             winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnSourceDebugToggled(winrt::Windows::Foundation::IInspectable const& sender,
                                  winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnPausedToggled(winrt::Windows::Foundation::IInspectable const& sender,
                             winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnAlbumArtToggled(winrt::Windows::Foundation::IInspectable const& sender,
                               winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);

    private:
        void UpdateSongWaveClip(double progressPercent);
        void RaiseSettingsChanged();

        std::function<void(lrp::ui::MusicPageSettings const&)> m_settingsChangedCallback;
        bool m_blockSettingsCallback{ false };
        winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard m_songProgressStoryboard{ nullptr };
        winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard m_songWaveStoryboard{ nullptr };
        double m_songProgressTarget{ 0.0 };
    };
}

namespace winrt::Last_Rich_Presence::factory_implementation
{
    struct MusicPageControl : MusicPageControlT<MusicPageControl, implementation::MusicPageControl>
    {
    };
}
