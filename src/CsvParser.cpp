#include "telemetry/CsvParser.hpp"
#include "telemetry/CsvReader.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace telemetry {
namespace {

std::string trim(std::string value)
{
    const char* whitespace = " \t\r\n";
    const std::size_t begin = value.find_first_not_of(whitespace);
    if (begin == std::string::npos) {
        return "";
    }
    const std::size_t end = value.find_last_not_of(whitespace);
    return value.substr(begin, end - begin + 1);
}

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

void stripUtf8Bom(std::string& value)
{
    if (value.size() >= 3 &&
        static_cast<unsigned char>(value[0]) == 0xEF &&
        static_cast<unsigned char>(value[1]) == 0xBB &&
        static_cast<unsigned char>(value[2]) == 0xBF) {
        value.erase(0, 3);
    }
}

bool parseFiniteDouble(const std::string& raw, double& out)
{
    const std::string value = trim(raw);
    if (value.empty()) {
        return false;
    }

    const std::string normalized = lower(value);
    if (normalized == "nan" || normalized == "na" || normalized == "null" ||
        normalized == "none" || normalized == "inf" || normalized == "+inf" ||
        normalized == "-inf" || normalized == "infinity" ||
        normalized == "+infinity" || normalized == "-infinity") {
        return false;
    }

    errno = 0;
    char* end = nullptr;
    const char* begin = value.c_str();
    const double parsed = std::strtod(begin, &end);
    if (begin == end || errno == ERANGE) {
        return false;
    }

    const std::string tail = trim(std::string(end));
    if (!tail.empty() || !std::isfinite(parsed)) {
        return false;
    }

    out = parsed;
    return true;
}

bool parseTruthFlag(const std::string& raw)
{
    const std::string value = lower(trim(raw));
    if (value == "true" || value == "yes") {
        return true;
    }
    double parsed = 0.0;
    return parseFiniteDouble(value, parsed) && std::llround(parsed) == 1;
}

std::unordered_map<std::string, std::size_t> buildHeaderIndex(std::vector<std::string> header)
{
    if (!header.empty()) {
        stripUtf8Bom(header[0]);
    }

    std::unordered_map<std::string, std::size_t> index;
    for (std::size_t i = 0; i < header.size(); ++i) {
        index[lower(trim(header[i]))] = i;
    }
    return index;
}

std::unordered_map<std::string, std::size_t>::const_iterator findFirst(
    const std::unordered_map<std::string, std::size_t>& index,
    const std::vector<std::string>& names)
{
    for (const auto& name : names) {
        const auto it = index.find(lower(name));
        if (it != index.end()) {
            return it;
        }
    }
    return index.end();
}

struct SelectedColumn {
    std::string outputName;
    std::size_t sourceIndex = 0;
};

} // namespace

CsvParser::CsvParser(std::size_t limitRows)
    : limitRows_(limitRows)
{
}

const std::vector<std::string>& CsvParser::requiredNumericColumns()
{
    static const std::vector<std::string> columns = {
        "p0_power",
        "p1_power",
        "ps0_input_power",
        "ps1_input_power",
        "gpu0_core_temp",
        "gpu1_core_temp",
        "gpu2_core_temp",
        "gpu3_core_temp",
        "gpu4_core_temp",
        "gpu5_core_temp",
        "p0_core_temp_mean",
    };
    return columns;
}

TelemetryDataset CsvParser::readSummitColumns(
    const std::string& path,
    CancellationToken* cancellation,
    const ProgressCallback& progress) const
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Не удалось открыть CSV-файл: " + path);
    }

    std::string line;
    if (!std::getline(input, line)) {
        throw std::runtime_error("CSV-файл пуст: " + path);
    }

    const auto header = CsvReader::parseLine(line);
    const auto headerIndex = buildHeaderIndex(header);

    auto hostnameIt = findFirst(headerIndex, {"hostname", "host", "node"});
    if (hostnameIt == headerIndex.end()) {
        throw std::runtime_error("В CSV нет обязательной колонки hostname/host/node.");
    }

    auto timestampIt = findFirst(headerIndex, {"timestamp", "time", "datetime", "ts"});
    const bool hasTimestamp = timestampIt != headerIndex.end();

    TelemetryDataset dataset;
    dataset.sourcePath = path;
    dataset.schema.hostnameColumn = header[hostnameIt->second];
    dataset.schema.timestampColumn = hasTimestamp ? header[timestampIt->second] : "timestamp";
    if (!hasTimestamp) {
        dataset.warnings.push_back("Колонка timestamp не найдена; используется порядковый номер строки.");
    }

    std::vector<SelectedColumn> selectedColumns;
    std::vector<std::string> missingSummitColumns;
    for (const std::string& column : requiredNumericColumns()) {
        const auto it = headerIndex.find(column);
        if (it == headerIndex.end()) {
            missingSummitColumns.push_back(column);
        } else {
            selectedColumns.push_back({column, it->second});
        }
    }

    const bool usingSummitColumns = !selectedColumns.empty();
    if (!usingSummitColumns) {
        const auto power15 = headerIndex.find("power_mean_15min");
        const auto core15 = headerIndex.find("core_temp_mean_15min");
        if (power15 != headerIndex.end() && core15 != headerIndex.end()) {
            selectedColumns.push_back({"power_mean_15min", power15->second});
            selectedColumns.push_back({"core_temp_mean_15min", core15->second});
            dataset.warnings.push_back(
                "Распознан датасет 1970187: для обычной загрузки используются агрегаты power_mean_15min и core_temp_mean_15min. "
                "Для обязательного lead time запускайте вкладку Результаты -> Валидация 1970187.");
        }
    }

    if (selectedColumns.empty()) {
        throw std::runtime_error(
            "CSV не похож ни на обычный Summit-датасет, ни на 1970187. "
            "Для Summit нужны p0_power/gpu*_core_temp, для 1970187 нужны power_mean_15min и core_temp_mean_15min.");
    }

    for (const auto& selected : selectedColumns) {
        dataset.schema.numericIndex[selected.outputName] = dataset.schema.numericColumns.size();
        dataset.schema.numericColumns.push_back(selected.outputName);
    }

    if (usingSummitColumns) {
        for (const auto& column : missingSummitColumns) {
            dataset.schema.missingColumns.push_back(column);
            dataset.warnings.push_back("Отсутствует колонка из ТЗ: " + column);
        }
    }

    const auto failureIt = headerIndex.find("is_failure");
    const bool hasFailureFlag = failureIt != headerIndex.end();
    if (hasFailureFlag) {
        dataset.warnings.push_back("Колонка is_failure будет использована как ground truth для Precision/Recall/F1.");
    }

    std::size_t maxRequiredColumn = hostnameIt->second;
    if (hasTimestamp) {
        maxRequiredColumn = std::max(maxRequiredColumn, timestampIt->second);
    }
    if (hasFailureFlag) {
        maxRequiredColumn = std::max(maxRequiredColumn, failureIt->second);
    }
    for (const auto& selected : selectedColumns) {
        maxRequiredColumn = std::max(maxRequiredColumn, selected.sourceIndex);
    }

    std::uint64_t nextId = 1;
    std::size_t processed = 0;
    while (std::getline(input, line)) {
        if (cancellation != nullptr && cancellation->isCancelled()) {
            dataset.warnings.push_back("Загрузка отменена пользователем.");
            break;
        }

        ++processed;
        if (processed % 5000 == 0) {
            reportProgress(progress, processed, limitRows_ > 0 ? limitRows_ : processed + 1, "reading csv");
        }

        const auto fields = CsvReader::parseLine(line);
        if (fields.size() <= maxRequiredColumn) {
            ++dataset.droppedRows;
            continue;
        }

        TelemetryRow row;
        row.id = nextId++;
        row.timestamp = hasTimestamp ? trim(fields[timestampIt->second]) : std::to_string(row.id);
        row.hostname = trim(fields[hostnameIt->second]);
        if (row.hostname.empty()) {
            ++dataset.droppedRows;
            continue;
        }
        if (hasFailureFlag) {
            row.syntheticAnomaly = parseTruthFlag(fields[failureIt->second]);
        }

        row.values.reserve(selectedColumns.size());
        bool ok = true;
        for (const auto& selected : selectedColumns) {
            double value = 0.0;
            if (!parseFiniteDouble(fields[selected.sourceIndex], value)) {
                ok = false;
                break;
            }
            row.values.push_back(value);
        }
        if (!ok) {
            ++dataset.droppedRows;
            continue;
        }

        dataset.rows.push_back(std::move(row));
        if (limitRows_ > 0 && dataset.rows.size() >= limitRows_) {
            break;
        }
    }

    if (dataset.droppedRows > 0) {
        dataset.warnings.push_back("Удалено строк с NaN/пустыми значениями: " + std::to_string(dataset.droppedRows));
    }
    reportProgress(progress, 100, 100, "csv loaded");
    return dataset;
}

} // namespace telemetry
