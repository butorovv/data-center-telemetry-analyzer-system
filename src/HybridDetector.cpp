#include "telemetry/HybridDetector.hpp"

#include <chrono>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace telemetry {
namespace {

std::string timedKey(const std::string& timestamp, const std::string& hostname)
{
    return timestamp + '\x1F' + hostname;
}

} // namespace

DetectorResult HybridDetector::verifyWithGraph(
    const TelemetryDataset& dataset,
    const GraphContext& graph,
    const DetectorResult& isolationForestResult) const
{
    const auto started = std::chrono::steady_clock::now();

    DetectorResult result;
    result.algorithm = "hybrid_iforest_graph";
    result.rowIds = isolationForestResult.rowIds;
    result.labels.assign(isolationForestResult.labels.size(), 0);
    // Keep the original Isolation Forest score; graph verification only changes the final label.
    result.scores = isolationForestResult.scores;
    result.threshold = isolationForestResult.threshold;

    const std::size_t rows = std::min(dataset.rows.size(), isolationForestResult.labels.size());
    std::unordered_map<std::string, std::size_t> timestampCounts;
    timestampCounts.reserve(rows);
    for (std::size_t i = 0; i < rows; ++i) {
        ++timestampCounts[dataset.rows[i].timestamp];
    }
    bool hasSharedTimestamps = false;
    for (const auto& [timestamp, count] : timestampCounts) {
        (void)timestamp;
        if (count > 1) {
            hasSharedTimestamps = true;
            break;
        }
    }

    std::unordered_set<std::string> candidateAtTime;
    std::unordered_set<std::string> candidateHosts;
    candidateAtTime.reserve(rows);
    candidateHosts.reserve(graph.hosts.size());

    for (std::size_t i = 0; i < rows; ++i) {
        if (isolationForestResult.labels[i] == 0) {
            continue;
        }
        candidateAtTime.insert(timedKey(dataset.rows[i].timestamp, dataset.rows[i].hostname));
        candidateHosts.insert(dataset.rows[i].hostname);
    }

    for (std::size_t i = 0; i < rows; ++i) {
        if (isolationForestResult.labels[i] == 0) {
            continue;
        }

        const auto hostIt = graph.hostToIndex.find(dataset.rows[i].hostname);
        if (hostIt == graph.hostToIndex.end()) {
            continue;
        }

        bool verified = false;
        const auto& neighbors = graph.adjacency[hostIt->second];
        for (const std::size_t neighbor : neighbors) {
            const std::string& neighborHost = graph.hosts[neighbor];
            if (hasSharedTimestamps) {
                verified = candidateAtTime.find(timedKey(dataset.rows[i].timestamp, neighborHost)) !=
                    candidateAtTime.end();
            } else {
                verified = candidateHosts.find(neighborHost) != candidateHosts.end();
            }
            if (verified) {
                break;
            }
        }

        result.labels[i] = verified ? 1 : 0;
    }

    const auto finished = std::chrono::steady_clock::now();
    const double verificationMs = std::chrono::duration<double, std::milli>(finished - started).count();
    result.executionMs = isolationForestResult.executionMs + verificationMs;

    std::ostringstream params;
    params << "base=isolation_forest;graph=hostname_rack_prefix;verification_ms=" << verificationMs;
    result.parameters = params.str();
    return result;
}

} // namespace telemetry
