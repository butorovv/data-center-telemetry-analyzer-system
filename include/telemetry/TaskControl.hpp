#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>

namespace telemetry {

struct OperationProgress {
    int percent = 0;
    std::size_t current = 0;
    std::size_t total = 0;
    std::string stage;
};

class CancellationToken {
public:
    void cancel() { cancelled_.store(true); }
    void reset() { cancelled_.store(false); }
    bool isCancelled() const { return cancelled_.load(); }

private:
    std::atomic<bool> cancelled_{false};
};

using ProgressCallback = std::function<void(const OperationProgress&)>;

inline void reportProgress(
    const ProgressCallback& callback,
    std::size_t current,
    std::size_t total,
    const std::string& stage)
{
    if (!callback) {
        return;
    }
    OperationProgress progress;
    progress.current = current;
    progress.total = total;
    progress.stage = stage;
    progress.percent = total == 0 ? 0 : static_cast<int>((100 * current) / total);
    if (progress.percent > 100) {
        progress.percent = 100;
    }
    callback(progress);
}

} // namespace telemetry
