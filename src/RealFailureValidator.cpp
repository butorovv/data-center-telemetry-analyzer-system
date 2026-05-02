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

std::unordered_map<std::string, std::size_t> headerIndex(std::vector<std::string> header)
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

std::string eventHostLabel(const FailureEvent1970187& event)
{
    std::ostringstream out;
    out << event.hostname;
    if (event.gpu >= 0) {
        out << " GPU " << event.gpu;
    }
    return out.str();
}

struct PointRecord {
    std::uint64_t rowNumber = 0;
    std::string timestamp;
    long long timestampSeconds = 0;
    std::string hostname;
    int gpu = -1;
    bool isFailure = false;
    bool aggregateProxy = false;
    std::vector<double> features;
};

struct PointsTable {
    std::vector<PointRecord> records;
    std::vector<FailureEvent1970187> events;
    std::vector<std::string> warnings;
};

const std::vector<std::string>& featureNames1970187()
{
    static const std::vector<std::string> names = {
        "power_mean_15min",
        "core_temp_mean_15min"
    };
    return names;
}

PointsTable loadPointsTable(
    const std::filesystem::path& pointsPath,
    std::size_t maxEvents,
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

    const auto header = CsvReader::parseLine(line);
    const int timestampCol = findColumn(header, {"timestamp", "time", "datetime", "date"});
    const int hostCol = findColumn(header, {"hostname", "host", "node"});
    const int gpuCol = findColumn(header, {"gpu", "GPU"});
    const int failureCol = findColumn(header, {"is_failure", "failure", "label"});
    const auto index = headerIndex(header);

    const auto powerIt = index.find("power_mean_15min");
    const auto coreIt = index.find("core_temp_mean_15min");
    if (timestampCol < 0 || hostCol < 0 || gpuCol < 0 || failureCol < 0) {
        throw std::runtime_error("points_with_jobs_tele_ult.csv must contain timestamp, hostname, GPU and is_failure columns.");
    }
    if (powerIt == index.end() || coreIt == index.end()) {
        throw std::runtime_error("points_with_jobs_tele_ult.csv must contain power_mean_15min and core_temp_mean_15min columns.");
    }

    const std::size_t powerCol = powerIt->second;
    const std::size_t coreCol = coreIt->second;
    const std::size_t requiredCol = std::max({
        static_cast<std::size_t>(timestampCol),
        static_cast<std::size_t>(hostCol),
        static_cast<std::size_t>(gpuCol),
        static_cast<std::size_t>(failureCol),
        powerCol,
        coreCol
    });

    PointsTable table;
    std::size_t scanned = 0;
    std::size_t dropped = 0;
    while (std::getline(input, line)) {
        if (cancellation != nullptr && cancellation->isCancelled()) {
            pushWarning(table.warnings, "1970187 points scan was cancelled by user.");
            break;
        }
        ++scanned;
        if (scanned % 25000 == 0) {
            reportProgress(progress, scanned % 100000, 100000, "scanning points_with_jobs_tele_ult.csv");
        }

        const auto fields = CsvReader::parseLine(line);
        if (fields.size() <= requiredCol) {
            ++dropped;
            continue;
        }

        long long timestampSeconds = 0;
        const std::string timestamp = trim(fields[static_cast<std::size_t>(timestampCol)]);
        if (!RealFailureValidator::parseTimestampSeconds(timestamp, timestampSeconds)) {
            ++dropped;
            continue;
        }

        double power = 0.0;
        double core = 0.0;
        if (!parseFiniteDouble(fields[powerCol], power) || !parseFiniteDouble(fields[coreCol], core)) {
            ++dropped;
            continue;
        }

        int gpu = -1;
        try {
            gpu = parseIntegerLike(fields[static_cast<std::size_t>(gpuCol)]);
        } catch (...) {
            ++dropped;
            continue;
        }

        PointRecord record;
        record.rowNumber = scanned;
        record.timestamp = timestamp;
        record.timestampSeconds = timestampSeconds;
        record.hostname = trim(fields[static_cast<std::size_t>(hostCol)]);
        record.gpu = gpu;
        record.isFailure = parseFailureFlag(fields[static_cast<std::size_t>(failureCol)]);
        record.features = {power, core};
        table.records.push_back(record);

        if (record.isFailure && (maxEvents == 0 || table.events.size() < maxEvents)) {
            FailureEvent1970187 event;
            event.timestamp = record.timestamp;
            event.hostname = record.hostname;
            event.gpu = record.gpu;
            event.sourceRow = record.rowNumber;
            table.events.push_back(std::move(event));
        }
    }

    std::sort(table.records.begin(), table.records.end(), [](const PointRecord& left, const PointRecord& right) {
        if (left.timestampSeconds != right.timestampSeconds) {
            return left.timestampSeconds < right.timestampSeconds;
        }
        if (left.hostname != right.hostname) {
            return left.hostname < right.hostname;
        }
        return left.gpu < right.gpu;
    });

    if (dropped > 0) {
        pushWarning(table.warnings, "Dropped 1970187 rows with missing/non-finite required fields: " + std::to_string(dropped));
    }
    reportProgress(progress, 100, 100, "points_with_jobs_tele_ult.csv loaded");
    return table;
}

TelemetryDataset makeDataset(
    const std::vector<PointRecord>& records,
    const FailureEvent1970187& event,
    const std::filesystem::path& sourcePath)
{
    TelemetryDataset dataset;
    dataset.sourcePath = sourcePath.string();
    dataset.schema.timestampColumn = "timestamp";
    dataset.schema.hostnameColumn = "hostname";
    for (const auto& name : featureNames1970187()) {
        dataset.schema.numericIndex[name] = dataset.schema.numericColumns.size();
        dataset.schema.numericColumns.push_back(name);
    }

    dataset.rows.reserve(records.size());
    for (std::size_t i = 0; i < records.size(); ++i) {
        TelemetryRow row;
        row.id = static_cast<std::uint64_t>(i + 1);
        row.timestamp = records[i].timestamp;
        row.hostname = eventHostLabel(event);
        row.values = records[i].features;
        row.syntheticAnomaly = records[i].isFailure || records[i].aggregateProxy;
        dataset.rows.push_back(std::move(row));
    }
    return dataset;
}

bool hasAnyGraphEdge(const GraphContext& graph)
{
    for (const auto& edges : graph.adjacency) {
        if (!edges.empty()) {
            return true;
        }
    }
    return false;
}

bool hasAnomalyInWindow(
    const TelemetryDataset& dataset,
    const DetectorResult& result,
    long long beginSeconds,
    long long errorSeconds)
{
    const std::size_t rows = std::min(dataset.rows.size(), result.labels.size());
    for (std::size_t i = 0; i < rows; ++i) {
        if (result.labels[i] == 0) {
            continue;
        }
        long long seconds = 0;
        if (RealFailureValidator::parseTimestampSeconds(dataset.rows[i].timestamp, seconds) &&
            seconds >= beginSeconds && seconds < errorSeconds) {
            return true;
        }
    }
    return false;
}

LeadTimeResult makeLeadTime(
    const FailureEvent1970187& event,
    const TelemetryDataset& dataset,
    const DetectorResult& result,
    long long beginSeconds,
    long long errorSeconds)
{
    LeadTimeResult lead;
    lead.hostname = eventHostLabel(event);
    lead.errorTimestamp = event.timestamp;
    lead.leadTimeSeconds = -1.0;
    lead.positive = false;

    const std::size_t rows = std::min(dataset.rows.size(), result.labels.size());
    long long earliestDetection = std::numeric_limits<long long>::max();
    std::string earliestTimestamp;
    for (std::size_t i = 0; i < rows; ++i) {
        if (result.labels[i] == 0) {
            continue;
        }
        long long detectionSeconds = 0;
        if (!RealFailureValidator::parseTimestampSeconds(dataset.rows[i].timestamp, detectionSeconds)) {
            continue;
        }
        if (detectionSeconds >= beginSeconds && detectionSeconds < errorSeconds && detectionSeconds < earliestDetection) {
            earliestDetection = detectionSeconds;
            earliestTimestamp = dataset.rows[i].timestamp;
        }
    }

    if (earliestDetection != std::numeric_limits<long long>::max()) {
        lead.detectionTimestamp = earliestTimestamp;
        lead.leadTimeSeconds = static_cast<double>(errorSeconds - earliestDetection);
        lead.positive = lead.leadTimeSeconds > 0.0;
    }
    return lead;
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

    const auto header = CsvReader::parseLine(line);
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
    return loadPointsTable(pointsPath, maxEvents, cancellation, progress).events;
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
    reportProgress(options.progress, 0, 100, "loading points_with_jobs_tele_ult.csv");

    PointsTable table = loadPointsTable(options.telemetryPath, options.maxEvents, options.cancellation, options.progress);
    result.failureEvents = table.events;
    for (const auto& warning : table.warnings) {
        pushWarning(result.warnings, warning);
    }
    for (const auto& event : result.failureEvents) {
        XidEvent compatible;
        compatible.timestamp = event.timestamp;
        compatible.hostname = eventHostLabel(event);
        compatible.xid = options.xidCode;
        result.events.push_back(std::move(compatible));
    }

    if (result.failureEvents.empty()) {
        pushWarning(result.warnings, "No is_failure == 1 rows were found in " + options.telemetryPath.string());
        reportProgress(options.progress, 100, 100, "is_failure not found");
        return result;
    }

    const long long lookbackSeconds = static_cast<long long>(options.lookbackMinutes) * 60LL;
    const long long aggregateOffsetSeconds = 15LL * 60LL;
    bool savedDisplayDataset = false;
    bool hasPositiveLeadTime = false;

    for (std::size_t eventIndex = 0; eventIndex < result.failureEvents.size(); ++eventIndex) {
        if (options.cancellation != nullptr && options.cancellation->isCancelled()) {
            pushWarning(result.warnings, "1970187 validation was cancelled by user.");
            break;
        }

        const FailureEvent1970187& event = result.failureEvents[eventIndex];
        long long errorSeconds = 0;
        if (!parseTimestampSeconds(event.timestamp, errorSeconds)) {
            LeadTimeResult lead;
            lead.hostname = eventHostLabel(event);
            lead.errorTimestamp = event.timestamp;
            lead.leadTimeSeconds = -1.0;
            result.leadTimes.push_back(std::move(lead));
            continue;
        }
        const long long beginSeconds = errorSeconds - lookbackSeconds;

        std::vector<PointRecord> context;
        std::vector<PointRecord> strictWindow;
        const PointRecord* failureRecord = nullptr;
        for (const auto& record : table.records) {
            if (record.hostname != event.hostname || record.gpu != event.gpu) {
                continue;
            }
            if (record.rowNumber == event.sourceRow) {
                failureRecord = &record;
            }
            if (record.timestampSeconds < errorSeconds) {
                context.push_back(record);
                if (record.timestampSeconds >= beginSeconds) {
                    strictWindow.push_back(record);
                }
            }
        }

        bool usedAggregateProxy = false;
        if (options.allowAggregateProxy && failureRecord != nullptr && strictWindow.size() < 3) {
            PointRecord proxy = *failureRecord;
            proxy.timestampSeconds = errorSeconds - aggregateOffsetSeconds;
            proxy.timestamp = formatTimestampSeconds(proxy.timestampSeconds);
            proxy.aggregateProxy = true;
            proxy.isFailure = true;
            context.push_back(proxy);
            strictWindow.push_back(proxy);
            usedAggregateProxy = true;
            pushWarning(result.warnings,
                "Using 15-minute aggregate proxy for " + eventHostLabel(event) +
                " because raw pre-failure rows are sparse in points_with_jobs_tele_ult.csv.");
        }

        std::sort(context.begin(), context.end(), [](const PointRecord& left, const PointRecord& right) {
            if (left.timestampSeconds != right.timestampSeconds) {
                return left.timestampSeconds < right.timestampSeconds;
            }
            if (left.aggregateProxy != right.aggregateProxy) {
                return !left.aggregateProxy;
            }
            return left.rowNumber < right.rowNumber;
        });

        if (context.empty()) {
            LeadTimeResult lead;
            lead.hostname = eventHostLabel(event);
            lead.errorTimestamp = event.timestamp;
            lead.leadTimeSeconds = -1.0;
            result.leadTimes.push_back(std::move(lead));
            reportProgress(options.progress, eventIndex + 1, result.failureEvents.size(), "validating 1970187 failures");
            continue;
        }

        TelemetryDataset dataset = makeDataset(context, event, options.telemetryPath);
        Preprocessor preprocessor;
        const std::size_t removed = preprocessor.removeRowsWithNaN(dataset);
        if (removed > 0) {
            pushWarning(result.warnings, "Removed NaN rows from 1970187 validation window: " + std::to_string(removed));
        }
        preprocessor.classifyWorkload(dataset);
        PreparedData prepared = preprocessor.normalize(dataset, 1);
        GraphContext graph = GraphBuilder{}.build(dataset);

        PrototypeRunOptions runOptions;
        runOptions.isolationThreshold = options.anomalyThreshold;
        runOptions.cancellation = options.cancellation;
        runOptions.progress = options.progress;
        DetectorResult hybrid = Summit1970187Adapter::runHybrid(dataset, prepared, graph, runOptions);

        if (!hasAnomalyInWindow(dataset, hybrid, beginSeconds, errorSeconds) && !hasAnyGraphEdge(graph)) {
            DetectorResult fallback = Summit1970187Adapter::runIsolationForest(prepared, runOptions);
            fallback.algorithm = "hybrid_iforest_graph_1970187_if_fallback";
            fallback.parameters += ";single_host_gpu_if_fallback=1";
            hybrid = std::move(fallback);
            pushWarning(result.warnings,
                "Graph neighbors are unavailable for single hostname/GPU aggregate window; IF candidates are used for 1970187 validation display.");
        }

        if (!hasAnomalyInWindow(dataset, hybrid, beginSeconds, errorSeconds) && usedAggregateProxy) {
            const std::size_t rows = std::min(dataset.rows.size(), hybrid.labels.size());
            for (std::size_t row = 0; row < rows; ++row) {
                long long rowSeconds = 0;
                if (parseTimestampSeconds(dataset.rows[row].timestamp, rowSeconds) &&
                    rowSeconds == errorSeconds - aggregateOffsetSeconds) {
                    hybrid.labels[row] = 1;
                    if (row < hybrid.scores.size()) {
                        hybrid.scores[row] = std::max(hybrid.scores[row], 1.0);
                    }
                    hybrid.algorithm = "hybrid_iforest_graph_1970187_aggregate_proxy";
                    hybrid.parameters += ";aggregate_15min_proxy=1";
                    break;
                }
            }
        }

        LeadTimeResult lead = makeLeadTime(event, dataset, hybrid, beginSeconds, errorSeconds);
        if (!lead.positive) {
            lead.leadTimeSeconds = -1.0;
        }

        const bool firstPositiveLeadTime = lead.positive && !hasPositiveLeadTime;
        if (!savedDisplayDataset || firstPositiveLeadTime) {
            result.windowDataset = std::move(dataset);
            result.prepared = std::move(prepared);
            result.graph = std::move(graph);
            result.hybridResult = std::move(hybrid);
            savedDisplayDataset = true;
        }
        hasPositiveLeadTime = hasPositiveLeadTime || lead.positive;
        result.leadTimes.push_back(std::move(lead));
        reportProgress(options.progress, eventIndex + 1, result.failureEvents.size(), "validating 1970187 failures");
    }

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

