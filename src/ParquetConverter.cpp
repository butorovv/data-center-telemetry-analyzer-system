#include "telemetry/ParquetConverter.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

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

std::filesystem::path fallbackScriptPath()
{
    std::vector<std::filesystem::path> candidates;
    try {
        const auto cwd = std::filesystem::current_path();
        candidates.push_back(cwd / "scripts" / "parquet_to_csv.py");
        candidates.push_back(cwd.parent_path() / "scripts" / "parquet_to_csv.py");
    } catch (...) {
    }
    candidates.push_back(std::filesystem::path("scripts") / "parquet_to_csv.py");
    candidates.push_back(std::filesystem::path("..") / "scripts" / "parquet_to_csv.py");

    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) {
            return std::filesystem::absolute(candidate, ec);
        }
    }
    return {};
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
        lastError_ = "Parquet conversion was cancelled before start.";
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
        lastError_ = "Cannot create CSV: " + csvPath.string();
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
            lastError_ = "Parquet conversion was cancelled by user.";
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
    const auto script = fallbackScriptPath();
    if (script.empty()) {
        lastError_ =
            "Apache Arrow is not linked and Python fallback script scripts/parquet_to_csv.py was not found. "
            "Run the GUI from the project root or rebuild the package with the scripts directory.";
        return false;
    }

    const std::vector<std::string> launchers = {"python", "py -3", "python3"};
    for (const auto& launcher : launchers) {
        if (cancellation != nullptr && cancellation->isCancelled()) {
            lastError_ = "Parquet conversion was cancelled by user.";
            return false;
        }

        std::ostringstream command;
        command << launcher << ' ' << shellQuote(script)
                << " --input " << shellQuote(parquetPath)
                << " --output " << shellQuote(csvPath);
        const int rc = std::system(command.str().c_str());
        if (rc == 0 && std::filesystem::exists(csvPath)) {
            reportProgress(progress, 100, 100, "parquet converted by python fallback");
            return true;
        }
    }

    lastError_ =
        "Apache Arrow is not linked and Python fallback failed. "
        "Install fallback dependencies with: python -m pip install pandas pyarrow. "
        "Script: " + script.string();
    return false;
#endif
}

} // namespace telemetry
