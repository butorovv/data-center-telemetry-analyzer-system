#include "telemetry/ParquetConverter.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

#ifdef HAS_ARROW_PARQUET
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/reader.h>
#endif

namespace telemetry {
namespace {

std::string shellQuote(const std::filesystem::path& path)
{
    std::string value = path.string();
    std::string quoted = "\"";
    for (const char ch : value) {
        if (ch == '"') {
            quoted += "\\\"";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted.push_back('"');
    return quoted;
}

} // namespace

bool ParquetConverter::arrowEnabled() const
{
#ifdef HAS_ARROW_PARQUET
    return true;
#else
    return false;
#endif
}

bool ParquetConverter::convertToCsv(
    const std::filesystem::path& parquetPath,
    const std::filesystem::path& csvPath,
    CancellationToken* cancellation,
    const ProgressCallback& progress) const
{
    lastError_.clear();
    reportProgress(progress, 0, 100, "converting parquet");
    if (cancellation != nullptr && cancellation->isCancelled()) {
        lastError_ = "Операция отменена до начала конвертации Parquet.";
        return false;
    }

#ifdef HAS_ARROW_PARQUET
    auto maybeInput = arrow::io::ReadableFile::Open(parquetPath.string());
    if (!maybeInput.ok()) {
        lastError_ = maybeInput.status().ToString();
        return false;
    }

    std::unique_ptr<parquet::arrow::FileReader> reader;
    const auto openStatus = parquet::arrow::OpenFile(*maybeInput, arrow::default_memory_pool(), &reader);
    if (!openStatus.ok()) {
        lastError_ = openStatus.ToString();
        return false;
    }

    std::shared_ptr<arrow::Table> table;
    const auto readStatus = reader->ReadTable(&table);
    if (!readStatus.ok()) {
        lastError_ = readStatus.ToString();
        return false;
    }

    std::ofstream out(csvPath);
    if (!out) {
        lastError_ = "Не удалось создать CSV: " + csvPath.string();
        return false;
    }

    for (int col = 0; col < table->num_columns(); ++col) {
        if (col != 0) {
            out << ',';
        }
        out << table->field(col)->name();
    }
    out << '\n';

    const int64_t rows = table->num_rows();
    for (int64_t row = 0; row < rows; ++row) {
        if (cancellation != nullptr && cancellation->isCancelled()) {
            lastError_ = "Конвертация Parquet отменена пользователем.";
            return false;
        }
        for (int col = 0; col < table->num_columns(); ++col) {
            if (col != 0) {
                out << ',';
            }
            auto scalarResult = table->column(col)->GetScalar(row);
            if (scalarResult.ok() && scalarResult.ValueOrDie()->is_valid) {
                out << scalarResult.ValueOrDie()->ToString();
            }
        }
        out << '\n';
        if (row % 5000 == 0) {
            reportProgress(progress, static_cast<std::size_t>(row), static_cast<std::size_t>(rows), "arrow parquet");
        }
    }
    reportProgress(progress, 100, 100, "parquet converted");
    return true;
#else
    std::ostringstream command;
    command << "python " << shellQuote("scripts/parquet_to_csv.py")
            << " --input " << shellQuote(parquetPath)
            << " --output " << shellQuote(csvPath);
    int rc = std::system(command.str().c_str());
    if (rc != 0) {
        std::ostringstream fallback;
        fallback << "python3 " << shellQuote("scripts/parquet_to_csv.py")
                 << " --input " << shellQuote(parquetPath)
                 << " --output " << shellQuote(csvPath);
        rc = std::system(fallback.str().c_str());
    }
    if (rc != 0) {
        lastError_ =
            "Apache Arrow не подключен, Python fallback не выполнился. Установите Arrow/parquet-cpp или pandas/pyarrow.";
        return false;
    }
    reportProgress(progress, 100, 100, "parquet converted by fallback");
    return true;
#endif
}

} // namespace telemetry
