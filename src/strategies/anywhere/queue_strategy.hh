#ifndef STRATEGIES_ANYWHERE_QUEUE_STRATEGY_HH_
#define STRATEGIES_ANYWHERE_QUEUE_STRATEGY_HH_

#include "strategies/queue_strategy.hh"

#include "submission_span.hh"
#include <memory>
#include <mutex>

namespace low_latency {

class QueueContext;

// Queue strategy for ANYWHERE mode. Accumulates SubmissionSpan handles
// for every queue submission unconditionally. At present time, the device
// strategy drains all queues' spans. Graphics-only filtering is done at
// the device strategy level.
class AnywhereQueueStrategy final : public QueueStrategy {
  public:
    std::mutex mutex{};
    std::unique_ptr<SubmissionSpan> submission_span{};

  public:
    explicit AnywhereQueueStrategy(QueueContext& queue);
    virtual ~AnywhereQueueStrategy();

  public:
    virtual void
    notify_submit(const VkSubmitInfo& submit,
                  std::shared_ptr<TimestampPool::Handle> handle) override;
    virtual void
    notify_submit(const VkSubmitInfo2& submit,
                  std::shared_ptr<TimestampPool::Handle> handle) override;
    virtual void notify_present(const VkPresentInfoKHR& present) override;
};

} // namespace low_latency

#endif
