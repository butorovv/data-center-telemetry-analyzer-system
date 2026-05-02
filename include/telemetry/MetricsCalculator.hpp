#pragma once

#include "telemetry/Models.hpp"

namespace telemetry {

class MetricsCalculator {
public:
    Metrics calculate(const TelemetryDataset& dataset, const DetectorResult& result) const;
};

} // namespace telemetry
