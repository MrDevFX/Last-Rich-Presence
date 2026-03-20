#pragma once

#include "MainWindow.g.h"
#include "AppEnums.h"
#include "DiagnosticsLog.h"
#include "MediaDetector.h"
#include "PresenceManager.h"
#include "CreativeDetector.h"
#include "CreativePresenceManager.h"
#include "ProductiveDetector.h"
#include "ProductivePresenceManager.h"

namespace winrt::Last_Rich_Presence::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow() = default;
        ~MainWindow();

        void InitWindow();
        void HandleRedirectedActivation();

        // XAML event handlers
        void OnEnableToggled(winrt::Windows::Foundation::IInspectable const& sender,
                             winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnNavSelectionChanged(winrt::Microsoft::UI::Xaml::Controls::NavigationView const& sender,
                                   winrt::Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const& args);
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
        void OnDefaultIdleStatusToggled(winrt::Windows::Foundation::IInspectable const& sender,
                                        winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnCloseToTrayToggled(winrt::Windows::Foundation::IInspectable const& sender,
                                  winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnLaunchOnStartupToggled(winrt::Windows::Foundation::IInspectable const& sender,
                                      winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnStartMinimizedToggled(winrt::Windows::Foundation::IInspectable const& sender,
                                     winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnTrayLeftClickToggleToggled(winrt::Windows::Foundation::IInspectable const& sender,
                                          winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnSensitiveKeywordFilterToggled(winrt::Windows::Foundation::IInspectable const& sender,
                                             winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnStrictBrowserPrivacyToggled(winrt::Windows::Foundation::IInspectable const& sender,
                                           winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnSuppressBrowserArtToggled(winrt::Windows::Foundation::IInspectable const& sender,
                                         winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnThemeSelectionChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                     winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& e);
        void OnActivityTypeSelectionChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                            winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& e);
        void OnProductiveToggleChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                       winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnProductiveMetadataToggled(winrt::Windows::Foundation::IInspectable const& sender,
                                         winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnProductiveSelectionChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                          winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& e);
        void OnProductiveAppFilterToggled(winrt::Windows::Foundation::IInspectable const& sender,
                                          winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnProductiveSelectAllAppsClicked(winrt::Windows::Foundation::IInspectable const& sender,
                                              winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnProductiveDeselectAllAppsClicked(winrt::Windows::Foundation::IInspectable const& sender,
                                                winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnCreativeToggleChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                     winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnCreativeSelectionChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                        winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& e);
        void OnCreativeAppFilterToggled(winrt::Windows::Foundation::IInspectable const& sender,
                                        winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnCreativeSelectAllAppsClicked(winrt::Windows::Foundation::IInspectable const& sender,
                                            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnCreativeDeselectAllAppsClicked(winrt::Windows::Foundation::IInspectable const& sender,
                                              winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
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
        void OnApplyBlockedAppSitesClicked(winrt::Windows::Foundation::IInspectable const& sender,
                                           winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);

    private:
        void ApplyGlobalEnableRuntimeState();
        void ShutdownWindow();
        void InitializeSystemTray();
        void CleanupSystemTray();
        void ShowTrayContextMenu();
        void ShowWindowFromTray();
        void HideWindowToTray();
        void ToggleWindowVisibilityFromTray();
        void ApplyLaunchOnStartupState(bool enabled, bool userInitiated);
        void HandleTrayMenuCommand(uint32_t commandId);
        bool TryHandleTrayWindowMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result);
        static LRESULT CALLBACK TrayWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
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
        void SyncActivityTypeOverridesFromControls();
        void ApplyActivityTypeOverrides();
        void SetAllProductiveAppFilterChecks(bool enabled);
        void SetAllCreativeAppFilterChecks(bool enabled);
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
        void UpdateThumbnail(const MediaInfo& info);
        void UpdateProductiveAppIcon(const ProductiveActivityInfo& info);
        void UpdateCreativeAppIcon(const CreativeActivityInfo& info);
        void ShowConnectionInfoBar(bool connected);
        void LoadSettings();
        void SaveSettings();
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
        bool m_hiddenToTray{false};
        bool m_trayIconAdded{false};
        bool m_trayIconOwned{false};
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
        std::chrono::steady_clock::time_point m_lastTrayToggleAt{};
        std::chrono::steady_clock::time_point m_lastCreativeActiveSeenAt{};
        HWND m_windowHandle{ nullptr };
        WNDPROC m_originalWndProc{ nullptr };
        HICON m_trayIconHandle{ nullptr };
        lrp::DiagnosticsLog m_diagnosticLog;
        std::wstring m_lastMergeState;
        std::wstring m_lastUiTrackKey;
        std::wstring m_lastUiSourceKey;
        std::wstring m_activePageTag{L"Home"};
        std::wstring m_queuedPageTag;
        bool m_pageTransitionInProgress{false};
        CreativeActivityInfo m_lastCreativeAcceptedActivity;
        std::chrono::steady_clock::time_point m_lastPresencePushAt{};
        bool m_lastPresencePushPlaying{false};
        bool m_reduceMotionRequested{false};
        uint64_t m_connectionInfoBarVersion{0};
    };
}

namespace winrt::Last_Rich_Presence::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
