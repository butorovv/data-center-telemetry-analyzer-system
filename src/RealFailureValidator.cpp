#include "telemetry/RealFailureValidator.hpp"

#include "telemetry/CsvReader.hpp"
#include "telemetry/Preprocessor.hpp"
#include "telemetry/Summit1970187Adapter.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace telemetry {
namespace {

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

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

void stripUtf8Bom(std::string& value)
{
    if (value.size() >= 3 &&
        static_cast<unsigned char>(value[0]) == 0xEF &&
        static_cast<unsigned char>(value[1]) == 0xBB &&
        static_cast<unsigned char>(value[2]) == 0xBF) {
        value.erase(0, 3);
    }
}

int findColumn(const std::vector<std::string>& header, const std::vector<std::string>& aliases)
{
    for (std::size_t i = 0; i < header.size(); ++i) {
        const std::string name = lower(trim(header[i]));
        for (const auto& alias : aliases) {
            if (name == lower(alias)) {
                return static_cast<int>(i);
            }
        }
    }
    return -1;
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

    char* end = nullptr;
    const double parsed = std::strtod(value.c_str(), &end);
    if (end == value.c_str() || !std::isfinite(parsed)) {
        return false;
    }
    if (!trim(std::string(end)).empty()) {
        return false;
    }
    out = parsed;
    return true;
}

int parseIntegerLike(const std::string& raw)
{
    double value = 0.0;
    if (parseFiniteDouble(raw, value)) {
        return static_cast<int>(std::llround(value));
    }
    const std::string valueText = trim(raw);
    std::smatch match;
    if (std::regex_search(valueText, match, std::regex(R"(([0-9]+))"))) {
        return std::stoi(match[1].str());
    }
    throw std::runtime_error("Cannot parse integer value: " + raw);
}

bool parseFailureFlag(const std::string& raw)
{
    const std::string value = lower(trim(raw));
    if (value == "true" || value == "yes") {
        return true;
    }
    double numeric = 0.0;
    return parseFiniteDouble(value, numeric) && std::llround(numeric) == 1;
}

#ifdef _WIN32
long long utcFromTm(std::tm* tm)
{
    return static_cast<long long>(_mkgmtime(tm));
}

std::tm utcTmFromSeconds(std::time_t value)
{
    std::tm tm{};
    gmtime_s(&tm, &value);
    return tm;
}
#else
long long utcFromTm(std::tm* tm)
{
    return static_cast<long long>(timegm(tm));
}

std::tm utcTmFromSeconds(std::time_t value)
{
    std::tm tm{};
    gmtime_r(&value, &tm);
    return tm;
}
#endif

std::string formatTimestampSeconds(long long seconds)
{
    const std::time_t value = static_cast<std::time_t>(seconds);
    const std::tm tm = utcTmFromSeconds(value);
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "+00:00";
    return out.str();
}

void pushWarning(std::vector<std::string>& warnings, const std::string& message)
{
    constexpr std::size_t kMaxWarnings = 24;
    if (warnings.size() < kMaxWarnings) {
        warnings.push_back(message);
    } else if (warnings.size() == kMaxWarnings) {
        warnings.push_back("Further validation warnings were suppressed.");
    }
}

struct PointRecord {
    std::uint64_t rowNumber = 0;
    std::string timestamp;
    long long timestampSeconds = 0;
    std::string hostname;
    int gpu = -1;
    bool isFailure = false;
    std::vector<double> features;
};

struct PointsTable {
    std::vector<PointRecord> records;
    std::vector<FailureEvent1970187> events;
    std::vector<std::string> featureNames;
    FailureValidationSummary summary;
    std::vector<std::string> warnings;
};

struct FeatureSelection {
    std::vector<std::string> names;
    std::vector<std::size_t> columns;
};

int normalizedWindowMinutes(int windowMinutes)
{
    if (windowMinutes == 1 || windowMinutes == 5 || windowMinutes == 15) {
        return windowMinutes;
    }
    return 15;
}

std::string windowTypeName(int windowMinutes)
{
    return std::to_string(normalizedWindowMinutes(windowMinutes)) + "min";
}

int validationLeadSeconds(int windowMinutes)
{
    return normalizedWindowMinutes(windowMinutes) * 60;
}

std::string normalizedValidationAlgorithm(std::string value)
{
    value = lower(trim(value));
    if (value == "iforest" || value == "if" || value == "isolation-forest") {
        return "isolation_forest";
    }
    if (value == "isolation_forest" || value == "hybrid") {
        return value;
    }
    return "hybrid";
}

bool endsWith(const std::string& value, const std::string& suffix)
{
    return value.size() >= suffix.size() &&
        value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool startsWithAny(const std::string& value, const std::vector<std::string>& prefixes)
{
    for (const auto& prefix : prefixes) {
        if (value.rfind(prefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

FeatureSelection selectWindowFeatures(const std::vector<std::string>& header, int windowMinutes)
{
    const std::string suffix = "_" + windowTypeName(windowMinutes);
    const std::vector<std::string> allowedPrefixes = {
        "power_mean_", "power_min_", "power_max_", "power_range_", "power_fluct_",
        "core_temp_mean_", "core_temp_min_", "core_temp_max_", "core_temp_range_", "core_temp_fluct_",
        "mem_temp_mean_", "mem_temp_min_", "mem_temp_max_", "mem_temp_range_", "mem_temp_fluct_"
    };

    FeatureSelection selection;
    for (std::size_t i = 0; i < header.size(); ++i) {
        const std::string name = lower(trim(header[i]));
        if (endsWith(name, suffix) && startsWithAny(name, allowedPrefixes)) {
            selection.names.push_back(name);
            selection.columns.push_back(i);
        }
    }
    return selection;
}

void markSkippedFailure(FailureValidationSummary& summary, const std::string& reason)
{
    ++summary.skippedFailureRows;
    if (reason == "missing_timestamp") {
        ++summary.missingTimestamp;
    } else if (reason == "missing_host_gpu") {
        ++summary.missingHostOrGpu;
    } else if (reason == "missing_required_features") {
        ++summary.missingRequiredFeatures;
    } else if (reason == "non_finite_numeric") {
        ++summary.nonFiniteNumericValues;
    }
}

PointsTable loadPointsTable(
    const std::filesystem::path& pointsPath,
    std::size_t maxEvents,
    int windowMinutes,
    CancellationToken* cancellation,
    const ProgressCallback& progress)
{
    std::ifstream input(pointsPath, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot open 1970187 points CSV: " + pointsPath.string());
    }

    std::string line;
    if (!std::getline(input, line)) {
        throw std::runtime_error("1970187 points CSV is empty: " + pointsPath.string());
    }

    auto header = CsvReader::parseLine(line);
    if (!header.empty()) {
        stripUtf8Bom(header[0]);
    }
    const int timestampCol = findColumn(header, {"timestamp", "time", "datetime", "date"});
    const int hostCol = findColumn(header, {"hostname", "host", "node"});
    const int gpuCol = findColumn(header, {"gpu", "GPU"});
    const int failureCol = findColumn(header, {"is_failure", "failure", "label"});
    const FeatureSelection features = selectWindowFeatures(header, windowMinutes);

    if (timestampCol < 0 || hostCol < 0 || gpuCol < 0 || failureCol < 0) {
        throw std::runtime_error("points_with_jobs_tele_ult.csv must contain timestamp, hostname, GPU and is_failure columns.");
    }
    if (features.columns.empty()) {
        throw std::runtime_error("points_with_jobs_tele_ult.csv has no numeric telemetry features for window " + windowTypeName(windowMinutes) + ".");
    }

    PointsTable table;
    table.featureNames = features.names;
    table.summary.windowMinutes = normalizedWindowMinutes(windowMinutes);
    table.summary.windowType = windowTypeName(windowMinutes);
    table.summary.leadTimeSeconds = validationLeadSeconds(windowMinutes);

    std::size_t scanned = 0;
    while (std::getline(input, line)) {
        if (cancellation != nullptr && cancellation->isCancelled()) {
            pushWarning(table.warnings, "1970187 points scan was cancelled by user.");
            break;
        }
        ++scanned;
        ++table.summary.totalRows;
        if (scanned % 25000 == 0) {
            reportProgress(progress, scanned % 100000, 100000, "scanning points_with_jobs_tele_ult.csv");
        }

        const auto fields = CsvReader::parseLine(line);
        const bool canReadFailure = fields.size() > static_cast<std::size_t>(failureCol);
        const bool isFailure = canReadFailure && parseFailureFlag(fields[static_cast<std::size_t>(failureCol)]);
        if (isFailure) {
            ++table.summary.totalFailureRows;
        }

        if (fields.size() <= static_cast<std::size_t>(timestampCol) || trim(fields[static_cast<std::size_t>(timestampCol)]).empty()) {
            if (isFailure) {
                markSkippedFailure(table.summary, "missing_timestamp");
            }
            continue;
        }
        long long timestampSeconds = 0;
        const std::string timestamp = trim(fields[static_cast<std::size_t>(timestampCol)]);
        if (!RealFailureValidator::parseTimestampSeconds(timestamp, timestampSeconds)) {
            if (isFailure) {
                markSkippedFailure(table.summary, "missing_timestamp");
            }
            continue;
        }

        if (fields.size() <= static_cast<std::size_t>(hostCol) || fields.size() <= static_cast<std::size_t>(gpuCol) ||
            trim(fields[static_cast<std::size_t>(hostCol)]).empty()) {
            if (isFailure) {
                markSkippedFailure(table.summary, "missing_host_gpu");
            }
            continue;
        }

        int gpu = -1;
        try {
            gpu = parseIntegerLike(fields[static_cast<std::size_t>(gpuCol)]);
        } catch (...) {
            if (isFailure) {
                markSkippedFailure(table.summary, "missing_host_gpu");
            }
            continue;
        }

        bool missingFeature = false;
        bool nonFinite = false;
        std::vector<double> rowFeatures;
        rowFeatures.reserve(features.columns.size());
        for (const std::size_t col : features.columns) {
            if (fields.size() <= col || trim(fields[col]).empty()) {
                missingFeature = true;
                break;
            }
            double value = 0.0;
            if (!parseFiniteDouble(fields[col], value)) {
                nonFinite = true;
                break;
            }
            rowFeatures.push_back(value);
        }
        if (missingFeature || rowFeatures.size() != features.columns.size()) {
            if (isFailure) {
                markSkippedFailure(table.summary, "missing_required_features");
            }
            continue;
        }
        if (nonFinite) {
            if (isFailure) {
                markSkippedFailure(table.summary, "non_finite_numeric");
            }
            continue;
        }

        PointRecord record;
        record.rowNumber = scanned;
        record.timestamp = timestamp;
        record.timestampSeconds = timestampSeconds;
        record.hostname = trim(fields[static_cast<std::size_t>(hostCol)]);
        record.gpu = gpu;
        record.isFailure = isFailure;
        record.features = std::move(rowFeatures);
        table.records.push_back(record);

        if (record.isFailure) {
            ++table.summary.validFailureRows;
            if (maxEvents == 0 || table.events.size() < maxEvents) {
                FailureEvent1970187 event;
                event.timestamp = record.timestamp;
                event.hostname = record.hostname;
                event.gpu = record.gpu;
                event.sourceRow = record.rowNumber;
                table.events.push_back(std::move(event));
            }
        }
    }

    reportProgress(progress, 100, 100, "points_with_jobs_tele_ult.csv loaded");
    return table;
}

TelemetryDataset makeDataset(
    const std::vector<PointRecord>& records,
    const std::vector<std::string>& featureNames,
    const std::filesystem::path& sourcePath)
{
    TelemetryDataset dataset;
    dataset.sourcePath = sourcePath.string();
    dataset.schema.timestampColumn = "timestamp";
    dataset.schema.hostnameColumn = "hostname";
    for (const auto& name : featureNames) {
        dataset.schema.numericIndex[name] = dataset.schema.numericColumns.size();
        dataset.schema.numericColumns.push_back(name);
    }

    dataset.rows.reserve(records.size());
    for (std::size_t i = 0; i < records.size(); ++i) {
        TelemetryRow row;
        row.id = static_cast<std::uint64_t>(records[i].rowNumber);
        row.timestamp = records[i].timestamp;
        row.hostname = records[i].hostname;
        row.values = records[i].features;
        row.syntheticAnomaly = records[i].isFailure;
        dataset.rows.push_back(std::move(row));
    }
    return dataset;
}

} // namespace

std::vector<XidEvent> RealFailureValidator::loadXidEvents(const std::filesystem::path& path) const
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot open XID log: " + path.string());
    }

    std::vector<XidEvent> events;
    std::string line;
    if (!std::getline(input, line)) {
        return events;
    }

    auto header = CsvReader::parseLine(line);
    if (!header.empty()) {
        stripUtf8Bom(header[0]);
    }
    const int timestampCol = findColumn(header, {"timestamp", "time", "datetime", "date"});
    const int hostCol = findColumn(header, {"hostname", "host", "node"});
    const int xidCol = findColumn(header, {"xid", "xid_code", "error", "code"});

    if (timestampCol >= 0 && xidCol >= 0) {
        while (std::getline(input, line)) {
            const auto fields = CsvReader::parseLine(line);
            if (fields.size() <= static_cast<std::size_t>(std::max(timestampCol, xidCol))) {
                continue;
            }
            XidEvent event;
            event.timestamp = trim(fields[static_cast<std::size_t>(timestampCol)]);
            event.hostname = hostCol >= 0 && fields.size() > static_cast<std::size_t>(hostCol)
                ? trim(fields[static_cast<std::size_t>(hostCol)])
                : "";
            try {
                event.xid = parseIntegerLike(fields[static_cast<std::size_t>(xidCol)]);
            } catch (...) {
                continue;
            }
            events.push_back(std::move(event));
        }
        return events;
    }

    input.clear();
    input.seekg(0);
    std::regex xidPattern(R"((XID|xid)[^0-9]*([0-9]+))");
    std::regex timestampPattern(R"(([0-9]{4}-[0-9]{2}-[0-9]{2}[ T][0-9]{2}:[0-9]{2}:[0-9]{2}(?:[+-][0-9]{2}:?[0-9]{2}|Z)?))");
    std::regex hostPattern(R"((node[0-9]+|rack[0-9a-zA-Z]+[_-]?position[0-9]+|[A-Za-z][0-9]{2,}[A-Za-z]?[0-9]*))");
    while (std::getline(input, line)) {
        std::smatch xidMatch;
        if (!std::regex_search(line, xidMatch, xidPattern)) {
            continue;
        }
        XidEvent event;
        event.xid = std::stoi(xidMatch[2].str());

        std::smatch timestampMatch;
        if (std::regex_search(line, timestampMatch, timestampPattern)) {
            event.timestamp = timestampMatch[1].str();
        }
        std::smatch hostMatch;
        if (std::regex_search(line, hostMatch, hostPattern)) {
            event.hostname = hostMatch[1].str();
        }
        events.push_back(std::move(event));
    }
    return events;
}

std::vector<FailureEvent1970187> RealFailureValidator::loadFailureEventsFromPoints(
    const std::filesystem::path& pointsPath,
    std::size_t maxEvents,
    CancellationToken* cancellation,
    const ProgressCallback& progress) const
{
    return loadPointsTable(pointsPath, maxEvents, 15, cancellation, progress).events;
}

std::vector<LeadTimeResult> RealFailureValidator::calculateLeadTimes(
    const TelemetryDataset& dataset,
    const DetectorResult& hybridResult,
    const std::vector<XidEvent>& events,
    int xidCode) const
{
    std::vector<LeadTimeResult> results;
    const std::size_t rows = std::min(dataset.rows.size(), hybridResult.labels.size());

    for (const auto& event : events) {
        if (event.xid != xidCode) {
            continue;
        }

        long long errorSeconds = 0;
        if (!parseTimestampSeconds(event.timestamp, errorSeconds)) {
            continue;
        }

        long long bestDetectionSeconds = std::numeric_limits<long long>::min();
        std::string bestDetectionTimestamp;
        for (std::size_t row = 0; row < rows; ++row) {
            if (hybridResult.labels[row] == 0) {
                continue;
            }
            if (!event.hostname.empty() && dataset.rows[row].hostname != event.hostname) {
                continue;
            }

            long long detectionSeconds = 0;
            if (!parseTimestampSeconds(dataset.rows[row].timestamp, detectionSeconds)) {
                continue;
            }
            if (detectionSeconds < errorSeconds && detectionSeconds > bestDetectionSeconds) {
                bestDetectionSeconds = detectionSeconds;
                bestDetectionTimestamp = dataset.rows[row].timestamp;
            }
        }

        if (bestDetectionSeconds == std::numeric_limits<long long>::min()) {
            continue;
        }

        LeadTimeResult result;
        result.hostname = event.hostname;
        result.errorTimestamp = event.timestamp;
        result.detectionTimestamp = bestDetectionTimestamp;
        result.leadTimeSeconds = static_cast<double>(errorSeconds - bestDetectionSeconds);
        result.positive = result.leadTimeSeconds > 0.0;
        results.push_back(std::move(result));
    }

    return results;
}

FailureValidationResult RealFailureValidator::validate1970187(const FailureValidationOptions& options) const
{
    FailureValidationResult result;
    const int windowMinutes = normalizedWindowMinutes(options.validationWindowMinutes);
    const int leadSeconds = validationLeadSeconds(windowMinutes);
    const std::string windowType = windowTypeName(windowMinutes);
    const std::string validationAlgorithm = normalizedValidationAlgorithm(options.validationAlgorithm);
    reportProgress(options.progress, 0, 100, "loading points_with_jobs_tele_ult.csv");

    PointsTable table = loadPointsTable(
        options.telemetryPath,
        options.maxEvents,
        windowMinutes,
        options.cancellation,
        options.progress);
    result.summary = table.summary;
    result.summary.algorithm = validationAlgorithm;
    result.summary.threshold = options.anomalyThreshold;
    for (const auto& warning : table.warnings) {
        pushWarning(result.warnings, warning);
    }

    if (!options.failuresPath.empty()) {
        std::ifstream failures(options.failuresPath, std::ios::binary);
        if (failures) {
            std::size_t lines = 0;
            std::string ignored;
            while (std::getline(failures, ignored)) {
                ++lines;
            }
            if (lines > 0) {
                pushWarning(result.warnings,
                    "Optional sanity-check: failures.csv rows including header = " + std::to_string(lines) +
                    "; not used as ground truth for current validation.");
            }
        } else {
            pushWarning(result.warnings,
                "Optional sanity-check: failures.csv was not opened; validation uses points_with_jobs_tele_ult.csv only.");
        }
    }

    result.failureEvents = table.events;
    for (const auto& event : result.failureEvents) {
        XidEvent compatible;
        compatible.timestamp = event.timestamp;
        compatible.hostname = event.hostname;
        compatible.xid = options.xidCode;
        result.events.push_back(std::move(compatible));
    }

    if (table.records.empty()) {
        pushWarning(result.warnings, "No valid rows were found in " + options.telemetryPath.string() + " for window " + windowType + ".");
        reportProgress(options.progress, 100, 100, "1970187 validation complete");
        return result;
    }

    if (result.failureEvents.empty()) {
        pushWarning(result.warnings,
            "No valid is_failure == 1 rows were found in " + options.telemetryPath.string() +
            " for window " + windowType + ".");
        reportProgress(options.progress, 100, 100, "1970187 validation complete");
        return result;
    }

    TelemetryDataset dataset = makeDataset(table.records, table.featureNames, options.telemetryPath);
    Preprocessor preprocessor;
    const std::size_t removed = preprocessor.removeRowsWithNaN(dataset);
    if (removed > 0) {
        pushWarning(result.warnings, "Removed NaN rows from 1970187 validation dataset: " + std::to_string(removed));
    }
    preprocessor.classifyWorkload(dataset);
    PreparedData prepared = preprocessor.normalize(dataset, 1);
    GraphContext graph = GraphBuilder{}.build(dataset);

    PrototypeRunOptions runOptions;
    runOptions.isolationThreshold = options.anomalyThreshold;
    runOptions.cancellation = options.cancellation;
    runOptions.progress = options.progress;

    DetectorResult iforest = Summit1970187Adapter::runIsolationForest(prepared, runOptions);
    iforest.parameters += ";1970187_window=" + windowType + ";validation_algorithm=isolation_forest;forced_anomaly=0";

    DetectorResult selected = iforest;
    DetectorResult hybrid;
    const bool useHybrid = validationAlgorithm == "hybrid";
    if (useHybrid) {
        hybrid = Summit1970187Adapter::runHybrid(dataset, prepared, graph, runOptions);
        hybrid.parameters += ";1970187_window=" + windowType + ";validation_algorithm=hybrid;forced_anomaly=0";
        selected = hybrid;
    }

    auto buildIndex = [](const DetectorResult& detector) {
        std::unordered_map<std::uint64_t, std::size_t> index;
        const std::size_t rows = std::min(detector.rowIds.size(), detector.labels.size());
        for (std::size_t i = 0; i < rows; ++i) {
            index[detector.rowIds[i]] = i;
        }
        return index;
    };

    const auto ifIndexBySourceRow = buildIndex(iforest);
    const auto selectedIndexBySourceRow = buildIndex(selected);
    const auto hybridIndexBySourceRow = useHybrid ? buildIndex(hybrid) : std::unordered_map<std::uint64_t, std::size_t>{};

    double positiveScoreSum = 0.0;
    double negativeScoreSum = 0.0;
    bool hasPositiveLeadTime = false;
    for (std::size_t eventIndex = 0; eventIndex < result.failureEvents.size(); ++eventIndex) {
        if (options.cancellation != nullptr && options.cancellation->isCancelled()) {
            pushWarning(result.warnings, "1970187 validation was cancelled by user.");
            break;
        }

        const FailureEvent1970187& event = result.failureEvents[eventIndex];
        LeadTimeResult lead;
        lead.hostname = event.hostname;
        lead.gpu = event.gpu;
        lead.errorTimestamp = event.timestamp;
        lead.detectionTimestamp = "-";
        lead.leadTimeSeconds = -1.0;
        lead.windowType = windowType;
        lead.dataSource = windowType + "_aggregate";
        lead.proxyUsed = false;
        lead.threshold = options.anomalyThreshold;

        const auto ifIndexIt = ifIndexBySourceRow.find(event.sourceRow);
        if (ifIndexIt != ifIndexBySourceRow.end()) {
            const std::size_t row = ifIndexIt->second;
            lead.ifScore = row < iforest.scores.size() ? iforest.scores[row] : 0.0;
            lead.ifDetected = lead.ifScore >= options.anomalyThreshold;
        }

        if (useHybrid) {
            const auto hybridIndexIt = hybridIndexBySourceRow.find(event.sourceRow);
            if (hybridIndexIt != hybridIndexBySourceRow.end()) {
                const std::size_t row = hybridIndexIt->second;
                lead.hybridDetected = row < hybrid.labels.size() && hybrid.labels[row] != 0;
            }
        }

        const auto selectedIndexIt = selectedIndexBySourceRow.find(event.sourceRow);
        if (selectedIndexIt != selectedIndexBySourceRow.end()) {
            const std::size_t row = selectedIndexIt->second;
            lead.score = row < selected.scores.size() ? selected.scores[row] : lead.ifScore;
        } else {
            lead.score = lead.ifScore;
        }

        lead.algorithmDetected = useHybrid ? lead.hybridDetected : lead.ifDetected;
        lead.positive = lead.algorithmDetected;

        if (lead.positive) {
            long long errorSeconds = 0;
            if (parseTimestampSeconds(event.timestamp, errorSeconds)) {
                lead.detectionTimestamp = formatTimestampSeconds(errorSeconds - leadSeconds);
            }
            lead.leadTimeSeconds = static_cast<double>(leadSeconds);
            ++result.summary.positiveCount;
            positiveScoreSum += lead.score;
            hasPositiveLeadTime = true;
        } else {
            ++result.summary.negativeCount;
            negativeScoreSum += lead.score;
        }

        result.leadTimes.push_back(std::move(lead));
        reportProgress(options.progress, eventIndex + 1, result.failureEvents.size(), "validating 1970187 failures");
    }

    result.summary.processedEvents = result.leadTimes.size();
    if (result.summary.processedEvents > 0) {
        result.summary.detectionRate =
            static_cast<double>(result.summary.positiveCount) / static_cast<double>(result.summary.processedEvents);
    }
    if (result.summary.positiveCount > 0) {
        result.summary.meanScorePositive = positiveScoreSum / static_cast<double>(result.summary.positiveCount);
    }
    if (result.summary.negativeCount > 0) {
        result.summary.meanScoreNegative = negativeScoreSum / static_cast<double>(result.summary.negativeCount);
    }
    result.summary.proxyUsed = 0;

    result.windowDataset = std::move(dataset);
    result.prepared = std::move(prepared);
    result.graph = std::move(graph);
    result.hybridResult = std::move(selected);

    if (!hasPositiveLeadTime) {
        pushWarning(result.warnings, "No positive lead time was confirmed from is_failure rows; lead time is -1 where no anomaly was found.");
    }
    reportProgress(options.progress, 100, 100, "1970187 validation complete");
    return result;
}

bool RealFailureValidator::parseTimestampSeconds(const std::string& text, long long& seconds)
{
    std::string value = trim(text);
    if (value.empty()) {
        return false;
    }

    static const std::regex isoPattern(
        R"(^\s*([0-9]{4})-([0-9]{2})-([0-9]{2})[ T]([0-9]{2}):([0-9]{2}):([0-9]{2})(?:\.[0-9]+)?\s*([Zz]|[+-][0-9]{2}:?[0-9]{2})?.*)");
    std::smatch match;
    if (std::regex_match(value, match, isoPattern)) {
        std::tm tm{};
        tm.tm_year = std::stoi(match[1].str()) - 1900;
        tm.tm_mon = std::stoi(match[2].str()) - 1;
        tm.tm_mday = std::stoi(match[3].str());
        tm.tm_hour = std::stoi(match[4].str());
        tm.tm_min = std::stoi(match[5].str());
        tm.tm_sec = std::stoi(match[6].str());
        long long parsed = utcFromTm(&tm);

        if (match[7].matched) {
            const std::string offset = match[7].str();
            if (offset != "Z" && offset != "z") {
                const int sign = offset[0] == '-' ? -1 : 1;
                const int hours = std::stoi(offset.substr(1, 2));
                const int minutes = std::stoi(offset.substr(offset.find(':') == std::string::npos ? 3 : 4, 2));
                const int offsetSeconds = sign * (hours * 3600 + minutes * 60);
                parsed -= offsetSeconds;
            }
        }
        seconds = parsed;
        return true;
    }

    char* end = nullptr;
    const double numeric = std::strtod(value.c_str(), &end);
    if (end != value.c_str()) {
        seconds = static_cast<long long>(numeric);
        return true;
    }
    return false;
}

} // namespace telemetry
