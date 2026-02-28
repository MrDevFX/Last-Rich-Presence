#pragma once
#include "pch.h"

#include <cstdint>
#include <condition_variable>

struct CreativeActivityInfo
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

enum class CreativeDetectionMode
{
    ForegroundPreferredVisibleFallback = 0,
    ForegroundOnly = 1,
    VisibleWindowOnly = 2
};

inline bool operator==(const CreativeActivityInfo& left, const CreativeActivityInfo& right)
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

inline bool operator!=(const CreativeActivityInfo& left, const CreativeActivityInfo& right)
{
    return !(left == right);
}

class CreativeDetector
{
public:
    using ActivityChangedCallback = std::function<void(const CreativeActivityInfo&)>;

    CreativeDetector();
    ~CreativeDetector();

    void Start();
    void Stop();
    void SetCallback(ActivityChangedCallback callback);
    void SetDetectionMode(CreativeDetectionMode mode);
    CreativeDetectionMode GetDetectionMode() const;
    CreativeActivityInfo GetCurrentActivity() const;

private:
    void PollThread();
    CreativeActivityInfo SnapshotForegroundCreativeApp() const;
    void InvokeCallback(const CreativeActivityInfo& info);

    mutable std::mutex m_stateMutex;
    CreativeActivityInfo m_currentActivity;

    mutable std::mutex m_callbackMutex;
    ActivityChangedCallback m_callback;

    std::thread m_worker;
    std::mutex m_sleepMutex;
    std::condition_variable m_sleepCv;
    std::atomic<bool> m_running{ false };
    std::atomic<int> m_detectionMode{
        static_cast<int>(CreativeDetectionMode::ForegroundPreferredVisibleFallback)
    };
};
