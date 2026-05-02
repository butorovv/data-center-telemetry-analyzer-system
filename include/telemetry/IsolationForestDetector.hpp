#pragma once

#include "telemetry/Models.hpp"
#include "telemetry/TaskControl.hpp"

#include <cstddef>
#include <memory>
#include <random>
#include <vector>

namespace telemetry {

struct IsolationForestConfig {
    std::size_t trees = 100;
    std::size_t sampleSize = 256;
    double threshold = 0.75;
    std::size_t threads = 0;
    unsigned seed = 2024;
    CancellationToken* cancellation = nullptr;
    ProgressCallback progress;
};

class IsolationForestDetector {
public:
    explicit IsolationForestDetector(IsolationForestConfig config = {});

    DetectorResult run(const PreparedData& data) const;

private:
    struct Node {
        int feature = -1;
        double split = 0.0;
        std::size_t sampleCount = 0;
        std::unique_ptr<Node> left;
        std::unique_ptr<Node> right;

        bool isLeaf() const { return !left && !right; }
    };

    using Matrix = std::vector<std::vector<double>>;

    std::unique_ptr<Node> buildTree(
        const Matrix& samples,
        const std::vector<std::size_t>& indices,
        std::size_t depth,
        std::size_t maxDepth,
        std::mt19937& rng) const;

    double pathLength(const Node* node, const std::vector<double>& sample, std::size_t depth) const;
    static double cFactor(double sampleCount);

    IsolationForestConfig config_;
};

} // namespace telemetry

