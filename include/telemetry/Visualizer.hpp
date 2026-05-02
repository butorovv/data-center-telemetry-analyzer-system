#pragma once

#include "telemetry/Models.hpp"

#include <filesystem>
#include <string>

namespace telemetry {

class Visualizer {
public:
    bool exportSeries(
        const TelemetryDataset& dataset,
        const DetectorResult& result,
        const std::string& metric,
        const std::string& hostname,
        const std::filesystem::path& csvPath) const;

    bool runPythonPlot(
        const std::filesystem::path& csvPath,
        const std::filesystem::path& outputPng,
        const std::string& title) const;
};

} // namespace telemetry
