cmake_minimum_required(VERSION 3.16)

project(SummitTelemetryAnalyzer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

option(TELEMETRY_STRICT_WARNINGS "Enable strict compiler warnings" ON)
option(BUILD_QT_GUI "Build Qt Widgets GUI" ON)
option(ENABLE_ARROW "Enable Apache Arrow / parquet-cpp support when available" ON)
option(ENABLE_POSTGRES "Enable PostgreSQL/libpqxx support when available" ON)

find_package(SQLite3 QUIET)
find_package(Boost QUIET)

if(ENABLE_ARROW)
    find_package(Arrow QUIET CONFIG)
    find_package(Parquet QUIET CONFIG)
endif()

if(ENABLE_POSTGRES)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(PQXX QUIET libpqxx)
    endif()
    find_path(PQXX_INCLUDE_DIR pqxx/pqxx)
    find_library(PQXX_LIBRARY pqxx)
    find_library(PQ_LIBRARY pq)
endif()

if(BUILD_QT_GUI)
    find_package(Qt5 QUIET COMPONENTS Widgets Charts Sql Concurrent)
    if(NOT Qt5_FOUND)
        find_package(Qt6 QUIET COMPONENTS Widgets Charts Sql Concurrent)
    endif()
endif()

set(TELEMETRY_CORE_SOURCES
    src/CsvParser.cpp
    src/CsvReader.cpp
    src/DataLoader.cpp
    src/DatabaseManager.cpp
    src/GraphBuilder.cpp
    src/HybridDetector.cpp
    src/IForestDetector.cpp
    src/IsolationForestDetector.cpp
    src/KMeansDetector.cpp
    src/MetricsCalculator.cpp
    src/ParquetConverter.cpp
    src/Preprocessor.cpp
    src/RealFailureValidator.cpp
    src/SummitPrototypeAdapter.cpp
    src/Summit1970187Adapter.cpp
    src/Visualizer.cpp
)

add_library(telemetry_core STATIC ${TELEMETRY_CORE_SOURCES})

target_include_directories(telemetry_core PUBLIC include)


add_executable(telemetry_analyzer
    src/main.cpp
)

target_link_libraries(telemetry_analyzer PRIVATE telemetry_core)

if(SQLite3_FOUND)
    target_link_libraries(telemetry_core PUBLIC SQLite::SQLite3)
    target_compile_definitions(telemetry_core PUBLIC HAS_SQLITE3=1)
endif()

if(Boost_FOUND)
    target_include_directories(telemetry_core PUBLIC ${Boost_INCLUDE_DIRS})
    target_compile_definitions(telemetry_core PUBLIC HAS_BOOST_GRAPH=1)
endif()

if(ENABLE_ARROW AND Arrow_FOUND AND Parquet_FOUND)
    target_link_libraries(telemetry_core PUBLIC Arrow::arrow_shared Parquet::parquet_shared)
    target_compile_definitions(telemetry_core PUBLIC HAS_ARROW_PARQUET=1)
endif()

if(ENABLE_POSTGRES AND PQXX_INCLUDE_DIR AND PQXX_LIBRARY AND PQ_LIBRARY)
    target_include_directories(telemetry_core PUBLIC ${PQXX_INCLUDE_DIR})
    target_link_libraries(telemetry_core PUBLIC ${PQXX_LIBRARY} ${PQ_LIBRARY})
    target_compile_definitions(telemetry_core PUBLIC HAS_PQXX=1)
endif()

if(TELEMETRY_STRICT_WARNINGS)
    if(MSVC)
        target_compile_options(telemetry_core PRIVATE /W4)
        target_compile_options(telemetry_analyzer PRIVATE /W4)
    else()
        target_compile_options(telemetry_core PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(telemetry_analyzer PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

if(BUILD_QT_GUI AND (Qt5_FOUND OR Qt6_FOUND))
    add_executable(telemetry_gui
        src/gui/ExecutionThread.cpp
        src/gui/MainWindow.cpp
        src/gui/main_gui.cpp

        include/telemetry/gui/MainWindow.hpp
        include/telemetry/gui/ExecutionThread.hpp
    )
    target_link_libraries(telemetry_gui PRIVATE telemetry_core)
    target_compile_definitions(telemetry_gui PRIVATE TELEMETRY_QT_GUI=1)
    if(Qt6_FOUND)
        target_link_libraries(telemetry_gui PRIVATE Qt6::Widgets Qt6::Charts Qt6::Sql Qt6::Concurrent)
    else()
        target_link_libraries(telemetry_gui PRIVATE Qt5::Widgets Qt5::Charts Qt5::Sql Qt5::Concurrent)
    endif()
    if(TELEMETRY_STRICT_WARNINGS)
        if(MSVC)
            target_compile_options(telemetry_gui PRIVATE /W4)
        else()
            target_compile_options(telemetry_gui PRIVATE -Wall -Wextra -Wpedantic)
        endif()
    endif()
endif()

install(TARGETS telemetry_analyzer RUNTIME DESTINATION bin)
if(TARGET telemetry_gui)
    install(TARGETS telemetry_gui RUNTIME DESTINATION bin)
endif()



