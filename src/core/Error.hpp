#pragma once

#include <stdexcept>

namespace rm {

// Thrown when a Metal object that must exist cannot be created (no GPU, no
// command queue, shader pipeline failed). These are startup-fatal: there is
// no recovery path at this layer, which is why they are exceptions and not
// std::expected — expected is for recoverable parse/IO errors in loaders.
class RendererError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

} // namespace rm
