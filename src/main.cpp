#include "telemetry/DataLoader.hpp"
#include "telemetry/DatabaseManager.hpp"
#include "telemetry/GraphBuilder.hpp"
#include "telemetry/HybridDetector.hpp"
#include "telemetry/IsolationForestDetector.hpp"
#include "telemetry/KMeansDetector.hpp"
#include "telemetry/MetricsCalculator.hpp"
#include "telemetry/Preprocessor.hpp"
#include "telemetry/RealFailureValidator.hpp"
#include "telemetry/SummitPrototypeAdapter.hpp"
#include "telemetry/Visualizer.hpp"

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace telemetry;

namespace {

struct Options {
    std::string csvPath;
    std::string xidLogPath;
    std::string failuresPath = "second_dataset/failures.csv";
    std::string pointsPath = "second_dataset/points_with_jobs_tele_ult.csv";
    std::string locationsPath = "second_dataset/locations_by_serials.csv";
    std::string postgresConnection;
    std::filesystem::path dbPath = "telemetry.sqlite";
    std::string algorithm = "all";
    std::size_t limitRows = 0;
    std::size_t threads = 0;
    std::size_t slidingWindow = 1;
    bool plot = false;
    bool validate1970187 = false;
    bool injectSynthetic = true;
    double anomalyThreshold = 0.75;
    std::string plotMetric;
    std::string plotHost;
};

void printUsage()
{
    std::cout
        << "Usage:\n"
        << "  telemetry_analyzer --input <file.csv|file.parquet|file.tar>\n"
        << "                     [--db telemetry.sqlite] [--postgres \"connection string\"]\n"
        << "                     [--algorithm all|kmeans|iforest|hybrid]\n"
        << "                     [--xid-log log.csv] [--no-synthetic]\n"
        << "                     [--threshold 0.75] [--limit N] [--threads N] [--window N] [--plot]\n"
        << "  telemetry_analyzer --validate-1970187 [--points points_with_jobs_tele_ult.csv]\n"
        << "                     [--threshold 0.75]\n\n"
        << "--csv is kept as an alias for --input. If no input is given, the interactive menu is started.\n";
}

Options parseOptions(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto requireValue = [&](const std::string& name) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error("Missing value for " + name);
            }
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") {
            printUsage();
            std::exit(0);
        } else if (arg == "--csv" || arg == "--input") {
            options.csvPath = requireValue(arg);
        } else if (arg == "--xid-log") {
            options.xidLogPath = requireValue(arg);
            options.injectSynthetic = false;
        } else if (arg == "--failures") {
            options.failuresPath = requireValue(arg);
            options.validate1970187 = true;
        } else if (arg == "--points") {
            options.pointsPath = requireValue(arg);
            options.validate1970187 = true;
        } else if (arg == "--locations") {
            options.locationsPath = requireValue(arg);
        } else if (arg == "--validate-1970187") {
            options.validate1970187 = true;
            options.injectSynthetic = false;
        } else if (arg == "--threshold") {
            options.anomalyThreshold = std::stod(requireValue(arg));
        } else if (arg == "--postgres") {
            options.postgresConnection = requireValue(arg);
        } else if (arg == "--db") {
            options.dbPath = requireValue(arg);
        } else if (arg == "--algorithm") {
            options.algorithm = requireValue(arg);
        } else if (arg == "--limit") {
            options.limitRows = static_cast<std::size_t>(std::stoull(requireValue(arg)));
        } else if (arg == "--threads") {
            options.threads = static_cast<std::size_t>(std::stoull(requireValue(arg)));
        } else if (arg == "--window") {
            options.slidingWindow = static_cast<std::size_t>(std::stoull(requireValue(arg)));
        } else if (arg == "--plot") {
            options.plot = true;
        } else if (arg == "--no-synthetic") {
            options.injectSynthetic = false;
        } else if (arg == "--plot-metric") {
            options.plotMetric = requireValue(arg);
        } else if (arg == "--plot-host") {
            options.plotHost = requireValue(arg);
        } else {
            throw std::runtime_error("Unknown argument: " + arg);
        }
    }
    return options;
}

std::string yesNo(bool value)
{
    return value ? "yes" : "no";
}

void printMetrics(const DetectorResult& result, const Metrics& metrics)
{
    const std::size_t anomalies = std::count(result.labels.begin(), result.labels.end(), 1);
    std::cout << '\n'
              << "Algorithm: " << result.algorithm << '\n'
              << "Rows: " << result.labels.size() << ", anomalies: " << anomalies << '\n'
              << "Time: " << std::fixed << std::setprecision(2) << result.executionMs << " ms\n"
              << "Precision: " << std::setprecision(4) << metrics.precision
              << ", Recall: " << metrics.recall
              << ", F1: " << metrics.f1 << '\n'
              << "TP=" << metrics.tp << " FP=" << metrics.fp
              << " FN=" << metrics.fn << " TN=" << metrics.tn << "\n";
}

struct RuntimeState {
    TelemetryDataset dataset;
    PreparedData prepared;
    GraphContext graph;
    DatabaseManager database;
    std::optional<DetectorResult> lastResult;
    std::optional<DetectorResult> lastIsolationForest;
    bool databaseReady = false;
};

bool prepareData(RuntimeState& state, const Options& options)
{
    Preprocessor preprocessor;

    std::cout << "Loading input: " << options.csvPath << '\n';
    DataLoader loader(options.limitRows);
    CancellationToken token;
    state.dataset = loader.load(options.csvPath, &token, [](const OperationProgress& progress) {
        if (progress.current == progress.total || progress.current % 5000 == 0 || progress.percent == 100) {
            std::cout << "[progress] " << progress.percent << "% " << progress.stage << '\n';
        }
    });
    std::cout << "Loaded rows: " << state.dataset.rows.size()
              << ", numeric columns: " << state.dataset.schema.numericColumns.size() << '\n';
    for (const auto& warning : state.dataset.warnings) {
        std::cerr << "Warning: " << warning << '\n';
    }

    const std::size_t removed = preprocessor.removeRowsWithNaN(state.dataset);
    std::cout << "Removed rows with NaN: " << removed << '\n';

    if (options.injectSynthetic) {
        const auto synthetic = preprocessor.injectSyntheticAnomalies(state.dataset);
        std::cout << "Synthetic anomalies injected: " << synthetic.size() << '\n';
    } else {
        std::cout << "Synthetic anomalies skipped.\n";
    }

    preprocessor.classifyWorkload(state.dataset);
    state.prepared = preprocessor.normalize(state.dataset, options.slidingWindow);
    std::cout << "Prepared features: " << state.prepared.features.size()
              << " x " << (state.prepared.features.empty() ? 0 : state.prepared.features.front().size())
              << ", sliding window: " << options.slidingWindow << '\n';

    state.graph = GraphBuilder{}.build(state.dataset);
    std::size_t edges = 0;
    for (const auto& list : state.graph.adjacency) {
        edges += list.size();
    }
    std::cout << "Graph hosts: " << state.graph.hosts.size()
              << ", edges: " << edges / 2 << '\n';

    if (!options.postgresConnection.empty()) {
        if (!state.database.openPostgres(options.postgresConnection)) {
            std::cerr << "PostgreSQL open failed, falling back to file/SQLite storage.\n";
            state.database.open(options.dbPath);
        }
    } else if (!state.database.open(options.dbPath)) {
        std::cerr << "Database open failed.\n";
        return false;
    }
    if (!state.database.initialize(state.dataset.schema, &state.prepared)) {
        std::cerr << "Database initialization failed.\n";
        return false;
    }
    state.database.insertTelemetry(state.dataset);
    state.database.insertNormalizedFeatures(state.prepared);
    state.database.insertSyntheticAnomalies(state.dataset);
    state.databaseReady = true;

    std::cout << "Storage: " << state.database.storagePath()
              << " (SQLite: " << yesNo(state.database.sqliteEnabled())
              << ", PostgreSQL: " << yesNo(state.database.postgresEnabled()) << ")\n";
    return true;
}

DetectorResult runKMeans(const RuntimeState& state, const Options& options)
{
    KMeansConfig config;
    config.threads = options.threads;
    return KMeansDetector(config).run(state.prepared);
}

DetectorResult runIsolationForest(const RuntimeState& state, const Options& options)
{
    IsolationForestConfig config;
    config.threads = options.threads;
    config.threshold = options.anomalyThreshold;
    return IsolationForestDetector(config).run(state.prepared);
}

Metrics saveAndPrint(RuntimeState& state, const DetectorResult& result)
{
    const Metrics metrics = MetricsCalculator{}.calculate(state.dataset, result);
    printMetrics(result, metrics);
    if (state.databaseReady) {
        state.database.saveResult(result, metrics, state.dataset.rows.size());
    }
    state.lastResult = result;
    return metrics;
}

std::vector<DetectorResult> runAlgorithms(RuntimeState& state, const Options& options)
{
    std::vector<DetectorResult> results;
    const std::string algorithm = options.algorithm;

    if (algorithm == "all" || algorithm == "kmeans") {
        auto result = runKMeans(state, options);
        saveAndPrint(state, result);
        results.push_back(std::move(result));
    }

    bool needIsolationForest = algorithm == "all" || algorithm == "iforest" ||
        algorithm == "isolation_forest" || algorithm == "hybrid";
    if (needIsolationForest) {
        auto iforest = runIsolationForest(state, options);
        state.lastIsolationForest = iforest;
        if (algorithm == "all" || algorithm == "iforest" || algorithm == "isolation_forest") {
            saveAndPrint(state, iforest);
            results.push_back(iforest);
        }
    }

    if (algorithm == "all" || algorithm == "hybrid") {
        if (!state.lastIsolationForest.has_value()) {
            state.lastIsolationForest = runIsolationForest(state, options);
        }
        PrototypeRunOptions prototypeOptions;
        prototypeOptions.isolationTrees = 100;
        prototypeOptions.isolationSampleSize = 256;
        prototypeOptions.isolationThreshold = options.anomalyThreshold;
        auto hybrid = SummitPrototypeAdapter::runHybrid(state.dataset, state.prepared, state.graph, prototypeOptions);
        saveAndPrint(state, hybrid);
        results.push_back(std::move(hybrid));
    }

    return results;
}

void validateRealFailure1970187(const RuntimeState& state, const Options& options)
{
    if (options.xidLogPath.empty()) {
        return;
    }
    if (!state.lastResult.has_value() || state.lastResult->algorithm != "hybrid_iforest_graph") {
        std::cout << "XID validation requires a hybrid result; run --algorithm hybrid or --algorithm all.\n";
        return;
    }

    RealFailureValidator validator;
    const auto events = validator.loadXidEvents(options.xidLogPath);
    const auto leadTimes = validator.calculateLeadTimes(state.dataset, *state.lastResult, events, 94);
    std::cout << "\nValidation 1970187 / is_failure == 1\n";
    if (leadTimes.empty()) {
        std::cout << "No positive lead-time match was calculated. Check that telemetry and XID log timestamps/hosts align.\n";
        return;
    }
    for (const auto& item : leadTimes) {
        std::cout << "host=" << item.hostname
                  << " detection=" << item.detectionTimestamp
                  << " error=" << item.errorTimestamp
                  << " lead_time_sec=" << item.leadTimeSeconds
                  << " positive=" << yesNo(item.positive) << '\n';
    }
}

void createPlot(const RuntimeState& state, const Options& options)
{
    if (!state.lastResult.has_value()) {
        std::cout << "No detector result available for plotting.\n";
        return;
    }
    if (state.dataset.schema.numericColumns.empty() || state.dataset.rows.empty()) {
        std::cout << "No telemetry data available for plotting.\n";
        return;
    }

    std::string metric = options.plotMetric.empty() ? state.dataset.schema.numericColumns.front() : options.plotMetric;
    for (const auto& column : state.dataset.schema.numericColumns) {
        if (column.find("gpu0_core_temp") != std::string::npos) {
            metric = column;
            break;
        }
    }
    const std::string host = options.plotHost.empty() ? state.dataset.rows.front().hostname : options.plotHost;

    const std::filesystem::path csvPath = "telemetry_plot_series.csv";
    const std::filesystem::path pngPath = "telemetry_plot.png";
    Visualizer visualizer;
    if (!visualizer.exportSeries(state.dataset, *state.lastResult, metric, host, csvPath)) {
        std::cout << "Plot CSV export failed.\n";
        return;
    }
    const bool rendered = visualizer.runPythonPlot(csvPath, pngPath, "Telemetry: " + metric + " / " + host);
    std::cout << "Plot data: " << csvPath << '\n';
    if (rendered) {
        std::cout << "Plot image: " << pngPath << '\n';
    } else {
        std::cout << "Plot image was not rendered; install python with matplotlib to enable PNG output.\n";
    }
}

int runValidation1970187(const Options& options)
{
    RealFailureValidator validator;
    FailureValidationOptions validationOptions;
    validationOptions.failuresPath = options.failuresPath;
    validationOptions.telemetryPath = options.pointsPath;
    validationOptions.locationsPath = options.locationsPath;
    validationOptions.xidCode = 94;
    validationOptions.lookbackMinutes = 20;
    validationOptions.anomalyThreshold = options.anomalyThreshold;
    validationOptions.maxEvents = 0;
    CancellationToken token;
    validationOptions.cancellation = &token;
    validationOptions.progress = [](const OperationProgress& progress) {
        if (progress.percent == 100 || progress.current == progress.total || progress.current % 25000 == 0) {
            std::cout << "[progress] " << progress.percent << "% " << progress.stage << '\n';
        }
    };

    const auto validation = validator.validate1970187(validationOptions);
    std::cout << "\nValidation 1970187 / is_failure == 1\n";
    for (const auto& warning : validation.warnings) {
        std::cout << "Warning: " << warning << '\n';
    }
    std::cout << "is_failure events: " << validation.events.size() << '\n';
    std::cout << "Telemetry window rows: " << validation.windowDataset.rows.size() << '\n';
    std::cout << "Hybrid result rows: " << validation.hybridResult.labels.size() << '\n';

    const std::size_t anomalies = std::count(validation.hybridResult.labels.begin(), validation.hybridResult.labels.end(), 1);
    std::cout << "Hybrid anomalies: " << anomalies << '\n';

    bool hasPositiveLeadTime = false;
    for (const auto& item : validation.leadTimes) {
        hasPositiveLeadTime = hasPositiveLeadTime || item.positive;
        std::cout << "host=" << item.hostname
                  << " detection=" << item.detectionTimestamp
                  << " error=" << item.errorTimestamp
                  << " lead_time_sec=" << item.leadTimeSeconds
                  << " positive=" << yesNo(item.positive) << '\n';
    }
    if (!hasPositiveLeadTime) {
        std::cout << "No positive lead time was confirmed for is_failure rows.\n";
        return 3;
    }
    return 0;
}
int runBatch(const Options& options)
{
    if (options.validate1970187) {
        return runValidation1970187(options);
    }

    RuntimeState state;
    if (!prepareData(state, options)) {
        return 2;
    }
    runAlgorithms(state, options);
    validateRealFailure1970187(state, options);
    state.database.exportResultsCsv("anomaly_results_export.csv");
    std::cout << "\nResults exported: anomaly_results_export.csv\n";
    if (options.plot) {
        createPlot(state, options);
    }
    return 0;
}

std::size_t readSizeOrDefault(const std::string& prompt, std::size_t defaultValue)
{
    std::cout << prompt << " [" << defaultValue << "]: ";
    std::string value;
    std::getline(std::cin, value);
    if (value.empty()) {
        return defaultValue;
    }
    return static_cast<std::size_t>(std::stoull(value));
}

std::string readStringOrDefault(const std::string& prompt, const std::string& defaultValue)
{
    std::cout << prompt << " [" << defaultValue << "]: ";
    std::string value;
    std::getline(std::cin, value);
    return value.empty() ? defaultValue : value;
}

int runInteractive()
{
    RuntimeState state;
    Options options;

    while (true) {
        std::cout << "\nSummit telemetry analyzer\n"
                  << "1. Load CSV, preprocess, save to DB\n"
                  << "2. Run k-means\n"
                  << "3. Run Isolation Forest\n"
                  << "4. Run hybrid IF + graph\n"
                  << "5. Export anomaly results to CSV\n"
                  << "6. Plot last result\n"
                  << "7. Show graph adjacency\n"
                  << "0. Exit\n"
                  << "> ";
        std::string choice;
        std::getline(std::cin, choice);

        try {
            if (choice == "0") {
                return 0;
            }
            if (choice == "1") {
                options.csvPath = readStringOrDefault("Input path", "data/sample_telemetry.csv");
                options.dbPath = readStringOrDefault("SQLite DB path", "telemetry.sqlite");
                options.limitRows = readSizeOrDefault("Row limit, 0 means all", 0);
                options.slidingWindow = readSizeOrDefault("Sliding window", 1);
                prepareData(state, options);
            } else if (choice == "2") {
                if (state.prepared.features.empty()) {
                    std::cout << "Load data first.\n";
                    continue;
                }
                auto result = runKMeans(state, options);
                saveAndPrint(state, result);
            } else if (choice == "3") {
                if (state.prepared.features.empty()) {
                    std::cout << "Load data first.\n";
                    continue;
                }
                auto result = runIsolationForest(state, options);
                state.lastIsolationForest = result;
                saveAndPrint(state, result);
            } else if (choice == "4") {
                if (state.prepared.features.empty()) {
                    std::cout << "Load data first.\n";
                    continue;
                }
                if (!state.lastIsolationForest.has_value()) {
                    state.lastIsolationForest = runIsolationForest(state, options);
                }
                PrototypeRunOptions prototypeOptions;
                auto result = SummitPrototypeAdapter::runHybrid(state.dataset, state.prepared, state.graph, prototypeOptions);
                saveAndPrint(state, result);
            } else if (choice == "5") {
                const auto path = readStringOrDefault("Export CSV path", "anomaly_results_export.csv");
                if (state.databaseReady && state.database.exportResultsCsv(path)) {
                    std::cout << "Exported to " << path << '\n';
                } else {
                    std::cout << "No database/result data to export.\n";
                }
            } else if (choice == "6") {
                options.plotMetric = readStringOrDefault("Metric", "");
                options.plotHost = readStringOrDefault("Hostname", "");
                createPlot(state, options);
            } else if (choice == "7") {
                for (std::size_t i = 0; i < state.graph.hosts.size(); ++i) {
                    std::cout << state.graph.hosts[i] << ": ";
                    for (const std::size_t neighbor : state.graph.adjacency[i]) {
                        std::cout << state.graph.hosts[neighbor] << ' ';
                    }
                    std::cout << '\n';
                }
            }
        } catch (const std::exception& ex) {
            std::cerr << "Error: " << ex.what() << '\n';
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    try {
        Options options = parseOptions(argc, argv);
        if (options.csvPath.empty() && !options.validate1970187) {
            return runInteractive();
        }
        return runBatch(options);
    } catch (const std::exception& ex) {
        std::cerr << "Fatal error: " << ex.what() << '\n';
        return 1;
    }
}






