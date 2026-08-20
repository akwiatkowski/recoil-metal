#!/bin/sh
# No duration may be written as a number of ticks (PLAN2.md §5.1, D2).
#
# The rule: every duration and rate is authored once, in SECONDS, and the number of ticks is
# always derived from the configured rate. A constant like
#
#     inline constexpr int kProjectileLifetimeTicks = 300;   // "thirty seconds at 10 Hz"
#
# is only correct at one rate, with the rate recorded in a comment instead of in the
# arithmetic. Change the rate and nothing fails to compile, no test goes red, and the balance
# quietly changes. That is the bug this catches, and a grep is the only tool that can see it —
# so it is registered as a test rather than left as a script somebody remembers to run.
#
# What is allowed: a name ending in `Ticks` or `TickCount` whose value comes from
# `rate.ticks(...)`, and `TickCount` variables generally. Also allowed are the `*TicksPerSecond`
# constants — those are the RATE itself, which is the one number the whole scheme is defined
# against, not a duration expressed in it. What is not allowed: a tick-denominated constant
# with a literal on the right-hand side.
set -eu

root=${1:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
sim="$root/src/core/sim"

# `constexpr <int type> k...Ticks... = <digits>` — a hardcoded duration in ticks.
# `TickRate.hpp` is exempt: it quotes both historical offenders in its own explanation of why
# they are forbidden, which is documentation rather than a constant.
found=$(grep -rnE \
    'constexpr[[:space:]]+(int|unsigned|std::[a-z0-9_]+|TickCount)[[:space:]]+k[A-Za-z]*(Ticks|TickCount)[A-Za-z]*[[:space:]]*=[[:space:]]*[0-9]' \
    "$sim" "$root/src/core/unit" 2>/dev/null \
    | grep -v '/TickRate.hpp:' \
    | grep -vE 'kTicksPerSecond|kMinTicksPerSecond|kMaxTicksPerSecond|kDefaultTicksPerSecond' \
    || true)

if [ -n "$found" ]; then
    echo "check_no_tick_literals: a duration is written in ticks." >&2
    echo "$found" >&2
    echo "" >&2
    echo "Author it in seconds and derive the tick count:" >&2
    echo "  inline constexpr Seconds kThing = Seconds{30.0f};" >&2
    echo "  const TickCount ticks = rate.ticks(kThing);" >&2
    echo "See PLAN2.md §5.1 and core/sim/TickRate.hpp." >&2
    exit 1
fi

echo "check_no_tick_literals: no tick-denominated constants."
