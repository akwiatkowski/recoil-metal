#include "core/model/Model.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace rm {

std::array<float, 3> rotateByQuaternion(const std::array<float, 4>& q,
                                        const std::array<float, 3>& v) noexcept {
    // v' = v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v)
    //
    // The standard sandwich qvq* written without building a matrix: fewer
    // operations, and no chance of transposing one by accident.
    const float w = q[0];
    const std::array<float, 3> u{{q[1], q[2], q[3]}};

    const std::array<float, 3> t{{u[1] * v[2] - u[2] * v[1] + w * v[0],
                                  u[2] * v[0] - u[0] * v[2] + w * v[1],
                                  u[0] * v[1] - u[1] * v[0] + w * v[2]}};

    return {{v[0] + 2.0f * (u[1] * t[2] - u[2] * t[1]),
             v[1] + 2.0f * (u[2] * t[0] - u[0] * t[2]),
             v[2] + 2.0f * (u[0] * t[1] - u[1] * t[0])}};
}

std::array<float, 4> multiplyQuaternions(const std::array<float, 4>& a,
                                         const std::array<float, 4>& b) noexcept {
    return {{a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3],
             a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2],
             a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1],
             a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0]}};
}

std::array<float, 3> Model::modelSpacePosition(const ModelVertex& vertex) const noexcept {
    if (family == Family::SupremeCommander) {
        return vertex.position;  // already there
    }
    if (vertex.boneIndex >= bones.size()) {
        return vertex.position;
    }

    const ModelBone& bone = bones[vertex.boneIndex];
    return {{vertex.position[0] + bone.globalOffset[0],
             vertex.position[1] + bone.globalOffset[1],
             vertex.position[2] + bone.globalOffset[2]}};
}

float Model::computedRadius() const noexcept {
    float longest = 0.0f;
    for (const ModelVertex& vertex : vertices) {
        const std::array<float, 3> p = modelSpacePosition(vertex);
        longest = std::max(longest, std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]));
    }
    return longest;
}

void computeBounds(Model& model) noexcept {
    if (model.vertices.empty()) {
        return;
    }

    std::array<float, 3> lo{{std::numeric_limits<float>::max(),
                             std::numeric_limits<float>::max(),
                             std::numeric_limits<float>::max()}};
    std::array<float, 3> hi{{std::numeric_limits<float>::lowest(),
                             std::numeric_limits<float>::lowest(),
                             std::numeric_limits<float>::lowest()}};

    for (const ModelVertex& vertex : model.vertices) {
        const std::array<float, 3> p = model.modelSpacePosition(vertex);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            lo[axis] = std::min(lo[axis], p[axis]);
            hi[axis] = std::max(hi[axis], p[axis]);
        }
    }

    model.boundsMin = lo;
    model.boundsMax = hi;
}

} // namespace rm
