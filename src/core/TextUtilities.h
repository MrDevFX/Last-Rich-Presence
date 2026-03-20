#pragma once

#include <algorithm>
#include <cwctype>
#include <string>
#include <utility>
#include <vector>

namespace lrp
{
    inline std::wstring ToLowerCopy(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
        return value;
    }

    inline std::wstring TrimCopy(std::wstring value)
    {
        auto isWhitespace = [](wchar_t ch)
        {
            return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
        };

        while (!value.empty() && isWhitespace(value.front()))
            value.erase(value.begin());
        while (!value.empty() && isWhitespace(value.back()))
            value.pop_back();

        return value;
    }

    inline std::wstring NormalizeForMatch(const std::wstring& value)
    {
        std::wstring out;
        out.reserve(value.size());

        for (wchar_t ch : value)
        {
            auto lowered = static_cast<wchar_t>(towlower(ch));
            if (iswalnum(lowered) || lowered == L' ')
                out.push_back(lowered);
        }

        return TrimCopy(std::move(out));
    }

    inline bool TitlesLikelyMatch(const std::wstring& left, const std::wstring& right)
    {
        auto a = NormalizeForMatch(left);
        auto b = NormalizeForMatch(right);
        if (a.empty() || b.empty())
            return false;

        return a == b || a.find(b) != std::wstring::npos || b.find(a) != std::wstring::npos;
    }

    inline std::vector<std::wstring> ParseDelimitedTerms(const std::wstring& raw)
    {
        std::vector<std::wstring> terms;
        std::wstring current;

        for (wchar_t ch : raw)
        {
            if (ch == L',' || ch == L';' || ch == L'\n' || ch == L'\r')
            {
                auto term = TrimCopy(current);
                if (!term.empty())
                    terms.push_back(std::move(term));
                current.clear();
                continue;
            }

            current.push_back(ch);
        }

        auto tail = TrimCopy(std::move(current));
        if (!tail.empty())
            terms.push_back(std::move(tail));

        return terms;
    }

    inline std::vector<std::wstring> SplitTitle(const std::wstring& title)
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

    inline std::wstring JoinTitleParts(const std::vector<std::wstring>& parts, size_t count)
    {
        std::wstring joined;
        for (size_t i = 0; i < count; ++i)
        {
            if (i > 0)
                joined += L" - ";
            joined += parts[i];
        }

        return joined;
    }

    template <typename IsLikelySuffix>
    std::wstring ExtractProjectHint(const std::wstring& windowTitle, IsLikelySuffix&& isLikelySuffix)
    {
        auto title = TrimCopy(windowTitle);
        if (title.empty())
            return {};

        auto parts = SplitTitle(title);
        if (parts.empty())
            return title;

        size_t keepCount = parts.size();
        while (keepCount > 1 && isLikelySuffix(parts[keepCount - 1]))
            --keepCount;

        auto result = TrimCopy(JoinTitleParts(parts, keepCount));
        if (result.empty())
            result = title;

        if (isLikelySuffix(result))
            return {};

        return result;
    }
}
