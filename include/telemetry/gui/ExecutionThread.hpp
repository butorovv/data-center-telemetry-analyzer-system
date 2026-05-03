#pragma once

#include "telemetry/DataLoader.hpp"
#include "telemetry/DatabaseManager.hpp"
#include "telemetry/GraphBuilder.hpp"
#include "telemetry/MetricsCalculator.hpp"
#include "telemetry/Preprocessor.hpp"
#include "telemetry/RealFailureValidator.hpp"
#include "telemetry/SummitPrototypeAdapter.hpp"
#include "telemetry/TaskControl.hpp"

#include <QThread>
#include <QString>

namespace telemetry::gui {

class ExecutionThread : public QThread {
    Q_OBJECT

public:
    enum class Task {
        Load,
        Analyze,
        Validate1970187
    };

    explicit ExecutionThread(QObject* parent = nullptr);

    void configureLoad(const QString& inputPath, const QString& dbPath, const QString& postgresConnection);
    void configureAnalyze(const QString& algorithm, double anomalyThreshold);
    void configureValidation(const QString& failuresPath, const QString& telemetryPath, const QString& locationsPath, const QString& validationAlgorithm, double anomalyThreshold, int windowMinutes);

    const TelemetryDataset& dataset() const { return dataset_; }
    const PreparedData& preparedData() const { return prepared_; }
    const DetectorResult& lastResult() const { return lastResult_; }

public slots:
    void cancel();

signals:
    void progressChanged(int percent, QString stage);
    void logMessage(QString message);
    void loadFinished(int rows, int features);
    void analysisFinished(QString algorithm, int anomalies, double precision, double recall, double f1, double ms);
    void validationFinished(QString algorithm, int positive, int negative, double detectionRate, double ms);
    void resultRow(int rawIndex, QString timestamp, QString hostname, QString algorithm, bool isAnomaly, double score);
    void chartReset(QString metricName);
    void chartPoint(int rawIndex, QString timestamp, double value, bool isAnomaly);
    void leadTimeRow(QString hostname, int gpu, QString failureTimestamp, QString windowType, double threshold, QString detectionTimestamp, double leadTimeSeconds, bool positive, bool algorithmDetected, double ifScore, bool ifDetected, bool hybridDetected);
    void failed(QString message);

protected:
    void run() override;

private:
    void runLoad();
    void runAnalyze();
    void runValidation();
    void emitResultRows(const DetectorResult& result);
    void emitChartPoints(const DetectorResult& result);
    void emitValidationChartPoints(const std::vector<LeadTimeResult>& leadTimes);

    Task task_ = Task::Load;
    QString inputPath_;
    QString dbPath_ = "telemetry.sqlite";
    QString postgresConnection_;
    QString algorithm_ = "hybrid";
    QString failuresPath_;
    QString validationTelemetryPath_;
    QString locationsPath_;
    QString validationAlgorithm_ = "hybrid";
    double anomalyThreshold_ = 0.75;
    int validationWindowMinutes_ = 15;

    CancellationToken cancellation_;
    TelemetryDataset dataset_;
    PreparedData prepared_;
    GraphContext graph_;
    DetectorResult lastResult_;
    DatabaseManager database_;
    bool databaseReady_ = false;
};

} // namespace telemetry::gui
