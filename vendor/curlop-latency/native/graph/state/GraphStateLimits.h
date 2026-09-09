#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace curlop
{
// Physical voice capacity is a prepared-resource request, not a product byte
// limit. The public/state representation is the host's non-negative int range;
// allocation failure and realtime capacity are handled by the preparation path.
inline constexpr int kMaxPhysicalVoices = std::numeric_limits<int>::max();
inline constexpr int kMaxStealPolicy = 2;

inline int clampPhysicalVoices(int voices) noexcept
{
    if (voices < 0)
        return 0;
    return voices;
}

inline int clampStealPolicy(int policy) noexcept
{
    if (policy < 0)
        return 0;
    if (policy > kMaxStealPolicy)
        return kMaxStealPolicy;
    return policy;
}

enum class PhysicalVoicePreparationFailure : std::uint8_t
{
    None,
    InvalidRequest,
    ArithmeticOverflow,
    ResourceBudgetExceeded
};

struct PhysicalVoicePreparationRequest
{
    int voices = 1;
    std::size_t baseInputCount = 0;
    std::size_t perVoiceInputCount = 0;
    std::size_t blockFrames = 0;
    std::size_t fixedBytesPerExpandedInput = 0;
    std::size_t bytesPerExpandedInputFrame = 0;
    std::size_t bytesPerVoice = 0;
    // Supplied by the off-thread owner from the current runtime environment.
    // max() means arithmetic-only validation, never a fixed product ceiling.
    std::size_t byteBudget = std::numeric_limits<std::size_t>::max();
};

struct PhysicalVoicePreparationResult
{
    PhysicalVoicePreparationFailure failure =
        PhysicalVoicePreparationFailure::None;
    std::size_t expandedInputCount = 0;
    std::size_t requiredBytes = 0;

    constexpr bool ok() const noexcept
    {
        return failure == PhysicalVoicePreparationFailure::None;
    }

    constexpr const char* diagnostic() const noexcept
    {
        switch (failure)
        {
            case PhysicalVoicePreparationFailure::None:
                return "";
            case PhysicalVoicePreparationFailure::InvalidRequest:
                return "invalid physical voice preparation request";
            case PhysicalVoicePreparationFailure::ArithmeticOverflow:
                return "physical voice preparation size overflow";
            case PhysicalVoicePreparationFailure::ResourceBudgetExceeded:
                return "physical voice preparation exceeds available resources";
        }
        return "invalid physical voice preparation request";
    }
};

namespace detail
{
constexpr bool checkedAddSize(
    std::size_t first,
    std::size_t second,
    std::size_t& result) noexcept
{
    if (second > std::numeric_limits<std::size_t>::max() - first)
        return false;
    result = first + second;
    return true;
}

constexpr bool checkedMultiplySize(
    std::size_t first,
    std::size_t second,
    std::size_t& result) noexcept
{
    if (first != 0
        && second > std::numeric_limits<std::size_t>::max() / first)
        return false;
    result = first * second;
    return true;
}
} // namespace detail

// Resource validation is shape- and environment-derived. It deliberately
// imposes no voice-count constant: the same request may pass on a larger
// machine, while any request whose minimum prepared storage cannot fit the
// current environment fails before graph publication.
constexpr PhysicalVoicePreparationResult validatePhysicalVoicePreparation(
    const PhysicalVoicePreparationRequest& request) noexcept
{
    PhysicalVoicePreparationResult result;
    if (request.voices < 1
        || request.perVoiceInputCount > request.baseInputCount
        || request.blockFrames == 0)
    {
        result.failure = PhysicalVoicePreparationFailure::InvalidRequest;
        return result;
    }

    const auto voices = static_cast<std::size_t>(request.voices);
    std::size_t addedInputs = 0;
    std::size_t expandedInputs = request.baseInputCount;
    if (! detail::checkedMultiplySize(
            voices - 1u, request.perVoiceInputCount, addedInputs)
        || ! detail::checkedAddSize(
            expandedInputs, addedInputs, expandedInputs)
        || (voices > 1u
            && ! detail::checkedAddSize(
                expandedInputs, 1u, expandedInputs)))
    {
        result.failure = PhysicalVoicePreparationFailure::ArithmeticOverflow;
        return result;
    }
    result.expandedInputCount = expandedInputs;

    std::size_t perInputFrameBytes = 0;
    std::size_t perInputBytes = 0;
    std::size_t expandedInputBytes = 0;
    std::size_t voiceBytes = 0;
    std::size_t requiredBytes = 0;
    if (! detail::checkedMultiplySize(
            request.blockFrames,
            request.bytesPerExpandedInputFrame,
            perInputFrameBytes)
        || ! detail::checkedAddSize(
            request.fixedBytesPerExpandedInput,
            perInputFrameBytes,
            perInputBytes)
        || ! detail::checkedMultiplySize(
            expandedInputs, perInputBytes, expandedInputBytes)
        || ! detail::checkedMultiplySize(
            voices, request.bytesPerVoice, voiceBytes)
        || ! detail::checkedAddSize(
            expandedInputBytes, voiceBytes, requiredBytes))
    {
        result.failure = PhysicalVoicePreparationFailure::ArithmeticOverflow;
        return result;
    }

    result.requiredBytes = requiredBytes;
    if (requiredBytes > request.byteBudget)
        result.failure =
            PhysicalVoicePreparationFailure::ResourceBudgetExceeded;
    return result;
}
}
