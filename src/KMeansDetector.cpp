#include "telemetry/KMeansDetector.hpp"
#include "telemetry/SummitPrototypeAdapter.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <limits>
#include <numeric>
#include <random>
#include <sstream>
#include <thread>

namespace telemetry {
namespace {

double squaredDistance(const std::vector<double>& a, const std::vector<double>& b)
{
    double result = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double diff = a[i] - b[i];
        result += diff * diff;
    }
    return result;
}

std::size_t effectiveThreads(std::size_t configured, std::size_t samples)
{
    const std::size_t hardware = std::max(1u, std::thread::hardware_concurrency());
    const std::size_t threads = configured == 0 ? hardware : configured;
    return std::max<std::size_t>(1, std::min(threads, std::max<std::size_t>(1, samples)));
}

} // namespace

KMeansDetector::KMeansDetector(KMeansConfig config)
    : config_(config)
{
}

DetectorResult KMeansDetector::run(const PreparedData& data) const
{
    PrototypeRunOptions options;
    options.kmeansClusters = config_.clusters;
    options.kmeansIterations = std::min<std::size_t>(config_.maxIterations, 10);
    options.cancellation = config_.cancellation;
    options.progress = config_.progress;
    return SummitPrototypeAdapter::runKMeans(data, options);

    const auto started = std::chrono::steady_clock::now();

    DetectorResult result;
    result.algorithm = "kmeans";
    result.rowIds = data.rowIds;
    result.labels.assign(data.features.size(), 0);
    result.scores.assign(data.features.size(), 0.0);

    if (data.features.empty()) {
        return result;
    }

    const std::size_t samples = data.features.size();
    const std::size_t dimensions = data.features.front().size();
    const std::size_t clusters = std::max<std::size_t>(1, std::min(config_.clusters, samples));
    const std::size_t threads = effectiveThreads(config_.threads, samples);
    std::mt19937 rng(config_.seed);

    std::vector<std::vector<double>> centers;
    centers.reserve(clusters);

    std::uniform_int_distribution<std::size_t> firstCenter(0, samples - 1);
    centers.push_back(data.features[firstCenter(rng)]);

    std::vector<double> nearestSquared(samples, std::numeric_limits<double>::max());
    while (centers.size() < clusters) {
        double total = 0.0;
        for (std::size_t i = 0; i < samples; ++i) {
            nearestSquared[i] = std::min(nearestSquared[i], squaredDistance(data.features[i], centers.back()));
            total += nearestSquared[i];
        }

        if (total <= 1e-12) {
            centers.push_back(data.features[firstCenter(rng)]);
            continue;
        }

        std::uniform_real_distribution<double> pick(0.0, total);
        double target = pick(rng);
        std::size_t chosen = samples - 1;
        for (std::size_t i = 0; i < samples; ++i) {
            target -= nearestSquared[i];
            if (target <= 0.0) {
                chosen = i;
                break;
            }
        }
        centers.push_back(data.features[chosen]);
    }

    std::vector<std::size_t> assignments(samples, 0);
    std::vector<double> distances(samples, 0.0);

    auto assignRange = [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            double bestDistance = std::numeric_limits<double>::max();
            std::size_t bestCluster = 0;
            for (std::size_t cluster = 0; cluster < clusters; ++cluster) {
                const double distance = squaredDistance(data.features[i], centers[cluster]);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    bestCluster = cluster;
                }
            }
            assignments[i] = bestCluster;
            distances[i] = std::sqrt(bestDistance);
        }
    };

    for (std::size_t iteration = 0; iteration < config_.maxIterations; ++iteration) {
        if (config_.cancellation != nullptr && config_.cancellation->isCancelled()) {
            result.parameters = "cancelled=true";
            return result;
        }
        std::vector<std::future<void>> futures;
        const std::size_t block = (samples + threads - 1) / threads;
        for (std::size_t thread = 0; thread < threads; ++thread) {
            const std::size_t begin = thread * block;
            const std::size_t end = std::min(samples, begin + block);
            if (begin >= end) {
                continue;
            }
            futures.push_back(std::async(std::launch::async, assignRange, begin, end));
        }
        for (auto& future : futures) {
            future.get();
        }

        std::vector<std::vector<double>> newCenters(clusters, std::vector<double>(dimensions, 0.0));
        std::vector<std::size_t> counts(clusters, 0);
        for (std::size_t i = 0; i < samples; ++i) {
            const std::size_t cluster = assignments[i];
            ++counts[cluster];
            for (std::size_t dim = 0; dim < dimensions; ++dim) {
                newCenters[cluster][dim] += data.features[i][dim];
            }
        }

        for (std::size_t cluster = 0; cluster < clusters; ++cluster) {
            if (counts[cluster] == 0) {
                newCenters[cluster] = data.features[firstCenter(rng)];
                continue;
            }
            for (double& value : newCenters[cluster]) {
                value /= static_cast<double>(counts[cluster]);
            }
        }

        double shift = 0.0;
        for (std::size_t cluster = 0; cluster < clusters; ++cluster) {
            shift += squaredDistance(centers[cluster], newCenters[cluster]);
        }
        centers = std::move(newCenters);
        reportProgress(config_.progress, iteration + 1, config_.maxIterations, "k-means iterations");
        if (shift < 1e-8) {
            break;
        }
    }

    if (config_.cancellation != nullptr && config_.cancellation->isCancelled()) {
        result.parameters = "cancelled=true";
        return result;
    }
    assignRange(0, samples);

    std::vector<std::size_t> finalCounts(clusters, 0);
    for (const std::size_t assignment : assignments) {
        ++finalCounts[assignment];
    }

    std::vector<double> anomalyDistances = distances;
    const std::size_t smallClusterLimit = std::max<std::size_t>(1, samples / 100);
    if (clusters > 1) {
        for (std::size_t i = 0; i < samples; ++i) {
            const std::size_t ownCluster = assignments[i];
            if (finalCounts[ownCluster] > smallClusterLimit) {
                continue;
            }

            double nearestOtherCenter = std::numeric_limits<double>::max();
            for (std::size_t cluster = 0; cluster < clusters; ++cluster) {
                if (cluster == ownCluster) {
                    continue;
                }
                nearestOtherCenter = std::min(
                    nearestOtherCenter,
                    std::sqrt(squaredDistance(data.features[i], centers[cluster])));
            }
            anomalyDistances[i] = std::max(anomalyDistances[i], nearestOtherCenter);
        }
    }

    const double mean = std::accumulate(anomalyDistances.begin(), anomalyDistances.end(), 0.0) /
        static_cast<double>(anomalyDistances.size());
    double variance = 0.0;
    for (const double distance : anomalyDistances) {
        const double diff = distance - mean;
        variance += diff * diff;
    }
    const double stddev = std::sqrt(variance / static_cast<double>(anomalyDistances.size()));
    result.threshold = mean + config_.thresholdSigma * stddev;
    if (result.threshold <= 1e-12) {
        result.threshold = mean + 1e-12;
    }

    for (std::size_t i = 0; i < samples; ++i) {
        result.labels[i] = anomalyDistances[i] > result.threshold ? 1 : 0;
        result.scores[i] = std::min(1.0, anomalyDistances[i] / result.threshold);
    }

    const auto finished = std::chrono::steady_clock::now();
    result.executionMs = std::chrono::duration<double, std::milli>(finished - started).count();

    std::ostringstream params;
    params << "k=" << clusters << ";init=kmeans++;threshold=mean+"
           << config_.thresholdSigma << "*std;small_cluster_guard="
           << smallClusterLimit << ";threads=" << threads;
    result.parameters = params.str();
    return result;
}

} // namespace telemetry
