#pragma once

#include "bsci/Target.h"

#include <ll/api/service/Bedrock.h>
#include <mc/network/Packet.h>
#include <mc/world/actor/player/Player.h>
#include <mc/world/level/BlockPos.h>
#include <mc/world/level/Level.h>
#include <mc/world/level/dimension/Dimension.h>


namespace bsci {

inline void sendToPlayerAt(
    DimensionType const& dimId,
    BlockPos const&      pos,
    Packet const&        packet,
    Target const&        target = Target::All()
) {
    if (target.type() == Target::Type::None) return;

    if (target.type() == Target::Type::All) {
        return packet.sendTo(pos, dimId);
    }

    auto level = ll::service::getLevel();
    if (!level) return;

    for (auto const& uuid : target.ids()) {
        auto* player = level->getPlayer(uuid);
        if (!player) continue;
        const auto& dim = player->getDimension();
        if (dim.getDimensionId() == dimId && dim.is2DPositionRelevantForPlayer(pos, *player)) {
            packet.sendTo(*player);
        }
    }
}

} // namespace bsci