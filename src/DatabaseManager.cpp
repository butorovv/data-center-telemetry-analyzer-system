#include "telemetry/DatabaseManager.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#ifdef HAS_SQLITE3
#include <sqlite3.h>
#endif

#ifdef HAS_PQXX
#include <pqxx/pqxx>
#endif

namespace telemetry {
namespace {

std::filesystem::path fallbackDirectory(const std::filesystem::path& base)
{
    std::filesystem::path dir = base;
    dir += ".files";
    return dir;
}

bool fileExists(const std::filesystem::path& path)
{
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

#ifdef HAS_SQLITE3
bool execSql(sqlite3* db, const std::string& sql)
{
    char* error = nullptr;
    const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error);
    if (rc != SQLITE_OK) {
        std::cerr << "SQLite error: " << (error ? error : "unknown") << '\n';
        sqlite3_free(error);
        return false;
    }
    return true;
}
#endif

} // namespace

DatabaseManager::DatabaseManager() = default;

DatabaseManager::~DatabaseManager()
{
#ifdef HAS_SQLITE3
    if (db_ != nullptr) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
#endif
}

bool DatabaseManager::openPostgres(const std::string& connectionString)
{
#ifdef HAS_PQXX
    try {
        pg_ = std::make_unique<pqxx::connection>(connectionString);
        return pg_->is_open();
    } catch (const std::exception& error) {
        std::cerr << "Cannot open PostgreSQL connection: " << error.what() << '\n';
        pg_.reset();
        return false;
    }
#else
    (void)connectionString;
    std::cerr << "PostgreSQL support is not compiled in. Rebuild with libpqxx.\n";
    return false;
#endif
}

bool DatabaseManager::open(const std::filesystem::path& path)
{
    storagePath_ = path;
#ifdef HAS_SQLITE3
    if (db_ != nullptr) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
    if (sqlite3_open(path.string().c_str(), &db_) != SQLITE_OK) {
        std::cerr << "Cannot open SQLite database: " << path << '\n';
        db_ = nullptr;
        return false;
    }
    execSql(db_, "PRAGMA journal_mode=WAL;");
    execSql(db_, "PRAGMA synchronous=NORMAL;");
    return true;
#else
    opened_ = true;
    std::filesystem::create_directories(fallbackDirectory(storagePath_));
    return true;
#endif
}

bool DatabaseManager::initialize(const TelemetrySchema& schema, const PreparedData* normalized)
{
    schema_ = schema;
    normalizedFeatureNames_ = normalized != nullptr && !normalized->featureNames.empty()
        ? normalized->featureNames
        : schema.numericColumns;

#ifdef HAS_PQXX
    if (pg_ && pg_->is_open()) {
        try {
            pqxx::work tx(*pg_);
            tx.exec(R"SQL(
                CREATE TABLE IF NOT EXISTS raw_telemetry (
                    id SERIAL PRIMARY KEY,
                    timestamp TIMESTAMP NOT NULL,
                    hostname TEXT NOT NULL,
                    p0_power REAL, p1_power REAL,
                    ps0_input_power REAL, ps1_input_power REAL,
                    gpu0_core_temp REAL, gpu1_core_temp REAL,
                    gpu2_core_temp REAL, gpu3_core_temp REAL,
                    gpu4_core_temp REAL, gpu5_core_temp REAL,
                    p0_core_temp_mean REAL
                );
                CREATE TABLE IF NOT EXISTS normalized_features (
                    id SERIAL PRIMARY KEY,
                    raw_id INTEGER REFERENCES raw_telemetry(id) ON DELETE CASCADE,
                    features_vector REAL[],
                    mean REAL,
                    stddev REAL
                );
                CREATE TABLE IF NOT EXISTS anomaly_results (
                    id SERIAL PRIMARY KEY,
                    algorithm_name TEXT NOT NULL,
                    raw_id INTEGER REFERENCES raw_telemetry(id),
                    is_anomaly BOOLEAN NOT NULL,
                    anomaly_score REAL,
                    execution_time_ms REAL,
                    analysis_timestamp TIMESTAMP DEFAULT NOW()
                );
                CREATE TABLE IF NOT EXISTS execution_log (
                    id SERIAL PRIMARY KEY,
                    start_time TIMESTAMP,
                    end_time TIMESTAMP,
                    algorithm_name TEXT,
                    total_rows_processed INTEGER,
                    parameters JSONB
                );
            )SQL");
            tx.commit();
            return true;
        } catch (const std::exception& error) {
            std::cerr << "PostgreSQL schema error: " << error.what() << '\n';
            return false;
        }
    }
#endif

#ifdef HAS_SQLITE3
    if (db_ == nullptr) {
        return false;
    }

    const char* drops[] = {
        "DROP TABLE IF EXISTS raw_telemetry;",
        "DROP TABLE IF EXISTS normalized_features;",
        "DROP TABLE IF EXISTS anomaly_results;",
        "DROP TABLE IF EXISTS synthetic_anomalies;",
        "DROP TABLE IF EXISTS execution_log;"
    };
    for (const char* drop : drops) {
        if (!execSql(db_, drop)) {
            return false;
        }
    }

    std::ostringstream telemetry;
    telemetry << "CREATE TABLE raw_telemetry ("
              << "id INTEGER PRIMARY KEY,"
              << "timestamp TEXT NOT NULL,"
              << "hostname TEXT NOT NULL";
    for (const auto& column : rawTelemetryColumns()) {
        telemetry << ',' << quoteIdentifier(column) << " REAL";
    }
    telemetry << ");";

    const std::string normalizedSql =
        "CREATE TABLE normalized_features ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "raw_id INTEGER,"
        "features_vector TEXT,"
        "mean REAL,"
        "stddev REAL,"
        "FOREIGN KEY(raw_id) REFERENCES raw_telemetry(id) ON DELETE CASCADE"
        ");";

    const std::string syntheticSql =
        "CREATE TABLE synthetic_anomalies ("
        "record_id INTEGER PRIMARY KEY,"
        "description TEXT"
        ");";

    const std::string executionSql =
        "CREATE TABLE execution_log ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "start_time TEXT,"
        "end_time TEXT,"
        "algorithm_name TEXT,"
        "total_rows_processed INTEGER,"
        "parameters TEXT"
        ");";

    const std::string resultsSql =
        "CREATE TABLE anomaly_results ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "algorithm_name TEXT NOT NULL,"
        "raw_id INTEGER,"
        "is_anomaly INTEGER,"
        "anomaly_score REAL,"
        "execution_time_ms REAL,"
        "analysis_timestamp TEXT,"
        "FOREIGN KEY(raw_id) REFERENCES raw_telemetry(id)"
        ");";

    return execSql(db_, telemetry.str()) &&
        execSql(db_, normalizedSql.str()) &&
        execSql(db_, syntheticSql) &&
        execSql(db_, executionSql) &&
        execSql(db_, resultsSql);
#else
    if (!opened_) {
        return false;
    }
    const auto dir = fallbackDirectory(storagePath_);
    std::filesystem::create_directories(dir);
    for (const char* name : {
             "telemetry.csv",
             "normalized_features.csv",
             "synthetic_anomalies.csv",
             "execution_log.csv",
             "anomaly_results.csv",
             "schema.sql"
         }) {
        std::error_code ec;
        std::filesystem::remove(dir / name, ec);
    }
    std::ofstream schemaFile(dir / "schema.sql");
    schemaFile << "CREATE TABLE raw_telemetry(id,timestamp,hostname";
    for (const auto& column : rawTelemetryColumns()) {
        schemaFile << ',' << column;
    }
    schemaFile << ");\n";
    schemaFile << "CREATE TABLE normalized_features(id,raw_id,features_vector,mean,stddev);\n";
    schemaFile << "CREATE TABLE synthetic_anomalies(record_id,description);\n";
    schemaFile << "CREATE TABLE execution_log(id,start_time,end_time,algorithm_name,total_rows_processed,parameters);\n";
    schemaFile << "CREATE TABLE anomaly_results(id,algorithm_name,raw_id,is_anomaly,anomaly_score,execution_time_ms,analysis_timestamp);\n";
    return true;
#endif
}

bool DatabaseManager::insertTelemetry(const TelemetryDataset& dataset)
{
#ifdef HAS_PQXX
    if (pg_ && pg_->is_open()) {
        try {
            pqxx::work tx(*pg_);
            for (const auto& row : dataset.rows) {
                bool ok = false;
                std::ostringstream sql;
                sql << "INSERT INTO raw_telemetry(timestamp,hostname";
                for (const auto& column : rawTelemetryColumns()) {
                    sql << ',' << column;
                }
                sql << ") VALUES (" << tx.quote(row.timestamp) << ',' << tx.quote(row.hostname);
                for (const auto& column : rawTelemetryColumns()) {
                    const double value = valueByColumn(row, column, ok);
                    if (ok) {
                        sql << ',' << value;
                    } else {
                        sql << ",NULL";
                    }
                }
                sql << ");";
                tx.exec(sql.str());
            }
            tx.commit();
            return true;
        } catch (const std::exception& error) {
            std::cerr << "PostgreSQL insert telemetry error: " << error.what() << '\n';
            return false;
        }
    }
#endif

#ifdef HAS_SQLITE3
    if (db_ == nullptr) {
        return false;
    }
    std::ostringstream sql;
    sql << "INSERT INTO raw_telemetry(id,timestamp,hostname";
    for (const auto& column : rawTelemetryColumns()) {
        sql << ',' << quoteIdentifier(column);
    }
    sql << ") VALUES (?,?,?";
    for (std::size_t i = 0; i < rawTelemetryColumns().size(); ++i) {
        sql << ",?";
    }
    sql << ");";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.str().c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    execSql(db_, "BEGIN TRANSACTION;");
    for (const auto& row : dataset.rows) {
        sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(row.id));
        sqlite3_bind_text(stmt, 2, row.timestamp.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, row.hostname.c_str(), -1, SQLITE_TRANSIENT);
        for (std::size_t i = 0; i < rawTelemetryColumns().size(); ++i) {
            bool ok = false;
            const double value = valueByColumn(row, rawTelemetryColumns()[i], ok);
            if (ok) {
                sqlite3_bind_double(stmt, static_cast<int>(4 + i), value);
            } else {
                sqlite3_bind_null(stmt, static_cast<int>(4 + i));
            }
        }
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            execSql(db_, "ROLLBACK;");
            return false;
        }
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    }
    sqlite3_finalize(stmt);
    return execSql(db_, "COMMIT;");
#else
    const auto path = fallbackDirectory(storagePath_) / "raw_telemetry.csv";
    std::ofstream out(path);
    out << "id,timestamp,hostname";
    for (const auto& column : rawTelemetryColumns()) {
        out << ',' << csvEscape(column);
    }
    out << '\n';
    for (const auto& row : dataset.rows) {
        out << row.id << ',' << csvEscape(row.timestamp) << ',' << csvEscape(row.hostname);
        for (const auto& column : rawTelemetryColumns()) {
            bool ok = false;
            const double value = valueByColumn(row, column, ok);
            if (ok) {
                out << ',' << value;
            } else {
                out << ',';
            }
        }
        out << '\n';
    }
    return true;
#endif
}

bool DatabaseManager::insertNormalizedFeatures(const PreparedData& data)
{
    if (data.features.empty()) {
        return true;
    }
#ifdef HAS_PQXX
    if (pg_ && pg_->is_open()) {
        try {
            pqxx::work tx(*pg_);
            for (std::size_t row = 0; row < data.features.size(); ++row) {
                tx.exec_params(
                    "INSERT INTO normalized_features(raw_id,features_vector,mean,stddev) "
                    "VALUES ($1,$2::real[],$3,$4)",
                    static_cast<int>(data.rowIds[row]),
                    postgresArray(data.features[row]),
                    data.means.empty() ? 0.0 : data.means.front(),
                    data.stddevs.empty() ? 0.0 : data.stddevs.front());
            }
            tx.commit();
            return true;
        } catch (const std::exception& error) {
            std::cerr << "PostgreSQL insert normalized features error: " << error.what() << '\n';
            return false;
        }
    }
#endif

#ifdef HAS_SQLITE3
    if (db_ == nullptr) {
        return false;
    }
    const char* sql =
        "INSERT INTO normalized_features(raw_id,features_vector,mean,stddev) VALUES (?,?,?,?);";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    execSql(db_, "BEGIN TRANSACTION;");
    for (std::size_t row = 0; row < data.features.size(); ++row) {
        sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(data.rowIds[row]));
        const std::string vector = joinDoubles(data.features[row]);
        sqlite3_bind_text(stmt, 2, vector.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 3, data.means.empty() ? 0.0 : data.means.front());
        sqlite3_bind_double(stmt, 4, data.stddevs.empty() ? 0.0 : data.stddevs.front());
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            execSql(db_, "ROLLBACK;");
            return false;
        }
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    }
    sqlite3_finalize(stmt);
    return execSql(db_, "COMMIT;");
#else
    const auto path = fallbackDirectory(storagePath_) / "normalized_features.csv";
    std::ofstream out(path);
    out << "id,raw_id,features_vector,mean,stddev";
    out << '\n';
    for (std::size_t row = 0; row < data.features.size(); ++row) {
        out << (row + 1) << ',' << data.rowIds[row] << ',' << csvEscape(joinDoubles(data.features[row]))
            << ',' << (data.means.empty() ? 0.0 : data.means.front())
            << ',' << (data.stddevs.empty() ? 0.0 : data.stddevs.front());
        out << '\n';
    }
    return true;
#endif
}

bool DatabaseManager::insertSyntheticAnomalies(const TelemetryDataset& dataset)
{
#ifdef HAS_SQLITE3
    if (db_ == nullptr) {
        return false;
    }
    const char* sql = "INSERT OR REPLACE INTO synthetic_anomalies(record_id,description) VALUES (?,?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    execSql(db_, "BEGIN TRANSACTION;");
    for (const auto& row : dataset.rows) {
        if (!row.syntheticAnomaly) {
            continue;
        }
        sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(row.id));
        const std::string description = "gpu_temp_plus_15_cpu_minus_20_percent";
        sqlite3_bind_text(stmt, 2, description.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            execSql(db_, "ROLLBACK;");
            return false;
        }
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    }
    sqlite3_finalize(stmt);
    return execSql(db_, "COMMIT;");
#else
    const auto path = fallbackDirectory(storagePath_) / "synthetic_anomalies.csv";
    std::ofstream out(path);
    out << "record_id,description\n";
    for (const auto& row : dataset.rows) {
        if (row.syntheticAnomaly) {
            out << row.id << ",gpu_temp_plus_15_cpu_minus_20_percent\n";
        }
    }
    return true;
#endif
}

bool DatabaseManager::saveResult(const DetectorResult& result, const Metrics& metrics, std::size_t rowCount)
{
    (void)metrics;
#ifdef HAS_PQXX
    if (pg_ && pg_->is_open()) {
        try {
            pqxx::work tx(*pg_);
            std::ostringstream params;
            params << "{\"precision\":" << metrics.precision
                   << ",\"recall\":" << metrics.recall
                   << ",\"f1\":" << metrics.f1
                   << ",\"raw_parameters\":\"" << result.parameters << "\"}";
            tx.exec_params(
                "INSERT INTO execution_log(start_time,end_time,algorithm_name,total_rows_processed,parameters) "
                "VALUES (NOW(),NOW(),$1,$2,$3::jsonb)",
                result.algorithm,
                static_cast<int>(rowCount),
                params.str());
            for (std::size_t i = 0; i < result.labels.size(); ++i) {
                const std::uint64_t rowId = i < result.rowIds.size() ? result.rowIds[i] : static_cast<std::uint64_t>(i + 1);
                tx.exec_params(
                    "INSERT INTO anomaly_results(algorithm_name,raw_id,is_anomaly,anomaly_score,execution_time_ms) "
                    "VALUES ($1,$2,$3,$4,$5)",
                    result.algorithm,
                    static_cast<int>(rowId),
                    result.labels[i] != 0,
                    i < result.scores.size() ? result.scores[i] : 0.0,
                    result.executionMs);
            }
            tx.commit();
            return true;
        } catch (const std::exception& error) {
            std::cerr << "PostgreSQL save result error: " << error.what() << '\n';
            return false;
        }
    }
#endif

#ifdef HAS_SQLITE3
    if (db_ == nullptr) {
        return false;
    }

    const char* logSql =
        "INSERT INTO execution_log(start_time,end_time,algorithm_name,total_rows_processed,parameters) "
        "VALUES (?,?,?,?,?);";
    sqlite3_stmt* logStmt = nullptr;
    if (sqlite3_prepare_v2(db_, logSql, -1, &logStmt, nullptr) != SQLITE_OK) {
        return false;
    }
    const std::string now = nowIso();
    sqlite3_bind_text(logStmt, 1, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(logStmt, 2, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(logStmt, 3, result.algorithm.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(logStmt, 4, static_cast<sqlite3_int64>(rowCount));
    sqlite3_bind_text(logStmt, 5, result.parameters.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(logStmt) != SQLITE_DONE) {
        sqlite3_finalize(logStmt);
        return false;
    }
    sqlite3_finalize(logStmt);
    currentRunId_ = static_cast<int>(sqlite3_last_insert_rowid(db_));

    const char* resultSql =
        "INSERT INTO anomaly_results(algorithm_name,raw_id,is_anomaly,anomaly_score,execution_time_ms,analysis_timestamp) "
        "VALUES (?,?,?,?,?,?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, resultSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    execSql(db_, "BEGIN TRANSACTION;");
    for (std::size_t i = 0; i < result.labels.size(); ++i) {
        const std::uint64_t rowId = i < result.rowIds.size() ? result.rowIds[i] : static_cast<std::uint64_t>(i + 1);
        sqlite3_bind_text(stmt, 1, result.algorithm.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(rowId));
        sqlite3_bind_int(stmt, 3, result.labels[i] != 0 ? 1 : 0);
        sqlite3_bind_double(stmt, 4, i < result.scores.size() ? result.scores[i] : 0.0);
        sqlite3_bind_double(stmt, 5, result.executionMs);
        sqlite3_bind_text(stmt, 6, now.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            execSql(db_, "ROLLBACK;");
            return false;
        }
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    }
    sqlite3_finalize(stmt);
    return execSql(db_, "COMMIT;");
#else
    const auto dir = fallbackDirectory(storagePath_);
    std::filesystem::create_directories(dir);
    ++currentRunId_;

    const auto logPath = dir / "execution_log.csv";
    const bool writeLogHeader = !fileExists(logPath);
    std::ofstream log(logPath, std::ios::app);
    if (writeLogHeader) {
        log << "id,start_time,end_time,algorithm_name,total_rows_processed,parameters\n";
    }
    const std::string now = nowIso();
    log << currentRunId_ << ',' << csvEscape(now) << ',' << csvEscape(now) << ','
        << csvEscape(result.algorithm) << ',' << rowCount << ',' << csvEscape(result.parameters) << '\n';

    const auto resultPath = dir / "anomaly_results.csv";
    const bool writeHeader = !fileExists(resultPath);
    std::ofstream out(resultPath, std::ios::app);
    if (writeHeader) {
        out << "id,algorithm_name,raw_id,is_anomaly,anomaly_score,execution_time_ms,analysis_timestamp\n";
    }
    for (std::size_t i = 0; i < result.labels.size(); ++i) {
        const std::uint64_t rowId = i < result.rowIds.size() ? result.rowIds[i] : static_cast<std::uint64_t>(i + 1);
        out << (i + 1) << ',' << csvEscape(result.algorithm) << ',' << rowId << ','
            << (result.labels[i] != 0 ? 1 : 0) << ',' << (i < result.scores.size() ? result.scores[i] : 0.0)
            << ',' << result.executionMs << ',' << csvEscape(now) << '\n';
    }
    return true;
#endif
}

bool DatabaseManager::exportResultsCsv(const std::filesystem::path& path) const
{
#ifdef HAS_SQLITE3
    if (db_ == nullptr) {
        return false;
    }
    const char* sql =
        "SELECT id,algorithm_name,raw_id,is_anomaly,anomaly_score,execution_time_ms,analysis_timestamp "
        "FROM anomaly_results ORDER BY id,raw_id;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    std::ofstream out(path);
    out << "id,algorithm_name,raw_id,is_anomaly,anomaly_score,execution_time_ms,analysis_timestamp\n";
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* algorithmText = sqlite3_column_text(stmt, 1);
        const unsigned char* timestampText = sqlite3_column_text(stmt, 6);
        out << sqlite3_column_int(stmt, 0) << ','
            << csvEscape(algorithmText ? reinterpret_cast<const char*>(algorithmText) : "") << ','
            << sqlite3_column_int64(stmt, 2) << ','
            << sqlite3_column_int(stmt, 3) << ','
            << sqlite3_column_double(stmt, 4) << ','
            << sqlite3_column_double(stmt, 5) << ','
            << csvEscape(timestampText ? reinterpret_cast<const char*>(timestampText) : "") << '\n';
    }
    sqlite3_finalize(stmt);
    return true;
#else
    const auto source = fallbackDirectory(storagePath_) / "anomaly_results.csv";
    if (!fileExists(source)) {
        std::ofstream out(path);
        out << "run_id,record_id,algorithm,is_anomaly,score,execution_ms\n";
        return true;
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::copy_file(source, path, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        std::ifstream in(source, std::ios::binary);
        std::ofstream out(path, std::ios::binary);
        out << in.rdbuf();
    }
    return true;
#endif
}

bool DatabaseManager::sqliteEnabled() const
{
#ifdef HAS_SQLITE3
    return db_ != nullptr;
#else
    return false;
#endif
}

bool DatabaseManager::postgresEnabled() const
{
#ifdef HAS_PQXX
    return pg_ && pg_->is_open();
#else
    return false;
#endif
}

std::string DatabaseManager::quoteIdentifier(const std::string& value)
{
    std::string sanitized;
    sanitized.reserve(value.size() + 4);
    for (const unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '_') {
            sanitized.push_back(static_cast<char>(ch));
        } else {
            sanitized.push_back('_');
        }
    }
    if (sanitized.empty() || std::isdigit(static_cast<unsigned char>(sanitized.front()))) {
        sanitized.insert(sanitized.begin(), 'c');
        sanitized.insert(sanitized.begin() + 1, '_');
    }
    return '"' + sanitized + '"';
}

std::string DatabaseManager::csvEscape(const std::string& value)
{
    if (value.find_first_of(",\"\n\r") == std::string::npos) {
        return value;
    }
    std::string escaped = "\"";
    for (const char ch : value) {
        if (ch == '"') {
            escaped += "\"\"";
        } else {
            escaped.push_back(ch);
        }
    }
    escaped.push_back('"');
    return escaped;
}

std::string DatabaseManager::joinDoubles(const std::vector<double>& values)
{
    std::ostringstream out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out << ';';
        }
        out << values[i];
    }
    return out.str();
}

std::string DatabaseManager::postgresArray(const std::vector<double>& values)
{
    std::ostringstream out;
    out << '{';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        out << values[i];
    }
    out << '}';
    return out.str();
}

std::string DatabaseManager::nowIso()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
    return out.str();
}

const std::vector<std::string>& DatabaseManager::rawTelemetryColumns()
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

double DatabaseManager::valueByColumn(const TelemetryRow& row, const std::string& column, bool& ok) const
{
    const auto it = schema_.numericIndex.find(column);
    if (it == schema_.numericIndex.end() || it->second >= row.values.size()) {
        ok = false;
        return 0.0;
    }
    ok = true;
    return row.values[it->second];
}

} // namespace telemetry
