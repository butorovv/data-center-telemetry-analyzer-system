#include "telemetry/gui/ExecutionThread.hpp"

#include <algorithm>
#include <filesystem>

namespace telemetry::gui {

ExecutionThread::ExecutionThread(QObject* parent)
    : QThread(parent)
{
}

void ExecutionThread::configureLoad(const QString& inputPath, const QString& dbPath, const QString& postgresConnection)
{
    task_ = Task::Load;
    inputPath_ = inputPath;
    dbPath_ = dbPath.isEmpty() ? QStringLiteral("telemetry.sqlite") : dbPath;
    postgresConnection_ = postgresConnection;
    cancellation_.reset();
}

void ExecutionThread::configureAnalyze(const QString& algorithm, double anomalyThreshold)
{
    task_ = Task::Analyze;
    algorithm_ = algorithm;
    anomalyThreshold_ = anomalyThreshold;
    cancellation_.reset();
}

void ExecutionThread::configureValidation(
    const QString& failuresPath,
    const QString& telemetryPath,
    const QString& locationsPath,
    double anomalyThreshold)
{
    task_ = Task::Validate1970187;
    failuresPath_ = failuresPath;
    validationTelemetryPath_ = telemetryPath;
    locationsPath_ = locationsPath;
    anomalyThreshold_ = anomalyThreshold;
    cancellation_.reset();
}

void ExecutionThread::cancel()
{
    cancellation_.cancel();
}

void ExecutionThread::run()
{
    try {
        if (task_ == Task::Load) {
            runLoad();
        } else if (task_ == Task::Analyze) {
            runAnalyze();
        } else {
            runValidation();
        }
    } catch (const std::exception& error) {
        emit failed(QString::fromStdString(error.what()));
    }
}

void ExecutionThread::runLoad()
{
    emit logMessage(QStringLiteral("Загрузка данных..."));
    DataLoader loader;
    dataset_ = loader.load(
        inputPath_.toStdString(),
        &cancellation_,
        [this](const OperationProgress& progress) {
            emit progressChanged(progress.percent, QString::fromStdString(progress.stage));
        });

    for (const auto& warning : dataset_.warnings) {
        emit logMessage(QString::fromStdString("Предупреждение: " + warning));
    }

    Preprocessor preprocessor;
    const std::size_t removed = preprocessor.removeRowsWithNaN(dataset_);
    emit logMessage(QStringLiteral("Удалено строк с NaN: %1").arg(static_cast<qulonglong>(removed)));
    preprocessor.classifyWorkload(dataset_);
    prepared_ = preprocessor.normalize(dataset_, 1);
    graph_ = GraphBuilder{}.build(dataset_);

    if (!postgresConnection_.isEmpty()) {
        database_.openPostgres(postgresConnection_.toStdString());
    }
    if (!database_.postgresEnabled()) {
        database_.open(dbPath_.toStdString());
    }
    databaseReady_ = database_.initialize(dataset_.schema, &prepared_);
    if (databaseReady_) {
        database_.insertTelemetry(dataset_);
        database_.insertNormalizedFeatures(prepared_);
    }

    emit progressChanged(100, QStringLiteral("готово"));
    emit loadFinished(static_cast<int>(dataset_.rows.size()), static_cast<int>(prepared_.featureNames.size()));
}

void ExecutionThread::runAnalyze()
{
    if (prepared_.features.empty()) {
        emit failed(QStringLiteral("Сначала загрузите данные."));
        return;
    }

    PrototypeRunOptions options;
    options.isolationThreshold = anomalyThreshold_;
    options.cancellation = &cancellation_;
    options.progress = [this](const OperationProgress& progress) {
        emit progressChanged(progress.percent, QString::fromStdString(progress.stage));
    };

    const QString algorithm = algorithm_.toLower();
    if (algorithm == QStringLiteral("k-means")) {
        lastResult_ = SummitPrototypeAdapter::runKMeans(prepared_, options);
    } else if (algorithm == QStringLiteral("isolation forest")) {
        lastResult_ = SummitPrototypeAdapter::runIsolationForest(prepared_, options);
    } else {
        lastResult_ = SummitPrototypeAdapter::runHybrid(dataset_, prepared_, graph_, options);
    }

    Metrics metrics = MetricsCalculator{}.calculate(dataset_, lastResult_);
    if (databaseReady_) {
        database_.saveResult(lastResult_, metrics, dataset_.rows.size());
    }
    emitResultRows(lastResult_);
    emitChartPoints(lastResult_);
    const int anomalies = static_cast<int>(std::count(lastResult_.labels.begin(), lastResult_.labels.end(), 1));
    emit analysisFinished(
        QString::fromStdString(lastResult_.algorithm),
        anomalies,
        metrics.precision,
        metrics.recall,
        metrics.f1,
        lastResult_.executionMs);
}

void ExecutionThread::runValidation()
{
    if (validationTelemetryPath_.isEmpty()) {
        emit failed(QStringLiteral("Для проверки 1970187 нужен points_with_jobs_tele_ult.csv."));
        return;
    }

    RealFailureValidator validator;
    FailureValidationOptions options;
    options.failuresPath = failuresPath_.toStdString();
    options.telemetryPath = validationTelemetryPath_.toStdString();
    options.locationsPath = locationsPath_.toStdString();
    options.xidCode = 94;
    options.lookbackMinutes = 20;
    options.anomalyThreshold = anomalyThreshold_;
    options.maxEvents = 0;
    options.cancellation = &cancellation_;
    options.progress = [this](const OperationProgress& progress) {
        emit progressChanged(progress.percent, QString::fromStdString(progress.stage));
    };

    const auto validation = validator.validate1970187(options);
    for (const auto& warning : validation.warnings) {
        emit logMessage(QString::fromStdString("Валидация 1970187: " + warning));
    }

    if (validation.events.empty()) {
        emit failed(QStringLiteral("В points_with_jobs_tele_ult.csv не найдены строки is_failure == 1. Lead time невозможно рассчитать."));
        return;
    }
    if (validation.windowDataset.rows.empty() || validation.hybridResult.labels.empty()) {
        emit failed(QStringLiteral("Не удалось построить окно телеметрии и результат адаптера 1970187."));
        return;
    }

    dataset_ = validation.windowDataset;
    prepared_ = validation.prepared;
    graph_ = validation.graph;
    lastResult_ = validation.hybridResult;

    Metrics metrics = MetricsCalculator{}.calculate(dataset_, lastResult_);
    if (databaseReady_) {
        database_.saveResult(lastResult_, metrics, dataset_.rows.size());
    }
    emitResultRows(lastResult_);
    emitChartPoints(lastResult_);
    for (const auto& item : validation.leadTimes) {
        emit leadTimeRow(
            QString::fromStdString(item.hostname),
            QString::fromStdString(item.detectionTimestamp),
            QString::fromStdString(item.errorTimestamp),
            item.leadTimeSeconds,
            item.positive);
    }

    const int anomalies = static_cast<int>(std::count(lastResult_.labels.begin(), lastResult_.labels.end(), 1));
    emit analysisFinished(
        QStringLiteral("1970187 is_failure validation"),
        anomalies,
        metrics.precision,
        metrics.recall,
        metrics.f1,
        lastResult_.executionMs);
    emit progressChanged(100, QStringLiteral("валидация завершена"));
}

void ExecutionThread::emitResultRows(const DetectorResult& result)
{
    const std::size_t rows = std::min(dataset_.rows.size(), result.labels.size());
    for (std::size_t i = 0; i < rows; ++i) {
        if (!result.labels[i]) {
            continue;
        }
        emit resultRow(
            static_cast<int>(i),
            QString::fromStdString(dataset_.rows[i].timestamp),
            QString::fromStdString(dataset_.rows[i].hostname),
            QString::fromStdString(result.algorithm),
            result.labels[i] != 0,
            i < result.scores.size() ? result.scores[i] : 0.0);
    }
}

void ExecutionThread::emitChartPoints(const DetectorResult& result)
{
    if (dataset_.rows.empty() || dataset_.schema.numericColumns.empty()) {
        return;
    }

    std::size_t metricIndex = 0;
    const std::vector<std::string> preferredMetrics = {
        "gpu0_core_temp",
        "p0_core_temp_mean",
        "core_temp_mean_15min",
        "power_mean_15min"
    };
    for (const auto& preferred : preferredMetrics) {
        auto it = std::find(dataset_.schema.numericColumns.begin(), dataset_.schema.numericColumns.end(), preferred);
        if (it != dataset_.schema.numericColumns.end()) {
            metricIndex = static_cast<std::size_t>(std::distance(dataset_.schema.numericColumns.begin(), it));
            break;
        }
    }

    emit chartReset(QString::fromStdString(dataset_.schema.numericColumns[metricIndex]));
    const std::size_t rows = std::min(dataset_.rows.size(), result.labels.size());
    for (std::size_t i = 0; i < rows; ++i) {
        const auto& row = dataset_.rows[i];
        const double value = metricIndex < row.values.size() ? row.values[metricIndex] : 0.0;
        emit chartPoint(
            static_cast<int>(i),
            QString::fromStdString(row.timestamp),
            value,
            result.labels[i] != 0);
    }
}

} // namespace telemetry::gui


