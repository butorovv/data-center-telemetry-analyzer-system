#pragma once

#include "telemetry/Models.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace telemetry {

class Preprocessor {
public:
    std::size_t removeRowsWithNaN(TelemetryDataset& dataset) const;

    std::vector<std::uint64_t> injectSyntheticAnomalies(
        TelemetryDataset& dataset,
        double fraction = 0.005,
        double gpuTemperatureDelta = 15.0,
        double cpuScale = 0.8,
        std::uint32_t seed = 42) const;

    void classifyWorkload(TelemetryDataset& dataset) const;

    PreparedData normalize(const TelemetryDataset& dataset, std::size_t slidingWindow = 1) const;
};

} // namespace telemetry
