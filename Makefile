CXX ?= g++
BUILD_DIR ?= build
TARGET ?= $(BUILD_DIR)/telemetry_analyzer.exe

CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pedantic -Iinclude

SOURCES := \
	src/CsvParser.cpp \
	src/CsvReader.cpp \
	src/DatabaseManager.cpp \
	src/DataLoader.cpp \
	src/GraphBuilder.cpp \
	src/HybridDetector.cpp \
	src/IForestDetector.cpp \
	src/IsolationForestDetector.cpp \
	src/KMeansDetector.cpp \
	src/MetricsCalculator.cpp \
	src/ParquetConverter.cpp \
	src/Preprocessor.cpp \
	src/RealFailureValidator.cpp \
	src/SummitPrototypeAdapter.cpp \
	src/Summit1970187Adapter.cpp \
	src/Visualizer.cpp \
	src/main.cpp

.PHONY: all clean run smoke-test

all: $(TARGET)

$(TARGET): $(SOURCES)
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(SOURCES) -o $(TARGET)

run: $(TARGET)
	./$(TARGET)

smoke-test: $(TARGET)
	./$(TARGET) --input data/sample_telemetry.csv --algorithm all --threads 2 --window 2

clean:
	rm -rf $(BUILD_DIR)

