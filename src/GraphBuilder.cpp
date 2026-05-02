#include "telemetry/GraphBuilder.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <regex>
#include <unordered_map>

#ifdef HAS_BOOST_GRAPH
#include <boost/graph/adjacency_list.hpp>
#endif

namespace telemetry {
namespace {

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

void addUndirectedEdge(GraphContext& context, std::size_t left, std::size_t right)
{
    if (left == right) {
        return;
    }
    auto addOneWay = [&](std::size_t from, std::size_t to) {
        auto& list = context.adjacency[from];
        if (std::find(list.begin(), list.end(), to) == list.end()) {
            list.push_back(to);
        }
    };
    addOneWay(left, right);
    addOneWay(right, left);
}

} // namespace

std::string GraphBuilder::rackPrefix(const std::string& hostname)
{
    const HostLocation location = parseHostLocation(hostname);
    if (location.valid) {
        return location.rack;
    }

    std::string host = lower(hostname);
    const std::size_t dot = host.find('.');
    if (dot != std::string::npos) {
        host = host.substr(0, dot);
    }

    const std::size_t delimiter = host.find_last_of("-_/");
    if (delimiter != std::string::npos && delimiter + 1 < host.size()) {
        bool suffixHasDigit = false;
        for (std::size_t i = delimiter + 1; i < host.size(); ++i) {
            suffixHasDigit = suffixHasDigit || std::isdigit(static_cast<unsigned char>(host[i]));
        }
        if (suffixHasDigit) {
            return host.substr(0, delimiter);
        }
    }

    while (!host.empty() && std::isdigit(static_cast<unsigned char>(host.back()))) {
        host.pop_back();
    }
    return host.empty() ? lower(hostname) : host;
}

HostLocation GraphBuilder::parseHostLocation(const std::string& hostname)
{
    const std::string host = lower(hostname);

    std::smatch match;
    static const std::regex rackPositionPattern(R"(rack([0-9a-z]+)[_\-]?position([0-9]+))");
    if (std::regex_search(host, match, rackPositionPattern)) {
        HostLocation location;
        location.rack = "rack" + match[1].str();
        location.position = std::stoi(match[2].str());
        location.valid = true;
        return location;
    }

    static const std::regex nodePattern(R"(node([0-9]+))");
    if (std::regex_search(host, match, nodePattern)) {
        HostLocation location;
        location.rack = "node";
        location.position = std::stoi(match[1].str());
        location.valid = true;
        return location;
    }

    if (host.size() >= 3 &&
        std::isdigit(static_cast<unsigned char>(host[1])) &&
        std::isdigit(static_cast<unsigned char>(host[2]))) {
        HostLocation location;
        location.rack.assign(1, host[0]);
        location.position = (host[1] - '0') * 10 + (host[2] - '0');
        location.valid = true;
        return location;
    }

    static const std::regex suffixPattern(R"((.*?)([0-9]+)$)");
    if (std::regex_match(host, match, suffixPattern) && !match[1].str().empty()) {
        HostLocation location;
        location.rack = match[1].str();
        location.position = std::stoi(match[2].str());
        location.valid = true;
        return location;
    }

    return HostLocation{};
}

GraphContext GraphBuilder::build(const TelemetryDataset& dataset) const
{
    GraphContext context;
    for (const auto& row : dataset.rows) {
        if (context.hostToIndex.find(row.hostname) != context.hostToIndex.end()) {
            continue;
        }
        const std::size_t index = context.hosts.size();
        context.hostToIndex[row.hostname] = index;
        context.hosts.push_back(row.hostname);
    }
    context.adjacency.assign(context.hosts.size(), {});

    std::unordered_map<std::string, std::vector<std::size_t>> byRack;
    bool parsedAnyLocation = false;
    for (std::size_t i = 0; i < context.hosts.size(); ++i) {
        const HostLocation location = parseHostLocation(context.hosts[i]);
        context.locations[context.hosts[i]] = location;
        if (location.valid) {
            parsedAnyLocation = true;
            byRack[location.rack].push_back(i);
        }
    }

    if (parsedAnyLocation) {
        for (auto& [rack, hosts] : byRack) {
            (void)rack;
            std::sort(hosts.begin(), hosts.end(), [&](std::size_t left, std::size_t right) {
                const auto& leftLocation = context.locations[context.hosts[left]];
                const auto& rightLocation = context.locations[context.hosts[right]];
                if (leftLocation.position != rightLocation.position) {
                    return leftLocation.position < rightLocation.position;
                }
                return context.hosts[left] < context.hosts[right];
            });

            for (std::size_t i = 0; i < hosts.size(); ++i) {
                for (std::size_t j = i + 1; j < hosts.size(); ++j) {
                    const auto& leftLocation = context.locations[context.hosts[hosts[i]]];
                    const auto& rightLocation = context.locations[context.hosts[hosts[j]]];
                    const int delta = std::abs(leftLocation.position - rightLocation.position);
                    if (delta == 1) {
                        addUndirectedEdge(context, hosts[i], hosts[j]);
                    } else if (delta > 1) {
                        break;
                    }
                }
            }
        }
    } else {
        std::vector<std::size_t> hosts(context.hosts.size());
        for (std::size_t i = 0; i < hosts.size(); ++i) {
            hosts[i] = i;
        }
        std::sort(hosts.begin(), hosts.end(), [&](std::size_t left, std::size_t right) {
            return context.hosts[left] < context.hosts[right];
        });
        for (std::size_t i = 1; i < hosts.size(); ++i) {
            addUndirectedEdge(context, hosts[i - 1], hosts[i]);
        }
    }

    for (std::size_t i = 0; i < context.hosts.size(); ++i) {
        if (!context.adjacency[i].empty()) {
            continue;
        }
        std::vector<std::size_t> sorted(context.hosts.size());
        for (std::size_t j = 0; j < sorted.size(); ++j) {
            sorted[j] = j;
        }
        std::sort(sorted.begin(), sorted.end(), [&](std::size_t left, std::size_t right) {
            return context.hosts[left] < context.hosts[right];
        });
        const auto it = std::find(sorted.begin(), sorted.end(), i);
        if (it != sorted.end()) {
            if (it != sorted.begin()) {
                addUndirectedEdge(context, i, *(it - 1));
            }
            if (std::next(it) != sorted.end()) {
                addUndirectedEdge(context, i, *std::next(it));
            }
        }
    }

#ifdef HAS_BOOST_GRAPH
    using BoostGraph = boost::adjacency_list<boost::vecS, boost::vecS, boost::undirectedS>;
    BoostGraph graph(context.hosts.size());
    for (std::size_t from = 0; from < context.adjacency.size(); ++from) {
        for (const std::size_t to : context.adjacency[from]) {
            if (from < to) {
                boost::add_edge(from, to, graph);
            }
        }
    }
#endif

    return context;
}

} // namespace telemetry
