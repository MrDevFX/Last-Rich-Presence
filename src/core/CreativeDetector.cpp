#include "pch.h"
#include "CreativeDetector.h"

#include <algorithm>
#include <cwctype>
#include <vector>

namespace
{
    struct AdobeProcessMatch
    {
        std::wstring key;
        std::wstring name;
    };

    std::wstring ToLowerCopy(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
        return value;
    }

    std::wstring TrimCopy(std::wstring value)
    {
        auto isWs = [](wchar_t ch)
        {
            return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
        };

        while (!value.empty() && isWs(value.front()))
            value.erase(value.begin());
        while (!value.empty() && isWs(value.back()))
            value.pop_back();
        return value;
    }

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

        // Accept visible windows and minimized windows (common for docked/hidden workflows).
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
        titleOut = TrimCopy(std::move(buffer));
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
        processPathOut = TrimCopy(path);
        exeNameOut = TrimCopy(BaseNameFromPath(path));
        return !processPathOut.empty() && !exeNameOut.empty();
    }

    bool IsIgnoredAdobeProcess(const std::wstring& exeLower)
    {
        return
            exeLower == L"creative cloud.exe" ||
            exeLower == L"ccxprocess.exe" ||
            exeLower == L"adobecollabsync.exe" ||
            exeLower == L"adobe desktop service.exe" ||
            exeLower == L"coresync.exe" ||
            exeLower == L"cclibrary.exe" ||
            exeLower == L"adobenotificationclient.exe";
    }

    bool MatchAdobeCreativeProcess(const std::wstring& exeLower, AdobeProcessMatch& matchOut)
    {
        if (exeLower.empty() || IsIgnoredAdobeProcess(exeLower))
            return false;

        if (exeLower == L"photoshop.exe")
        {
            matchOut = { L"PHXS", L"Adobe Photoshop" };
            return true;
        }
        if (exeLower == L"illustrator.exe")
        {
            matchOut = { L"ILST", L"Adobe Illustrator" };
            return true;
        }
        if (exeLower == L"xd.exe" || exeLower == L"adobe xd.exe" || exeLower == L"adobexd.exe")
        {
            matchOut = { L"XD", L"Adobe XD" };
            return true;
        }
        if (exeLower == L"bridge.exe" || exeLower == L"adobe bridge.exe" || exeLower == L"adobebridge.exe")
        {
            matchOut = { L"BRDG", L"Adobe Bridge" };
            return true;
        }
        if (exeLower == L"character animator.exe" || exeLower == L"adobe character animator.exe" || exeLower == L"characteranimator.exe")
        {
            matchOut = { L"CHAN", L"Adobe Character Animator" };
            return true;
        }
        if (exeLower == L"fresco.exe" || exeLower == L"adobe fresco.exe" || exeLower == L"adobefresco.exe")
        {
            matchOut = { L"FRSC", L"Adobe Fresco" };
            return true;
        }
        if (exeLower == L"dimension.exe" || exeLower == L"adobe dimension.exe" || exeLower == L"adobedimension.exe")
        {
            matchOut = { L"DIMN", L"Adobe Dimension" };
            return true;
        }
        if (exeLower == L"substance 3d painter.exe" || exeLower == L"adobe substance 3d painter.exe" || exeLower == L"substance painter.exe")
        {
            matchOut = { L"SBPT", L"Adobe Substance 3D Painter" };
            return true;
        }
        if (exeLower == L"substance 3d designer.exe" || exeLower == L"adobe substance 3d designer.exe" || exeLower == L"substance designer.exe")
        {
            matchOut = { L"SBDG", L"Adobe Substance 3D Designer" };
            return true;
        }
        if (exeLower == L"substance 3d sampler.exe" || exeLower == L"adobe substance 3d sampler.exe" || exeLower == L"substance sampler.exe")
        {
            matchOut = { L"SBSM", L"Adobe Substance 3D Sampler" };
            return true;
        }
        if (exeLower == L"substance 3d stager.exe" || exeLower == L"adobe substance 3d stager.exe" || exeLower == L"substance stager.exe")
        {
            matchOut = { L"SBST", L"Adobe Substance 3D Stager" };
            return true;
        }
        if (exeLower == L"substance 3d modeler.exe" || exeLower == L"adobe substance 3d modeler.exe" || exeLower == L"substance modeler.exe")
        {
            matchOut = { L"SBMD", L"Adobe Substance 3D Modeler" };
            return true;
        }
        if (exeLower == L"afterfx.exe")
        {
            matchOut = { L"AEFT", L"Adobe After Effects" };
            return true;
        }
        if (exeLower == L"indesign.exe")
        {
            matchOut = { L"IDSN", L"Adobe InDesign" };
            return true;
        }
        if (exeLower == L"incopy.exe")
        {
            matchOut = { L"AICY", L"Adobe InCopy" };
            return true;
        }
        if (exeLower == L"audition.exe" || exeLower == L"adobe audition.exe")
        {
            matchOut = { L"AUDT", L"Adobe Audition" };
            return true;
        }
        if (exeLower == L"dreamweaver.exe")
        {
            matchOut = { L"DRWV", L"Adobe Dreamweaver" };
            return true;
        }
        if (exeLower == L"animate.exe" || exeLower == L"adobe animate.exe")
        {
            matchOut = { L"FLPR", L"Adobe Animate" };
            return true;
        }
        if (exeLower == L"lightroom.exe")
        {
            matchOut = { L"LTRM", L"Adobe Lightroom" };
            return true;
        }
        if (exeLower == L"lightroomclassic.exe" || exeLower == L"lightroom classic.exe")
        {
            matchOut = { L"LTRC", L"Adobe Lightroom Classic" };
            return true;
        }
        if (exeLower == L"acrobat.exe" || exeLower == L"adobe acrobat.exe" || exeLower == L"acrord32.exe")
        {
            matchOut = { L"ACRO", L"Adobe Acrobat" };
            return true;
        }
        if (exeLower == L"adobe premiere pro.exe" || exeLower == L"premierepro.exe")
        {
            matchOut = { L"PPRO", L"Adobe Premiere Pro" };
            return true;
        }
        if (exeLower == L"adobe media encoder.exe" || exeLower == L"mediaencoder.exe")
        {
            matchOut = { L"AME", L"Adobe Media Encoder" };
            return true;
        }

        // Some Adobe apps may expose alternate process names, so keep a loose fallback.
        if (Contains(exeLower, L"photoshop"))
        {
            matchOut = { L"PHXS", L"Adobe Photoshop" };
            return true;
        }
        if (Contains(exeLower, L"illustrator"))
        {
            matchOut = { L"ILST", L"Adobe Illustrator" };
            return true;
        }
        if (Contains(exeLower, L"adobe xd") || Contains(exeLower, L"adobexd"))
        {
            matchOut = { L"XD", L"Adobe XD" };
            return true;
        }
        if (Contains(exeLower, L"bridge"))
        {
            matchOut = { L"BRDG", L"Adobe Bridge" };
            return true;
        }
        if (Contains(exeLower, L"character") && Contains(exeLower, L"animator"))
        {
            matchOut = { L"CHAN", L"Adobe Character Animator" };
            return true;
        }
        if (Contains(exeLower, L"fresco"))
        {
            matchOut = { L"FRSC", L"Adobe Fresco" };
            return true;
        }
        if (Contains(exeLower, L"dimension"))
        {
            matchOut = { L"DIMN", L"Adobe Dimension" };
            return true;
        }
        if (Contains(exeLower, L"substance") && Contains(exeLower, L"painter"))
        {
            matchOut = { L"SBPT", L"Adobe Substance 3D Painter" };
            return true;
        }
        if (Contains(exeLower, L"substance") && Contains(exeLower, L"designer"))
        {
            matchOut = { L"SBDG", L"Adobe Substance 3D Designer" };
            return true;
        }
        if (Contains(exeLower, L"substance") && Contains(exeLower, L"sampler"))
        {
            matchOut = { L"SBSM", L"Adobe Substance 3D Sampler" };
            return true;
        }
        if (Contains(exeLower, L"substance") && Contains(exeLower, L"stager"))
        {
            matchOut = { L"SBST", L"Adobe Substance 3D Stager" };
            return true;
        }
        if (Contains(exeLower, L"substance") && Contains(exeLower, L"modeler"))
        {
            matchOut = { L"SBMD", L"Adobe Substance 3D Modeler" };
            return true;
        }
        if (Contains(exeLower, L"afterfx") || (Contains(exeLower, L"after") && Contains(exeLower, L"effects")))
        {
            matchOut = { L"AEFT", L"Adobe After Effects" };
            return true;
        }
        if (Contains(exeLower, L"premiere") && Contains(exeLower, L"pro"))
        {
            matchOut = { L"PPRO", L"Adobe Premiere Pro" };
            return true;
        }
        if (Contains(exeLower, L"acrobat") || Contains(exeLower, L"acrord32"))
        {
            matchOut = { L"ACRO", L"Adobe Acrobat" };
            return true;
        }

        return false;
    }

    bool IsLikelyAppSuffix(std::wstring part, const AdobeProcessMatch& app)
    {
        part = ToLowerCopy(TrimCopy(std::move(part)));
        if (part.empty())
            return false;

        auto appLower = ToLowerCopy(app.name);

        return
            part == appLower ||
            Contains(part, L"adobe") ||
            Contains(part, L"home screen") ||
            Contains(part, L"start workspace") ||
            Contains(part, L"welcome");
    }

    std::wstring JoinParts(const std::vector<std::wstring>& parts, size_t count)
    {
        std::wstring joined;
        for (size_t i = 0; i < count; ++i)
        {
            if (i > 0) joined += L" - ";
            joined += parts[i];
        }
        return joined;
    }

    std::vector<std::wstring> SplitTitle(const std::wstring& title)
    {
        std::vector<std::wstring> parts;
        size_t start = 0;
        while (start <= title.size())
        {
            auto pos = title.find(L" - ", start);
            if (pos == std::wstring::npos)
            {
                parts.push_back(title.substr(start));
                break;
            }

            parts.push_back(title.substr(start, pos - start));
            start = pos + 3;
        }

        for (auto& part : parts)
            part = TrimCopy(std::move(part));

        return parts;
    }

    std::wstring ExtractProjectHint(const std::wstring& windowTitle, const AdobeProcessMatch& app)
    {
        auto title = TrimCopy(windowTitle);
        if (title.empty())
            return {};

        auto parts = SplitTitle(title);
        if (parts.empty())
            return title;

        size_t keepCount = parts.size();
        while (keepCount > 1 && IsLikelyAppSuffix(parts[keepCount - 1], app))
            --keepCount;

        auto result = TrimCopy(JoinParts(parts, keepCount));
        if (result.empty())
            result = title;

        if (IsLikelyAppSuffix(result, app))
            return {};

        return result;
    }

    bool TryBuildCreativeInfoForWindow(HWND hwnd, CreativeActivityInfo& infoOut)
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

        auto lowerTitle = ToLowerCopy(windowTitle);
        if (lowerTitle == L"program manager")
            return false;

        std::wstring processPath;
        std::wstring exeName;
        if (!TryGetProcessPathAndExeName(processId, processPath, exeName))
            return false;

        AdobeProcessMatch app{};
        auto exeLower = ToLowerCopy(exeName);
        if (!MatchAdobeCreativeProcess(exeLower, app))
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

CreativeDetector::CreativeDetector()
{
}

CreativeDetector::~CreativeDetector()
{
    Stop();
}

void CreativeDetector::Start()
{
    if (m_running.exchange(true))
        return;

    m_worker = std::thread(&CreativeDetector::PollThread, this);
}

void CreativeDetector::Stop()
{
    if (!m_running.exchange(false))
        return;

    m_sleepCv.notify_all();

    if (m_worker.joinable())
        m_worker.join();
}

void CreativeDetector::SetCallback(ActivityChangedCallback callback)
{
    std::lock_guard lock(m_callbackMutex);
    m_callback = std::move(callback);
}

void CreativeDetector::SetDetectionMode(CreativeDetectionMode mode)
{
    m_detectionMode = static_cast<int>(mode);
}

CreativeDetectionMode CreativeDetector::GetDetectionMode() const
{
    auto raw = m_detectionMode.load();
    if (raw < static_cast<int>(CreativeDetectionMode::ForegroundPreferredVisibleFallback) ||
        raw > static_cast<int>(CreativeDetectionMode::VisibleWindowOnly))
    {
        return CreativeDetectionMode::ForegroundPreferredVisibleFallback;
    }

    return static_cast<CreativeDetectionMode>(raw);
}

CreativeActivityInfo CreativeDetector::GetCurrentActivity() const
{
    std::lock_guard lock(m_stateMutex);
    return m_currentActivity;
}

void CreativeDetector::PollThread()
{
    CreativeActivityInfo previous{};
    bool hasPrevious = false;

    while (m_running.load())
    {
        CreativeActivityInfo current = SnapshotForegroundCreativeApp();
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

CreativeActivityInfo CreativeDetector::SnapshotForegroundCreativeApp() const
{
    CreativeActivityInfo info;
    auto mode = GetDetectionMode();

    if (mode != CreativeDetectionMode::VisibleWindowOnly &&
        TryBuildCreativeInfoForWindow(GetForegroundWindow(), info))
    {
        return info;
    }

    if (mode == CreativeDetectionMode::ForegroundOnly)
        return {};

    // Fallback: if something else is overlaying Adobe, keep detecting the top-most
    // visible Adobe window so Creative MVP does not flap to "none".
    struct SearchContext
    {
        CreativeActivityInfo result;
        bool found{ false };
    } ctx;

    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL
    {
        auto* ctxPtr = reinterpret_cast<SearchContext*>(lParam);
        if (!ctxPtr)
            return TRUE;

        CreativeActivityInfo candidate;
        if (TryBuildCreativeInfoForWindow(hwnd, candidate))
        {
            ctxPtr->result = std::move(candidate);
            ctxPtr->found = true;
            return FALSE; // top-most matching Adobe window found
        }

        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    if (ctx.found)
        return ctx.result;

    return {};
}

void CreativeDetector::InvokeCallback(const CreativeActivityInfo& info)
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
