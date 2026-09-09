#pragma once

#include "graph/transport/FeedbackSccPlan.h"

#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace curlop::transport {

// A build-time-only view of graph regions that may be scheduled independently.
// It deliberately owns no workers, buffers, or callback synchronization. The
// APG bundle prepares it beside the graph so future executors never infer graph
// topology from the audio callback.
struct PreparedExecutionEdge {
    int sourceModule = -1;
    int destinationModule = -1;
    bool feedbackBoundary = false;
};

class PreparedExecutionPlan final {
public:
    // The current renderer owns one JUCE AudioProcessorGraph containing module
    // subgraphs plus inserted wire, sum, meter and control processors.  Its
    // render sequence is atomic: a module-level topological level is useful
    // topology evidence, but is not itself a callable parallel job.  A future
    // executor must construct isolated region render graphs and explicitly
    // select that surface before it can use these levels concurrently.
    enum class RenderSurface {
        AtomicAudioProcessorGraph,
        IsolatedRegionGraphs
    };

    struct Region {
        std::vector<int> modules;
        bool serial = false;
        // This is distinct from the conservative serial fallback used for an
        // undeclared cycle. A declared z^-1 SCC owns a sample-by-sample DSP
        // renderer, while its Script/VM producers still run exactly once for
        // the containing host block before that renderer starts.
        bool feedbackScc = false;
    };

    // A fixed source-to-destination dependency crossing between prepared
    // regions. The eventual isolated renderer owns the concrete boundary
    // buffer; retaining module identity here lets it build that buffer and its
    // deterministic merge order off the callback.
    struct Boundary {
        int sourceRegion = -1;
        int destinationRegion = -1;
        int sourceModule = -1;
        int destinationModule = -1;
    };

    static PreparedExecutionPlan build(
        int moduleCount,
        const std::vector<PreparedExecutionEdge>& edges)
    {
        PreparedExecutionPlan result;
        result.moduleCount_ = std::max(0, moduleCount);

        std::vector<FeedbackSccEdge> feedbackEdges;
        feedbackEdges.reserve(edges.size());
        for (const auto& edge : edges) {
            if (! result.validIndex(edge.sourceModule)
                || ! result.validIndex(edge.destinationModule)) {
                result.valid_ = false;
                return result;
            }
            feedbackEdges.push_back({ edge.sourceModule, edge.destinationModule,
                                      edge.feedbackBoundary });
        }
        const auto feedback = FeedbackSccPlan::build(result.moduleCount_, feedbackEdges);
        if (! feedback.valid()) {
            result.valid_ = false;
            return result;
        }

        std::vector<int> regionForModule(static_cast<std::size_t>(result.moduleCount_), -1);
        for (const auto& component : feedback.serialComponents()) {
            const int region = static_cast<int>(result.regions_.size());
            result.regions_.push_back({ component, true, true });
            for (const auto module : component)
                regionForModule[static_cast<std::size_t>(module)] = region;
        }
        for (int module = 0; module < result.moduleCount_; ++module) {
            if (regionForModule[static_cast<std::size_t>(module)] >= 0)
                continue;
            const int region = static_cast<int>(result.regions_.size());
            result.regions_.push_back({ { module }, false, false });
            regionForModule[static_cast<std::size_t>(module)] = region;
        }

        std::sort(result.regions_.begin(), result.regions_.end(),
                  [] (const Region& left, const Region& right) {
                      return left.modules.front() < right.modules.front();
                  });
        for (std::size_t region = 0; region < result.regions_.size(); ++region)
            for (const auto module : result.regions_[region].modules)
                regionForModule[static_cast<std::size_t>(module)] =
                    static_cast<int>(region);

        for (const auto& edge : edges) {
            const int source = regionForModule[static_cast<std::size_t>(edge.sourceModule)];
            const int destination = regionForModule[static_cast<std::size_t>(edge.destinationModule)];
            if (source != destination)
                result.boundaries_.push_back({ source, destination,
                                               edge.sourceModule, edge.destinationModule });
        }
        // Builder iteration order is not scheduling authority. Keep the
        // eventual region-buffer construction and fixed merge order stable
        // even when GraphState yields the same dependency set in a different
        // insertion order.
        std::sort(result.boundaries_.begin(), result.boundaries_.end(),
                  [] (const Boundary& left, const Boundary& right) {
                      return std::tie(left.sourceRegion, left.destinationRegion,
                                      left.sourceModule, left.destinationModule)
                          < std::tie(right.sourceRegion, right.destinationRegion,
                                     right.sourceModule, right.destinationModule);
                  });

        std::vector<std::set<int>> outgoing(result.regions_.size());
        std::vector<int> incoming(result.regions_.size(), 0);
        for (const auto& edge : edges) {
            const int source = regionForModule[static_cast<std::size_t>(edge.sourceModule)];
            const int destination = regionForModule[static_cast<std::size_t>(edge.destinationModule)];
            // A delayed route stays internal to its serial region. A boundary
            // crossing a region would have failed FeedbackSccPlan already.
            if (source == destination)
                continue;
            if (outgoing[static_cast<std::size_t>(source)].insert(destination).second)
                ++incoming[static_cast<std::size_t>(destination)];
        }

        std::set<int> ready;
        for (std::size_t region = 0; region < incoming.size(); ++region)
            if (incoming[region] == 0)
                ready.insert(static_cast<int>(region));
        std::size_t emitted = 0;
        while (! ready.empty()) {
            std::vector<int> level(ready.begin(), ready.end());
            ready.clear();
            emitted += level.size();
            result.levels_.push_back(std::move(level));
            for (const auto source : result.levels_.back())
                for (const auto destination : outgoing[static_cast<std::size_t>(source)])
                    if (--incoming[static_cast<std::size_t>(destination)] == 0)
                        ready.insert(destination);
        }
        if (emitted != result.regions_.size()) {
            // Undeclared cycles remain legal in the current APG/Script V2
            // model. They have no safe independently schedulable region, so
            // preserve the production renderer's conservative serial fallback
            // instead of rejecting an otherwise valid graph at build time.
            result.levels_.clear();
            for (std::size_t region = 0; region < result.regions_.size(); ++region) {
                result.regions_[region].serial = true;
                result.levels_.push_back({ static_cast<int>(region) });
            }
        }
        result.valid_ = true;
        return result;
    }

    bool valid() const noexcept { return valid_; }
    int moduleCount() const noexcept { return moduleCount_; }
    const std::vector<Region>& regions() const noexcept { return regions_; }
    const std::vector<Boundary>& boundaries() const noexcept { return boundaries_; }
    const std::vector<std::vector<int>>& levels() const noexcept { return levels_; }
    bool hasParallelLevel() const noexcept
    {
        for (const auto& level : levels_)
            if (level.size() > 1u)
                return true;
        return false;
    }

    bool canExecuteConcurrently(RenderSurface surface) const noexcept
    {
        return valid_ && hasParallelLevel()
            && surface == RenderSurface::IsolatedRegionGraphs;
    }

    bool moduleInFeedbackScc(int module) const noexcept
    {
        for (const auto& region : regions_)
            if (region.feedbackScc
                && std::find(region.modules.begin(), region.modules.end(), module)
                    != region.modules.end())
                return true;
        return false;
    }

private:
    bool validIndex(int module) const noexcept
    {
        return module >= 0 && module < moduleCount_;
    }

    int moduleCount_ = 0;
    bool valid_ = true;
    std::vector<Region> regions_;
    std::vector<Boundary> boundaries_;
    std::vector<std::vector<int>> levels_;
};

} // namespace curlop::transport
