#pragma once

#include "telemetry/TaskControl.hpp"

#include <filesystem>
#include <string>

namespace telemetry {

class ParquetConverter {
public:
    bool convertToCsv(
        const std::filesystem::path& parquetPath,
        const std::filesystem::path& csvPath,
        CancellationToken* cancellation = nullptr,
        const ProgressCallback& progress = {}) const;

    std::string lastError() const { return lastError_; }
    bool arrowEnabled() const;

private:
    mutable std::string lastError_;
};

} // namespace telemetry
