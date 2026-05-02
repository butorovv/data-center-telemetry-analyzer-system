#include "telemetry/SummitPrototypeAdapter.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
#include <limits>
#include <numeric>
#include <random>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace telemetry {
namespace {

constexpr double kEulerGamma = 0.57721566490153286060;

struct Matrix {
    std::size_t rows = 0;
    std::size_t cols = 0;
    std::vector<double> values;

    double operator()(std::size_t row, std::size_t col) const
    {
        return values[row * cols + col];
    }
};

Matrix toMatrix(const PreparedData& data)
{
    Matrix matrix;
    matrix.rows = data.features.size();
    matrix.cols = data.features.empty() ? 0 : data.features.front().size();
    matrix.values.reserve(matrix.rows * matrix.cols);
    for (const auto& row : data.features) {
        matrix.values.insert(matrix.values.end(), row.begin(), row.end());
    }
    return matrix;
}

DetectorResult makeResultHeader(const std::string& algorithm, const PreparedData& data)
{
    DetectorResult result;
    result.algorithm = algorithm;
    result.rowIds = data.rowIds;
    result.labels.assign(data.features.size(), 0);
    result.scores.assign(data.features.size(), 0.0);
    return result;
}

double squaredDistanceToCentroid(
    const Matrix& data,
    std::size_t row,
    const std::vector<double>& centroids,
    std::size_t centroid)
{
    double distance = 0.0;
    for (std::size_t col = 0; col < data.cols; ++col) {
        const double diff = data(row, col) - centroids[centroid * data.cols + col];
        distance += diff * diff;
    }
    return distance;
}

std::vector<char> detectKMeansAnomalies(
    const Matrix& data,
    std::vector<double>* outDistances,
    double* outThreshold,
    std::size_t clusterCount,
    std::size_t maxIterations,
    const PrototypeRunOptions& options)
{
    std::vector<char> anomalies(data.rows, 0);
    if (outDistances != nullptr) {
        outDistances->assign(data.rows, 0.0);
    }
    if (outThreshold != nullptr) {
        *outThreshold = 0.0;
    }
    if (data.rows == 0 || data.cols == 0) {
        return anomalies;
    }

    clusterCount = std::max<std::size_t>(1, std::min(clusterCount, data.rows));
    maxIterations = std::max<std::size_t>(1, maxIterations);

    std::vector<std::size_t> rowIndices(data.rows);
    std::iota(rowIndices.begin(), rowIndices.end(), 0);
    std::mt19937 rng(123);
    std::shuffle(rowIndices.begin(), rowIndices.end(), rng);

    std::vector<double> centroids(clusterCount * data.cols, 0.0);
    for (std::size_t cluster = 0; cluster < clusterCount; ++cluster) {
        const std::size_t sourceRow = rowIndices[cluster];
        for (std::size_t col = 0; col < data.cols; ++col) {
            centroids[cluster * data.cols + col] = data(sourceRow, col);
        }
    }

    std::vector<std::size_t> assignments(data.rows, std::numeric_limits<std::size_t>::max());
    for (std::size_t iteration = 0; iteration < maxIterations; ++iteration) {
        if (options.cancellation != nullptr && options.cancellation->isCancelled()) {
            return anomalies;
        }

        bool changed = false;
        for (std::size_t row = 0; row < data.rows; ++row) {
            std::size_t bestCluster = 0;
            double bestDistance = squaredDistanceToCentroid(data, row, centroids, 0);
            for (std::size_t cluster = 1; cluster < clusterCount; ++cluster) {
                const double distance = squaredDistanceToCentroid(data, row, centroids, cluster);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    bestCluster = cluster;
                }
            }
            if (assignments[row] != bestCluster) {
                assignments[row] = bestCluster;
                changed = true;
            }
        }

        std::vector<double> sums(clusterCount * data.cols, 0.0);
        std::vector<std::size_t> counts(clusterCount, 0);
        for (std::size_t row = 0; row < data.rows; ++row) {
            const std::size_t cluster = assignments[row];
            ++counts[cluster];
            for (std::size_t col = 0; col < data.cols; ++col) {
                sums[cluster * data.cols + col] += data(row, col);
            }
        }

        std::uniform_int_distribution<std::size_t> rowPick(0, data.rows - 1);
        for (std::size_t cluster = 0; cluster < clusterCount; ++cluster) {
            if (counts[cluster] == 0) {
                const std::size_t sourceRow = rowPick(rng);
                for (std::size_t col = 0; col < data.cols; ++col) {
                    centroids[cluster * data.cols + col] = data(sourceRow, col);
                }
                continue;
            }
            for (std::size_t col = 0; col < data.cols; ++col) {
                centroids[cluster * data.cols + col] = sums[cluster * data.cols + col] / static_cast<double>(counts[cluster]);
            }
        }

        reportProgress(options.progress, iteration + 1, maxIterations, "k-means");
        if (!changed) {
            break;
        }
    }

    std::vector<double> distances(data.rows, 0.0);
    double meanDistance = 0.0;
    for (std::size_t row = 0; row < data.rows; ++row) {
        double bestDistance = squaredDistanceToCentroid(data, row, centroids, 0);
        for (std::size_t cluster = 1; cluster < clusterCount; ++cluster) {
            bestDistance = std::min(bestDistance, squaredDistanceToCentroid(data, row, centroids, cluster));
        }
        distances[row] = std::sqrt(bestDistance);
        meanDistance += distances[row];
    }
    meanDistance /= static_cast<double>(data.rows);

    double variance = 0.0;
    for (const double distance : distances) {
        const double diff = distance - meanDistance;
        variance += diff * diff;
    }
    const double stdDistance = std::sqrt(variance / static_cast<double>(data.rows));
    const double threshold = meanDistance + 2.5 * stdDistance;
    for (std::size_t row = 0; row < data.rows; ++row) {
        anomalies[row] = distances[row] > threshold ? 1 : 0;
    }

    if (outDistances != nullptr) {
        *outDistances = std::move(distances);
    }
    if (outThreshold != nullptr) {
        *outThreshold = threshold;
    }
    return anomalies;
}

double averagePathLength(std::size_t sampleSize)
{
    if (sampleSize <= 1) {
        return 0.0;
    }
    if (sampleSize == 2) {
        return 1.0;
    }
    const double n = static_cast<double>(sampleSize);
    return 2.0 * (std::log(n - 1.0) + kEulerGamma) - 2.0 * (n - 1.0) / n;
}

struct IsolationNode {
    bool external = true;
    std::size_t sampleSize = 0;
    std::size_t feature = 0;
    double splitValue = 0.0;
    int left = -1;
    int right = -1;
};

class IsolationTree {
public:
    void build(const Matrix& data, const std::vector<std::size_t>& sample, std::size_t heightLimit, std::mt19937& rng)
    {
        nodes_.clear();
        buildNode(data, sample, 0, heightLimit, rng);
    }

    double pathLength(const Matrix& data, std::size_t row) const
    {
        if (nodes_.empty()) {
            return 0.0;
        }
        return pathLength(data, row, 0, 0.0);
    }

private:
    int buildNode(const Matrix& data, const std::vector<std::size_t>& sample, std::size_t depth, std::size_t heightLimit, std::mt19937& rng)
    {
        const int nodeIndex = static_cast<int>(nodes_.size());
        nodes_.push_back(IsolationNode{});
        nodes_[static_cast<std::size_t>(nodeIndex)].sampleSize = sample.size();

        if (depth >= heightLimit || sample.size() <= 1 || data.cols == 0) {
            return nodeIndex;
        }

        std::uniform_int_distribution<std::size_t> featurePick(0, data.cols - 1);
        std::size_t selectedFeature = 0;
        double minValue = 0.0;
        double maxValue = 0.0;
        bool found = false;
        for (std::size_t attempt = 0; attempt < data.cols; ++attempt) {
            const std::size_t feature = featurePick(rng);
            double currentMin = std::numeric_limits<double>::infinity();
            double currentMax = -std::numeric_limits<double>::infinity();
            for (const std::size_t row : sample) {
                const double value = data(row, feature);
                currentMin = std::min(currentMin, value);
                currentMax = std::max(currentMax, value);
            }
            if (currentMin < currentMax) {
                selectedFeature = feature;
                minValue = currentMin;
                maxValue = currentMax;
                found = true;
                break;
            }
        }
        if (!found) {
            return nodeIndex;
        }

        std::uniform_real_distribution<double> splitPick(minValue, maxValue);
        std::vector<std::size_t> leftSample;
        std::vector<std::size_t> rightSample;
        double splitValue = minValue;
        for (int attempt = 0; attempt < 8; ++attempt) {
            splitValue = splitPick(rng);
            leftSample.clear();
            rightSample.clear();
            for (const std::size_t row : sample) {
                if (data(row, selectedFeature) < splitValue) {
                    leftSample.push_back(row);
                } else {
                    rightSample.push_back(row);
                }
            }
            if (!leftSample.empty() && !rightSample.empty()) {
                break;
            }
        }
        if (leftSample.empty() || rightSample.empty()) {
            return nodeIndex;
        }

        nodes_[static_cast<std::size_t>(nodeIndex)].external = false;
        nodes_[static_cast<std::size_t>(nodeIndex)].feature = selectedFeature;
        nodes_[static_cast<std::size_t>(nodeIndex)].splitValue = splitValue;
        const int leftChild = buildNode(data, leftSample, depth + 1, heightLimit, rng);
        const int rightChild = buildNode(data, rightSample, depth + 1, heightLimit, rng);
        nodes_[static_cast<std::size_t>(nodeIndex)].left = leftChild;
        nodes_[static_cast<std::size_t>(nodeIndex)].right = rightChild;
        return nodeIndex;
    }

    double pathLength(const Matrix& data, std::size_t row, int nodeIndex, double depth) const
    {
        const IsolationNode& node = nodes_[static_cast<std::size_t>(nodeIndex)];
        if (node.external) {
            return depth + averagePathLength(node.sampleSize);
        }
        if (data(row, node.feature) < node.splitValue) {
            return pathLength(data, row, node.left, depth + 1.0);
        }
        return pathLength(data, row, node.right, depth + 1.0);
    }

    std::vector<IsolationNode> nodes_;
};

std::vector<std::size_t> sampleRows(std::size_t rowCount, std::size_t sampleSize, std::mt19937& rng)
{
    sampleSize = std::min(sampleSize, rowCount);
    std::vector<std::size_t> sample;
    sample.reserve(sampleSize);
    if (sampleSize == rowCount) {
        sample.resize(rowCount);
        std::iota(sample.begin(), sample.end(), 0);
        return sample;
    }

    std::unordered_set<std::size_t> used;
    used.reserve(sampleSize * 2);
    std::uniform_int_distribution<std::size_t> rowPick(0, rowCount - 1);
    while (sample.size() < sampleSize) {
        const std::size_t row = rowPick(rng);
        if (used.insert(row).second) {
            sample.push_back(row);
        }
    }
    return sample;
}

class IsolationForest {
public:
    void fit(const Matrix& data, const PrototypeRunOptions& options)
    {
        trees_.clear();
        if (data.rows == 0 || data.cols == 0) {
            normalizationSampleSize_ = 0;
            return;
        }

        normalizationSampleSize_ = std::min(options.isolationSampleSize, data.rows);
        const std::size_t heightLimit = static_cast<std::size_t>(std::ceil(std::log2(static_cast<double>(normalizationSampleSize_))));
        trees_.resize(options.isolationTrees);

        std::vector<std::future<IsolationTree>> futures;
        futures.reserve(options.isolationTrees);
        std::atomic<std::size_t> completed{0};
        for (std::size_t i = 0; i < options.isolationTrees; ++i) {
            futures.push_back(std::async(std::launch::async, [&, i]() {
                IsolationTree tree;
                if (options.cancellation != nullptr && options.cancellation->isCancelled()) {
                    return tree;
                }
                std::mt19937 rng(options.isolationSeed + static_cast<unsigned>(i * 7919));
                const auto sample = sampleRows(data.rows, normalizationSampleSize_, rng);
                tree.build(data, sample, heightLimit, rng);
                const std::size_t done = ++completed;
                reportProgress(options.progress, done, options.isolationTrees + data.rows, "building isolation forest");
                return tree;
            }));
        }

        for (std::size_t i = 0; i < futures.size(); ++i) {
            trees_[i] = futures[i].get();
        }
        if (options.cancellation != nullptr && options.cancellation->isCancelled()) {
            trees_.clear();
        }
    }

    std::vector<double> scores(const Matrix& data, const PrototypeRunOptions& options) const
    {
        std::vector<double> result(data.rows, 0.0);
        if (trees_.empty() || normalizationSampleSize_ == 0) {
            return result;
        }

        const double c = averagePathLength(normalizationSampleSize_);
        if (c <= 0.0) {
            return result;
        }

        for (std::size_t row = 0; row < data.rows; ++row) {
            if (options.cancellation != nullptr && options.cancellation->isCancelled()) {
                return result;
            }
            double pathSum = 0.0;
            for (const IsolationTree& tree : trees_) {
                pathSum += tree.pathLength(data, row);
            }
            const double averagePath = pathSum / static_cast<double>(trees_.size());
            result[row] = std::pow(2.0, -averagePath / c);
            reportProgress(options.progress, options.isolationTrees + row + 1, options.isolationTrees + data.rows, "scoring isolation forest");
        }
        return result;
    }

private:
    std::vector<IsolationTree> trees_;
    std::size_t normalizationSampleSize_ = 0;
};

std::string hostTimeKey(const std::string& hostname, const std::string& timestamp)
{
    return hostname + '\x1F' + timestamp;
}

DetectorResult isolationForestResult(const PreparedData& data, const PrototypeRunOptions& options)
{
    DetectorResult result = makeResultHeader("isolation_forest", data);
    result.threshold = options.isolationThreshold;
    const Matrix matrix = toMatrix(data);

    IsolationForest forest;
    forest.fit(matrix, options);
    const auto scores = forest.scores(matrix, options);
    for (std::size_t i = 0; i < scores.size(); ++i) {
        result.scores[i] = scores[i];
        result.labels[i] = scores[i] > options.isolationThreshold ? 1 : 0;
    }

    std::ostringstream params;
    params << "source=summit_adapter;trees=" << options.isolationTrees
           << ";sample_size=" << options.isolationSampleSize
           << ";threshold=" << options.isolationThreshold;
    result.parameters = params.str();
    return result;
}

} // namespace

DetectorResult SummitPrototypeAdapter::runKMeans(const PreparedData& data, const PrototypeRunOptions& options)
{
    const auto started = std::chrono::steady_clock::now();
    DetectorResult result = makeResultHeader("kmeans", data);
    const Matrix matrix = toMatrix(data);

    std::vector<double> distances;
    double threshold = 0.0;
    const auto predictions = detectKMeansAnomalies(
        matrix,
        &distances,
        &threshold,
        options.kmeansClusters,
        options.kmeansIterations,
        options);

    result.threshold = threshold;
    for (std::size_t i = 0; i < predictions.size(); ++i) {
        result.labels[i] = predictions[i] != 0 ? 1 : 0;
        result.scores[i] = threshold > 1e-12 && i < distances.size()
            ? std::min(1.0, distances[i] / threshold)
            : static_cast<double>(result.labels[i]);
    }

    const auto finished = std::chrono::steady_clock::now();
    result.executionMs = std::chrono::duration<double, std::milli>(finished - started).count();
    std::ostringstream params;
    params << "source=summit_adapter;k=" << options.kmeansClusters
           << ";iterations=" << options.kmeansIterations
           << ";threshold=mean+2.5*std";
    result.parameters = params.str();
    return result;
}

DetectorResult SummitPrototypeAdapter::runIsolationForest(const PreparedData& data, const PrototypeRunOptions& options)
{
    const auto started = std::chrono::steady_clock::now();
    DetectorResult result = isolationForestResult(data, options);
    const auto finished = std::chrono::steady_clock::now();
    result.executionMs = std::chrono::duration<double, std::milli>(finished - started).count();
    return result;
}

DetectorResult SummitPrototypeAdapter::runHybrid(
    const TelemetryDataset& dataset,
    const PreparedData& data,
    const GraphContext& graph,
    const PrototypeRunOptions& options)
{
    const auto started = std::chrono::steady_clock::now();
    DetectorResult iforest = isolationForestResult(data, options);
    DetectorResult result = makeResultHeader("hybrid_iforest_graph", data);
    result.threshold = iforest.threshold;

    std::unordered_set<std::string> candidateHostTimes;
    const std::size_t rows = std::min(dataset.rows.size(), iforest.labels.size());
    for (std::size_t row = 0; row < rows; ++row) {
        if (iforest.labels[row]) {
            candidateHostTimes.insert(hostTimeKey(dataset.rows[row].hostname, dataset.rows[row].timestamp));
        }
    }

    for (std::size_t row = 0; row < rows; ++row) {
        if (!iforest.labels[row]) {
            continue;
        }
        const auto vertexIt = graph.hostToIndex.find(dataset.rows[row].hostname);
        if (vertexIt == graph.hostToIndex.end()) {
            continue;
        }
        for (const std::size_t neighbor : graph.adjacency[vertexIt->second]) {
            const std::string& neighborHostname = graph.hosts[neighbor];
            if (candidateHostTimes.find(hostTimeKey(neighborHostname, dataset.rows[row].timestamp)) != candidateHostTimes.end()) {
                result.labels[row] = 1;
                result.scores[row] = iforest.scores[row];
                break;
            }
        }
    }

    const auto finished = std::chrono::steady_clock::now();
    result.executionMs = std::chrono::duration<double, std::milli>(finished - started).count();
    std::ostringstream params;
    params << "source=summit_adapter;base=isolation_forest;threshold=" << options.isolationThreshold;
    result.parameters = params.str();
    return result;
}

} // namespace telemetry

