#include "pch.h"
#include "MusicPageControl.xaml.h"
#if __has_include("MusicPageControl.g.cpp")
#include "MusicPageControl.g.cpp"
#endif

#include <cmath>
#include <utility>

#include "PageModels.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media;
using namespace Microsoft::UI::Xaml::Media::Animation;
using namespace Windows::Foundation;

namespace
{
    constexpr int kProgressStepMs = 860;
    constexpr int kWaveShiftMs = 980;
    constexpr int kWaveGlowMs = 760;
    constexpr double kProgressRetargetEpsilon = 0.05;

    Duration MotionDuration(int milliseconds)
    {
        return DurationHelper::FromTimeSpan(std::chrono::milliseconds(milliseconds));
    }

    SineEase MotionEaseInOutSine()
    {
        auto ease = SineEase();
        ease.EasingMode(EasingMode::EaseInOut);
        return ease;
    }

    struct ScopedCallbackBlock
    {
        explicit ScopedCallbackBlock(bool& blocked)
            : m_blocked(blocked)
            , m_restore(blocked)
        {
            m_blocked = true;
        }

        ~ScopedCallbackBlock()
        {
            m_blocked = m_restore;
        }

    private:
        bool& m_blocked;
        bool m_restore;
    };
}

namespace winrt::Last_Rich_Presence::implementation
{
    MusicPageControl::MusicPageControl()
    {
        InitializeComponent();

        SongWaveViewport().SizeChanged([this](IInspectable const&, SizeChangedEventArgs const&)
        {
            UpdateSongWaveClip(SongProgressBar().Value());
        });

        SongProgressBar().ValueChanged([this](IInspectable const&, auto const& args)
        {
            UpdateSongWaveClip(args.NewValue());
        });

        UpdateSongWaveClip(0.0);
        SetSongWaveActive(false, false);
    }

    void MusicPageControl::ApplyState(lrp::ui::MusicPageState const& state)
    {
        auto hasMedia = state.hasMedia;
        auto isPlaying = state.isPlaying;
        auto motionEnabled = state.motionEnabled;

        MusicEmptyState().Visibility(hasMedia ? Visibility::Collapsed : Visibility::Visible);
        NowPlayingCard().Visibility(hasMedia ? Visibility::Visible : Visibility::Collapsed);

        if (!hasMedia)
        {
            SongTitle().Text(state.title.empty() ? hstring(L"Nothing playing") : state.title);
            ArtistName().Text(state.artist.empty() ? hstring(L"\x2014") : state.artist);
            AlbumName().Text(state.albumTitle);
            PositionText().Text(state.positionText.empty() ? hstring(L"0:00") : state.positionText);
            DurationText().Text(state.durationText.empty() ? hstring(L"0:00") : state.durationText);
            PlayPauseIcon().Glyph(L"\xE768");
            PlaybackStateText().Text(state.playbackStateText.empty() ? hstring(L"Idle") : state.playbackStateText);
            SourceBadge().Visibility(Visibility::Collapsed);
            SourceText().Text(state.sourceText.empty() ? hstring(L"No source") : state.sourceText);
            SourceDebugText().Visibility(Visibility::Collapsed);
            SourceDebugText().Text(L"");
            SetSongProgress(0.0, motionEnabled, false, false);
            SetSongWaveActive(false, motionEnabled);
            return;
        }

        SongTitle().Text(state.title.empty() ? hstring(L"Nothing playing") : state.title);
        ArtistName().Text(state.artist.empty() ? hstring(L"\x2014") : state.artist);
        AlbumName().Text(state.albumTitle);
        PositionText().Text(state.positionText.empty() ? hstring(L"0:00") : state.positionText);
        DurationText().Text(state.durationText.empty() ? hstring(L"0:00") : state.durationText);
        PlayPauseIcon().Glyph(isPlaying ? L"\xE768" : L"\xE769");
        PlaybackStateText().Text(
            state.playbackStateText.empty() ? (isPlaying ? hstring(L"Playing") : hstring(L"Paused")) : state.playbackStateText);

        if (state.showSourceBadge && !state.sourceText.empty())
        {
            SourceBadge().Visibility(Visibility::Visible);
            SourceText().Text(state.sourceText);
            SourceDebugText().Text(state.sourceDebugText);
            SourceDebugText().Visibility(
                state.showSourceDebug && !state.sourceDebugText.empty() ? Visibility::Visible : Visibility::Collapsed);
        }
        else
        {
            SourceBadge().Visibility(Visibility::Collapsed);
            SourceText().Text(L"No source");
            SourceDebugText().Visibility(Visibility::Collapsed);
            SourceDebugText().Text(L"");
        }

        SetSongProgress(state.progressPercent, motionEnabled, isPlaying, hasMedia);
        SetSongWaveActive(isPlaying && hasMedia, motionEnabled);
    }

    void MusicPageControl::ApplySettings(lrp::ui::MusicPageSettings const& settings)
    {
        ScopedCallbackBlock callbackBlock(m_blockSettingsCallback);

        TimestampToggle().IsOn(settings.showTimestamps);
        SourceToggle().IsOn(settings.showSource);
        SourceDebugToggle().IsOn(settings.sourceDebugMode);
        PausedToggle().IsOn(settings.showPaused);
        AlbumArtToggle().IsOn(settings.showAlbumArt);
    }

    lrp::ui::MusicPageSettings MusicPageControl::ReadSettings()
    {
        lrp::ui::MusicPageSettings settings{};
        settings.showTimestamps = TimestampToggle().IsOn();
        settings.showSource = SourceToggle().IsOn();
        settings.sourceDebugMode = SourceDebugToggle().IsOn();
        settings.showPaused = PausedToggle().IsOn();
        settings.showAlbumArt = AlbumArtToggle().IsOn();
        return settings;
    }

    void MusicPageControl::SetSettingsChangedCallback(std::function<void(lrp::ui::MusicPageSettings const&)> callback)
    {
        m_settingsChangedCallback = std::move(callback);
    }

    void MusicPageControl::SetAlbumThumbnail(ImageSource const& source)
    {
        AlbumThumbnail().Source(source);
    }

    void MusicPageControl::ClearAlbumThumbnail()
    {
        AlbumThumbnail().Source(nullptr);
    }

    void MusicPageControl::SetSongProgress(double progressPercent, bool motionEnabled, bool isPlaying, bool hasMedia)
    {
        if (progressPercent < 0.0)
            progressPercent = 0.0;
        if (progressPercent > 100.0)
            progressPercent = 100.0;

        auto current = SongProgressBar().Value();
        auto delta = std::abs(progressPercent - current);
        auto canInterpolate =
            motionEnabled &&
            isPlaying &&
            hasMedia &&
            progressPercent > 0.0 &&
            delta > 0.02 &&
            delta <= 16.0;

        if (m_songProgressStoryboard &&
            std::abs(progressPercent - m_songProgressTarget) <= kProgressRetargetEpsilon)
        {
            return;
        }

        if (!canInterpolate)
        {
            if (m_songProgressStoryboard)
            {
                try
                {
                    m_songProgressStoryboard.Stop();
                }
                catch (...) {}
                m_songProgressStoryboard = nullptr;
            }

            m_songProgressTarget = progressPercent;
            SongProgressBar().Value(progressPercent);
            UpdateSongWaveClip(progressPercent);
            return;
        }

        if (m_songProgressStoryboard)
        {
            try
            {
                m_songProgressStoryboard.Stop();
            }
            catch (...) {}
            m_songProgressStoryboard = nullptr;
        }

        auto progressAnimation = DoubleAnimation();
        progressAnimation.From(current);
        progressAnimation.To(progressPercent);
        progressAnimation.Duration(MotionDuration(kProgressStepMs));
        progressAnimation.EnableDependentAnimation(true);

        auto storyboard = Storyboard();
        storyboard.Children().Append(progressAnimation);
        Storyboard::SetTarget(progressAnimation, SongProgressBar());
        Storyboard::SetTargetProperty(progressAnimation, L"Value");

        auto weak = get_weak();
        storyboard.Completed([weak](IInspectable const&, IInspectable const&)
        {
            if (auto strong = weak.get())
                strong->m_songProgressStoryboard = nullptr;
        });

        m_songProgressTarget = progressPercent;
        m_songProgressStoryboard = storyboard;
        storyboard.Begin();
    }

    void MusicPageControl::SetSongWaveActive(bool active, bool motionEnabled)
    {
        if (!motionEnabled)
            active = false;

        if (!active)
        {
            if (m_songWaveStoryboard)
            {
                try
                {
                    m_songWaveStoryboard.Stop();
                }
                catch (...) {}
                m_songWaveStoryboard = nullptr;
            }

            SongWaveFillTransform().X(0.0);
            SongWaveFillPath().Opacity(0.95);
            return;
        }

        if (m_songWaveStoryboard)
            return;

        auto shift = DoubleAnimation();
        shift.From(0.0);
        shift.To(-10.0);
        shift.AutoReverse(true);
        shift.Duration(MotionDuration(kWaveShiftMs));
        shift.EasingFunction(MotionEaseInOutSine());
        shift.RepeatBehavior(RepeatBehaviorHelper::Forever());

        auto glow = DoubleAnimation();
        glow.From(0.70);
        glow.To(1.0);
        glow.AutoReverse(true);
        glow.Duration(MotionDuration(kWaveGlowMs));
        glow.EasingFunction(MotionEaseInOutSine());
        glow.RepeatBehavior(RepeatBehaviorHelper::Forever());

        auto storyboard = Storyboard();
        storyboard.Children().Append(shift);
        storyboard.Children().Append(glow);
        Storyboard::SetTarget(shift, SongWaveFillTransform());
        Storyboard::SetTargetProperty(shift, L"X");
        Storyboard::SetTarget(glow, SongWaveFillPath());
        Storyboard::SetTargetProperty(glow, L"Opacity");

        m_songWaveStoryboard = storyboard;
        storyboard.Begin();
    }

    void MusicPageControl::ResetScrollPosition()
    {
        MusicPageRoot().ChangeView(
            nullptr,
            box_value(0.0).as<Windows::Foundation::IReference<double>>(),
            nullptr,
            true);
    }

    void MusicPageControl::OnTimestampToggled(IInspectable const&, RoutedEventArgs const&)
    {
        RaiseSettingsChanged();
    }

    void MusicPageControl::OnSourceToggled(IInspectable const&, RoutedEventArgs const&)
    {
        RaiseSettingsChanged();
    }

    void MusicPageControl::OnSourceDebugToggled(IInspectable const&, RoutedEventArgs const&)
    {
        RaiseSettingsChanged();
    }

    void MusicPageControl::OnPausedToggled(IInspectable const&, RoutedEventArgs const&)
    {
        RaiseSettingsChanged();
    }

    void MusicPageControl::OnAlbumArtToggled(IInspectable const&, RoutedEventArgs const&)
    {
        RaiseSettingsChanged();
    }

    void MusicPageControl::UpdateSongWaveClip(double progressPercent)
    {
        auto width = SongWaveViewport().ActualWidth();
        auto height = SongWaveViewport().ActualHeight();

        if (width <= 0.0)
        {
            SongWaveFillClip().Rect(Windows::Foundation::Rect{ 0.0f, 0.0f, 0.0f, static_cast<float>(height > 0.0 ? height : 12.0) });
            return;
        }

        if (height <= 0.0)
            height = 12.0;

        auto clamped = progressPercent;
        if (clamped < 0.0)
            clamped = 0.0;
        if (clamped > 100.0)
            clamped = 100.0;

        auto fillWidth = width * (clamped / 100.0);
        if (fillWidth < 0.0)
            fillWidth = 0.0;
        if (fillWidth > width)
            fillWidth = width;

        SongWaveFillClip().Rect(Windows::Foundation::Rect{
            0.0f,
            0.0f,
            static_cast<float>(fillWidth),
            static_cast<float>(height)
            });
    }

    void MusicPageControl::RaiseSettingsChanged()
    {
        if (m_blockSettingsCallback)
            return;

        if (m_settingsChangedCallback)
        {
            try
            {
                m_settingsChangedCallback(ReadSettings());
            }
            catch (...) {}
        }
    }
}
