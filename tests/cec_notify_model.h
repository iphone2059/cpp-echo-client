#pragma once

#include "cec_types.h"

// WinSock2.h must be included before the Windows networking headers below.
// clang-format off
#include <WinSock2.h>
#include <Windows.h>
// clang-format on

#include <cstdint>

// Deterministic model of the documented worker arming sequence. It drives the production policy
// helpers (cec_notify_should_arm and the notification mark transitions) so the contract itself is
// what gets exercised, and it counts the observations that expose a missed arm.

struct cec_notify_status_case {
    int                    status;
    cec_rio_notify_outcome expected;
    const char*            name;
};

struct cec_notify_model {
    bool          armed{ false };
    std::uint32_t outstanding{ 0 };
    std::uint64_t arms{ 0 };
    std::uint64_t deliveries{ 0 };
    std::uint64_t empty_deliveries{ 0 };
    std::uint64_t timeout_wakeups_while_outstanding{ 0 };
    bool          stopped{ false };
};

inline void cec_notify_model_post(cec_notify_model* model, std::uint32_t count) noexcept {
    model->outstanding += count;
    if (cec_notify_should_arm(model->armed, model->outstanding) && cec_notification_mark_rearmed(&model->armed)) {
        ++model->arms;
    }
}

inline void cec_notify_model_deliver(cec_notify_model* model, std::uint32_t retired) noexcept {
    if (!cec_notification_mark_delivered(&model->armed)) {
        // A delivery observed without a pending arm is the shutdown race the teardown contract
        // tolerates: it must not rearm and it is not a consumed notification.
        ++model->empty_deliveries;
        return;
    }
    ++model->deliveries;
    model->outstanding -= retired <= model->outstanding ? retired : model->outstanding;
    if (!model->stopped && cec_notify_should_arm(model->armed, model->outstanding) &&
        cec_notification_mark_rearmed(&model->armed)) {
        ++model->arms;
    }
}

inline void cec_notify_model_timeout(cec_notify_model* model) noexcept {
    if (model->outstanding != 0 && !model->armed) {
        ++model->timeout_wakeups_while_outstanding;
    }
}
