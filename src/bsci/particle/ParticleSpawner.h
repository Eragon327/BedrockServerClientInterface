#pragma once

#include "bsci/GeometryGroup.h"

#include <memory>

#include <mc/util/MolangVariableMap.h>

namespace bsci {
class ParticleSpawner : public GeometryGroup {
    struct Entry;
    struct Impl;
    std::shared_ptr<Impl> impl;

    GeoId particle(
        DimensionType      dim,
        Vec3 const&        pos,
        std::string const& name,
        MolangVariableMap  var    = {},
        Target const&      target = Target::All()
    ) const;

public:
    BSCI_API ParticleSpawner();

    ~ParticleSpawner() override;

    GeoId point(
        DimensionType        dim,
        Vec3 const&          pos,
        mce::Color const&    color  = mce::Color::WHITE(),
        std::optional<float> radius = {},
        Target const&        target = Target::All()
    ) override;

    GeoId line(
        DimensionType        dim,
        Vec3 const&          begin,
        Vec3 const&          end,
        mce::Color const&    color     = mce::Color::WHITE(),
        std::optional<float> thickness = {},
        Target const&        target    = Target::All()
    ) override;

    bool remove(GeoId) override;

    size_t removeFor(mce::UUID const& uuid) override;

    GeoId merge(std::span<GeoId>) override;

    bool shift(GeoId, Vec3 const&) override;
};
} // namespace bsci
