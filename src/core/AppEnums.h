#pragma once

enum class CreativePriorityMode
{
    Auto = 0,
    PreferMedia = 1,
    PreferCreative = 2
};

enum class CreativePrivacyMode
{
    Normal = 0,
    AppOnly = 1,
    Private = 2
};

enum class CreativeIdleBehavior
{
    HoldLast5Seconds = 0,
    ClearImmediately = 1
};

enum class AppThemeMode
{
    FollowSystem = 0,
    Light = 1,
    Dark = 2
};

enum class CreativeDetectionMode
{
    ForegroundPreferredVisibleFallback = 0,
    ForegroundOnly = 1,
    VisibleWindowOnly = 2
};

enum class ProductiveDetectionMode
{
    ForegroundPreferredVisibleFallback = 0,
    ForegroundOnly = 1,
    VisibleWindowOnly = 2
};
