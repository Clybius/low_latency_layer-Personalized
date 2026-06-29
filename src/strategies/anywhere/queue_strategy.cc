#include "queue_strategy.hh"

#include "device_context.hh"
#include "queue_context.hh"

namespace low_latency {

AnywhereQueueStrategy::AnywhereQueueStrategy(QueueContext& queue)
    : QueueStrategy(queue) {}

AnywhereQueueStrategy::~AnywhereQueueStrategy() {}

void AnywhereQueueStrategy::notify_submit(
    [[maybe_unused]] const VkSubmitInfo& submit,
    std::shared_ptr<TimestampPool::Handle> handle) {

    const auto lock = std::scoped_lock(this->mutex);
    if (this->submission_span) {
        this->submission_span->update(std::move(handle));
    } else {
        this->submission_span =
            std::make_unique<SubmissionSpan>(std::move(handle));
    }
}

void AnywhereQueueStrategy::notify_submit(
    [[maybe_unused]] const VkSubmitInfo2& submit,
    std::shared_ptr<TimestampPool::Handle> handle) {

    const auto lock = std::scoped_lock(this->mutex);
    if (this->submission_span) {
        this->submission_span->update(std::move(handle));
    } else {
        this->submission_span =
            std::make_unique<SubmissionSpan>(std::move(handle));
    }
}

// Stub — ANYWHERE mode doesn't use the per-queue present hook.
void AnywhereQueueStrategy::notify_present(
    [[maybe_unused]] const VkPresentInfoKHR& present) {}

} // namespace low_latency
