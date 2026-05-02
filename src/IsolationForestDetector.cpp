#include "telemetry/IsolationForestDetector.hpp"
#include "telemetry/SummitPrototypeAdapter.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace telemetry {
namespace {

constexpr double EulerGamma = 0.5772156649015329;

std::size_t effectiveThreads(std::size_t configured, std::size_t workItems)
{
    const std::size_t hardware = std::max(1u, std::thread::hardware_concurrency());
    const std::size_t threads = configured == 0 ? hardware : configured;
    return std::max<std::size_t>(1, std::min(threads, std::max<std::size_t>(1, workItems)));
}

std::vector<std::size_t> sampleIndices(std::size_t population, std::size_t sampleSize, std::mt19937& rng)
{
    sampleSize = std::min(sampleSize, population);
    if (sampleSize == population) {
        std::vector<std::size_t> indices(population);
        std::iota(indices.begin(), indices.end(), 0);
        return indices;
    }

    std::unordered_set<std::size_t> selected;
    selected.reserve(sampleSize * 2);
    std::uniform_int_distribution<std::size_t> pick(0, population - 1);
    while (selected.size() < sampleSize) {
        selected.insert(pick(rng));
    }
    return {selected.begin(), selected.end()};
}

} // namespace

IsolationForestDetector::IsolationForestDetector(IsolationForestConfig config)
    : config_(config)
{
}

std::unique_ptr<IsolationForestDetector::Node> IsolationForestDetector::buildTree(
    const Matrix& samples,
    const std::vector<std::size_t>& indices,
    std::size_t depth,
    std::size_t maxDepth,
    std::mt19937& rng) const
{
    auto node = std::make_unique<Node>();
    node->sampleCount = indices.size();

    if (indices.size() <= 1 || depth >= maxDepth || samples.empty()) {
        return node;
    }

    const std::size_t dimensions = samples.front().size();
    std::vector<int> validFeatures;
    validFeatures.reserve(dimensions);
    std::vector<double> mins(dimensions, std::numeric_limits<double>::max());
    std::vector<double> maxs(dimensions, std::numeric_limits<double>::lowest());

    for (const std::size_t index : indices) {
        for (std::size_t feature = 0; feature < dimensions; ++feature) {
            mins[feature] = std::min(mins[feature], samples[index][feature]);
            maxs[feature] = std::max(maxs[feature], samples[index][feature]);
        }
    }

    for (std::size_t feature = 0; feature < dimensions; ++feature) {
        if (maxs[feature] - mins[feature] > 1e-12) {
            validFeatures.push_back(static_cast<int>(feature));
        }
    }

    if (validFeatures.empty()) {
        return node;
    }

    std::uniform_int_distribution<std::size_t> featurePick(0, validFeatures.size() - 1);
    node->feature = validFeatures[featurePick(rng)];
    std::uniform_real_distribution<double> splitPick(mins[static_cast<std::size_t>(node->feature)],
                                                     maxs[static_cast<std::size_t>(node->feature)]);
    node->split = splitPick(rng);

    std::vector<std::size_t> left;
    std::vector<std::size_t> right;
    left.reserve(indices.size());
    right.reserve(indices.size());

    for (const std::size_t index : indices) {
        if (samples[index][static_cast<std::size_t>(node->feature)] < node->split) {
            left.push_back(index);
        } else {
            right.push_back(index);
        }
    }

    if (left.empty() || right.empty()) {
        node->feature = -1;
        return node;
    }

    node->left = buildTree(samples, left, depth + 1, maxDepth, rng);
    node->right = buildTree(samples, right, depth + 1, maxDepth, rng);
    return node;
}

double IsolationForestDetector::pathLength(const Node* node, const std::vector<double>& sample, std::size_t depth) const
{
    if (node == nullptr) {
        return static_cast<double>(depth);
    }
    if (node->isLeaf() || node->feature < 0) {
        return static_cast<double>(depth) + cFactor(static_cast<double>(node->sampleCount));
    }

    if (sample[static_cast<std::size_t>(node->feature)] < node->split) {
        return pathLength(node->left.get(), sample, depth + 1);
    }
    return pathLength(node->right.get(), sample, depth + 1);
}

double IsolationForestDetector::cFactor(double sampleCount)
{
    if (sampleCount <= 1.0) {
        return 0.0;
    }
    if (sampleCount <= 2.0) {
        return 1.0;
    }
    return 2.0 * (std::log(sampleCount - 1.0) + EulerGamma) -
        (2.0 * (sampleCount - 1.0) / sampleCount);
}

DetectorResult IsolationForestDetector::run(const PreparedData& data) const
{
    PrototypeRunOptions options;
    options.isolationTrees = config_.trees;
    options.isolationSampleSize = config_.sampleSize;
    options.isolationThreshold = config_.threshold;
    options.isolationSeed = config_.seed == 2024 ? 777 : config_.seed;
    options.cancellation = config_.cancellation;
    options.progress = config_.progress;
    return SummitPrototypeAdapter::runIsolationForest(data, options);

    const auto started = std::chrono::steady_clock::now();

    DetectorResult result;
    result.algorithm = "isolation_forest";
    result.rowIds = data.rowIds;
    result.labels.assign(data.features.size(), 0);
    result.scores.assign(data.features.size(), 0.0);
    result.threshold = config_.threshold;

    if (data.features.empty()) {
        return result;
    }

    const std::size_t samples = data.features.size();
    const std::size_t treeCount = std::max<std::size_t>(1, config_.trees);
    const std::size_t sampleSize = std::max<std::size_t>(2, std::min(config_.sampleSize, samples));
    const std::size_t maxDepth = static_cast<std::size_t>(std::ceil(std::log2(static_cast<double>(sampleSize))));
    const std::size_t buildThreads = effectiveThreads(config_.threads, treeCount);
    const std::size_t scoreThreads = effectiveThreads(config_.threads, samples);

    std::vector<std::unique_ptr<Node>> forest(treeCount);
    std::vector<std::thread> workers;
    const std::size_t treeBlock = (treeCount + buildThreads - 1) / buildThreads;
    std::atomic<std::size_t> completedTrees{0};

    for (std::size_t workerIndex = 0; workerIndex < buildThreads; ++workerIndex) {
        const std::size_t begin = workerIndex * treeBlock;
        const std::size_t end = std::min(treeCount, begin + treeBlock);
        if (begin >= end) {
            continue;
        }
        workers.emplace_back([&, begin, end, workerIndex]() {
            std::mt19937 rng(config_.seed + static_cast<unsigned>(workerIndex * 7919 + begin));
            for (std::size_t tree = begin; tree < end; ++tree) {
                if (config_.cancellation != nullptr && config_.cancellation->isCancelled()) {
                    break;
                }
                auto indices = sampleIndices(samples, sampleSize, rng);
                forest[tree] = buildTree(data.features, indices, 0, maxDepth, rng);
                const std::size_t done = ++completedTrees;
                reportProgress(config_.progress, done, treeCount + samples, "building isolation forest");
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    if (config_.cancellation != nullptr && config_.cancellation->isCancelled()) {
        result.parameters = "cancelled=true";
        return result;
    }

    const double normalizer = std::max(1e-12, cFactor(static_cast<double>(sampleSize)));
    std::vector<std::thread> scorers;
    const std::size_t scoreBlock = (samples + scoreThreads - 1) / scoreThreads;
    std::atomic<std::size_t> scoredSamples{0};
    for (std::size_t workerIndex = 0; workerIndex < scoreThreads; ++workerIndex) {
        const std::size_t begin = workerIndex * scoreBlock;
        const std::size_t end = std::min(samples, begin + scoreBlock);
        if (begin >= end) {
            continue;
        }
        scorers.emplace_back([&, begin, end]() {
            for (std::size_t sample = begin; sample < end; ++sample) {
                if (config_.cancellation != nullptr && config_.cancellation->isCancelled()) {
                    break;
                }
                double totalPath = 0.0;
                for (const auto& tree : forest) {
                    if (!tree) {
                        continue;
                    }
                    totalPath += pathLength(tree.get(), data.features[sample], 0);
                }
                const double avgPath = totalPath / static_cast<double>(forest.size());
                const double score = std::pow(2.0, -avgPath / normalizer);
                result.scores[sample] = std::clamp(score, 0.0, 1.0);
                result.labels[sample] = result.scores[sample] >= config_.threshold ? 1 : 0;
                const std::size_t done = ++scoredSamples;
                reportProgress(config_.progress, treeCount + done, treeCount + samples, "scoring isolation forest");
            }
        });
    }
    for (auto& scorer : scorers) {
        scorer.join();
    }
    if (config_.cancellation != nullptr && config_.cancellation->isCancelled()) {
        result.parameters = "cancelled=true";
        return result;
    }

    const auto finished = std::chrono::steady_clock::now();
    result.executionMs = std::chrono::duration<double, std::milli>(finished - started).count();

    std::ostringstream params;
    params << "trees=" << treeCount << ";sample_size=" << sampleSize
           << ";threshold=" << config_.threshold << ";threads=" << buildThreads;
    result.parameters = params.str();
    return result;
}

} // namespace telemetry
