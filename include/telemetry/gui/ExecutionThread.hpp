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
    void configureValidation(const QString& failuresPath, const QString& telemetryPath, const QString& locationsPath, double anomalyThreshold);

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
    void resultRow(int rawIndex, QString timestamp, QString hostname, QString algorithm, bool isAnomaly, double score);
    void chartReset(QString metricName);
    void chartPoint(int rawIndex, QString timestamp, double value, bool isAnomaly);
    void leadTimeRow(QString hostname, QString detectionTimestamp, QString errorTimestamp, double leadTimeSeconds, bool positive);
    void failed(QString message);

protected:
    void run() override;

private:
    void runLoad();
    void runAnalyze();
    void runValidation();
    void emitResultRows(const DetectorResult& result);
    void emitChartPoints(const DetectorResult& result);

    Task task_ = Task::Load;
    QString inputPath_;
    QString dbPath_ = "telemetry.sqlite";
    QString postgresConnection_;
    QString algorithm_ = "hybrid";
    QString failuresPath_;
    QString validationTelemetryPath_;
    QString locationsPath_;
    double anomalyThreshold_ = 0.75;

    CancellationToken cancellation_;
    TelemetryDataset dataset_;
    PreparedData prepared_;
    GraphContext graph_;
    DetectorResult lastResult_;
    DatabaseManager database_;
    bool databaseReady_ = false;
};

} // namespace telemetry::gui
