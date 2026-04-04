#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

#include <winrt/base.h>

namespace lrp::ui
{
    enum class AppPage
    {
        Home,
        Music,
        Productivity,
        Creative,
        Settings
    };

    inline constexpr std::array<AppPage, 5> kAppPageOrder{
        AppPage::Home,
        AppPage::Music,
        AppPage::Creative,
        AppPage::Productivity,
        AppPage::Settings
    };

    inline constexpr std::array<std::wstring_view, kAppPageOrder.size()> kAppPageTags{
        L"Home",
        L"Music",
        L"Creative",
        L"Productivity",
        L"Settings"
    };

    inline constexpr std::optional<int> TryAppPageIndex(AppPage page) noexcept
    {
        for (size_t index = 0; index < kAppPageOrder.size(); ++index)
        {
            if (kAppPageOrder[index] == page)
                return static_cast<int>(index);
        }

        return std::nullopt;
    }

    inline std::wstring_view AppPageTag(AppPage page) noexcept
    {
        auto index = TryAppPageIndex(page);
        WINRT_ASSERT(index.has_value());
        if (!index)
            return {};

        return kAppPageTags[static_cast<size_t>(*index)];
    }

    inline winrt::hstring AppPageTagHString(AppPage page)
    {
        return winrt::hstring(AppPageTag(page));
    }

    inline int AppPageIndex(AppPage page) noexcept
    {
        auto index = TryAppPageIndex(page);
        WINRT_ASSERT(index.has_value());
        return index.value_or(-1);
    }

    inline constexpr std::optional<AppPage> TryAppPageFromTag(std::wstring_view tag) noexcept
    {
        for (size_t index = 0; index < kAppPageTags.size(); ++index)
        {
            if (kAppPageTags[index] == tag)
                return kAppPageOrder[index];
        }

        return std::nullopt;
    }

    inline bool IsForwardAppPageTransition(AppPage from, AppPage to) noexcept
    {
        auto fromIndex = TryAppPageIndex(from);
        auto toIndex = TryAppPageIndex(to);
        WINRT_ASSERT(fromIndex.has_value());
        WINRT_ASSERT(toIndex.has_value());
        if (!fromIndex || !toIndex)
            return false;

        return *toIndex >= *fromIndex;
    }

    inline constexpr std::optional<AppPage> TryAppPageAtIndex(int index) noexcept
    {
        if (index < 0 || index >= static_cast<int>(kAppPageOrder.size()))
            return std::nullopt;

        return kAppPageOrder[static_cast<size_t>(index)];
    }

    inline constexpr bool IsKnownAppPageTag(std::wstring_view tag) noexcept
    {
        return TryAppPageFromTag(tag).has_value();
    }
}
