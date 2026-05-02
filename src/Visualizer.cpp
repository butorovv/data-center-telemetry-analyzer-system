#include "telemetry/Visualizer.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

namespace telemetry {
namespace {

std::string shellQuote(const std::filesystem::path& path)
{
    std::string value = path.string();
    std::string quoted = "\"";
    for (const char ch : value) {
        if (ch == '"') {
            quoted += "\\\"";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted.push_back('"');
    return quoted;
}

std::string quoteText(const std::string& text)
{
    std::string quoted = "\"";
    for (const char ch : text) {
        if (ch == '"') {
            quoted += "\\\"";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted.push_back('"');
    return quoted;
}

std::string csvEscape(const std::string& value)
{
    if (value.find_first_of(",\"\n\r") == std::string::npos) {
        return value;
    }
    std::string escaped = "\"";
    for (const char ch : value) {
        if (ch == '"') {
            escaped += "\"\"";
        } else {
            escaped.push_back(ch);
        }
    }
    escaped.push_back('"');
    return escaped;
}

} // namespace

bool Visualizer::exportSeries(
    const TelemetryDataset& dataset,
    const DetectorResult& result,
    const std::string& metric,
    const std::string& hostname,
    const std::filesystem::path& csvPath) const
{
    if (dataset.rows.empty() || dataset.schema.numericColumns.empty()) {
        return false;
    }

    std::size_t metricIndex = dataset.schema.numericColumns.size();
    for (std::size_t i = 0; i < dataset.schema.numericColumns.size(); ++i) {
        if (dataset.schema.numericColumns[i] == metric) {
            metricIndex = i;
            break;
        }
    }
    if (metricIndex == dataset.schema.numericColumns.size()) {
        for (std::size_t i = 0; i < dataset.schema.numericColumns.size(); ++i) {
            if (dataset.schema.numericColumns[i].find(metric) != std::string::npos) {
                metricIndex = i;
                break;
            }
        }
    }
    if (metricIndex == dataset.schema.numericColumns.size()) {
        metricIndex = 0;
    }

    std::ofstream out(csvPath);
    out << "timestamp,hostname,value,anomaly,score\n";
    const std::size_t rows = std::min(dataset.rows.size(), result.labels.size());
    for (std::size_t i = 0; i < rows; ++i) {
        const auto& row = dataset.rows[i];
        if (!hostname.empty() && row.hostname != hostname) {
            continue;
        }
        out << csvEscape(row.timestamp) << ',' << csvEscape(row.hostname) << ','
            << row.values[metricIndex] << ',' << (result.labels[i] != 0 ? 1 : 0) << ','
            << (i < result.scores.size() ? result.scores[i] : 0.0) << '\n';
    }
    return true;
}

bool Visualizer::runPythonPlot(
    const std::filesystem::path& csvPath,
    const std::filesystem::path& outputPng,
    const std::string& title) const
{
    std::ostringstream command;
    command << "python " << shellQuote("scripts/plot_telemetry.py")
            << " --input " << shellQuote(csvPath)
            << " --output " << shellQuote(outputPng)
            << " --title " << quoteText(title);

    int rc = std::system(command.str().c_str());
    if (rc == 0) {
        return true;
    }

    std::ostringstream fallback;
    fallback << "python3 " << shellQuote("scripts/plot_telemetry.py")
             << " --input " << shellQuote(csvPath)
             << " --output " << shellQuote(outputPng)
             << " --title " << quoteText(title);
    rc = std::system(fallback.str().c_str());
    if (rc != 0) {
        std::cerr << "Python plot script failed. CSV data was exported to " << csvPath << '\n';
        return false;
    }
    return true;
}

} // namespace telemetry
