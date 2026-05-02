#include "telemetry/IForestDetector.hpp"

namespace telemetry {

IForestDetector::IForestDetector(IForestConfig config)
    : config_(config)
{
}

DetectorResult IForestDetector::run(const PreparedData& data) const
{
    return IsolationForestDetector(config_).run(data);
}

} // namespace telemetry
