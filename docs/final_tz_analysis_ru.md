# Анализ текущей реализации относительно финального ТЗ

## Шаг 0. Доступность файлов

Файлы проекта доступны в рабочем каталоге `C:\study\НИР_КП\project_KP`. Исходник НИР `summit_anomaly_prototype.cpp` сначала отсутствовал в файловой системе, затем был передан в чате. Поэтому алгоритмическая логика перенесена в модуль-адаптер `SummitPrototypeAdapter`, а не выдумана заново.

Файлы датасета `1970187` и сопутствующий XID-лог в проекте отсутствуют. Поэтому реальный расчет lead time > 0 невозможно подтвердить локальным прогоном до добавления этих файлов.

## Шаг 1. Интерпретация скрытых требований

Принято к реализации:

- валидация `1970187`: поиск XID 94, расчет `lead time = timestamp_ошибки_из_лога - timestamp_обнаружения_алгоритмом`;
- lead time положителен, если алгоритм обнаружил аномалию раньше официальной ошибки;
- обязательная схема БД: `raw_telemetry`, `normalized_features`, `anomaly_results`, `execution_log`;
- длительные операции должны иметь прогресс и отмену;
- граф соседства строится по `hostname`: `nodeXXX`, `rackN_positionM`, либо fallback по лексикографическому порядку;
- `.tar` временно распаковывается, внутри ищется CSV/Parquet;
- Parquet основной путь через Apache Arrow, Python fallback только если Arrow недоступен.

## Шаг 2. Что было в текущей реализации

Уже было:

- C++17 CLI-приложение;
- CMake;
- чтение CSV;
- удаление NaN;
- Z-нормализация;
- синтетические аномалии;
- k-means;
- Isolation Forest;
- гибрид IF + граф;
- расчет Precision/Recall/F1;
- SQLite/fallback CSV-хранилище;
- экспорт CSV;
- Python/matplotlib график;
- русская документация.

Частично было:

- граф соседства: был по префиксу hostname, но не строго по позиции `±1`;
- БД: была SQLite/fallback, но не по финальной PostgreSQL-схеме;
- визуализация: была через внешний Python, но не Qt;
- многопоточность: была внутри IF/k-means, но без единого UI-прогресса и Cancel.

Отсутствовало:

- Qt GUI;
- QProgressBar и кнопка Cancel;
- PostgreSQL/libpqxx;
- Dockerfile/docker-compose/.env;
- Parquet/Arrow;
- TAR-загрузка;
- модуль валидации `1970187`/XID 94;
- отдельные модули `ParquetConverter`, `DataLoader`, `ExecutionThread`, `MainWindow`;
- интеграционный фасад для исходника НИР.

## Шаг 3. Что добавлено

### Интеграция исходника НИР

Добавлены файлы:

- `include/telemetry/SummitPrototypeAdapter.hpp`
- `src/SummitPrototypeAdapter.cpp`

Назначение:

- адаптировать алгоритмическую логику присланного `summit_anomaly_prototype.cpp` к текущим структурам проекта;
- возвращать общий `DetectorResult`;
- использовать тот же смысл параметров:
  - k-means: `k=3`, порог `mean + 2.5 * std`;
  - Isolation Forest: `100` деревьев, sample size `256`, threshold `0.6`;
  - hybrid: IF-кандидаты + графовая проверка по timestamp.

### Загрузка разных форматов

Добавлены:

- `include/telemetry/CsvParser.hpp`
- `src/CsvParser.cpp`
- `include/telemetry/ParquetConverter.hpp`
- `src/ParquetConverter.cpp`
- `include/telemetry/DataLoader.hpp`
- `src/DataLoader.cpp`
- `scripts/parquet_to_csv.py`

Теперь загрузчик:

- определяет `.csv`, `.parquet`, `.tar`;
- CSV читает напрямую;
- Parquet конвертирует в CSV через Arrow при наличии сборки;
- если Arrow не собран, вызывает Python fallback;
- TAR распаковывает во временный каталог и ищет внутри первый CSV/Parquet;
- при отсутствии ожидаемых колонок пишет предупреждения и продолжает работу с доступными.

### PostgreSQL и схема БД

`DatabaseManager` доработан под таблицы:

- `raw_telemetry`;
- `normalized_features`;
- `anomaly_results`;
- `execution_log`.

Добавлен метод `openPostgres(...)`. Если `libpqxx` не собран, используется SQLite/fallback.

### Граф соседства

`GraphBuilder` доработан:

- распознает `nodeXXX`;
- распознает `rackN_positionM`;
- распознает старый формат из НИР вида `a01`;
- соединяет позиции `±1`;
- если формат не распознан, использует соседей по лексикографической сортировке.

Гибридный алгоритм проверяет соседей на том же timestamp.

### Qt GUI

Добавлены:

- `include/telemetry/gui/ExecutionThread.hpp`
- `src/gui/ExecutionThread.cpp`
- `include/telemetry/gui/MainWindow.hpp`
- `src/gui/MainWindow.cpp`
- `src/gui/main_gui.cpp`

GUI содержит вкладки:

- "Загрузка";
- "Анализ";
- "Результаты";
- "Графики";
- "Информация".

Есть:

- выбор CSV/Parquet/TAR;
- PostgreSQL connection string;
- QProgressBar;
- кнопка "Отмена";
- запуск алгоритмов;
- таблица результатов;
- таблица lead time для XID 94;
- экспорт CSV;
- график score/аномалий.

### Валидация 1970187

Добавлены:

- `include/telemetry/RealFailureValidator.hpp`
- `src/RealFailureValidator.cpp`

Модуль:

- читает CSV-лог или текстовый лог;
- ищет события XID;
- фильтрует XID 94;
- ищет ближайшее более раннее обнаружение гибридным алгоритмом;
- считает lead time в секундах;
- возвращает `LeadTimeResult`.

Фактический критерий `lead time > 0` можно проверить только при наличии реального датасета и лога.

### Docker

Добавлены:

- `Dockerfile`;
- `docker-compose.yml`;
- `.env`.

Команда:

```bash
docker-compose up --build
```

поднимает PostgreSQL и запускает CLI-пример на `data/sample_telemetry.csv`.

## Шаг 4. Проверка критериев

В текущей среде можно проверить:

- сборку CLI через `g++`;
- CSV-загрузку;
- предупреждения по отсутствующим колонкам;
- граф по hostname;
- запуск алгоритмов;
- экспорт CSV.

Нельзя полноценно проверить в текущей среде без внешних компонентов:

- Qt GUI, если Qt dev-пакеты не установлены;
- Arrow-конвертацию, если Arrow dev-пакеты не установлены;
- PostgreSQL через libpqxx, если libpqxx не установлен;
- lead time на `1970187`, потому что отсутствует датасет и XID-лог.

## Команды

CLI:

```powershell
.\telemetry_analyzer.exe --input data\sample_telemetry.csv --algorithm all --threads 2 --window 2
```

Реальная валидация после добавления файлов:

```bash
./build/telemetry_analyzer \
  --input /data/1970187.csv \
  --xid-log /data/1970187_xid.log \
  --algorithm hybrid \
  --no-synthetic
```

Docker:

```bash
docker-compose up --build
```
