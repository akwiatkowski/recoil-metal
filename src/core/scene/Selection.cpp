#include "core/scene/Selection.hpp"

// `applyClick` is a template now — the rules are the same for any identity that compares
// equal, and the identity changed from a (batch, instance) pair to a unit handle. Its body
// moved to the header; this file stays so the build's source list does not have to change
// and so there is somewhere obvious to put a non-template selection rule if one arrives.
