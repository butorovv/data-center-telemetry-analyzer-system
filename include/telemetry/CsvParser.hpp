#pragma once

#include "telemetry/Models.hpp"
#include "telemetry/TaskControl.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace telemetry {

class CsvParser {
public:
    explicit CsvParser(std::size_t limitRows = 0);

    TelemetryDataset readSummitColumns(
        const std::string& path,
        CancellationToken* cancellation = nullptr,
        const ProgressCallback& progress = {}) const;

    static const std::vector<std::string>& requiredNumericColumns();

private:
    std::size_t limitRows_;
};

} // namespace telemetry
