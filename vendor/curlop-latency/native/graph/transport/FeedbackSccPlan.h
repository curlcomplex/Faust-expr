#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace curlop::transport {

struct FeedbackSccEdge {
    int sourceModule = -1;
    int destinationModule = -1;
    bool feedbackBoundary = false;
};

// Deterministic partitioning for the eventual local feedback executor. Only a
// strongly connected component containing a declared feedback boundary enters
// the one-sample serial domain; all other module indices remain block-domain
// work for the top-level graph scheduler.
class FeedbackSccPlan final {
public:
    static FeedbackSccPlan build(int moduleCount,
                                 const std::vector<FeedbackSccEdge>& edges)
    {
        FeedbackSccPlan result;
        result.moduleCount_ = std::max(0, moduleCount);
        result.serialModules_.assign(static_cast<std::size_t>(result.moduleCount_), false);
        std::vector<std::vector<int>> outgoing(
            static_cast<std::size_t>(result.moduleCount_));
        for (const auto& edge : edges) {
            if (! result.validIndex(edge.sourceModule)
                || ! result.validIndex(edge.destinationModule)) {
                result.valid_ = false;
                return result;
            }
            outgoing[static_cast<std::size_t>(edge.sourceModule)].push_back(
                edge.destinationModule);
        }
        for (auto& neighbours : outgoing) {
            std::sort(neighbours.begin(), neighbours.end());
            neighbours.erase(std::unique(neighbours.begin(), neighbours.end()),
                             neighbours.end());
        }

        std::vector<int> index(static_cast<std::size_t>(result.moduleCount_), -1);
        std::vector<int> lowlink(static_cast<std::size_t>(result.moduleCount_));
        std::vector<int> stack;
        std::vector<bool> onStack(static_cast<std::size_t>(result.moduleCount_), false);
        int nextIndex = 0;
        std::vector<int> componentOf(static_cast<std::size_t>(result.moduleCount_), -1);
        std::vector<std::vector<int>> components;
        const auto visit = [&] (auto&& self, int module) -> void {
            index[static_cast<std::size_t>(module)] = nextIndex;
            lowlink[static_cast<std::size_t>(module)] = nextIndex++;
            stack.push_back(module);
            onStack[static_cast<std::size_t>(module)] = true;
            for (const auto target : outgoing[static_cast<std::size_t>(module)]) {
                if (index[static_cast<std::size_t>(target)] == -1) {
                    self(self, target);
                    lowlink[static_cast<std::size_t>(module)] = std::min(
                        lowlink[static_cast<std::size_t>(module)],
                        lowlink[static_cast<std::size_t>(target)]);
                } else if (onStack[static_cast<std::size_t>(target)]) {
                    lowlink[static_cast<std::size_t>(module)] = std::min(
                        lowlink[static_cast<std::size_t>(module)],
                        index[static_cast<std::size_t>(target)]);
                }
            }
            if (lowlink[static_cast<std::size_t>(module)]
                != index[static_cast<std::size_t>(module)])
                return;
            std::vector<int> component;
            while (! stack.empty()) {
                const int member = stack.back();
                stack.pop_back();
                onStack[static_cast<std::size_t>(member)] = false;
                componentOf[static_cast<std::size_t>(member)] =
                    static_cast<int>(components.size());
                component.push_back(member);
                if (member == module)
                    break;
            }
            std::sort(component.begin(), component.end());
            components.push_back(std::move(component));
        };
        for (int module = 0; module < result.moduleCount_; ++module)
            if (index[static_cast<std::size_t>(module)] == -1)
                visit(visit, module);

        std::vector<bool> serialComponent(components.size(), false);
        for (const auto& edge : edges) {
            if (! edge.feedbackBoundary)
                continue;
            const auto source = componentOf[static_cast<std::size_t>(edge.sourceModule)];
            const auto destination = componentOf[static_cast<std::size_t>(edge.destinationModule)];
            if (source != destination) {
                result.valid_ = false;
                return result;
            }
            serialComponent[static_cast<std::size_t>(source)] = true;
            ++result.boundaryCount_;
        }
        for (std::size_t component = 0; component < components.size(); ++component) {
            if (! serialComponent[component])
                continue;
            result.serialComponents_.push_back(components[component]);
            for (const auto member : components[component])
                result.serialModules_[static_cast<std::size_t>(member)] = true;
        }
        std::sort(result.serialComponents_.begin(), result.serialComponents_.end(),
                  [] (const auto& left, const auto& right) {
                      return left.front() < right.front();
                  });
        return result;
    }

    bool valid() const noexcept { return valid_; }
    std::size_t boundaryCount() const noexcept { return boundaryCount_; }
    bool moduleRequiresSerialExecution(int module) const noexcept
    {
        return validIndex(module)
            && serialModules_[static_cast<std::size_t>(module)];
    }
    const std::vector<std::vector<int>>& serialComponents() const noexcept
    {
        return serialComponents_;
    }

private:
    bool validIndex(int module) const noexcept
    {
        return module >= 0 && module < moduleCount_;
    }

    int moduleCount_ = 0;
    bool valid_ = true;
    std::size_t boundaryCount_ = 0;
    std::vector<bool> serialModules_;
    std::vector<std::vector<int>> serialComponents_;
};

} // namespace curlop::transport
