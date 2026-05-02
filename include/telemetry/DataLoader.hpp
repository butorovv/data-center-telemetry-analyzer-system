#pragma once

#include "telemetry/Models.hpp"
#include "telemetry/TaskControl.hpp"

#include <cstddef>
#include <filesystem>

namespace telemetry {

class DataLoader {
public:
    explicit DataLoader(std::size_t limitRows = 0);

    TelemetryDataset load(
        const std::filesystem::path& path,
        CancellationToken* cancellation = nullptr,
        const ProgressCallback& progress = {}) const;

private:
    std::filesystem::path materializeInput(
        const std::filesystem::path& path,
        CancellationToken* cancellation,
        const ProgressCallback& progress) const;

    std::filesystem::path extractTarAndFindDataFile(
        const std::filesystem::path& tarPath,
        CancellationToken* cancellation,
        const ProgressCallback& progress) const;

    std::size_t limitRows_;
};

} // namespace telemetry
