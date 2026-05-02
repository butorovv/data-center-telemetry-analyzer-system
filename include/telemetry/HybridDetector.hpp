#pragma once

#include "telemetry/GraphBuilder.hpp"
#include "telemetry/Models.hpp"

namespace telemetry {

class HybridDetector {
public:
    DetectorResult verifyWithGraph(
        const TelemetryDataset& dataset,
        const GraphContext& graph,
        const DetectorResult& isolationForestResult) const;
};

} // namespace telemetry
