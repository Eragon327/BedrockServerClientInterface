#pragma once

#include "bsci/Marcos.h"
#include "bsci/Target.h"

#include <mc/deps/core/math/Color.h>
#include <mc/world/level/dimension/DimensionType.h>
#include <mc/world/phys/AABB.h>


namespace bsci {
class GeometryGroup {
public:
    struct GeoId {
        uint64             value;
        constexpr bool     operator==(GeoId const& other) const { return other.value == value; }
        explicit constexpr operator bool() const { return value == 0; }

        static constexpr GeoId invalid() { return GeoId{0}; }
    };

protected:
    BSCI_API GeoId getNextGeoId() const;

public:
    BSCI_API static std::unique_ptr<GeometryGroup> createDefault();

    BSCI_API virtual ~GeometryGroup() = default;

    BSCI_API virtual GeoId point(
        DimensionType        dim,
        Vec3 const&          pos,
        mce::Color const&    color  = mce::Color::WHITE(),
        std::optional<float> radius = {},
        Target const&        target = Target::All()
    );

    virtual GeoId line(
        DimensionType        dim,
        Vec3 const&          begin,
        Vec3 const&          end,
        mce::Color const&    color     = mce::Color::WHITE(),
        std::optional<float> thickness = {},
        Target const&        target    = Target::All()
    ) = 0;

    virtual bool remove(GeoId) = 0;

    // 把该玩家从所有显式目标里摘除；Target::All 的形状保持不变。
    // 返回受影响的形状数量。
    BSCI_API virtual size_t removeFor(mce::UUID const& uuid);

    BSCI_API size_t removeFor(Player const& player) { return removeFor(player.getUuid()); }

    virtual GeoId merge(std::span<GeoId>) = 0;

    virtual bool shift(GeoId, Vec3 const&) = 0;

    BSCI_API virtual GeoId line(
        DimensionType        dim,
        std::span<Vec3>      dots,
        mce::Color const&    color     = mce::Color::WHITE(),
        std::optional<float> thickness = {},
        Target const&        target    = Target::All()
    );

    BSCI_API virtual GeoId
    box(DimensionType        dim,
        AABB const&          box,
        mce::Color const&    color     = mce::Color::WHITE(),
        std::optional<float> thickness = {},
        Target const&        target    = Target::All());

    BSCI_API virtual GeoId circle(
        DimensionType        dim,
        Vec3 const&          center,
        Vec3 const&          normal,
        float                radius,
        mce::Color const&    color     = mce::Color::WHITE(),
        std::optional<float> thickness = {},
        Target const&        target    = Target::All()
    );

    BSCI_API virtual GeoId cylinder(
        DimensionType        dim,
        Vec3 const&          topCenter,
        Vec3 const&          bottomCenter,
        float                radius,
        mce::Color const&    color     = mce::Color::WHITE(),
        std::optional<float> thickness = {},
        Target const&        target    = Target::All()
    );

    BSCI_API virtual GeoId sphere(
        DimensionType        dim,
        Vec3 const&          center,
        float                radius,
        mce::Color const&    color     = mce::Color::WHITE(),
        std::optional<float> thickness = {},
        Target const&        target    = Target::All()
    );

    BSCI_API virtual GeoId arrow(
        DimensionType        dim,
        Vec3 const&          begin,
        Vec3 const&          end,
        mce::Color const&    color            = mce::Color::WHITE(),
        std::optional<float> mArrowHeadLength = {},
        std::optional<float> mArrowHeadRadius = {},
        Target const&        target           = Target::All()
    );

    BSCI_API virtual GeoId text(
        DimensionType        dim,
        Vec3 const&          pos,
        std::string const&   text,
        mce::Color const&    color  = mce::Color::WHITE(),
        std::optional<float> scale  = {},
        Target const&        target = Target::All()
    );

    BSCI_API virtual GeoId cone(
        DimensionType        dim,
        Vec3 const&          topCenter,
        Vec3 const&          bottomCenter,
        float                topRadius,
        float                bottomRadius,
        mce::Color const&    color     = mce::Color::WHITE(),
        std::optional<float> thickness = {},
        Target const&        target    = Target::All()
    );
};
} // namespace bsci

namespace std {
template <>
struct hash<bsci::GeometryGroup::GeoId> {
    size_t operator()(bsci::GeometryGroup::GeoId const& id) {
        return std::hash<uint64>{}(id.value);
    }
};
} // namespace std
