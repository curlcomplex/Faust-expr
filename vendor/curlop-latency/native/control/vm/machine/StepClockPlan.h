#pragma once

#include <cstddef>
#include <cstdint>

#include "control/vm/machine/Signal.h"

namespace curlop
{

enum StepClockFlags : std::uint8_t
{
    GateOff = 1,
    GateOn  = 2,
    Reaim   = 4
};

struct StepClockEvent
{
    std::uint32_t frameOffset;
    std::uint32_t sourceOrdinal;
    std::uint64_t iteration;
    float pitch;
    float velocity;
    std::uint8_t flags;
};

struct StepClockPlanView
{
    const StepClockEvent* events;
    std::size_t count;
    int numSamples;
};

struct StepClockOutputBinding
{
    std::uint32_t lane;
    vm::SignalType type;
    bool usePlanValue;
};

} // namespace curlop
