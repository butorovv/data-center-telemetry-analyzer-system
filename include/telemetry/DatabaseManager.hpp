#pragma once

#include "telemetry/Models.hpp"

#include <filesystem>
#include <memory>
#include <string>

#ifdef HAS_SQLITE3
struct sqlite3;
#endif

#ifdef HAS_PQXX
namespace pqxx {
class connection;
}
#endif

namespace telemetry {

class DatabaseManager {
public:
    DatabaseManager();
    ~DatabaseManager();

    DatabaseManager(const DatabaseManager&) = delete;
    DatabaseManager& operator=(const DatabaseManager&) = delete;

    bool open(const std::filesystem::path& path);
    bool openPostgres(const std::string& connectionString);
    bool initialize(const TelemetrySchema& schema, const PreparedData* normalized = nullptr);
    bool insertTelemetry(const TelemetryDataset& dataset);
    bool insertNormalizedFeatures(const PreparedData& data);
    bool insertSyntheticAnomalies(const TelemetryDataset& dataset);
    bool saveResult(const DetectorResult& result, const Metrics& metrics, std::size_t rowCount);
    bool exportResultsCsv(const std::filesystem::path& path) const;

    bool sqliteEnabled() const;
    bool postgresEnabled() const;
    std::filesystem::path storagePath() const { return storagePath_; }

private:
    static std::string quoteIdentifier(const std::string& value);
    static std::string csvEscape(const std::string& value);
    static std::string joinDoubles(const std::vector<double>& values);
    static std::string postgresArray(const std::vector<double>& values);
    static std::string nowIso();
    static const std::vector<std::string>& rawTelemetryColumns();
    double valueByColumn(const TelemetryRow& row, const std::string& column, bool& ok) const;

    std::filesystem::path storagePath_;
    TelemetrySchema schema_;
    std::vector<std::string> normalizedFeatureNames_;
    int currentRunId_ = 0;

#ifdef HAS_SQLITE3
    sqlite3* db_ = nullptr;
#else
    bool opened_ = false;
#endif

#ifdef HAS_PQXX
    std::unique_ptr<pqxx::connection> pg_;
#endif
};

} // namespace telemetry
