#pragma once

#include "bsci/Target.h"

#include <ll/api/service/Bedrock.h>
#include <mc/network/Packet.h>
#include <mc/world/actor/player/Player.h>
#include <mc/world/level/BlockPos.h>
#include <mc/world/level/Level.h>
#include <mc/world/level/dimension/Dimension.h>


namespace bsci {

enum class SendMode {
    Positional, // 只发给目标中同维度且位置相关的玩家（创建形状时用）
    Direct      // 发给所有在线目标，不管距离/维度（更新、删除形状时用）
};

inline void sendToPlayerAt(
    DimensionType const& dimId,
    BlockPos const&      pos,
    Packet const&        packet,
    Target const&        target = Target::All(),
    SendMode             mode   = SendMode::Positional
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
        if (mode == SendMode::Positional) {
            const auto& dim = player->getDimension();
            if (dim.getDimensionId() != dimId || !dim.is2DPositionRelevantForPlayer(pos, *player)) {
                continue;
            }
        }
        packet.sendTo(*player);
    }
}

} // namespace bsci