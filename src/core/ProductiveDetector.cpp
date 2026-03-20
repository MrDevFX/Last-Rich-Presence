#include "pch.h"
#include "ProductiveDetector.h"
#include "TextUtilities.h"

#include <algorithm>
#include <cwctype>
#include <vector>

namespace
{
    struct ProductiveProcessMatch
    {
        std::wstring key;
        std::wstring name;
    };

    bool Contains(const std::wstring& haystackLower, const wchar_t* needleLower)
    {
        return haystackLower.find(needleLower) != std::wstring::npos;
    }

    std::wstring BaseNameFromPath(const std::wstring& path)
    {
        if (path.empty())
            return {};

        auto slash = path.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
            return path;

        return path.substr(slash + 1);
    }

    bool IsWindowCandidateVisible(HWND hwnd)
    {
        if (!hwnd || !IsWindow(hwnd))
            return false;

        // Accept visible windows and minimized windows so overlays do not break detection.
        return IsWindowVisible(hwnd) || IsIconic(hwnd);
    }

    bool TryGetWindowTitle(HWND hwnd, std::wstring& titleOut)
    {
        titleOut.clear();
        if (!hwnd || !IsWindow(hwnd))
            return false;

        int length = GetWindowTextLengthW(hwnd);
        if (length <= 0)
            return false;

        std::wstring buffer(static_cast<size_t>(length) + 1, L'\0');
        int written = GetWindowTextW(hwnd, buffer.data(), static_cast<int>(buffer.size()));
        if (written <= 0)
            return false;

        buffer.resize(static_cast<size_t>(written));
        titleOut = lrp::TrimCopy(std::move(buffer));
        return !titleOut.empty();
    }

    bool TryGetProcessPathAndExeName(DWORD processId, std::wstring& processPathOut, std::wstring& exeNameOut)
    {
        processPathOut.clear();
        exeNameOut.clear();
        if (processId == 0)
            return false;

        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
        if (!process)
            return false;

        std::wstring path(1024, L'\0');
        DWORD size = static_cast<DWORD>(path.size());
        bool ok = QueryFullProcessImageNameW(process, 0, path.data(), &size) != FALSE;
        CloseHandle(process);

        if (!ok || size == 0)
            return false;

        path.resize(size);
        processPathOut = lrp::TrimCopy(path);
        exeNameOut = lrp::TrimCopy(BaseNameFromPath(path));
        return !processPathOut.empty() && !exeNameOut.empty();
    }

    bool IsIgnoredProductiveProcess(const std::wstring& exeLower)
    {
        return
            exeLower == L"outlook.exe" ||
            exeLower == L"teams.exe" ||
            exeLower == L"ms-teams.exe" ||
            exeLower == L"onedrive.exe" ||
            exeLower == L"lync.exe" ||
            exeLower == L"officeclicktorun.exe" ||
            exeLower == L"msosync.exe" ||
            exeLower == L"msoia.exe";
    }

    bool MatchProductiveProcess(const std::wstring& exeLower, ProductiveProcessMatch& matchOut)
    {
        if (exeLower.empty() || IsIgnoredProductiveProcess(exeLower))
            return false;

        if (exeLower == L"winword.exe")
        {
            matchOut = { L"WORD", L"Microsoft Word" };
            return true;
        }
        if (exeLower == L"excel.exe")
        {
            matchOut = { L"XCEL", L"Microsoft Excel" };
            return true;
        }
        if (exeLower == L"powerpnt.exe")
        {
            matchOut = { L"PPT", L"Microsoft PowerPoint" };
            return true;
        }
        if (exeLower == L"onenote.exe")
        {
            matchOut = { L"ONEN", L"Microsoft OneNote" };
            return true;
        }
        if (exeLower == L"msaccess.exe")
        {
            matchOut = { L"ACCS", L"Microsoft Access" };
            return true;
        }
        if (exeLower == L"mspub.exe")
        {
            matchOut = { L"PUBR", L"Microsoft Publisher" };
            return true;
        }
        if (exeLower == L"visio.exe")
        {
            matchOut = { L"VISI", L"Microsoft Visio" };
            return true;
        }
        if (exeLower == L"winproj.exe")
        {
            matchOut = { L"PROJ", L"Microsoft Project" };
            return true;
        }
        if (exeLower == L"codex.exe" || exeLower == L"codex")
        {
            matchOut = { L"CODX", L"Codex" };
            return true;
        }

        return false;
    }

    bool IsLikelyAppSuffix(std::wstring part, const ProductiveProcessMatch& app)
    {
        part = lrp::ToLowerCopy(lrp::TrimCopy(std::move(part)));
        if (part.empty())
            return false;

        auto appLower = lrp::ToLowerCopy(app.name);

        if (part == appLower || Contains(part, appLower.c_str()))
            return true;

        return
            part == L"word" ||
            part == L"excel" ||
            part == L"powerpoint" ||
            part == L"onenote" ||
            part == L"access" ||
            part == L"publisher" ||
            part == L"visio" ||
            part == L"project" ||
            part == L"codex" ||
            part == L"openai codex" ||
            Contains(part, L"microsoft office");
    }

    std::wstring ExtractProjectHint(const std::wstring& windowTitle, const ProductiveProcessMatch& app)
    {
        return lrp::ExtractProjectHint(windowTitle, [&](const std::wstring& part)
        {
            return IsLikelyAppSuffix(part, app);
        });
    }

    bool TryBuildProductiveInfoForWindow(HWND hwnd, ProductiveActivityInfo& infoOut)
    {
        infoOut = {};

        if (!IsWindowCandidateVisible(hwnd))
            return false;

        DWORD processId = 0;
        GetWindowThreadProcessId(hwnd, &processId);
        if (processId == 0)
            return false;

        std::wstring windowTitle;
        if (!TryGetWindowTitle(hwnd, windowTitle))
            return false;

        auto lowerTitle = lrp::ToLowerCopy(windowTitle);
        if (lowerTitle == L"program manager")
            return false;

        std::wstring processPath;
        std::wstring exeName;
        if (!TryGetProcessPathAndExeName(processId, processPath, exeName))
            return false;

        ProductiveProcessMatch app{};
        auto exeLower = lrp::ToLowerCopy(exeName);
        if (!MatchProductiveProcess(exeLower, app))
            return false;

        infoOut.active = true;
        infoOut.processId = static_cast<uint32_t>(processId);
        infoOut.processName = exeName;
        infoOut.processPath = processPath;
        infoOut.appKey = app.key;
        infoOut.appName = app.name;
        infoOut.windowTitle = windowTitle;
        infoOut.projectHint = ExtractProjectHint(windowTitle, app);
        return true;
    }
}

ProductiveDetector::ProductiveDetector()
{
}

ProductiveDetector::~ProductiveDetector()
{
    Stop();
}

void ProductiveDetector::Start()
{
    if (m_running.exchange(true))
        return;

    m_worker = std::thread(&ProductiveDetector::PollThread, this);
}

void ProductiveDetector::Stop()
{
    if (!m_running.exchange(false))
        return;

    m_sleepCv.notify_all();

    if (m_worker.joinable())
        m_worker.join();
}

void ProductiveDetector::SetCallback(ActivityChangedCallback callback)
{
    std::lock_guard lock(m_callbackMutex);
    m_callback = std::move(callback);
}

void ProductiveDetector::SetDetectionMode(ProductiveDetectionMode mode)
{
    m_detectionMode = static_cast<int>(mode);
}

ProductiveDetectionMode ProductiveDetector::GetDetectionMode() const
{
    auto raw = m_detectionMode.load();
    if (raw < static_cast<int>(ProductiveDetectionMode::ForegroundPreferredVisibleFallback) ||
        raw > static_cast<int>(ProductiveDetectionMode::VisibleWindowOnly))
    {
        return ProductiveDetectionMode::ForegroundPreferredVisibleFallback;
    }

    return static_cast<ProductiveDetectionMode>(raw);
}

ProductiveActivityInfo ProductiveDetector::GetCurrentActivity() const
{
    std::lock_guard lock(m_stateMutex);
    return m_currentActivity;
}

void ProductiveDetector::PollThread()
{
    ProductiveActivityInfo previous{};
    bool hasPrevious = false;

    while (m_running.load())
    {
        ProductiveActivityInfo current = SnapshotProductiveApp();
        {
            std::lock_guard lock(m_stateMutex);
            m_currentActivity = current;
        }

        if (!hasPrevious || current != previous)
        {
            previous = current;
            hasPrevious = true;
            InvokeCallback(current);
        }

        std::unique_lock waitLock(m_sleepMutex);
        m_sleepCv.wait_for(waitLock, std::chrono::milliseconds(750), [this]()
        {
            return !m_running.load();
        });
    }
}

ProductiveActivityInfo ProductiveDetector::SnapshotProductiveApp() const
{
    ProductiveActivityInfo info;
    auto mode = GetDetectionMode();

    if (mode != ProductiveDetectionMode::VisibleWindowOnly &&
        TryBuildProductiveInfoForWindow(GetForegroundWindow(), info))
    {
        return info;
    }

    if (mode == ProductiveDetectionMode::ForegroundOnly)
        return {};

    struct SearchContext
    {
        ProductiveActivityInfo result;
        bool found{ false };
    } ctx;

    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL
    {
        auto* ctxPtr = reinterpret_cast<SearchContext*>(lParam);
        if (!ctxPtr)
            return TRUE;

        ProductiveActivityInfo candidate;
        if (TryBuildProductiveInfoForWindow(hwnd, candidate))
        {
            ctxPtr->result = std::move(candidate);
            ctxPtr->found = true;
            return FALSE; // top-most matching productivity window found
        }

        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    if (ctx.found)
        return ctx.result;

    return {};
}

void ProductiveDetector::InvokeCallback(const ProductiveActivityInfo& info)
{
    ActivityChangedCallback callback;
    {
        std::lock_guard lock(m_callbackMutex);
        callback = m_callback;
    }

    if (callback)
    {
        try { callback(info); } catch (...) {}
    }
}
