#pragma once

#include <stdexcept>
#include <string>

namespace rm {

// Thrown when a Metal object that must exist cannot be created (no GPU, no
// command queue, shader pipeline failed). These are startup-fatal: there is
// no recovery path at this layer, which is why they are exceptions and not
// std::expected — expected is for recoverable parse/IO errors in loaders.
class RendererError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Why a value and not an exception: a malformed map is a *recoverable* input
// error — the app can report it and carry on — which is the split this header
// already declared above. Loaders therefore return std::expected<T, MapError>.
//
// The code exists so tests can assert on the failure *kind* without pinning
// exact message text (brittle); the message carries the actionable detail for
// humans, in the spirit of Recoil's own "corrupt header for X (v=%d ts=%d …)".
struct MapError {
    enum class Code {
        NotSmf,       ///< magic string absent — not a Spring/Recoil map at all
        NotScmap,     ///< magic absent — not the Supreme Commander file expected
                      ///< (shared by .scmap, .scm and .sca; the message says which)
        BadHeader,    ///< version/tilesize/texelPerSquare/squareSize rejected
        BadGeometry,  ///< mapx/mapy not positive multiples of 128
        Truncated,    ///< a section pointer or its length runs past end-of-file
        TooLarge,     ///< valid, but beyond what this renderer can hold at once
    };

    Code code;
    std::string message;
};

} // namespace rm
