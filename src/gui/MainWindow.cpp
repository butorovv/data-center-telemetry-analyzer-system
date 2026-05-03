#include "telemetry/gui/MainWindow.hpp"

#include <QtCharts/QChart>
#include <QtCharts/QLineSeries>
#include <QtCharts/QScatterSeries>
#include <QtCharts/QValueAxis>

#include <QAbstractItemView>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QStringList>
#include <QTabWidget>
#include <QTableView>
#include <QTextStream>
#include <QVBoxLayout>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QStringConverter>
#endif

#include <algorithm>
#include <cmath>

namespace telemetry::gui {

namespace {

QString csvCell(const QString& value)
{
    QString escaped = value;
    escaped.replace(QStringLiteral("\""), QStringLiteral("\"\""));
    return QStringLiteral("\"") + escaped + QStringLiteral("\"");
}

void configureCsvStream(QTextStream& out)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    out.setEncoding(QStringConverter::Utf8);
#else
    out.setCodec("UTF-8");
#endif
}

void writeCsvModel(QTextStream& out, const QStringList& headers, QStandardItemModel* model)
{
    for (int col = 0; col < headers.size(); ++col) {
        if (col != 0) {
            out << ',';
        }
        out << csvCell(headers[col]);
    }
    out << "\n";

    if (model == nullptr) {
        return;
    }

    for (int row = 0; row < model->rowCount(); ++row) {
        for (int col = 0; col < model->columnCount(); ++col) {
            if (col != 0) {
                out << ',';
            }
            const QStandardItem* item = model->item(row, col);
            out << csvCell(item != nullptr ? item->text() : QString());
        }
        out << "\n";
    }
}

} // namespace
MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    tabs_ = new QTabWidget(this);
    tabs_->addTab(createLoadTab(), QStringLiteral("Загрузка"));
    tabs_->addTab(createAnalyzeTab(), QStringLiteral("Анализ"));
    tabs_->addTab(createResultsTab(), QStringLiteral("Результаты"));
    tabs_->addTab(createChartsTab(), QStringLiteral("Графики"));
    tabs_->addTab(createInfoTab(), QStringLiteral("Информация"));
    setCentralWidget(tabs_);
    statusBar()->showMessage(QStringLiteral("Готово"));
    setWindowTitle(QStringLiteral("Summit Telemetry Analyzer"));
    resize(1180, 760);
}

QWidget* MainWindow::createLoadTab()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    auto* form = new QFormLayout();

    inputPathEdit_ = new QLineEdit(page);
    auto* browseInput = new QPushButton(QStringLiteral("Выбрать"), page);
    connect(browseInput, &QPushButton::clicked, this, &MainWindow::chooseInputFile);

    auto* inputRow = new QHBoxLayout();
    inputRow->addWidget(inputPathEdit_);
    inputRow->addWidget(browseInput);

    dbPathEdit_ = new QLineEdit(QStringLiteral("telemetry.sqlite"), page);
    postgresEdit_ = new QLineEdit(page);
    postgresEdit_->setPlaceholderText(
        QStringLiteral("host=localhost port=5432 dbname=telemetry user=telemetry password=telemetry"));

    form->addRow(QStringLiteral("Файл CSV / Parquet / TAR"), inputRow);
    form->addRow(QStringLiteral("SQLite fallback"), dbPathEdit_);
    form->addRow(QStringLiteral("PostgreSQL"), postgresEdit_);
    layout->addLayout(form);

    progressBar_ = new QProgressBar(page);
    progressBar_->setRange(0, 100);
    layout->addWidget(progressBar_);

    auto* buttons = new QHBoxLayout();
    loadButton_ = new QPushButton(QStringLiteral("Загрузить в БД"), page);
    cancelButton_ = new QPushButton(QStringLiteral("Cancel"), page);
    cancelButton_->setEnabled(false);
    buttons->addWidget(loadButton_);
    buttons->addWidget(cancelButton_);
    buttons->addStretch();
    layout->addLayout(buttons);

    connect(loadButton_, &QPushButton::clicked, this, &MainWindow::startLoad);
    connect(cancelButton_, &QPushButton::clicked, this, &MainWindow::cancelCurrentOperation);

    logEdit_ = new QPlainTextEdit(page);
    logEdit_->setReadOnly(true);
    layout->addWidget(logEdit_, 1);
    return page;
}

QWidget* MainWindow::createAnalyzeTab()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* controls = new QHBoxLayout();
    algorithmCombo_ = new QComboBox(page);
    algorithmCombo_->addItems({
        QStringLiteral("k-means"),
        QStringLiteral("Isolation Forest"),
        QStringLiteral("Hybrid")
    });

    analyzeButton_ = new QPushButton(QStringLiteral("Запустить"), page);
    controls->addWidget(new QLabel(QStringLiteral("Алгоритм:"), page));
    controls->addWidget(algorithmCombo_);
    controls->addWidget(analyzeButton_);
    controls->addStretch();
    layout->addLayout(controls);

    auto* note = new QLabel(
        QStringLiteral("Нажмите Запустить после загрузки данных. Результаты появятся на вкладке Результаты, график - на вкладке Графики."),
        page);
    note->setWordWrap(true);
    layout->addWidget(note);
    layout->addStretch();
    connect(analyzeButton_, &QPushButton::clicked, this, &MainWindow::startAnalyze);
    return page;
}

QWidget* MainWindow::createResultsTab()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    resultsModel_ = new QStandardItemModel(this);
    resultsModel_->setHorizontalHeaderLabels({
        QStringLiteral("raw_id"),
        QStringLiteral("Время"),
        QStringLiteral("Хост"),
        QStringLiteral("Алгоритм"),
        QStringLiteral("Подтв."),
        QStringLiteral("Score")
    });
    resultsTable_ = new QTableView(page);
    resultsTable_->setModel(resultsModel_);
    resultsTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    resultsTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    connect(resultsTable_, &QTableView::clicked, this, &MainWindow::centerChartOnSelectedRow);
    layout->addWidget(resultsTable_, 2);

    leadTimeModel_ = new QStandardItemModel(this);
    leadTimeModel_->setHorizontalHeaderLabels({
        QStringLiteral("host"),
        QStringLiteral("GPU"),
        QStringLiteral("failure_time"),
        QStringLiteral("window_type"),
        QStringLiteral("threshold"),
        QStringLiteral("detection_time"),
        QStringLiteral("lead_time_sec"),
        QStringLiteral("positive"),
        QStringLiteral("algorithm_detected"),
        QStringLiteral("if_score"),
        QStringLiteral("if_detected"),
        QStringLiteral("hybrid_detected")
    });
    leadTimeTable_ = new QTableView(page);
    leadTimeTable_->setModel(leadTimeModel_);
    leadTimeTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    layout->addWidget(new QLabel(QStringLiteral("Валидация 1970187 / is_failure == 1"), page));
    layout->addWidget(leadTimeTable_, 1);

    failuresPathEdit_ = new QLineEdit(QStringLiteral("second_dataset/failures.csv"), page);
    validationTelemetryPathEdit_ = new QLineEdit(QStringLiteral("second_dataset/points_with_jobs_tele_ult.csv"), page);
    locationsPathEdit_ = new QLineEdit(QStringLiteral("second_dataset/locations_by_serials.csv"), page);

    auto* validationForm = new QFormLayout();
    auto* failuresRow = new QHBoxLayout();
    auto* browseFailures = new QPushButton(QStringLiteral("Файл"), page);
    failuresRow->addWidget(failuresPathEdit_);
    failuresRow->addWidget(browseFailures);
    validationForm->addRow(QStringLiteral("failures.csv (не используется)"), failuresRow);

    auto* pointsRow = new QHBoxLayout();
    auto* browsePoints = new QPushButton(QStringLiteral("Файл"), page);
    pointsRow->addWidget(validationTelemetryPathEdit_);
    pointsRow->addWidget(browsePoints);
    validationForm->addRow(QStringLiteral("points_with_jobs_tele_ult.csv"), pointsRow);
    validationForm->addRow(QStringLiteral("locations_by_serials.csv (не используется)"), locationsPathEdit_);
    layout->addLayout(validationForm);

    auto* buttons = new QHBoxLayout();
    validationAlgorithmCombo_ = new QComboBox(page);
    validationAlgorithmCombo_->addItem(QStringLiteral("isolation_forest"), QStringLiteral("isolation_forest"));
    validationAlgorithmCombo_->addItem(QStringLiteral("hybrid"), QStringLiteral("hybrid"));
    validationAlgorithmCombo_->setCurrentIndex(1);
    validationWindowCombo_ = new QComboBox(page);
    validationWindowCombo_->addItem(QStringLiteral("1 min"), 1);
    validationWindowCombo_->addItem(QStringLiteral("5 min"), 5);
    validationWindowCombo_->addItem(QStringLiteral("15 min"), 15);
    validationWindowCombo_->setCurrentIndex(2);

    thresholdSpin_ = new QDoubleSpinBox(page);
    thresholdSpin_->setRange(0.0, 1.0);
    thresholdSpin_->setSingleStep(0.01);
    thresholdSpin_->setDecimals(2);
    thresholdSpin_->setValue(0.75);

    validateButton_ = new QPushButton(QStringLiteral("\u0412\u0430\u043b\u0438\u0434\u0430\u0446\u0438\u044f 1970187"), page);
    auto* exportAnomaliesButton = new QPushButton(QStringLiteral("\u042d\u043a\u0441\u043f\u043e\u0440\u0442 \u0430\u043d\u043e\u043c\u0430\u043b\u0438\u0439 CSV"), page);
    auto* exportValidationButton = new QPushButton(QStringLiteral("\u042d\u043a\u0441\u043f\u043e\u0440\u0442 \u0432\u0430\u043b\u0438\u0434\u0430\u0446\u0438\u0438 CSV"), page);
    buttons->addWidget(new QLabel(QStringLiteral("Validation algorithm"), page));
    buttons->addWidget(validationAlgorithmCombo_);
    buttons->addWidget(new QLabel(QStringLiteral("\u041e\u043a\u043d\u043e"), page));
    buttons->addWidget(validationWindowCombo_);
    buttons->addWidget(new QLabel(QStringLiteral("IF threshold"), page));
    buttons->addWidget(thresholdSpin_);
    buttons->addWidget(validateButton_);
    buttons->addWidget(exportAnomaliesButton);
    buttons->addWidget(exportValidationButton);
    buttons->addStretch();
    layout->addLayout(buttons);

    connect(browseFailures, &QPushButton::clicked, this, &MainWindow::chooseFailuresFile);
    connect(browsePoints, &QPushButton::clicked, this, &MainWindow::chooseValidationTelemetryFile);
    connect(validateButton_, &QPushButton::clicked, this, &MainWindow::startValidation);
    connect(exportAnomaliesButton, &QPushButton::clicked, this, &MainWindow::exportResultsCsv);
    connect(exportValidationButton, &QPushButton::clicked, this, &MainWindow::exportValidationCsv);
    return page;
}

QWidget* MainWindow::createChartsTab()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    auto* chart = new QChart();
    chart->setTitle(QStringLiteral("Телеметрия и аномалии"));
    chartView_ = new QChartView(chart, page);
    chartView_->setRenderHint(QPainter::Antialiasing);
    layout->addWidget(chartView_);
    return page;
}

QWidget* MainWindow::createInfoTab()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    auto* text = new QLabel(
        QStringLiteral("Интеллектуальная система анализа телеметрии серверной инфраструктуры дата-центров.\n\n"
                       "Сборка: C++17, Qt Widgets/Qt Charts, CMake. Метрики КП: Precision, Recall, F1. "
                       "Порог Isolation Forest по умолчанию: 0.75. Валидация 1970187 использует points_with_jobs_tele_ult.csv, колонку is_failure и 15-минутные агрегаты."),
        page);
    text->setWordWrap(true);
    layout->addWidget(text);
    layout->addStretch();
    return page;
}

void MainWindow::chooseInputFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Выберите датасет"),
        QString(),
        QStringLiteral("Telemetry (*.csv *.parquet *.tar);;All files (*.*)"));
    if (!path.isEmpty()) {
        inputPathEdit_->setText(path);
    }
}

void MainWindow::chooseFailuresFile()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Выберите failures.csv"), QString(), QStringLiteral("CSV (*.csv);;All files (*.*)"));
    if (!path.isEmpty()) {
        failuresPathEdit_->setText(path);
    }
}

void MainWindow::chooseValidationTelemetryFile()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Выберите points_with_jobs_tele_ult.csv"), QString(), QStringLiteral("CSV (*.csv);;All files (*.*)"));
    if (!path.isEmpty()) {
        validationTelemetryPathEdit_->setText(path);
    }
}

void MainWindow::startLoad()
{
    if (inputPathEdit_->text().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Нет файла"), QStringLiteral("Выберите входной CSV/Parquet/TAR файл."));
        return;
    }

    if (currentThread_ && currentThread_->isRunning()) {
        QMessageBox::information(this, QStringLiteral("Операция выполняется"), QStringLiteral("Дождитесь завершения или нажмите Cancel."));
        return;
    }

    auto* thread = new ExecutionThread(this);
    thread->configureLoad(inputPathEdit_->text(), dbPathEdit_->text(), postgresEdit_->text());
    attachThreadSignals(thread);
    currentThread_ = thread;
    progressBar_->setValue(0);
    setBusy(true);
    thread->start();
}

void MainWindow::startAnalyze()
{
    if (!currentThread_ || currentThread_->dataset().empty()) {
        QMessageBox::warning(
            this,
            QStringLiteral("Нет данных"),
            QStringLiteral("Сначала загрузите датасет на вкладке Загрузка. Для проверки 1970187 используйте вкладку Результаты -> Валидация 1970187."));
        if (tabs_) {
            tabs_->setCurrentIndex(0);
        }
        return;
    }
    if (currentThread_->isRunning()) {
        QMessageBox::information(this, QStringLiteral("Операция выполняется"), QStringLiteral("Дождитесь завершения или нажмите Cancel."));
        return;
    }

    resultsModel_->removeRows(0, resultsModel_->rowCount());
    leadTimeDetectionTimestamps_.clear();
    appendLog(QStringLiteral("Запуск анализа: %1, порог IF=%2").arg(algorithmCombo_->currentText()).arg(thresholdSpin_->value()));
    statusBar()->showMessage(QStringLiteral("Выполняется анализ..."));
    if (tabs_) {
        tabs_->setCurrentIndex(2);
    }
    currentThread_->configureAnalyze(algorithmCombo_->currentText(), thresholdSpin_->value());
    progressBar_->setValue(0);
    setBusy(true);
    currentThread_->start();
}

void MainWindow::startValidation()
{
    if (validationTelemetryPathEdit_->text().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Нет данных"), QStringLiteral("Укажите points_with_jobs_tele_ult.csv."));
        return;
    }
    if (currentThread_ && currentThread_->isRunning()) {
        QMessageBox::information(this, QStringLiteral("Операция выполняется"), QStringLiteral("Дождитесь завершения или нажмите Cancel."));
        return;
    }

    if (!currentThread_) {
        currentThread_ = new ExecutionThread(this);
        attachThreadSignals(currentThread_);
    }

    resultsModel_->removeRows(0, resultsModel_->rowCount());
    leadTimeModel_->removeRows(0, leadTimeModel_->rowCount());
    leadTimeDetectionTimestamps_.clear();
    const int validationWindowMinutes = validationWindowCombo_ != nullptr
        ? validationWindowCombo_->currentData().toInt()
        : 15;
    const QString validationAlgorithm = validationAlgorithmCombo_ != nullptr
        ? validationAlgorithmCombo_->currentData().toString()
        : QStringLiteral("hybrid");
    appendLog(QStringLiteral("\u0417\u0430\u043f\u0443\u0441\u043a \u0432\u0430\u043b\u0438\u0434\u0430\u0446\u0438\u0438 1970187: algorithm=%1, threshold=%2, \u043e\u043a\u043d\u043e=%3 min.")
        .arg(validationAlgorithm)
        .arg(thresholdSpin_->value(), 0, 'f', 4)
        .arg(validationWindowMinutes));
    statusBar()->showMessage(QStringLiteral("\u0412\u044b\u043f\u043e\u043b\u043d\u044f\u0435\u0442\u0441\u044f \u0432\u0430\u043b\u0438\u0434\u0430\u0446\u0438\u044f 1970187..."));
    if (tabs_) {
        tabs_->setCurrentIndex(2);
    }
    currentThread_->configureValidation(
        failuresPathEdit_->text(),
        validationTelemetryPathEdit_->text(),
        locationsPathEdit_->text(),
        validationAlgorithm,
        thresholdSpin_->value(),
        validationWindowMinutes);
    progressBar_->setValue(0);
    setBusy(true);
    currentThread_->start();
}

void MainWindow::cancelCurrentOperation()
{
    if (currentThread_) {
        currentThread_->cancel();
        appendLog(QStringLiteral("Запрошена отмена операции."));
    }
}

void MainWindow::exportResultsCsv()
{
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("\u042d\u043a\u0441\u043f\u043e\u0440\u0442 \u0430\u043d\u043e\u043c\u0430\u043b\u0438\u0439"), QStringLiteral("anomaly_results_gui.csv"));
    if (path.isEmpty()) {
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QStringLiteral("\u041e\u0448\u0438\u0431\u043a\u0430"), QStringLiteral("\u041d\u0435 \u0443\u0434\u0430\u043b\u043e\u0441\u044c \u0441\u043e\u0437\u0434\u0430\u0442\u044c CSV."));
        return;
    }

    QTextStream out(&file);
    configureCsvStream(out);
    writeCsvModel(
        out,
        {
            QStringLiteral("raw_id"),
            QStringLiteral("timestamp"),
            QStringLiteral("hostname"),
            QStringLiteral("algorithm"),
            QStringLiteral("is_anomaly"),
            QStringLiteral("score")
        },
        resultsModel_);
}

void MainWindow::exportValidationCsv()
{
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("\u042d\u043a\u0441\u043f\u043e\u0440\u0442 \u0432\u0430\u043b\u0438\u0434\u0430\u0446\u0438\u0438 1970187"), QStringLiteral("validation_1970187_results.csv"));
    if (path.isEmpty()) {
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QStringLiteral("\u041e\u0448\u0438\u0431\u043a\u0430"), QStringLiteral("\u041d\u0435 \u0443\u0434\u0430\u043b\u043e\u0441\u044c \u0441\u043e\u0437\u0434\u0430\u0442\u044c CSV."));
        return;
    }

    QTextStream out(&file);
    configureCsvStream(out);
    writeCsvModel(
        out,
        {
            QStringLiteral("host"),
            QStringLiteral("gpu"),
            QStringLiteral("failure_time"),
            QStringLiteral("window_type"),
            QStringLiteral("threshold"),
            QStringLiteral("detection_time"),
            QStringLiteral("lead_time_sec"),
            QStringLiteral("positive"),
            QStringLiteral("algorithm_detected"),
            QStringLiteral("if_score"),
            QStringLiteral("if_detected"),
            QStringLiteral("hybrid_detected")
        },
        leadTimeModel_);
}

void MainWindow::centerChartOnSelectedRow(const QModelIndex& index)
{
    if (!index.isValid()) {
        return;
    }
    const int rawIndex = resultsModel_->item(index.row(), 0)->text().toInt();
    appendLog(QStringLiteral("Центрирование графика на raw_id=%1").arg(rawIndex));
    rebuildChart(rawIndex);
}

void MainWindow::attachThreadSignals(ExecutionThread* thread)
{
    connect(thread, &ExecutionThread::progressChanged, this, [this](int percent, const QString& stage) {
        progressBar_->setValue(percent);
        appendLog(QStringLiteral("%1%: %2").arg(percent).arg(stage));
        statusBar()->showMessage(QStringLiteral("%1%: %2").arg(percent).arg(stage));
    });
    connect(thread, &ExecutionThread::logMessage, this, &MainWindow::appendLog);
    connect(thread, &ExecutionThread::failed, this, [this](const QString& message) {
        setBusy(false);
        statusBar()->showMessage(QStringLiteral("Ошибка: %1").arg(message));
        QMessageBox::warning(this, QStringLiteral("Ошибка"), message);
        appendLog(message);
    });
    connect(thread, &ExecutionThread::loadFinished, this, [this](int rows, int features) {
        setBusy(false);
        appendLog(QStringLiteral("Загрузка завершена: строк=%1, признаков=%2").arg(rows).arg(features));
        statusBar()->showMessage(QStringLiteral("Загрузка завершена: строк=%1, признаков=%2").arg(rows).arg(features));
        if (tabs_) {
            tabs_->setCurrentIndex(1);
        }
    });
    connect(thread, &ExecutionThread::analysisFinished, this, [this](const QString& algorithm, int anomalies, double p, double r, double f1, double ms) {
        setBusy(false);
        appendLog(QStringLiteral("%1: anomalies=%2 precision=%3 recall=%4 f1=%5 time_ms=%6")
            .arg(algorithm).arg(anomalies).arg(p).arg(r).arg(f1).arg(ms));
        statusBar()->showMessage(QStringLiteral("%1 завершен: anomalies=%2, F1=%3, time=%4 ms")
            .arg(algorithm).arg(anomalies).arg(f1).arg(ms));
        rebuildChart();
        if (tabs_) {
            tabs_->setCurrentIndex(anomalies > 0 ? 2 : 3);
        }
        if (anomalies == 0) {
            QMessageBox::information(
                this,
                QStringLiteral("Анализ завершен"),
                QStringLiteral("Алгоритм завершился, но при текущем пороге аномалий не найдено. Это нормально для строгого порога 0.75; попробуйте k-means или уменьшите порог IF."));
        }
    });
    connect(thread, &ExecutionThread::resultRow, this, &MainWindow::addResultRow);
    connect(thread, &ExecutionThread::chartReset, this, &MainWindow::resetChartData);
    connect(thread, &ExecutionThread::chartPoint, this, &MainWindow::addChartPoint);
    connect(thread, &ExecutionThread::leadTimeRow, this, &MainWindow::addLeadTimeRow);
    connect(thread, &ExecutionThread::validationFinished, this, [this](const QString& algorithm, int positive, int negative, double detectionRate, double ms) {
        setBusy(false);
        appendLog(QStringLiteral("Validation %1: positive=%2 negative=%3 detection_rate=%4 time_ms=%5")
            .arg(algorithm)
            .arg(positive)
            .arg(negative)
            .arg(detectionRate, 0, 'f', 4)
            .arg(ms));
        const QString message = QStringLiteral("\u0412\u0430\u043b\u0438\u0434\u0430\u0446\u0438\u044f \u0437\u0430\u0432\u0435\u0440\u0448\u0435\u043d\u0430. \u041f\u043e\u043b\u043e\u0436\u0438\u0442\u0435\u043b\u044c\u043d\u044b\u0445 \u0441\u0440\u0430\u0431\u0430\u0442\u044b\u0432\u0430\u043d\u0438\u0439: %1, \u043e\u0442\u0440\u0438\u0446\u0430\u0442\u0435\u043b\u044c\u043d\u044b\u0445: %2.")
            .arg(positive)
            .arg(negative);
        statusBar()->showMessage(message);
        rebuildChart();
        if (tabs_) {
            tabs_->setCurrentIndex(2);
        }
        QMessageBox::information(this, QStringLiteral("\u0412\u0430\u043b\u0438\u0434\u0430\u0446\u0438\u044f \u0437\u0430\u0432\u0435\u0440\u0448\u0435\u043d\u0430"), message);
    });
    connect(thread, &QThread::finished, this, [this]() {
        setBusy(false);
    });
}

void MainWindow::setBusy(bool busy)
{
    loadButton_->setEnabled(!busy);
    analyzeButton_->setEnabled(!busy);
    validateButton_->setEnabled(!busy);
    cancelButton_->setEnabled(busy);
}

void MainWindow::appendLog(const QString& message)
{
    if (logEdit_) {
        logEdit_->appendPlainText(message);
    }
}

void MainWindow::addResultRow(int rawIndex, const QString& timestamp, const QString& hostname,
                              const QString& algorithm, bool isAnomaly, double score)
{
    QList<QStandardItem*> row;
    row << new QStandardItem(QString::number(rawIndex))
        << new QStandardItem(timestamp)
        << new QStandardItem(hostname)
        << new QStandardItem(algorithm)
        << new QStandardItem(isAnomaly ? QStringLiteral("да") : QStringLiteral("нет"))
        << new QStandardItem(QString::number(score, 'f', 4));
    resultsModel_->appendRow(row);
}

void MainWindow::resetChartData(const QString& metricName)
{
    chartMetricName_ = metricName;
    chartXAxisTitle_ = metricName == QStringLiteral("lead_time_sec")
        ? QStringLiteral("failure_time (unix sec)")
        : QStringLiteral("raw_id");
    chartRawIndices_.clear();
    chartTimestamps_.clear();
    chartValues_.clear();
    chartAnomalies_.clear();
}

void MainWindow::addChartPoint(int rawIndex, const QString& timestamp, double value, bool isAnomaly)
{
    chartRawIndices_.push_back(rawIndex);
    chartTimestamps_.push_back(timestamp);
    chartValues_.push_back(value);
    chartAnomalies_.push_back(isAnomaly);
}

void MainWindow::addLeadTimeRow(const QString& hostname, int gpu, const QString& failureTimestamp,
                                const QString& windowType, double threshold, const QString& detectionTimestamp,
                                double leadTimeSeconds, bool positive, bool algorithmDetected,
                                double ifScore, bool ifDetected, bool hybridDetected)
{
    leadTimeDetectionTimestamps_.push_back(detectionTimestamp);
    QList<QStandardItem*> row;
    row << new QStandardItem(hostname)
        << new QStandardItem(gpu >= 0 ? QString::number(gpu) : QStringLiteral("-"))
        << new QStandardItem(failureTimestamp)
        << new QStandardItem(windowType)
        << new QStandardItem(QString::number(threshold, 'f', 4))
        << new QStandardItem(detectionTimestamp)
        << new QStandardItem(QString::number(leadTimeSeconds, 'f', 2))
        << new QStandardItem(positive ? QStringLiteral("\u0434\u0430") : QStringLiteral("\u043d\u0435\u0442"))
        << new QStandardItem(algorithmDetected ? QStringLiteral("\u0434\u0430") : QStringLiteral("\u043d\u0435\u0442"))
        << new QStandardItem(QString::number(ifScore, 'f', 4))
        << new QStandardItem(ifDetected ? QStringLiteral("\u0434\u0430") : QStringLiteral("\u043d\u0435\u0442"))
        << new QStandardItem(hybridDetected ? QStringLiteral("\u0434\u0430") : QStringLiteral("\u043d\u0435\u0442"));
    leadTimeModel_->appendRow(row);
    rebuildChart();
}

void MainWindow::rebuildChart(int centerRawIndex)
{
    auto* chart = new QChart();
    chart->setTitle(QStringLiteral("Телеметрия: %1").arg(chartMetricName_.isEmpty() ? QStringLiteral("metric") : chartMetricName_));

    auto* line = new QLineSeries(chart);
    auto* anomalies = new QScatterSeries(chart);
    auto* leadTimeMarkers = new QScatterSeries(chart);
    auto* leadTimeLines = new QLineSeries(chart);
    line->setName(QStringLiteral("значение"));
    line->setPointsVisible(true);
    line->setColor(QColor(0, 136, 204));
    anomalies->setName(QStringLiteral("аномалии"));
    leadTimeMarkers->setName(QStringLiteral("lead time"));
    leadTimeLines->setName(QStringLiteral("XID lead marker"));
    anomalies->setMarkerSize(11.0);
    anomalies->setColor(Qt::red);
    leadTimeMarkers->setMarkerSize(12.0);
    leadTimeMarkers->setColor(QColor(128, 0, 180));
    leadTimeLines->setColor(QColor(128, 0, 180));

    double minY = 0.0;
    double maxY = 1.0;
    if (!chartValues_.isEmpty()) {
        minY = chartValues_.front();
        maxY = chartValues_.front();
    }

    QVector<int> leadXs;
    for (int i = 0; i < chartValues_.size(); ++i) {
        const int x = chartRawIndices_[i];
        const double y = chartValues_[i];
        line->append(x, y);
        minY = std::min(minY, y);
        maxY = std::max(maxY, y);
        if (chartAnomalies_[i]) {
            anomalies->append(x, y);
        }
        if (leadTimeDetectionTimestamps_.contains(chartTimestamps_[i])) {
            leadTimeMarkers->append(x, y);
            leadXs.push_back(x);
        }
    }

    if (std::abs(maxY - minY) < 1e-9) {
        minY -= 1.0;
        maxY += 1.0;
    }
    for (const int x : leadXs) {
        leadTimeLines->append(x, minY);
        leadTimeLines->append(x, maxY);
    }

    chart->addSeries(line);
    chart->addSeries(anomalies);
    chart->addSeries(leadTimeMarkers);
    chart->addSeries(leadTimeLines);

    auto* axisX = new QValueAxis(chart);
    auto* axisY = new QValueAxis(chart);
    axisX->setTitleText(chartXAxisTitle_);
    axisY->setTitleText(chartMetricName_);

    if (!chartRawIndices_.isEmpty()) {
        int minX = chartRawIndices_.front();
        int maxX = chartRawIndices_.back();
        if (centerRawIndex >= 0) {
            minX = std::max(0, centerRawIndex - 50);
            maxX = centerRawIndex + 50;
        }
        if (minX == maxX) {
            minX -= 1;
            maxX += 1;
        }
        axisX->setRange(minX, maxX);
    }
    axisY->setRange(minY, maxY);

    chart->addAxis(axisX, Qt::AlignBottom);
    chart->addAxis(axisY, Qt::AlignLeft);
    line->attachAxis(axisX);
    line->attachAxis(axisY);
    anomalies->attachAxis(axisX);
    anomalies->attachAxis(axisY);
    leadTimeMarkers->attachAxis(axisX);
    leadTimeMarkers->attachAxis(axisY);
    leadTimeLines->attachAxis(axisX);
    leadTimeLines->attachAxis(axisY);

    chartView_->setChart(chart);
}

} // namespace telemetry::gui







