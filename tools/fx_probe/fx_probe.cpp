// Every fixed-point result the sim depends on, folded into one number.
//
// WHY THIS EXISTS. PLAN2.md §7 P2.1 asks for "identical results at -O0 and -O2". That is a
// claim about the COMPILER, not about the code, so no test linked into one binary can check
// it — a unit test only ever runs at whatever level that binary was built with.
//
// So: this prints a hash of ~200,000 results, `tools/check_fx_optimisation.sh` builds it at
// several optimisation levels, and the check is that the numbers match. `-Ofast` is included
// deliberately — it implies `-ffast-math`, which licenses the reassociation and contraction
// that make floating point non-deterministic. Integer arithmetic is unaffected by it, and
// this is what proves that rather than assuming it.
#include "core/sim/Fx.hpp"

#include <cstdio>

int main() {
    // FNV-1a, the same hash the replay log uses, for the same reason: short, no tuning
    // constants, and ours rather than a library's.
    unsigned long long acc = 1469598103934665603ull;
    const auto feed = [&acc](long long value) {
        acc ^= static_cast<unsigned long long>(value);
        acc *= 1099511628211ull;
    };

    // Every angle the type can hold.
    for (unsigned a = 0; a < 65536; ++a) {
        const auto angle = static_cast<rm::Brad>(a);
        feed(rm::sim::fxSin(angle).raw());
        feed(rm::sim::fxCos(angle).raw());
    }

    // A grid of vectors, covering all four quadrants and both axes.
    for (int x = -300; x <= 300; ++x) {
        for (int z = -300; z <= 300; z += 7) {
            const rm::sim::Polar polar =
                rm::sim::fxPolar(rm::sim::Fx::fromInt(x), rm::sim::Fx::fromInt(z));
            feed(polar.bearing);
            feed(polar.length.raw());
        }
    }

    for (int n = 0; n <= 200000; n += 13) {
        feed(rm::sim::fxSqrt(rm::sim::Fx::fromInt(n)).raw());
    }
    for (int n = -16384; n <= 16384; ++n) {
        feed(rm::sim::fxAsin(rm::sim::Fx::fromRaw(n)));
    }

    std::printf("%llu\n", acc);
    return 0;
}
