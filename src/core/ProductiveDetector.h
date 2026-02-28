#pragma once
#include "pch.h"

#include <condition_variable>
#include <cstdint>

struct ProductiveActivityInfo
{
    bool active{ false };
    uint32_t processId{ 0 };
    std::wstring processName;
    std::wstring processPath;
    std::wstring appKey;
    std::wstring appName;
    std::wstring windowTitle;
    std::wstring projectHint;
};

enum class ProductiveDetectionMode
{
    ForegroundPreferredVisibleFallback = 0,
    ForegroundOnly = 1,
    VisibleWindowOnly = 2
};

inline bool operator==(const ProductiveActivityInfo& left, const ProductiveActivityInfo& right)
{
    return
        left.active == right.active &&
        left.processId == right.processId &&
        left.processName == right.processName &&
        left.processPath == right.processPath &&
        left.appKey == right.appKey &&
        left.appName == right.appName &&
        left.windowTitle == right.windowTitle &&
        left.projectHint == right.projectHint;
}

inline bool operator!=(const ProductiveActivityInfo& left, const ProductiveActivityInfo& right)
{
    return !(left == right);
}

class ProductiveDetector
{
public:
    using ActivityChangedCallback = std::function<void(const ProductiveActivityInfo&)>;

    ProductiveDetector();
    ~ProductiveDetector();

    void Start();
    void Stop();
    void SetCallback(ActivityChangedCallback callback);
    void SetDetectionMode(ProductiveDetectionMode mode);
    ProductiveDetectionMode GetDetectionMode() const;
    ProductiveActivityInfo GetCurrentActivity() const;

private:
    void PollThread();
    ProductiveActivityInfo SnapshotProductiveApp() const;
    void InvokeCallback(const ProductiveActivityInfo& info);

    mutable std::mutex m_stateMutex;
    ProductiveActivityInfo m_currentActivity;

    mutable std::mutex m_callbackMutex;
    ActivityChangedCallback m_callback;

    std::thread m_worker;
    std::mutex m_sleepMutex;
    std::condition_variable m_sleepCv;
    std::atomic<bool> m_running{ false };
    std::atomic<int> m_detectionMode{
        static_cast<int>(ProductiveDetectionMode::ForegroundPreferredVisibleFallback)
    };
};
