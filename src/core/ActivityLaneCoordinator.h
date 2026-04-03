#pragma once

#include <string>

namespace lrp
{
    enum class ActivityLaneAction
    {
        None = 0,
        Set,
        Update,
        Hold,
        Clear,
        SuppressBySettings,
        SuppressByPriority
    };

    enum class ActivityLaneReason
    {
        None = 0,
        ActiveMatch,
        HeldActivity,
        AppClosed,
        DetectorIdle,
        FilteredApp,
        GlobalRpcDisabled,
        LaneDisabled,
        PrivateModeHidden,
        PrioritySuppressed
    };

    struct ActivityLaneState
    {
        ActivityLaneAction lastAction{ ActivityLaneAction::None };
        ActivityLaneReason lastReason{ ActivityLaneReason::None };
        std::wstring lastSignature;
        std::wstring lastDecisionKey;
    };

    struct ActivityLaneTransition
    {
        ActivityLaneAction action{ ActivityLaneAction::None };
        ActivityLaneReason reason{ ActivityLaneReason::None };
        std::wstring signature;
        std::wstring decisionKey;
        bool shouldEnsureRunning{ false };
        bool duplicate{ false };
    };

    inline bool IsPublishAction(ActivityLaneAction action) noexcept
    {
        return
            action == ActivityLaneAction::Set ||
            action == ActivityLaneAction::Update ||
            action == ActivityLaneAction::Hold;
    }

    inline std::wstring ToSettingString(ActivityLaneAction action)
    {
        switch (action)
        {
        case ActivityLaneAction::Set: return L"set";
        case ActivityLaneAction::Update: return L"update";
        case ActivityLaneAction::Hold: return L"hold";
        case ActivityLaneAction::Clear: return L"clear";
        case ActivityLaneAction::SuppressBySettings: return L"suppress-by-settings";
        case ActivityLaneAction::SuppressByPriority: return L"suppress-by-priority";
        default: return L"none";
        }
    }

    inline std::wstring ToSettingString(ActivityLaneReason reason)
    {
        switch (reason)
        {
        case ActivityLaneReason::ActiveMatch: return L"active_match";
        case ActivityLaneReason::HeldActivity: return L"held_activity";
        case ActivityLaneReason::AppClosed: return L"app_closed";
        case ActivityLaneReason::DetectorIdle: return L"detector_idle";
        case ActivityLaneReason::FilteredApp: return L"filtered_app";
        case ActivityLaneReason::GlobalRpcDisabled: return L"global_rpc_disabled";
        case ActivityLaneReason::LaneDisabled: return L"lane_disabled";
        case ActivityLaneReason::PrivateModeHidden: return L"private_mode_hidden";
        case ActivityLaneReason::PrioritySuppressed: return L"priority_suppressed";
        default: return L"none";
        }
    }

    inline void ResetActivityLaneState(ActivityLaneState& state)
    {
        state = {};
    }

    inline ActivityLaneTransition ResolveActivityLaneTransition(
        const ActivityLaneState& state,
        bool globalEnabled,
        bool laneEnabled,
        bool hasCandidate,
        bool heldActivity,
        bool filteredBySettings,
        bool hiddenByPrivateMode,
        bool suppressedByPriority,
        std::wstring signature,
        ActivityLaneReason idleReason)
    {
        ActivityLaneTransition transition{};
        transition.shouldEnsureRunning = globalEnabled && laneEnabled;

        if (!globalEnabled)
        {
            transition.action = ActivityLaneAction::SuppressBySettings;
            transition.reason = ActivityLaneReason::GlobalRpcDisabled;
        }
        else if (!laneEnabled)
        {
            transition.action = ActivityLaneAction::SuppressBySettings;
            transition.reason = ActivityLaneReason::LaneDisabled;
        }
        else if (hiddenByPrivateMode)
        {
            transition.action = ActivityLaneAction::SuppressBySettings;
            transition.reason = ActivityLaneReason::PrivateModeHidden;
        }
        else if (filteredBySettings)
        {
            transition.action = ActivityLaneAction::SuppressBySettings;
            transition.reason = ActivityLaneReason::FilteredApp;
        }
        else if (suppressedByPriority)
        {
            transition.action = ActivityLaneAction::SuppressByPriority;
            transition.reason = ActivityLaneReason::PrioritySuppressed;
        }
        else if (!hasCandidate)
        {
            transition.action = ActivityLaneAction::Clear;
            transition.reason = idleReason;
        }
        else if (heldActivity)
        {
            transition.action = ActivityLaneAction::Hold;
            transition.reason = ActivityLaneReason::HeldActivity;
        }
        else
        {
            transition.action = IsPublishAction(state.lastAction)
                ? ActivityLaneAction::Update
                : ActivityLaneAction::Set;
            transition.reason = ActivityLaneReason::ActiveMatch;
        }

        if (IsPublishAction(transition.action))
            transition.signature = std::move(signature);

        transition.decisionKey = ToSettingString(transition.action) + L"|" + ToSettingString(transition.reason);
        if (!transition.signature.empty())
            transition.decisionKey += L"|" + transition.signature;

        if (IsPublishAction(transition.action))
        {
            transition.duplicate =
                IsPublishAction(state.lastAction) &&
                transition.reason == state.lastReason &&
                !transition.signature.empty() &&
                transition.signature == state.lastSignature;
        }
        else
        {
            transition.duplicate = !transition.decisionKey.empty() && transition.decisionKey == state.lastDecisionKey;
        }
        return transition;
    }

    inline void CommitActivityLaneTransition(ActivityLaneState& state, const ActivityLaneTransition& transition)
    {
        state.lastAction = transition.action;
        state.lastReason = transition.reason;
        state.lastSignature = transition.signature;
        state.lastDecisionKey = transition.decisionKey;
    }
}
