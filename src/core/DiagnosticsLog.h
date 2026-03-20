#pragma once

#include <ctime>
#include <deque>
#include <string>

namespace lrp
{
    class DiagnosticsLog
    {
    public:
        void Append(const std::wstring& level, const std::wstring& component, const std::wstring& message)
        {
            std::time_t now = std::time(nullptr);
            std::tm localTime{};
            localtime_s(&localTime, &now);

            wchar_t timeBuf[16]{};
            wcsftime(timeBuf, 16, L"%H:%M:%S", &localTime);

            std::wstring line = L"[" + std::wstring(timeBuf) + L"] [" + level + L"] [" + component + L"] " + message;
            m_lines.push_back(std::move(line));

            while (m_lines.size() > kMaxLogLines)
                m_lines.pop_front();
        }

        void Clear()
        {
            m_lines.clear();
        }

        std::wstring JoinLines() const
        {
            std::wstring combined;
            for (const auto& line : m_lines)
            {
                if (!combined.empty())
                    combined += L"\n";
                combined += line;
            }

            return combined;
        }

        const std::deque<std::wstring>& Lines() const noexcept
        {
            return m_lines;
        }

    private:
        static constexpr size_t kMaxLogLines = 180;
        std::deque<std::wstring> m_lines;
    };
}
