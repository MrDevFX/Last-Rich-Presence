#include "pch.h"
#include "HomePageControl.xaml.h"
#include "PageModels.h"
#if __has_include("HomePageControl.g.cpp")
#include "HomePageControl.g.cpp"
#endif

#include <cmath>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media;
using namespace Microsoft::UI::Xaml::Media::Animation;
using namespace Windows::Foundation;

namespace
{
    struct MotionTokens
    {
        static constexpr int StandardMs = 220;
        static constexpr int ProgressStepMs = 860;
        static constexpr int SkeletonPulseMs = 700;
        static constexpr int SkeletonDwellMs = 600;
        static constexpr int HomeWaveShiftMs = 900;
        static constexpr int HomeWaveGlowMs = 700;
    };

    Duration MotionDuration(int milliseconds)
    {
        return DurationHelper::FromTimeSpan(std::chrono::milliseconds(milliseconds));
    }

    CubicEase MotionEaseOut()
    {
        auto ease = CubicEase();
        ease.EasingMode(EasingMode::EaseOut);
        return ease;
    }

    SineEase MotionEaseInOutSine()
    {
        auto ease = SineEase();
        ease.EasingMode(EasingMode::EaseInOut);
        return ease;
    }

    Brush LookupBrush(hstring const& key, wchar_t const* fallback)
    {
        auto resources = Application::Current().Resources();
        try
        {
            if (!key.empty())
                return resources.Lookup(box_value(key)).as<Brush>();
        }
        catch (...) {}

        try
        {
            return resources.Lookup(box_value(fallback)).as<Brush>();
        }
        catch (...) {}

        return nullptr;
    }

    hstring CoalesceText(hstring const& value, wchar_t const* fallback)
    {
        return value.empty() ? hstring(fallback) : value;
    }

    void AnimateOpacityPulse(UIElement const& element, double fromOpacity = 0.9, int durationMs = MotionTokens::StandardMs)
    {
        if (!element)
            return;

        auto animation = DoubleAnimation();
        animation.From(fromOpacity);
        animation.To(1.0);
        animation.Duration(MotionDuration(durationMs));
        animation.EasingFunction(MotionEaseOut());

        auto storyboard = Storyboard();
        storyboard.Children().Append(animation);
        Storyboard::SetTarget(animation, element);
        Storyboard::SetTargetProperty(animation, L"Opacity");

        element.Opacity(fromOpacity);
        storyboard.Begin();
    }
}

namespace winrt::Last_Rich_Presence::implementation
{
    HomePageControl::HomePageControl()
    {
        InitializeComponent();

        auto weak = get_weak();
        HomeMiniWaveViewport().SizeChanged([weak](IInspectable const&, SizeChangedEventArgs const&)
        {
            if (auto strong = weak.get())
                strong->UpdateMiniWaveClip(strong->HomeMiniProgressBar().Value());
        });

        HomeMiniProgressBar().ValueChanged([weak](IInspectable const&, auto const& args)
        {
            if (auto strong = weak.get())
                strong->UpdateMiniWaveClip(args.NewValue());
        });

        UpdateMiniWaveClip(0.0);
        SetMiniWaveActive(false, false);
    }

    HomePageControl::~HomePageControl()
    {
        if (m_trackSkeletonTimer)
            m_trackSkeletonTimer.Stop();

        if (m_trackSkeletonStoryboard)
        {
            try { m_trackSkeletonStoryboard.Stop(); } catch (...) {}
            m_trackSkeletonStoryboard = nullptr;
        }

        if (m_livePulseStoryboard)
        {
            try { m_livePulseStoryboard.Stop(); } catch (...) {}
            m_livePulseStoryboard = nullptr;
        }

        if (m_homeMiniProgressStoryboard)
        {
            try { m_homeMiniProgressStoryboard.Stop(); } catch (...) {}
            m_homeMiniProgressStoryboard = nullptr;
        }

        if (m_homeMiniWaveStoryboard)
        {
            try { m_homeMiniWaveStoryboard.Stop(); } catch (...) {}
            m_homeMiniWaveStoryboard = nullptr;
        }
    }

    void HomePageControl::ApplyState(lrp::ui::HomePageState const& state)
    {
        auto setStatusChip = [](auto const& icon,
                                auto const& text,
                                hstring const& glyph,
                                hstring const& label,
                                auto const& brush)
        {
            icon.Glyph(CoalesceText(glyph, L"\xE160"));
            text.Text(label);
            if (brush)
            {
                icon.Foreground(brush);
                text.Foreground(brush);
            }
        };

        m_suppressEnableToggled = true;
        EnableToggle().IsOn(state.richPresenceEnabled);
        m_suppressEnableToggled = false;

        StatusText().Text(CoalesceText(state.statusText, L"Disconnected from Discord"));
        StatusSubtext().Text(CoalesceText(state.statusSubtext, L"Waiting for connection..."));
        HomeHealthText().Text(CoalesceText(state.healthText, L"Discord: -- | Extension: -- | Hint age: --"));
        StatusIndicator().Fill(LookupBrush(state.statusIndicatorBrushKey, L"StatusDisconnectedBrush"));
        SetLivePulseActive(state.showLiveBadge, state.motionEnabled);

        HomeSourceValue().Text(CoalesceText(state.sourceValue, L"No active source"));
        HomeSourceSubtext().Text(CoalesceText(state.sourceSubtext, L"Waiting for active session"));
        HomeDetectedViaText().Text(CoalesceText(state.detectedViaText, L"Detected via --"));
        HomeExtensionText().Text(CoalesceText(state.extensionText, L"Native host offline"));
        HomeExtensionSubtext().Text(CoalesceText(state.extensionSubtext, L"Restart the app to refresh native-host registration"));
        HomeExtensionIndicator().Fill(LookupBrush(state.extensionIndicatorBrushKey, L"StatusDisconnectedBrush"));

        HomeMiniTitle().Text(CoalesceText(state.miniTitle, L"Nothing playing"));
        HomeMiniArtist().Text(CoalesceText(state.miniArtist, L"Start playback or activity"));
        HomeMiniTimer().Text(CoalesceText(state.miniTimerText, L"0:00 / 0:00"));
        auto miniBrush = LookupBrush(state.miniStatusBrushKey, L"TextFillColorSecondaryBrush");
        HomeMiniPlayIcon().Glyph(CoalesceText(state.miniStatusGlyph, L"\xE160"));
        HomeMiniStatusText().Text(CoalesceText(state.miniStatusText, L"Waiting"));
        if (miniBrush)
        {
            HomeMiniPlayIcon().Foreground(miniBrush);
            HomeMiniStatusText().Foreground(miniBrush);
        }
        HomePausedChip().Visibility(state.showPausedChip ? Visibility::Visible : Visibility::Collapsed);

        setStatusChip(
            HomeCreativeStatusIcon(),
            HomeCreativeStatusText(),
            state.creativeStatusGlyph,
            CoalesceText(state.creativeStatusText, L"Waiting"),
            LookupBrush(state.creativeStatusBrushKey, L"TextFillColorSecondaryBrush"));
        HomeCreativeTitle().Text(CoalesceText(state.creativeTitle, L"Awaiting Creative Activity"));
        HomeCreativeSubtitle().Text(CoalesceText(state.creativeSubtitle, L"Launch a supported app to update your status."));
        HomeCreativeWindowText().Text(CoalesceText(state.creativeWindowText, L"None"));
        HomeCreativeProcessText().Text(CoalesceText(state.creativeProcessText, L"None"));
        HomeCreativeSourceValue().Text(CoalesceText(state.creativeSourceValue, L"No active creativity source"));
        HomeCreativeSourceSubtext().Text(CoalesceText(state.creativeSourceSubtext, L"Waiting for supported apps..."));
        HomeCreativeDetectedViaText().Text(CoalesceText(state.creativeDetectedViaText, L"Detected via --"));
        HomeCreativeDetectorText().Text(CoalesceText(state.creativeDetectorText, L"Creativity detector waiting"));
        HomeCreativeDetectorSubtext().Text(CoalesceText(state.creativeDetectorSubtext, L"Waiting for supported apps..."));
        HomeCreativeDetectorIndicator().Fill(LookupBrush(state.creativeDetectorBrushKey, L"StatusConnectingBrush"));

        setStatusChip(
            HomeProductiveStatusIcon(),
            HomeProductiveStatusText(),
            state.productiveStatusGlyph,
            CoalesceText(state.productiveStatusText, L"Waiting"),
            LookupBrush(state.productiveStatusBrushKey, L"TextFillColorSecondaryBrush"));
        HomeProductiveTitle().Text(CoalesceText(state.productiveTitle, L"Awaiting Productive Activity"));
        HomeProductiveSubtitle().Text(CoalesceText(state.productiveSubtitle, L"Launch a supported Office app to update your status."));
        HomeProductiveWindowText().Text(CoalesceText(state.productiveWindowText, L"None"));
        HomeProductiveProcessText().Text(CoalesceText(state.productiveProcessText, L"None"));
        HomeProductiveSourceValue().Text(CoalesceText(state.productiveSourceValue, L"No active productivity source"));
        HomeProductiveSourceSubtext().Text(CoalesceText(state.productiveSourceSubtext, L"Waiting for supported apps..."));
        HomeProductiveDetectedViaText().Text(CoalesceText(state.productiveDetectedViaText, L"Detected via --"));
        HomeProductiveDetectorText().Text(CoalesceText(state.productiveDetectorText, L"Productivity detector waiting"));
        HomeProductiveDetectorSubtext().Text(CoalesceText(state.productiveDetectorSubtext, L"Waiting for supported apps..."));
        HomeProductiveDetectorIndicator().Fill(LookupBrush(state.productiveDetectorBrushKey, L"StatusConnectingBrush"));
    }

    bool HomePageControl::IsRichPresenceEnabled()
    {
        return EnableToggle().IsOn();
    }

    void HomePageControl::SetEnableToggledCallback(std::function<void(bool)> callback)
    {
        m_enableToggledCallback = std::move(callback);
    }

    void HomePageControl::SetMiniThumbnail(ImageSource const& source)
    {
        HomeMiniThumbnail().Source(source);
    }

    void HomePageControl::ClearMiniThumbnail()
    {
        HomeMiniThumbnail().Source(nullptr);
    }

    void HomePageControl::SetCreativeAppIcon(ImageSource const& source)
    {
        if (!source)
        {
            ClearCreativeAppIcon();
            return;
        }

        HomeCreativeAppIcon().Source(source);
        HomeCreativeAppIcon().Visibility(Visibility::Visible);
        HomeCreativeAppIconFallback().Visibility(Visibility::Collapsed);
    }

    void HomePageControl::ClearCreativeAppIcon()
    {
        HomeCreativeAppIcon().Source(nullptr);
        HomeCreativeAppIcon().Visibility(Visibility::Collapsed);
        HomeCreativeAppIconFallback().Visibility(Visibility::Visible);
    }

    void HomePageControl::SetProductiveAppIcon(ImageSource const& source)
    {
        if (!source)
        {
            ClearProductiveAppIcon();
            return;
        }

        HomeProductiveAppIcon().Source(source);
        HomeProductiveAppIcon().Visibility(Visibility::Visible);
        HomeProductiveAppIconFallback().Visibility(Visibility::Collapsed);
    }

    void HomePageControl::ClearProductiveAppIcon()
    {
        HomeProductiveAppIcon().Source(nullptr);
        HomeProductiveAppIcon().Visibility(Visibility::Collapsed);
        HomeProductiveAppIconFallback().Visibility(Visibility::Visible);
    }

    void HomePageControl::SetLivePulseActive(bool active, bool motionEnabled)
    {
        if (!active || !motionEnabled)
        {
            if (m_livePulseStoryboard)
            {
                try { m_livePulseStoryboard.Stop(); } catch (...) {}
                m_livePulseStoryboard = nullptr;
            }

            StatusLiveBadge().Visibility(active ? Visibility::Visible : Visibility::Collapsed);
            StatusLiveDot().Opacity(1.0);
            return;
        }

        StatusLiveBadge().Visibility(Visibility::Visible);

        if (m_livePulseStoryboard)
            return;

        auto pulse = DoubleAnimation();
        pulse.From(1.0);
        pulse.To(0.62);
        pulse.AutoReverse(true);
        pulse.Duration(MotionDuration(MotionTokens::StandardMs * 2));
        pulse.EasingFunction(MotionEaseInOutSine());
        pulse.RepeatBehavior(RepeatBehaviorHelper::Forever());

        auto storyboard = Storyboard();
        storyboard.Children().Append(pulse);
        Storyboard::SetTarget(pulse, StatusLiveDot());
        Storyboard::SetTargetProperty(pulse, L"Opacity");

        m_livePulseStoryboard = storyboard;
        storyboard.Begin();
    }

    void HomePageControl::SetMiniProgress(double progressPercent, bool motionEnabled, bool isPlaying, bool hasMedia)
    {
        if (progressPercent < 0.0)
            progressPercent = 0.0;
        if (progressPercent > 100.0)
            progressPercent = 100.0;

        auto current = HomeMiniProgressBar().Value();
        auto delta = std::abs(progressPercent - current);
        bool canInterpolate =
            motionEnabled &&
            isPlaying &&
            hasMedia &&
            progressPercent > 0.0 &&
            delta > 0.02 &&
            delta <= 16.0;

        if (!canInterpolate)
        {
            if (m_homeMiniProgressStoryboard)
            {
                try { m_homeMiniProgressStoryboard.Stop(); } catch (...) {}
                m_homeMiniProgressStoryboard = nullptr;
            }

            HomeMiniProgressBar().Value(progressPercent);
            UpdateMiniWaveClip(progressPercent);
            return;
        }

        if (m_homeMiniProgressStoryboard)
        {
            try { m_homeMiniProgressStoryboard.Stop(); } catch (...) {}
            m_homeMiniProgressStoryboard = nullptr;
        }

        auto progressAnimation = DoubleAnimation();
        progressAnimation.From(current);
        progressAnimation.To(progressPercent);
        progressAnimation.Duration(MotionDuration(MotionTokens::ProgressStepMs));
        progressAnimation.EnableDependentAnimation(true);

        auto storyboard = Storyboard();
        storyboard.Children().Append(progressAnimation);
        Storyboard::SetTarget(progressAnimation, HomeMiniProgressBar());
        Storyboard::SetTargetProperty(progressAnimation, L"Value");

        auto weak = get_weak();
        storyboard.Completed([weak](IInspectable const&, IInspectable const&)
        {
            if (auto strong = weak.get())
                strong->m_homeMiniProgressStoryboard = nullptr;
        });

        m_homeMiniProgressStoryboard = storyboard;
        storyboard.Begin();
    }

    void HomePageControl::SetMiniWaveActive(bool active, bool motionEnabled)
    {
        if (!motionEnabled)
            active = false;

        if (!active)
        {
            if (m_homeMiniWaveStoryboard)
            {
                try { m_homeMiniWaveStoryboard.Stop(); } catch (...) {}
                m_homeMiniWaveStoryboard = nullptr;
            }

            HomeMiniWaveFillTransform().X(0.0);
            HomeMiniWaveFillPath().Opacity(0.95);
            return;
        }

        if (m_homeMiniWaveStoryboard)
            return;

        auto shift = DoubleAnimation();
        shift.From(0.0);
        shift.To(-8.0);
        shift.AutoReverse(true);
        shift.Duration(MotionDuration(MotionTokens::HomeWaveShiftMs));
        shift.EasingFunction(MotionEaseInOutSine());
        shift.RepeatBehavior(RepeatBehaviorHelper::Forever());

        auto glow = DoubleAnimation();
        glow.From(0.74);
        glow.To(1.0);
        glow.AutoReverse(true);
        glow.Duration(MotionDuration(MotionTokens::HomeWaveGlowMs));
        glow.EasingFunction(MotionEaseInOutSine());
        glow.RepeatBehavior(RepeatBehaviorHelper::Forever());

        auto storyboard = Storyboard();
        storyboard.Children().Append(shift);
        storyboard.Children().Append(glow);
        Storyboard::SetTarget(shift, HomeMiniWaveFillTransform());
        Storyboard::SetTargetProperty(shift, L"X");
        Storyboard::SetTarget(glow, HomeMiniWaveFillPath());
        Storyboard::SetTargetProperty(glow, L"Opacity");

        m_homeMiniWaveStoryboard = storyboard;
        storyboard.Begin();
    }

    void HomePageControl::ShowTrackTransitionSkeleton(bool motionEnabled)
    {
        if (!motionEnabled)
        {
            HideTrackTransitionSkeleton();
            return;
        }

        auto overlay = HomeTrackSkeletonOverlay();
        overlay.Visibility(Visibility::Visible);
        overlay.Opacity(1.0);

        if (!m_trackSkeletonStoryboard)
        {
            auto pulse = DoubleAnimation();
            pulse.From(0.78);
            pulse.To(1.0);
            pulse.AutoReverse(true);
            pulse.Duration(MotionDuration(MotionTokens::SkeletonPulseMs));
            pulse.EasingFunction(MotionEaseInOutSine());
            pulse.RepeatBehavior(RepeatBehaviorHelper::Forever());

            auto storyboard = Storyboard();
            storyboard.Children().Append(pulse);
            Storyboard::SetTarget(pulse, overlay);
            Storyboard::SetTargetProperty(pulse, L"Opacity");

            m_trackSkeletonStoryboard = storyboard;
            storyboard.Begin();
        }

        if (!m_trackSkeletonTimer)
        {
            auto weak = get_weak();
            m_trackSkeletonTimer = DispatcherTimer();
            m_trackSkeletonTimer.Tick([weak](IInspectable const&, IInspectable const&)
            {
                if (auto strong = weak.get())
                {
                    if (strong->m_trackSkeletonTimer)
                        strong->m_trackSkeletonTimer.Stop();
                    strong->HideTrackTransitionSkeleton();
                }
            });
        }

        m_trackSkeletonTimer.Interval(std::chrono::milliseconds(MotionTokens::SkeletonDwellMs));
        m_trackSkeletonTimer.Start();
    }

    void HomePageControl::HideTrackTransitionSkeleton()
    {
        if (m_trackSkeletonTimer)
            m_trackSkeletonTimer.Stop();

        if (m_trackSkeletonStoryboard)
        {
            try { m_trackSkeletonStoryboard.Stop(); } catch (...) {}
            m_trackSkeletonStoryboard = nullptr;
        }

        auto overlay = HomeTrackSkeletonOverlay();
        overlay.Visibility(Visibility::Collapsed);
        overlay.Opacity(1.0);
    }

    void HomePageControl::PulseSourceCard()
    {
        AnimateOpacityPulse(HomeSourceCard());
    }

    void HomePageControl::PulseMiniPlayer()
    {
        AnimateOpacityPulse(HomeMiniPlayer());
    }

    void HomePageControl::ResetScrollPosition()
    {
        HomePage().ChangeView(
            nullptr,
            box_value(0.0).as<Windows::Foundation::IReference<double>>(),
            nullptr,
            true);
    }

    void HomePageControl::OnEnableToggled(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_suppressEnableToggled)
            return;

        if (m_enableToggledCallback)
            m_enableToggledCallback(EnableToggle().IsOn());
    }

    void HomePageControl::UpdateMiniWaveClip(double progressPercent)
    {
        auto width = HomeMiniWaveViewport().ActualWidth();
        auto height = HomeMiniWaveViewport().ActualHeight();

        if (width <= 0.0)
        {
            HomeMiniWaveFillClip().Rect(Rect{ 0.0f, 0.0f, 0.0f, static_cast<float>(height > 0.0 ? height : 8.0) });
            return;
        }

        if (height <= 0.0)
            height = 8.0;

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

        HomeMiniWaveFillClip().Rect(Rect{
            0.0f,
            0.0f,
            static_cast<float>(fillWidth),
            static_cast<float>(height)
            });
    }
}
