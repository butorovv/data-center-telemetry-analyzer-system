QT += widgets charts
CONFIG += c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = telemetry_gui
DESTDIR = $$OUT_PWD

INCLUDEPATH += $$PWD/include

win32-g++ {
    QMAKE_CXXFLAGS += -finput-charset=UTF-8 -fexec-charset=UTF-8
    LIBS += -lstdc++fs
}

HEADERS += \
    $$PWD/include/telemetry/CsvParser.hpp \
    $$PWD/include/telemetry/CsvReader.hpp \
    $$PWD/include/telemetry/DatabaseManager.hpp \
    $$PWD/include/telemetry/DataLoader.hpp \
    $$PWD/include/telemetry/GraphBuilder.hpp \
    $$PWD/include/telemetry/HybridDetector.hpp \
    $$PWD/include/telemetry/IForestDetector.hpp \
    $$PWD/include/telemetry/IsolationForestDetector.hpp \
    $$PWD/include/telemetry/KMeansDetector.hpp \
    $$PWD/include/telemetry/MetricsCalculator.hpp \
    $$PWD/include/telemetry/Models.hpp \
    $$PWD/include/telemetry/ParquetConverter.hpp \
    $$PWD/include/telemetry/Preprocessor.hpp \
    $$PWD/include/telemetry/RealFailureValidator.hpp \
    $$PWD/include/telemetry/SummitPrototypeAdapter.hpp \
    $$PWD/include/telemetry/Summit1970187Adapter.hpp \
    $$PWD/include/telemetry/TaskControl.hpp \
    $$PWD/include/telemetry/Visualizer.hpp \
    $$PWD/include/telemetry/gui/ExecutionThread.hpp \
    $$PWD/include/telemetry/gui/MainWindow.hpp

SOURCES += \
    $$PWD/src/CsvParser.cpp \
    $$PWD/src/CsvReader.cpp \
    $$PWD/src/DatabaseManager.cpp \
    $$PWD/src/DataLoader.cpp \
    $$PWD/src/GraphBuilder.cpp \
    $$PWD/src/HybridDetector.cpp \
    $$PWD/src/IForestDetector.cpp \
    $$PWD/src/IsolationForestDetector.cpp \
    $$PWD/src/KMeansDetector.cpp \
    $$PWD/src/MetricsCalculator.cpp \
    $$PWD/src/ParquetConverter.cpp \
    $$PWD/src/Preprocessor.cpp \
    $$PWD/src/RealFailureValidator.cpp \
    $$PWD/src/SummitPrototypeAdapter.cpp \
    $$PWD/src/Summit1970187Adapter.cpp \
    $$PWD/src/Visualizer.cpp \
    $$PWD/src/gui/ExecutionThread.cpp \
    $$PWD/src/gui/MainWindow.cpp \
    $$PWD/src/gui/main_gui.cpp


