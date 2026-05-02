#include "telemetry/CsvReader.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace telemetry {
namespace {

std::string trim(std::string value)
{
    auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

double parseDouble(const std::string& value)
{
    const std::string text = lower(trim(value));
    if (text.empty() || text == "nan" || text == "na" || text == "null" || text == "none") {
        return std::numeric_limits<double>::quiet_NaN();
    }

    char* end = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0') {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return parsed;
}

void stripUtf8Bom(std::string& value)
{
    if (value.size() >= 3 && static_cast<unsigned char>(value[0]) == 0xEF &&
        static_cast<unsigned char>(value[1]) == 0xBB &&
        static_cast<unsigned char>(value[2]) == 0xBF) {
        value.erase(0, 3);
    }
}

} // namespace

CsvReader::CsvReader(std::size_t limitRows)
    : limitRows_(limitRows)
{
}

std::vector<std::string> CsvReader::parseLine(const std::string& line)
{
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false;

    for (std::size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (ch == '"') {
            if (quoted && i + 1 < line.size() && line[i + 1] == '"') {
                field.push_back('"');
                ++i;
            } else {
                quoted = !quoted;
            }
        } else if (ch == ',' && !quoted) {
            fields.push_back(trim(field));
            field.clear();
        } else {
            field.push_back(ch);
        }
    }
    fields.push_back(trim(field));
    return fields;
}

TelemetryDataset CsvReader::read(const std::string& path) const
{
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Cannot open CSV file: " + path);
    }

    std::string line;
    if (!std::getline(input, line)) {
        throw std::runtime_error("CSV file is empty: " + path);
    }

    auto headers = parseLine(line);
    if (headers.empty()) {
        throw std::runtime_error("CSV header is empty: " + path);
    }
    stripUtf8Bom(headers.front());

    int hostnameIndex = -1;
    int timestampIndex = -1;
    for (std::size_t i = 0; i < headers.size(); ++i) {
        const std::string name = lower(trim(headers[i]));
        if (name == "hostname" || name == "host" || name == "node") {
            hostnameIndex = static_cast<int>(i);
        } else if (name == "timestamp" || name == "time" || name == "datetime" || name == "ts") {
            timestampIndex = static_cast<int>(i);
        }
    }

    TelemetryDataset dataset;
    dataset.schema.hostnameColumn = hostnameIndex >= 0 ? headers[static_cast<std::size_t>(hostnameIndex)] : "hostname";
    dataset.schema.timestampColumn = timestampIndex >= 0 ? headers[static_cast<std::size_t>(timestampIndex)] : "timestamp";

    std::vector<std::size_t> numericSourceIndices;
    for (std::size_t i = 0; i < headers.size(); ++i) {
        if (static_cast<int>(i) == hostnameIndex || static_cast<int>(i) == timestampIndex) {
            continue;
        }
        numericSourceIndices.push_back(i);
        dataset.schema.numericIndex[headers[i]] = dataset.schema.numericColumns.size();
        dataset.schema.numericColumns.push_back(headers[i]);
    }

    std::uint64_t nextId = 1;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        auto fields = parseLine(line);
        if (fields.size() < headers.size()) {
            fields.resize(headers.size());
        }

        TelemetryRow row;
        row.id = nextId++;
        row.timestamp = timestampIndex >= 0 ? fields[static_cast<std::size_t>(timestampIndex)] : std::to_string(row.id);
        row.hostname = hostnameIndex >= 0 ? fields[static_cast<std::size_t>(hostnameIndex)] : "unknown";
        if (row.hostname.empty()) {
            row.hostname = "unknown";
        }
        row.values.reserve(numericSourceIndices.size());
        for (const std::size_t sourceIndex : numericSourceIndices) {
            row.values.push_back(sourceIndex < fields.size() ? parseDouble(fields[sourceIndex])
                                                            : std::numeric_limits<double>::quiet_NaN());
        }
        dataset.rows.push_back(std::move(row));

        if (limitRows_ > 0 && dataset.rows.size() >= limitRows_) {
            break;
        }
    }

    return dataset;
}

} // namespace telemetry
