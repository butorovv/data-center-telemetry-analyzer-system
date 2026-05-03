#pragma once

#include "telemetry/GraphBuilder.hpp"
#include "telemetry/Models.hpp"
#include "telemetry/TaskControl.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace telemetry {

struct FailureEvent1970187 {
    std::string timestamp;
    std::string hostname;
    int gpu = -1;
    std::uint64_t sourceRow = 0;
};

struct FailureValidationOptions {
    // Kept for backward compatibility with older UI/CLI wiring. The current
    // 1970187 validator uses telemetryPath and is_failure from points CSV.
    std::filesystem::path failuresPath;
    std::filesystem::path telemetryPath;
    std::filesystem::path locationsPath;
    int xidCode = 94;
    int lookbackMinutes = 20;
    int validationWindowMinutes = 15;
    double anomalyThreshold = 0.75;
    std::string validationAlgorithm = "hybrid";
    // 0 means validate all is_failure rows.
    std::size_t maxEvents = 0;
    CancellationToken* cancellation = nullptr;
    ProgressCallback progress;
};

struct FailureValidationSummary {
    std::size_t totalRows = 0;
    std::size_t totalFailureRows = 0;
    std::size_t validFailureRows = 0;
    std::size_t skippedFailureRows = 0;
    std::size_t missingTimestamp = 0;
    std::size_t missingHostOrGpu = 0;
    std::size_t missingRequiredFeatures = 0;
    std::size_t nonFiniteNumericValues = 0;
    std::size_t processedEvents = 0;
    std::size_t positiveCount = 0;
    std::size_t negativeCount = 0;
    std::size_t proxyUsed = 0;
    double detectionRate = 0.0;
    double threshold = 0.75;
    std::string algorithm = "hybrid";
    double meanScorePositive = 0.0;
    double meanScoreNegative = 0.0;
    int windowMinutes = 15;
    int leadTimeSeconds = 900;
    std::string windowType = "15min";
};

struct FailureValidationResult {
    std::vector<XidEvent> events;
    std::vector<FailureEvent1970187> failureEvents;
    TelemetryDataset windowDataset;
    PreparedData prepared;
    GraphContext graph;
    DetectorResult hybridResult;
    std::vector<LeadTimeResult> leadTimes;
    FailureValidationSummary summary;
    std::vector<std::string> warnings;
};

class RealFailureValidator {
public:
    std::vector<XidEvent> loadXidEvents(const std::filesystem::path& path) const;
    std::vector<FailureEvent1970187> loadFailureEventsFromPoints(
        const std::filesystem::path& pointsPath,
        std::size_t maxEvents = 0,
        CancellationToken* cancellation = nullptr,
        const ProgressCallback& progress = {}) const;

    std::vector<LeadTimeResult> calculateLeadTimes(
        const TelemetryDataset& dataset,
        const DetectorResult& hybridResult,
        const std::vector<XidEvent>& events,
        int xidCode = 94) const;

    FailureValidationResult validate1970187(const FailureValidationOptions& options) const;

    static bool parseTimestampSeconds(const std::string& text, long long& seconds);
};

} // namespace telemetry
