#include "telemetry/Preprocessor.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numeric>
#include <random>
#include <string>

namespace telemetry {
namespace {

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool contains(const std::string& value, const std::string& needle)
{
    return value.find(needle) != std::string::npos;
}

} // namespace

std::size_t Preprocessor::removeRowsWithNaN(TelemetryDataset& dataset) const
{
    const std::size_t before = dataset.rows.size();
    const std::size_t expected = dataset.schema.numericColumns.size();
    dataset.rows.erase(
        std::remove_if(dataset.rows.begin(), dataset.rows.end(), [expected](const TelemetryRow& row) {
            if (row.values.size() != expected) {
                return true;
            }
            return std::any_of(row.values.begin(), row.values.end(), [](double value) {
                return !std::isfinite(value);
            });
        }),
        dataset.rows.end());
    return before - dataset.rows.size();
}

std::vector<std::uint64_t> Preprocessor::injectSyntheticAnomalies(
    TelemetryDataset& dataset,
    double fraction,
    double gpuTemperatureDelta,
    double cpuScale,
    std::uint32_t seed) const
{
    std::vector<std::uint64_t> injected;
    if (dataset.rows.empty() || fraction <= 0.0) {
        return injected;
    }

    std::vector<std::size_t> indices(dataset.rows.size());
    std::iota(indices.begin(), indices.end(), 0);
    std::mt19937 rng(seed);
    std::shuffle(indices.begin(), indices.end(), rng);

    const std::size_t count = std::max<std::size_t>(
        1,
        static_cast<std::size_t>(std::llround(static_cast<double>(dataset.rows.size()) * fraction)));

    for (std::size_t i = 0; i < std::min(count, indices.size()); ++i) {
        TelemetryRow& row = dataset.rows[indices[i]];
        bool changed = false;
        for (std::size_t col = 0; col < dataset.schema.numericColumns.size(); ++col) {
            const std::string name = lower(dataset.schema.numericColumns[col]);
            if (contains(name, "gpu") && contains(name, "temp")) {
                row.values[col] += gpuTemperatureDelta;
                changed = true;
            } else if ((contains(name, "cpu") || name.rfind("p0_", 0) == 0 ||
                        name.rfind("p1_", 0) == 0 || contains(name, "core_temp_mean")) &&
                       !contains(name, "gpu")) {
                row.values[col] *= cpuScale;
                changed = true;
            }
        }

        if (!changed && !row.values.empty()) {
            row.values.front() += gpuTemperatureDelta;
        }

        row.syntheticAnomaly = true;
        injected.push_back(row.id);
    }

    return injected;
}

void Preprocessor::classifyWorkload(TelemetryDataset& dataset) const
{
    if (dataset.rows.empty()) {
        return;
    }

    std::vector<std::size_t> powerColumns;
    for (std::size_t i = 0; i < dataset.schema.numericColumns.size(); ++i) {
        if (contains(lower(dataset.schema.numericColumns[i]), "power")) {
            powerColumns.push_back(i);
        }
    }
    if (powerColumns.empty()) {
        for (std::size_t i = 0; i < dataset.schema.numericColumns.size(); ++i) {
            powerColumns.push_back(i);
        }
    }

    std::vector<double> loads;
    loads.reserve(dataset.rows.size());
    for (const auto& row : dataset.rows) {
        double load = 0.0;
        for (const std::size_t col : powerColumns) {
            load += row.values[col];
        }
        loads.push_back(load);
    }

    const double mean = std::accumulate(loads.begin(), loads.end(), 0.0) / static_cast<double>(loads.size());
    double variance = 0.0;
    for (const double value : loads) {
        const double diff = value - mean;
        variance += diff * diff;
    }
    const double stddev = std::sqrt(variance / static_cast<double>(loads.size()));
    const double lowThreshold = mean - 0.5 * stddev;
    const double highThreshold = mean + 0.5 * stddev;

    for (std::size_t i = 0; i < dataset.rows.size(); ++i) {
        if (loads[i] < lowThreshold) {
            dataset.rows[i].workloadMode = "low";
        } else if (loads[i] > highThreshold) {
            dataset.rows[i].workloadMode = "high";
        } else {
            dataset.rows[i].workloadMode = "normal";
        }
    }
}

PreparedData Preprocessor::normalize(const TelemetryDataset& dataset, std::size_t slidingWindow) const
{
    PreparedData prepared;
    if (dataset.rows.empty()) {
        return prepared;
    }

    slidingWindow = std::max<std::size_t>(1, slidingWindow);
    prepared.featureNames = dataset.schema.numericColumns;
    if (slidingWindow > 1) {
        for (const auto& name : dataset.schema.numericColumns) {
            prepared.featureNames.push_back(name + "_ma" + std::to_string(slidingWindow));
        }
    }

    const std::size_t baseColumns = dataset.schema.numericColumns.size();
    std::vector<double> rolling(baseColumns, 0.0);
    prepared.features.reserve(dataset.rows.size());
    prepared.rowIds.reserve(dataset.rows.size());

    for (std::size_t rowIndex = 0; rowIndex < dataset.rows.size(); ++rowIndex) {
        const auto& row = dataset.rows[rowIndex];
        std::vector<double> features;
        features.reserve(prepared.featureNames.size());
        features.insert(features.end(), row.values.begin(), row.values.end());

        if (slidingWindow > 1) {
            for (std::size_t col = 0; col < baseColumns; ++col) {
                rolling[col] += row.values[col];
                if (rowIndex >= slidingWindow) {
                    rolling[col] -= dataset.rows[rowIndex - slidingWindow].values[col];
                }
                const std::size_t count = std::min(slidingWindow, rowIndex + 1);
                features.push_back(rolling[col] / static_cast<double>(count));
            }
        }

        prepared.rowIds.push_back(row.id);
        prepared.features.push_back(std::move(features));
    }

    const std::size_t columns = prepared.featureNames.size();
    prepared.means.assign(columns, 0.0);
    prepared.stddevs.assign(columns, 0.0);

    for (const auto& sample : prepared.features) {
        for (std::size_t col = 0; col < columns; ++col) {
            prepared.means[col] += sample[col];
        }
    }
    for (double& mean : prepared.means) {
        mean /= static_cast<double>(prepared.features.size());
    }

    for (const auto& sample : prepared.features) {
        for (std::size_t col = 0; col < columns; ++col) {
            const double diff = sample[col] - prepared.means[col];
            prepared.stddevs[col] += diff * diff;
        }
    }
    for (double& stddev : prepared.stddevs) {
        stddev = std::sqrt(stddev / static_cast<double>(prepared.features.size()));
        if (stddev < 1e-12) {
            stddev = 1.0;
        }
    }

    for (auto& sample : prepared.features) {
        for (std::size_t col = 0; col < columns; ++col) {
            sample[col] = (sample[col] - prepared.means[col]) / prepared.stddevs[col];
        }
    }

    return prepared;
}

} // namespace telemetry
