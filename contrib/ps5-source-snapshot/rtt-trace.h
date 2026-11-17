/* Bounded CPU-side RTT samples. Sequence starts at one. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
static inline bool sc1_rtt_trace_sample(uint64_t sequence, unsigned emitted)
{
    return emitted < 64 && (sequence <= 16 || (sequence % 256) == 0);
}
