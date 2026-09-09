#pragma once

#include <juce_core/juce_core.h>
#include <cstdint>
#include <optional>

namespace curlop { class ClipStateContainer; }
namespace curlop::gui { struct ModuleBrowserPresentation; }

namespace curlop::webview
{
inline constexpr int contractVersion = 1;

bool isValidUserEvent (const juce::var& value);
bool isCanonicalKeyDescription (const juce::String& description);
bool isValidSnapshot (const juce::var& value);
bool isSnapshotForViewedClip (const juce::var& value, int viewedClipId);
juce::var snapshotForViewedClip (const juce::var& value, int viewedClipId);
juce::var snapshotForWebViewPresentation (const juce::var& value,
                                          int viewedClipId,
                                          int activeClipId,
                                          bool performanceOnly);
juce::var graphSnapshotWithProjectAuthority (const juce::var& value,
                                             uint64_t projectEpoch);

// Resolve a graph/parameter mutation onto C++'s current viewed real clip.
// Missing clipId (-1) uses current authority; stale, unknown, and ephemeral
// identities are rejected with -1.
int resolveViewedRealMutationClipId (int requestedClipId,
                                     const ClipStateContainer& clips);
bool hasViewedWebViewMutationAuthority (const juce::var& userEvent,
                                        const ClipStateContainer& clips);
bool isValidCapabilityReport (const juce::var& value);
bool isValidReport (const juce::var& value);
bool isValidNativeResult (const juce::var& value);

juce::var buildWebViewResult (bool ok,
                              const juce::var& result = {},
                              const juce::String& error = {},
                              std::optional<uint64_t> projectEpoch = std::nullopt);
juce::var buildModuleBrowserPresentationSnapshot (bool inspectorOpen,
                                                  int inspectorWidth);
juce::var buildModuleBrowserPresentationSnapshot (
    const curlop::gui::ModuleBrowserPresentation& presentation);
juce::var buildGuiScaleSnapshot (float currentScale);
juce::var buildWireVisualOptionsSnapshot (int pitchColourMode,
                                          int selectionMode,
                                          int mutedMode);
}
