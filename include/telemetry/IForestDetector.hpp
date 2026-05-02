#pragma once

#include "telemetry/IsolationForestDetector.hpp"

namespace telemetry {

using IForestConfig = IsolationForestConfig;

class IForestDetector {
public:
    explicit IForestDetector(IForestConfig config = {});

    DetectorResult run(const PreparedData& data) const;

private:
    IForestConfig config_;
};

} // namespace telemetry
