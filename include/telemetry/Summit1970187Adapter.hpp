#pragma once

#include "telemetry/GraphBuilder.hpp"
#include "telemetry/Models.hpp"
#include "telemetry/TaskControl.hpp"

#include <cstddef>

namespace telemetry {

struct PrototypeRunOptions {
    std::size_t kmeansClusters = 3;
    std::size_t kmeansIterations = 10;
    std::size_t isolationTrees = 100;
    std::size_t isolationSampleSize = 256;
    double isolationThreshold = 0.75;
    unsigned isolationSeed = 777;
    CancellationToken* cancellation = nullptr;
    ProgressCallback progress;
};

class Summit1970187Adapter {
public:
    static DetectorResult runKMeans(const PreparedData& data, const PrototypeRunOptions& options = {});
    static DetectorResult runIsolationForest(const PreparedData& data, const PrototypeRunOptions& options = {});
    static DetectorResult runHybrid(
        const TelemetryDataset& dataset,
        const PreparedData& data,
        const GraphContext& graph,
        const PrototypeRunOptions& options = {});
};

} // namespace telemetry


