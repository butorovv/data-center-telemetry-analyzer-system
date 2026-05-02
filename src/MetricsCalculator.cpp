#include "telemetry/MetricsCalculator.hpp"

namespace telemetry {

Metrics MetricsCalculator::calculate(const TelemetryDataset& dataset, const DetectorResult& result) const
{
    Metrics metrics;
    const std::size_t rows = std::min(dataset.rows.size(), result.labels.size());
    for (std::size_t i = 0; i < rows; ++i) {
        const bool expected = dataset.rows[i].syntheticAnomaly;
        const bool predicted = result.labels[i] != 0;
        if (expected && predicted) {
            ++metrics.tp;
        } else if (!expected && predicted) {
            ++metrics.fp;
        } else if (expected && !predicted) {
            ++metrics.fn;
        } else {
            ++metrics.tn;
        }
    }

    metrics.precision = (metrics.tp + metrics.fp) > 0
        ? static_cast<double>(metrics.tp) / static_cast<double>(metrics.tp + metrics.fp)
        : 0.0;
    metrics.recall = (metrics.tp + metrics.fn) > 0
        ? static_cast<double>(metrics.tp) / static_cast<double>(metrics.tp + metrics.fn)
        : 0.0;
    metrics.f1 = (metrics.precision + metrics.recall) > 0.0
        ? 2.0 * metrics.precision * metrics.recall / (metrics.precision + metrics.recall)
        : 0.0;
    return metrics;
}

} // namespace telemetry
