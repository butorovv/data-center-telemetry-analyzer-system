#pragma once

#include "telemetry/Models.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace telemetry {

struct HostLocation {
    std::string rack;
    int position = -1;
    bool valid = false;
};

struct GraphContext {
    std::vector<std::string> hosts;
    std::unordered_map<std::string, std::size_t> hostToIndex;
    std::vector<std::vector<std::size_t>> adjacency;
    std::unordered_map<std::string, HostLocation> locations;
};

class GraphBuilder {
public:
    GraphContext build(const TelemetryDataset& dataset) const;
    static std::string rackPrefix(const std::string& hostname);
    static HostLocation parseHostLocation(const std::string& hostname);
};

} // namespace telemetry
