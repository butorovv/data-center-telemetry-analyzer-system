# Summit Telemetry Analyzer

Course project implementation for the topic "Intelligent system for analyzing telemetry of data-center server infrastructure".

## What is implemented

- C++17 modular application with CMake.
- CSV ingestion for Summit-like telemetry files, including `hostname`, timestamp and dynamic numeric columns.
- Auto input detection for `.csv`, `.parquet`, `.tar`.
- Parquet conversion through Apache Arrow/parquet-cpp when available, with Python fallback.
- TAR extraction into a temporary directory.
- Preprocessing: NaN row removal, synthetic anomaly injection before normalization, workload mode classification, Z-normalization, optional moving-window features.
- Algorithms:
  - k-means with k-means++ initialization and anomaly threshold `mean distance + 2.5 * std`;
  - multithreaded Isolation Forest with 100 trees, sample size 256 and score threshold 0.6 by default;
  - hybrid detector: Isolation Forest candidates verified through a hostname rack graph.
- Graph construction by rack prefix extracted from `hostname`; Boost.Graph is used automatically when headers are available.
- Storage:
  - PostgreSQL/libpqxx when enabled;
  - SQLite database when `SQLite3` is available at build time;
  - CSV/SQL fallback directory when DB development files are absent.
- Qt Widgets GUI when Qt is available.
- Interactive CLI menu and batch mode.
- CSV export and Python/matplotlib plot script for telemetry series with highlighted anomalies.

## Build

```bash
cmake -S . -B build
cmake --build build --config Release
```

## Build in Git Bash on Windows

The simplest path does not require CMake or make:

```bash
bash scripts/build_git_bash.sh
```

Smoke test:

```bash
bash scripts/build_git_bash.sh --smoke-test
```

If your Git Bash has `make`:

```bash
make
make smoke-test
```

The Git Bash build creates:

```text
build/telemetry_analyzer.exe
```

If Git Bash cannot find `g++`, add your MinGW/TDM-GCC folder to PATH first, for example:

```bash
export PATH=/c/TDM-GCC-64/bin:$PATH
```

## Build Qt GUI in Git Bash

Install Qt for MinGW with the Qt Charts module. Then run:

```bash
export QMAKE=/c/Qt/5.15.2/mingw81_64/bin/qmake.exe
export PATH=/c/Qt/5.15.2/mingw81_64/bin:/c/Qt/Tools/mingw810_64/bin:$PATH
bash scripts/build_qt_git_bash.sh
```

Run the GUI after build:

```bash
bash scripts/build_qt_git_bash.sh --run
```

The GUI executable is created in `build-gui/telemetry_gui.exe` or `build-gui/release/telemetry_gui.exe`.

On Ubuntu 22.04 install optional dependencies:

```bash
sudo apt install g++ cmake libsqlite3-dev libboost-dev python3-matplotlib
```

For the final version with GUI/PostgreSQL/Arrow on Ubuntu 22.04:

```bash
sudo apt install g++ cmake qtbase5-dev qtcharts5-dev libqt5sql5-psql \
  libboost-all-dev libpqxx-dev libpq-dev libarrow-dev libparquet-dev
```

## Quick demo

```bash
./build/telemetry_analyzer --input data/sample_telemetry.csv --algorithm all --plot
```

On Windows with a multi-config generator:

```powershell
.\build\Release\telemetry_analyzer.exe --input data\sample_telemetry.csv --algorithm all --plot
```

## Work with the real Summit sample

```bash
./build/telemetry_analyzer \
  --input b_snapshot_10sec_24h.csv \
  --db summit_telemetry.sqlite \
  --algorithm all \
  --threads 8 \
  --window 3
```

For a first smoke test on a large file:

```bash
./build/telemetry_analyzer --input b_snapshot_10sec_24h.csv --limit 100000 --algorithm all
```

## Docker

```bash
docker-compose up --build
```

The compose stack starts PostgreSQL 13 and runs the CLI demo against `data/sample_telemetry.csv`.

## 1970187 validation

The course-project validation uses `second_dataset/points_with_jobs_tele_ult.csv`. The column `is_failure` is ground truth only: `1` means a Double-Bit Error event, and it is explicitly excluded from detector features.

Validation windows are selectable:

- `--validation-window 1` uses telemetry columns ending with `_1min` and gives `lead_time_sec = 60` only for detected events.
- `--validation-window 5` uses `_5min` and gives `lead_time_sec = 300` only for detected events.
- `--validation-window 15` uses `_15min` and gives `lead_time_sec = 900` only for detected events.

`failures.csv` is optional sanity-check input; it is not used as the main ground-truth source.

```bash
./build/telemetry_analyzer --validate-1970187 \
  --points second_dataset/points_with_jobs_tele_ult.csv \
  --failures second_dataset/failures.csv \
  --threshold 0.75 \
  --validation-window 15
```

The honest validator no longer forces proxy rows to anomalies. If the hybrid algorithm does not mark a DBE row as anomalous by itself, the row is exported as `positive=no`, `algorithm_detected=no`, `lead_time_sec=-1`.

## Interactive mode

Run without arguments:

```bash
./build/telemetry_analyzer
```

Menu actions:

1. Load CSV, preprocess, save to database.
2. Run k-means.
3. Run Isolation Forest.
4. Run hybrid IF + graph.
5. Export anomaly results to CSV.
6. Plot the latest result.
7. Show graph adjacency.

## Database schema

The application creates:

- `raw_telemetry`
- `normalized_features`
- `anomaly_results`
- `execution_log`

## Project modules

- `csv_reader` -> `CsvReader`
- `preprocessor` -> `Preprocessor`
- `database_manager` -> `DatabaseManager`
- `graph_builder` -> `GraphBuilder`
- `kmeans_detector` -> `KMeansDetector`
- `isolation_forest_detector` -> `IsolationForestDetector`
- `hybrid_detector` -> `HybridDetector`
- `metrics_calculator` -> `MetricsCalculator`
- `visualizer` -> `Visualizer` and `scripts/plot_telemetry.py`

## Russian documentation

- `docs/development_log_ru.md` describes the created files, data structures and development progress in Russian.
- `docs/course_project_report.md` is a Russian draft of the explanatory note.
- `docs/presentation_plan.md` is a Russian defense presentation outline.

## Final course-project notes

Metrics used in the course project are only Precision, Recall and F1. NIR-only extra metrics are not calculated in the KP build.

Isolation Forest uses 100 trees, sample size 256 and default anomaly threshold 0.75. The threshold can be changed in the GUI on the Analysis tab or in CLI:

```bash
./build/telemetry_analyzer --input data/sample_telemetry.csv --algorithm hybrid --threshold 0.75
```

The 1970187 validation file is expected in `second_dataset/`:

- `points_with_jobs_tele_ult.csv` - used by the validator (`timestamp`, `hostname`, `GPU`, `is_failure`, and telemetry aggregates with `_1min`, `_5min`, `_15min`).
- `failures.csv` - optional sanity-check only.
- `locations_by_serials.csv` - kept as an auxiliary file and not required by the current validation flow.

CLI validation example:

```bash
./build/telemetry_analyzer --validate-1970187 \
  --points second_dataset/points_with_jobs_tele_ult.csv \
  --failures second_dataset/failures.csv \
  --threshold 0.75 \
  --validation-window 15
```

Validation-specific note: `Summit1970187Adapter` is a separate detector adapter for the aggregate second dataset. The main `SummitPrototypeAdapter` remains unchanged. The validator uses aggregate rows as input data, but it does not automatically count them as detected anomalies.

Docker launch remains:

```bash
docker-compose up --build
```

Parquet conversion prefers Apache Arrow/parquet-cpp when the C++ development libraries are available. If Arrow is not available in the local environment, the code keeps the Python fallback path and CSV inputs can be used for local debugging.
