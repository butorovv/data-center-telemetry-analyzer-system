#pragma once

#include "telemetry/gui/ExecutionThread.hpp"
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QtCharts/QChartView>
#include <QMainWindow>
#include <QPointer>
#include <QVector>

class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QModelIndex;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QStandardItemModel;
class QTableView;
class QTabWidget;

namespace telemetry::gui {

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

private slots:
    void chooseInputFile();
    void chooseFailuresFile();
    void chooseValidationTelemetryFile();
    void startLoad();
    void startAnalyze();
    void startValidation();
    void cancelCurrentOperation();
    void exportResultsCsv();
    void centerChartOnSelectedRow(const QModelIndex& index);

private:
    QWidget* createLoadTab();
    QWidget* createAnalyzeTab();
    QWidget* createResultsTab();
    QWidget* createChartsTab();
    QWidget* createInfoTab();

    void attachThreadSignals(ExecutionThread* thread);
    void setBusy(bool busy);
    void appendLog(const QString& message);
    void addResultRow(int rawIndex, const QString& timestamp, const QString& hostname,
                      const QString& algorithm, bool isAnomaly, double score);
    void resetChartData(const QString& metricName);
    void addChartPoint(int rawIndex, const QString& timestamp, double value, bool isAnomaly);
    void addLeadTimeRow(const QString& hostname, const QString& detection,
                        const QString& error, double leadTimeSeconds, bool positive);
    void rebuildChart(int centerRawIndex = -1);

    QLineEdit* inputPathEdit_ = nullptr;
    QLineEdit* dbPathEdit_ = nullptr;
    QLineEdit* postgresEdit_ = nullptr;
    QLineEdit* failuresPathEdit_ = nullptr;
    QLineEdit* validationTelemetryPathEdit_ = nullptr;
    QLineEdit* locationsPathEdit_ = nullptr;
    QComboBox* algorithmCombo_ = nullptr;
    QDoubleSpinBox* thresholdSpin_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
    QPushButton* loadButton_ = nullptr;
    QPushButton* analyzeButton_ = nullptr;
    QPushButton* validateButton_ = nullptr;
    QPlainTextEdit* logEdit_ = nullptr;
    QTableView* resultsTable_ = nullptr;
    QTableView* leadTimeTable_ = nullptr;
    QStandardItemModel* resultsModel_ = nullptr;
    QStandardItemModel* leadTimeModel_ = nullptr;
    QChartView* chartView_ = nullptr;
    QPointer<ExecutionThread> currentThread_;
    QTabWidget* tabs_ = nullptr;

    QString chartMetricName_;
    QVector<int> chartRawIndices_;
    QVector<QString> chartTimestamps_;
    QVector<double> chartValues_;
    QVector<bool> chartAnomalies_;
    QVector<QString> leadTimeDetectionTimestamps_;
};

} // namespace telemetry::gui

