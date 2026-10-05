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
    // Deliveries observed with no pending arm. Production fails fast on these, so the model records
    // them as violations instead of tolerating them as an empty delivery.
    std::uint64_t violations{ 0 };
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
        ++model->violations;
        return;
    }
    ++model->deliveries;
    model->outstanding -= retired <= model->outstanding ? retired : model->outstanding;
    if (!model->stopped && cec_notify_should_arm(model->armed, model->outstanding) &&
        cec_notification_mark_rearmed(&model->armed)) {
        ++model->arms;
    }
}

// Work retired by RIODequeueCompletion alone. The notification that woke the worker may still be
// pending in the port queue, so this drain path does not consume a notification.
inline void cec_notify_model_retire(cec_notify_model* model, std::uint32_t retired) noexcept {
    model->outstanding -= retired <= model->outstanding ? retired : model->outstanding;
}

inline void cec_notify_model_timeout(cec_notify_model* model) noexcept {
    if (model->outstanding != 0 && !model->armed) {
        ++model->timeout_wakeups_while_outstanding;
    }
}

// Teardown contract: the completion queue may be closed once no RIO operation is outstanding and the
// arm state was never violated. A notification that is still in flight (arms - deliveries == 1) is
// allowed to remain unconsumed; production never waits for it and never forges it.
inline bool cec_notify_model_may_close(const cec_notify_model* model) noexcept {
    return model->outstanding == 0 && model->violations == 0 && model->arms >= model->deliveries &&
           model->arms - model->deliveries <= 1U;
}
