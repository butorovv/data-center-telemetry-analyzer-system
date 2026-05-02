#pragma once

#include "telemetry/Models.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace telemetry {

class CsvReader {
public:
    explicit CsvReader(std::size_t limitRows = 0);

    TelemetryDataset read(const std::string& path) const;

    static std::vector<std::string> parseLine(const std::string& line);

private:
    std::size_t limitRows_;
};

} // namespace telemetry
