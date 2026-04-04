#pragma once

#include "MainWindow.g.h"
#include "ActivityLaneCoordinator.h"
#include "AppEnums.h"
#include "DiagnosticsLog.h"
#include "SettingsStore.h"
#include "MediaDetector.h"
#include "PresenceManager.h"
#include "CreativeDetector.h"
#include "CreativePresenceManager.h"
#include "ProductiveDetector.h"
#include "ProductivePresenceManager.h"
#include "AppPage.h"
#include "PageModels.h"
#include "WindowTrayController.h"
#include "HomePageControl.xaml.h"
#include "MusicPageControl.xaml.h"
#include "ProductivityPageControl.xaml.h"
#include "CreativePageControl.xaml.h"
#include "SettingsPageControl.xaml.h"

#include <optional>
#include <vector>

namespace winrt::Last_Rich_Presence::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow() = default;
        ~MainWindow();

        void InitWindow();
        void HandleRedirectedActivation();

        // Shell event handlers
        void OnNavSelectionChanged(winrt::Microsoft::UI::Xaml::Controls::NavigationView const& sender,
                                   winrt::Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const& args);
        void OnClearDiagnosticsClicked(winrt::Windows::Foundation::IInspectable const& sender,
                                       winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnExportDiagnosticsJsonClicked(winrt::Windows::Foundation::IInspectable const& sender,
                                            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnResetSettingsClicked(winrt::Windows::Foundation::IInspectable const& sender,
                                    winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnExportSettingsJsonClicked(winrt::Windows::Foundation::IInspectable const& sender,
                                         winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnImportSettingsJsonClicked(winrt::Windows::Foundation::IInspectable const& sender,
                                         winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);

    private:
        implementation::HomePageControl* HomePageControlImpl();
        implementation::MusicPageControl* MusicPageControlImpl();
        implementation::ProductivityPageControl* ProductivityPageControlImpl();
        implementation::CreativePageControl* CreativePageControlImpl();
        implementation::SettingsPageControl* SettingsPageControlImpl();
        winrt::Microsoft::UI::Xaml::FrameworkElement PageHost(lrp::ui::AppPage page);
        winrt::Microsoft::UI::Xaml::Controls::NavigationViewItem NavItemForPage(lrp::ui::AppPage page);
        void ShowOnlyPage(lrp::ui::AppPage page);
        void ResetPageScrollPosition(lrp::ui::AppPage page);
        void ConfigurePageControlCallbacks();
        void InitializeShellNavigation();
        void HandleHomeEnableToggled(bool enabled);
        void HandleMusicSettingsChanged(lrp::ui::MusicPageSettings const& settings);
        void HandleProductivitySettingsChanged(lrp::ui::ProductivityPageSettings const& settings);
        void HandleProductivitySelectAll();
        void HandleProductivityDeselectAll();
        void HandleCreativeSettingsChanged(lrp::ui::CreativePageSettings const& settings);
        void HandleCreativeSelectAll();
        void HandleCreativeDeselectAll();
        void HandleSettingsPageSettingsChanged(lrp::ui::SettingsPageSettings const& settings);
        void HandleApplyBlockedTerms(winrt::hstring const& blockedTermsRaw);
        void InitializeRuntimeComponents();
        void InitializeWindowChromeAndTray();
        void ApplyInitialSettingsAndVisibility();
        void RegisterRuntimeCallbacks();
        void StartRuntimeServices();
        void SyncMusicSettingsFromControls();
        void SyncShellSettingsFromControls();
        lrp::ui::MusicPageState BuildMusicPageState(const MediaInfo& info, int positionSeconds = -1, int durationSeconds = -1);
        lrp::ui::ProductivityPageState BuildProductivityPageState(const ProductiveActivityInfo& info);
        lrp::ui::CreativePageState BuildCreativePageState(const CreativeActivityInfo& info);
        lrp::ui::HomePageState BuildHomePageState(int positionSeconds = -1, int durationSeconds = -1);
        void ApplyGlobalEnableRuntimeState();
        void ShutdownWindow();
        void InitializeSystemTray();
        void CleanupSystemTray();
        void ShowWindowFromTray();
        void HideWindowToTray();
        void ToggleWindowVisibilityFromTray();
        void ApplyLaunchOnStartupState(bool enabled, bool userInitiated, bool persistSettings = true);
        void HandleTrayMenuCommand(lrp::ui::TrayMenuCommand command);
        void OnMediaChanged(const MediaInfo& info);
        void OnProductiveActivityChanged(const ProductiveActivityInfo& info);
        void OnCreativeActivityChanged(const CreativeActivityInfo& info);
        void UpdateUI(const MediaInfo& info);
        void RefreshMediaPresenceOutput();
        void UpdateProductivePreview(const ProductiveActivityInfo& info);
        void UpdateCreativePreview(const CreativeActivityInfo& info);
        void ApplyProductiveRuntimeState();
        void SyncProductiveRpcOutput();
        void UpdateHomeProductivePreview();
        void UpdateHomeCreativePreview();
        void SyncProductiveSettingsFromControls();
        void SyncCreativeSettingsFromControls();
        void ApplyActivityTypeOverrides();
        void ApplyCreativeDetectorRuntimeState();
        void RefreshCreativePreviewFromCurrentState();
        bool IsProductiveAppEnabled(const ProductiveActivityInfo& info) const;
        bool IsCreativeAppEnabled(const CreativeActivityInfo& info) const;
        bool TryGetEffectiveCreativeActivityForRpc(CreativeActivityInfo& infoOut, bool& heldOut) const;
        void SyncCreativeRpcOutput();
        void StartProgressTimer();
        void StopProgressTimer();
        bool IsMotionEnabled() const;
        void TransitionElementVisibility(winrt::Microsoft::UI::Xaml::FrameworkElement const& element, bool show, double offsetY);
        void SetSongProgress(double progressPercent);
        void UpdateSongWaveClip(double progressPercent);
        void SetSongWaveActive(bool active);
        void SetHomeMiniProgress(double progressPercent);
        void UpdateHomeMiniWaveClip(double progressPercent);
        void SetHomeMiniWaveActive(bool active);
        void UpdateConnectionStatus();
        void SetLivePulseActive(bool active);
        void ShowTrackTransitionSkeleton();
        void HideTrackTransitionSkeleton();
        void ApplyThemeMode();
        void UpdateSourceBadge(const MediaInfo& info);
        std::wstring FormatSourceDebugText(const MediaInfo& info) const;
        void AppendDiagnosticLog(const std::wstring& level, const std::wstring& component, const std::wstring& message);
        void RefreshDiagnosticsPanel();
        std::wstring BuildDiagnosticsSnapshotJson() const;
        std::wstring BuildSettingsSnapshotJson();
        lrp::settings::PersistedSettings BuildPersistedSettingsSnapshot();
        void ApplyPersistedSettingsSnapshot(const lrp::settings::PersistedSettings& settings);
        void AppendSettingsIssues(const std::wstring& operation, const std::vector<lrp::settings::SettingsIssue>& issues);
        void UpdateThumbnail(const MediaInfo& info);
        void UpdateProductiveAppIcon(const ProductiveActivityInfo& info);
        void UpdateCreativeAppIcon(const CreativeActivityInfo& info);
        void ShowConnectionInfoBar(bool connected);
        void LoadSettings();
        lrp::settings::SettingsSaveResult SaveSettings();
        static std::wstring FormatTime(int totalSeconds);

        std::shared_ptr<MediaDetector> m_mediaDetector;
        std::shared_ptr<PresenceManager> m_presence;
        std::shared_ptr<ProductiveDetector> m_productiveDetector;
        std::shared_ptr<ProductivePresenceManager> m_productivePresence;
        std::shared_ptr<CreativeDetector> m_creativeDetector;
        std::shared_ptr<CreativePresenceManager> m_creativePresence;
        std::shared_ptr<std::atomic<bool>> m_lifetimeToken;
        MediaInfo m_lastMedia;
        ProductiveActivityInfo m_lastProductiveActivity;
        CreativeActivityInfo m_lastCreativeActivity;
        Microsoft::UI::Xaml::DispatcherTimer m_progressTimer{nullptr};
        Microsoft::UI::Xaml::DispatcherTimer m_connectionInfoBarTimer{nullptr};
        Microsoft::UI::Xaml::DispatcherTimer m_trackSkeletonTimer{nullptr};
        Microsoft::UI::Xaml::Media::Animation::Storyboard m_connectionInfoBarFadeStoryboard{nullptr};
        Microsoft::UI::Xaml::Media::Animation::Storyboard m_pageTransitionStoryboard{nullptr};
        Microsoft::UI::Xaml::Media::Animation::Storyboard m_songProgressStoryboard{nullptr};
        Microsoft::UI::Xaml::Media::Animation::Storyboard m_homeMiniProgressStoryboard{nullptr};
        Microsoft::UI::Xaml::Media::Animation::Storyboard m_songWaveStoryboard{nullptr};
        Microsoft::UI::Xaml::Media::Animation::Storyboard m_homeMiniWaveStoryboard{nullptr};
        Microsoft::UI::Xaml::Media::Animation::Storyboard m_livePulseStoryboard{nullptr};
        Microsoft::UI::Xaml::Media::Animation::Storyboard m_trackSkeletonStoryboard{nullptr};
        Microsoft::UI::Dispatching::DispatcherQueue m_dispatcherQueue{nullptr};
        winrt::Windows::Foundation::IAsyncAction m_thumbnailUpdateTask{ nullptr };
        winrt::Windows::Foundation::IAsyncAction m_productiveIconUpdateTask{ nullptr };
        winrt::Windows::Foundation::IAsyncAction m_creativeIconUpdateTask{ nullptr };
        std::wstring m_lastProductiveIconCacheKey;
        uint64_t m_productiveIconRequestId{0};
        std::wstring m_lastCreativeIconCacheKey;
        uint64_t m_creativeIconRequestId{0};
        bool m_enabled{true};
        bool m_wasConnected{false};
        bool m_isInitializing{true};
        bool m_sourceDebugMode{false};
        bool m_isShuttingDown{false};
        bool m_exitRequested{false};
        bool m_closeToTrayOnClose{true};
        bool m_launchOnStartup{false};
        bool m_startMinimizedToTray{false};
        bool m_trayLeftClickToggles{true};
        bool m_sensitiveKeywordFilter{true};
        bool m_strictBrowserPrivacy{false};
        bool m_suppressBrowserAlbumArt{false};
        bool m_showDefaultIdleStatus{true};
        int m_mediaActivityTypeOverride{-1};
        int m_productiveActivityTypeOverride{-1};
        int m_creativeActivityTypeOverride{-1};
        AppThemeMode m_themeMode{AppThemeMode::FollowSystem};
        bool m_productivePresenceRunning{false};
        bool m_productiveEnabled{true};
        ProductiveDetectionMode m_productiveDetectionMode{ProductiveDetectionMode::ForegroundPreferredVisibleFallback};
        bool m_productiveShowProjectName{true};
        bool m_productiveWordEnabled{true};
        bool m_productiveExcelEnabled{true};
        bool m_productivePowerPointEnabled{true};
        bool m_productiveOneNoteEnabled{true};
        bool m_productiveAccessEnabled{true};
        bool m_productivePublisherEnabled{true};
        bool m_productiveVisioEnabled{true};
        bool m_productiveProjectEnabled{true};
        bool m_productiveCodexEnabled{true};
        bool m_creativePresenceRunning{false};
        bool m_creativeEnabled{true};
        CreativePriorityMode m_creativePriority{CreativePriorityMode::Auto};
        CreativeDetectionMode m_creativeDetectionMode{CreativeDetectionMode::ForegroundPreferredVisibleFallback};
        bool m_creativeShowProjectName{true};
        bool m_creativeShowWindowTitle{false};
        bool m_creativePhotoshopEnabled{true};
        bool m_creativeIllustratorEnabled{true};
        bool m_creativePremiereEnabled{true};
        bool m_creativeAfterEffectsEnabled{true};
        bool m_creativeInDesignEnabled{true};
        bool m_creativeAuditionEnabled{true};
        bool m_creativeMediaEncoderEnabled{true};
        bool m_creativeLightroomEnabled{true};
        bool m_creativeLightroomClassicEnabled{true};
        bool m_creativeInCopyEnabled{true};
        bool m_creativeDreamweaverEnabled{true};
        bool m_creativeAnimateEnabled{true};
        bool m_creativeXdEnabled{true};
        bool m_creativeBridgeEnabled{true};
        bool m_creativeCharacterAnimatorEnabled{true};
        bool m_creativeFrescoEnabled{true};
        bool m_creativeDimensionEnabled{true};
        bool m_creativeSubstanceEnabled{true};
        bool m_creativeAcrobatEnabled{true};
        bool m_creativeOtherAdobeEnabled{true};
        CreativePrivacyMode m_creativePrivacyMode{CreativePrivacyMode::Normal};
        CreativeIdleBehavior m_creativeIdleBehavior{CreativeIdleBehavior::HoldLast5Seconds};
        std::wstring m_blockedAppSiteTermsRaw;
        std::chrono::steady_clock::time_point m_lastCreativeActiveSeenAt{};
        lrp::settings::PersistedSettings m_settings{};
        lrp::ui::WindowTrayController m_trayController;
        lrp::DiagnosticsLog m_diagnosticLog;
        std::wstring m_lastMergeState;
        std::wstring m_lastUiTrackKey;
        std::wstring m_lastUiSourceKey;
        lrp::ui::AppPage m_activePage{lrp::ui::AppPage::Home};
        std::optional<lrp::ui::AppPage> m_queuedPage;
        bool m_pageTransitionInProgress{false};
        CreativeActivityInfo m_lastCreativeAcceptedActivity;
        lrp::ActivityLaneState m_productiveLaneState;
        lrp::ActivityLaneState m_creativeLaneState;
        std::chrono::steady_clock::time_point m_lastPresencePushAt{};
        bool m_lastPresencePushPlaying{false};
        bool m_reduceMotionRequested{false};
        uint64_t m_connectionInfoBarVersion{0};
        uint64_t m_launchOnStartupRequestVersion{0};
        bool m_windowInitialized{false};
    };
}

namespace winrt::Last_Rich_Presence::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
