#pragma once

#include "telemetry/Models.hpp"
#include "telemetry/TaskControl.hpp"

#include <cstddef>

namespace telemetry {

struct KMeansConfig {
    std::size_t clusters = 3;
    std::size_t maxIterations = 60;
    double thresholdSigma = 2.5;
    std::size_t threads = 0;
    unsigned seed = 1337;
    CancellationToken* cancellation = nullptr;
    ProgressCallback progress;
};

class KMeansDetector {
public:
    explicit KMeansDetector(KMeansConfig config = {});

    DetectorResult run(const PreparedData& data) const;

private:
    KMeansConfig config_;
};

} // namespace telemetry
