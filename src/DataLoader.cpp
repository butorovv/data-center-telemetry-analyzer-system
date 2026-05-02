#include "telemetry/DataLoader.hpp"
#include "telemetry/CsvParser.hpp"
#include "telemetry/ParquetConverter.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <stdexcept>

namespace telemetry {
namespace {

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string extensionOf(const std::filesystem::path& path)
{
    return lower(path.extension().string());
}

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

std::filesystem::path uniqueTempDir()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
        ("summit_telemetry_" + std::to_string(now));
    std::filesystem::create_directories(dir);
    return dir;
}

} // namespace

DataLoader::DataLoader(std::size_t limitRows)
    : limitRows_(limitRows)
{
}

TelemetryDataset DataLoader::load(
    const std::filesystem::path& path,
    CancellationToken* cancellation,
    const ProgressCallback& progress) const
{
    const std::filesystem::path csvPath = materializeInput(path, cancellation, progress);
    CsvParser parser(limitRows_);
    return parser.readSummitColumns(csvPath.string(), cancellation, progress);
}

std::filesystem::path DataLoader::materializeInput(
    const std::filesystem::path& path,
    CancellationToken* cancellation,
    const ProgressCallback& progress) const
{
    if (!std::filesystem::exists(path)) {
        throw std::runtime_error("Файл не найден: " + path.string());
    }

    const std::string ext = extensionOf(path);
    if (ext == ".csv") {
        return path;
    }
    if (ext == ".parquet") {
        const auto dir = uniqueTempDir();
        const auto csvPath = dir / (path.stem().string() + ".csv");
        ParquetConverter converter;
        if (!converter.convertToCsv(path, csvPath, cancellation, progress)) {
            throw std::runtime_error("Ошибка конвертации Parquet: " + converter.lastError());
        }
        return csvPath;
    }
    if (ext == ".tar") {
        const auto extracted = extractTarAndFindDataFile(path, cancellation, progress);
        return materializeInput(extracted, cancellation, progress);
    }

    throw std::runtime_error("Неподдерживаемый формат файла: " + path.string());
}

std::filesystem::path DataLoader::extractTarAndFindDataFile(
    const std::filesystem::path& tarPath,
    CancellationToken* cancellation,
    const ProgressCallback& progress) const
{
    if (cancellation != nullptr && cancellation->isCancelled()) {
        throw std::runtime_error("Распаковка TAR отменена пользователем.");
    }

    const auto dir = uniqueTempDir();
    std::ostringstream command;
    command << "tar -xf " << shellQuote(tarPath) << " -C " << shellQuote(dir);
    reportProgress(progress, 5, 100, "extracting tar");
    const int rc = std::system(command.str().c_str());
    if (rc != 0) {
        throw std::runtime_error("Не удалось распаковать TAR через системную команду tar.");
    }
    reportProgress(progress, 60, 100, "searching extracted files");

    std::filesystem::path firstCsv;
    std::filesystem::path firstParquet;
    for (const auto& item : std::filesystem::recursive_directory_iterator(dir)) {
        if (!item.is_regular_file()) {
            continue;
        }
        const std::string ext = extensionOf(item.path());
        if (ext == ".csv" && firstCsv.empty()) {
            firstCsv = item.path();
        } else if (ext == ".parquet" && firstParquet.empty()) {
            firstParquet = item.path();
        }
    }

    if (!firstCsv.empty()) {
        return firstCsv;
    }
    if (!firstParquet.empty()) {
        return firstParquet;
    }
    throw std::runtime_error("В TAR не найден CSV или Parquet файл.");
}

} // namespace telemetry
