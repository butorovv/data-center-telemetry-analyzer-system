#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace telemetry {

struct TelemetrySchema {
    std::string timestampColumn = "timestamp";
    std::string hostnameColumn = "hostname";
    std::vector<std::string> numericColumns;
    std::vector<std::string> safeColumnNames;
    std::vector<std::string> missingColumns;
    std::unordered_map<std::string, std::size_t> numericIndex;
};

struct TelemetryRow {
    std::uint64_t id = 0;
    std::string timestamp;
    std::string hostname;
    std::vector<double> values;
    bool syntheticAnomaly = false;
    std::string workloadMode = "unknown";
};

struct TelemetryDataset {
    TelemetrySchema schema;
    std::vector<TelemetryRow> rows;
    std::string sourcePath;
    std::size_t droppedRows = 0;
    std::vector<std::string> warnings;

    bool empty() const { return rows.empty(); }
};

struct PreparedData {
    std::vector<std::vector<double>> features;
    std::vector<std::uint64_t> rowIds;
    std::vector<std::string> featureNames;
    std::vector<double> means;
    std::vector<double> stddevs;
};

struct DetectorResult {
    std::string algorithm;
    std::vector<std::uint64_t> rowIds;
    std::vector<std::uint8_t> labels;
    std::vector<double> scores;
    double executionMs = 0.0;
    double threshold = 0.0;
    std::string parameters;
};

struct Metrics {
    std::size_t tp = 0;
    std::size_t fp = 0;
    std::size_t fn = 0;
    std::size_t tn = 0;
    double precision = 0.0;
    double recall = 0.0;
    double f1 = 0.0;
};

struct XidEvent {
    std::string timestamp;
    std::string hostname;
    int xid = 0;
};

struct LeadTimeResult {
    std::string hostname;
    std::string errorTimestamp;
    std::string detectionTimestamp;
    double leadTimeSeconds = 0.0;
    bool positive = false;
};

} // namespace telemetry
