#include "DebugDrawingHandler.h"
#include "BedrockServerClientInterface.h"
#include "bsci/utils/Utils.h"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <ll/api/base/Containers.h>
#include <ll/api/memory/Hook.h>
#include <ll/api/service/Bedrock.h>
#include <ll/api/thread/ServerThreadExecutor.h>

#include <mc/network/LoopbackPacketSender.h>
#include <mc/network/MinecraftPacketIds.h>
#include <mc/network/NetEventCallback.h>
#include <mc/network/packet/LevelChunkPacket.h>
#include <mc/network/packet/PrimitiveShapeDataPayload.h>
#include <mc/network/packet/PrimitiveShapesPacket.h>
#include <mc/network/packet/PrimitiveShapesPacketPayload.h>
#include <mc/world/level/ChunkPos.h>

#ifndef LL_PLAT_C
#include <mc/network/ServerNetworkHandler.h>
#include <mc/server/ServerPlayer.h>
#include <mc/world/Minecraft.h>
#endif

namespace bsci {
static std::atomic<uint64_t> nextId_{UINT64_MAX};

constexpr size_t shapeDisplayRadius = 48;

class DebugDrawingHandler::Impl {
public:
    using PacketPair = std::pair<GeoId, std::vector<std::weak_ptr<PrimitiveShapesPacket>>>;
    struct Hook;
    size_t id{};
    ll::ConcurrentDenseMap<GeoId, std::vector<std::shared_ptr<PrimitiveShapesPacket>>>
        geoPackets; // bool = true 表示启用ParticleSpawner
    ll::ConcurrentDenseMap<std::pair<ChunkPos, int>, std::vector<PacketPair>>
                                          chunkPackets; // 不应包含使用ParticleSpawner的GeoId
    ll::ConcurrentDenseMap<GeoId, Target> targets;
    std::recursive_mutex                  structureMutex;

public:
    // void sendPacketImmediately(DebugDrawerPacket& pkt) {
    //     if (pkt.mShapes->empty()) return;
    //     ll::thread::ServerThreadExecutor::getDefault().execute([pkt] { pkt.sendToClients(); });
    // }

    static bool compareByGeoId(const PacketPair& a, const GeoId& b) {
        return a.first.value < b.value;
    }

    void unindexPacket(GeoId geoId, std::shared_ptr<PrimitiveShapesPacket> const& packet) {
        if (!packet || packet->mShapes->empty()) return;
        auto const& shape = (*packet->mShapes)[0];
        if (!shape.mLocation->has_value() || !shape.mDimensionId->has_value()) return;
        auto key =
            std::make_pair(ChunkPos(shape.mLocation->value()), (int)shape.mDimensionId->value());
        chunkPackets.erase_if(key, [geoId](auto&& kv) {
            auto& packetPairs = kv.second;
            std::erase_if(packetPairs, [geoId](auto&& pair) {
                return pair.first.value == geoId.value;
            });
            return packetPairs.empty();
        });
    }

    [[nodiscard]] bool isVisibleTo(GeoId geoId, Player const& player) const {
        auto it = targets.find(geoId);
        if (it == targets.end()) return false;
        return it->second.contains(player);
    }

    void addPacket(
        GeoId                                   geoId,
        std::shared_ptr<PrimitiveShapesPacket>& packet,
        int                                     dimId,
        Vec3 const&                             pos,
        Target const&                           target
    ) {
        std::lock_guard l{structureMutex};
        if (packet) {
            geoPackets.emplace(geoId, std::vector<std::shared_ptr<PrimitiveShapesPacket>>{packet});
            targets.emplace(geoId, target);
            auto chunkPos         = ChunkPos(pos);
            auto key              = std::make_pair(chunkPos, dimId);
            auto [node, inserted] = chunkPackets.try_emplace(key);

            if (inserted) {
                node->second.emplace_back(
                    geoId,
                    std::vector<std::weak_ptr<PrimitiveShapesPacket>>{packet}
                );
            } else {
                auto it = std::lower_bound(
                    node->second.begin(),
                    node->second.end(),
                    geoId,
                    compareByGeoId
                );
                node->second
                    .emplace(it, geoId, std::vector<std::weak_ptr<PrimitiveShapesPacket>>{packet});
            }
        }
        // else {
        //     geoGroup.emplace(
        //         geoId,
        //         std::make_pair(std::vector<std::shared_ptr<PrimitiveShapesPacket>>{}, true)
        //     );
        // }
    }
};

static std::recursive_mutex              listMutex;
static std::vector<DebugDrawingHandler*> list;
static std::atomic_bool                  hasInstance{false};

static Player* getServerPlayer(::NetworkIdentifier const& id, ::SubClientId subId) {
#ifdef LL_PLAT_C
    (void)id;
    (void)subId;
    return nullptr;
#else
    auto minecraft = ll::service::getMinecraft();
    if (!minecraft) return nullptr;
    auto networkHandler = minecraft->getServerNetworkHandler();
    if (!networkHandler) return nullptr;
    return networkHandler->_getServerPlayer(id, subId);
#endif
}

// NOLINTNEXTLINE
LL_TYPE_INSTANCE_HOOK(
    DebugDrawingHandler::Impl::Hook,
    ll::memory::HookPriority::Normal,
    LoopbackPacketSender,
    &LoopbackPacketSender::$sendToClient,
    void,
    ::NetworkIdentifier const& id,
    ::Packet const&            packet,
    ::SubClientId              recipientSubId
) {
    if (packet.getId() == MinecraftPacketIds::FullChunkData && hasInstance) [[unlikely]] {

        const auto& levelChunkPacket = static_cast<LevelChunkPacket const&>(packet);
        const auto& chunkPos         = levelChunkPacket.mPos;
        const auto& dimId            = (int)*levelChunkPacket.mDimensionId;
        auto        key              = std::make_pair(chunkPos, dimId);

        auto* recipient = getServerPlayer(id, recipientSubId);

        std::vector<std::shared_ptr<PrimitiveShapesPacket>> _chunkPackets;

        if (recipient) {
            std::lock_guard l{listMutex};

            for (auto s : list) {
                s->impl->chunkPackets.erase_if(key, [&_chunkPackets, s, recipient](auto&& kv) {
                    auto& packetPairs = kv.second;
                    std::erase_if(packetPairs, [&_chunkPackets, s, recipient](auto&& packetPair) {
                        _chunkPackets.reserve(_chunkPackets.size() + packetPair.second.size());
                        std::erase_if(
                            packetPair.second,
                            [&_chunkPackets, s, recipient, geoId = packetPair.first](auto&& pkt) {
                                if (auto shared = pkt.lock()) {
                                    if (s->impl->isVisibleTo(geoId, *recipient)) {
                                        _chunkPackets.emplace_back(shared);
                                    }
                                    return false;
                                }
                                return true;
                            }
                        );
                        return packetPair.second.empty();
                    });
                    return packetPairs.empty();
                });
            }
        }
        for (auto& pkt : _chunkPackets) {
            if (pkt) pkt->sendToClient(id, recipientSubId);
        }
    }
    origin(id, packet, recipientSubId);
};

DebugDrawingHandler::DebugDrawingHandler() : impl(std::make_unique<Impl>()) {
    static ll::memory::HookRegistrar<DebugDrawingHandler::Impl::Hook> reg;
    std::lock_guard                                                   l{listMutex};
    hasInstance = true;
    impl->id    = list.size();
    list.push_back(this);
}

DebugDrawingHandler::~DebugDrawingHandler() {
    std::lock_guard l{listMutex};
    list.back()->impl->id = impl->id;
    std::swap(list[impl->id], list.back());
    list.pop_back();
    hasInstance = !list.empty();
}

GeometryGroup::GeoId DebugDrawingHandler::line(
    DimensionType        dim,
    Vec3 const&          begin,
    Vec3 const&          end,
    mce::Color const&    color,
    std::optional<float> thickness,
    Target const&        target
) {
    if (begin == end) return GeoId::invalid();

    Vec3   offset = end - begin;
    double len    = offset.length();
    if (len <= shapeDisplayRadius + 0.5) { // 防止浮点误差导致的无限递归
        auto packet = std::make_shared<PrimitiveShapesPacket>();
        packet->setSerializationMode(SerializationMode::CerealOnly);
        PrimitiveShapeDataPayload shape;
        shape.mNetworkId        = nextId_.fetch_sub(1);
        shape.mShapeType        = ::ScriptModuleMinecraft::ScriptPrimitiveShapeType::Line;
        shape.mLocation         = begin;
        shape.mColor            = color;
        shape.mDimensionId      = dim;
        shape.mExtraDataPayload = LineDataPayload{.mEndLocation = end};
        packet->mShapes->emplace_back(std::move(shape));
        ll::thread::ServerThreadExecutor::getDefault().execute([packet, begin, dim, target] {
            sendToPlayerAt(dim, begin, *packet, target);
        });

        auto id = getNextGeoId();
        impl->addPacket(id, packet, dim, begin, target);
        return id;
    }

    int segmentNum   = static_cast<int>(len / static_cast<double>(shapeDisplayRadius)) + 1;
    offset          /= segmentNum;
    Vec3 currentPos  = begin;
    Vec3 lastPos     = begin;

    std::vector<GeoId> ids;
    ids.reserve(segmentNum);
    for (int i = 0; i < segmentNum - 1; i++) {
        currentPos += offset;
        ids.emplace_back(line(dim, lastPos, currentPos, color, thickness, target));
        lastPos = currentPos;
    }
    ids.emplace_back(line(dim, currentPos, end, color, thickness, target)); // 避免浮点误差

    return merge(ids);
}

GeometryGroup::GeoId DebugDrawingHandler::box(
    DimensionType        dim,
    AABB const&          box,
    mce::Color const&    color,
    std::optional<float> thickness,
    Target const&        target
) {
    if ((box.max - box.min).lengthSqr() >= shapeDisplayRadius * shapeDisplayRadius)
        return Base::box(dim, box, color, thickness, target);

    auto packet = std::make_shared<PrimitiveShapesPacket>();
    packet->setSerializationMode(SerializationMode::CerealOnly);
    PrimitiveShapeDataPayload shape;
    shape.mNetworkId        = nextId_.fetch_sub(1);
    shape.mShapeType        = ::ScriptModuleMinecraft::ScriptPrimitiveShapeType::Box;
    shape.mLocation         = (box.min + box.max) / 2;
    shape.mColor            = color;
    shape.mDimensionId      = dim;
    shape.mExtraDataPayload = BoxDataPayload{.mBoxBound = box.max - box.min};
    packet->mShapes->emplace_back(std::move(shape));
    ll::thread::ServerThreadExecutor::getDefault().execute([packet, min = box.min, dim, target] {
        sendToPlayerAt(dim, min, *packet, target);
    });

    auto id = getNextGeoId();
    impl->addPacket(id, packet, dim, box.min, target);
    return id;
}


GeometryGroup::GeoId DebugDrawingHandler::circle(
    DimensionType        dim,
    Vec3 const&          center,
    Vec3 const&          normal,
    float                radius,
    mce::Color const&    color,
    std::optional<float> thickness,
    Target const&        target
) {
    auto const& config = BedrockServerClientInterface::getInstance().getConfig().debugDraw;

    if (radius > shapeDisplayRadius || !config.useNativeCircle) {
        return Base::circle(dim, center, normal, radius, color, thickness, target);
    }

    auto packet = std::make_shared<PrimitiveShapesPacket>();
    packet->setSerializationMode(SerializationMode::CerealOnly);
    PrimitiveShapeDataPayload shape;
    shape.mNetworkId   = nextId_.fetch_sub(1);
    shape.mShapeType   = ::ScriptModuleMinecraft::ScriptPrimitiveShapeType::Circle;
    shape.mRotation    = normal;
    shape.mLocation    = center;
    shape.mScale       = radius;
    shape.mColor       = color;
    shape.mDimensionId = dim;
    packet->mShapes->emplace_back(std::move(shape));
    ll::thread::ServerThreadExecutor::getDefault().execute([packet, center, dim, target] {
        sendToPlayerAt(dim, center, *packet, target);
    });

    auto id = getNextGeoId();
    impl->addPacket(id, packet, dim, center, target);
    return id;
}

GeometryGroup::GeoId DebugDrawingHandler::sphere(
    DimensionType        dim,
    Vec3 const&          center,
    float                radius,
    mce::Color const&    color,
    std::optional<float> thickness,
    Target const&        target
) {
    auto const& config = BedrockServerClientInterface::getInstance().getConfig().debugDraw;

    if (radius > shapeDisplayRadius || !config.useNativeSphere) {
        return Base::sphere(dim, center, radius, color, thickness, target);
    }


    auto packet = std::make_shared<PrimitiveShapesPacket>();
    packet->setSerializationMode(SerializationMode::CerealOnly);
    PrimitiveShapeDataPayload shape;
    shape.mNetworkId   = nextId_.fetch_sub(1);
    shape.mShapeType   = ::ScriptModuleMinecraft::ScriptPrimitiveShapeType::Sphere;
    shape.mLocation    = center;
    shape.mScale       = radius;
    shape.mColor       = color;
    shape.mDimensionId = dim;
    if (config.sphereSegments.has_value()) {
        shape.mExtraDataPayload = SphereDataPayload{.mNumSegments = config.sphereSegments.value()};
    }
    packet->mShapes->emplace_back(std::move(shape));
    ll::thread::ServerThreadExecutor::getDefault().execute([packet, center, dim, target] {
        sendToPlayerAt(dim, center, *packet, target);
    });

    auto id = getNextGeoId();
    impl->addPacket(id, packet, dim, center, target);
    return id;
}

GeometryGroup::GeoId DebugDrawingHandler::arrow(
    DimensionType        dim,
    Vec3 const&          begin,
    Vec3 const&          end,
    mce::Color const&    color,
    std::optional<float> mArrowHeadLength,
    std::optional<float> mArrowHeadRadius,
    Target const&        target
) {
    if (begin == end) return GeoId::invalid();

    Vec3   offset = end - begin;
    double len    = offset.length();
    if (len <= shapeDisplayRadius + 0.5) { // 防止浮点误差导致的无限递归
        auto packet = std::make_shared<PrimitiveShapesPacket>();
        packet->setSerializationMode(SerializationMode::CerealOnly);
        auto const& config = BedrockServerClientInterface::getInstance().getConfig().debugDraw;
        PrimitiveShapeDataPayload shape;
        shape.mNetworkId        = nextId_.fetch_sub(1);
        shape.mShapeType        = ::ScriptModuleMinecraft::ScriptPrimitiveShapeType::Arrow;
        shape.mLocation         = begin;
        shape.mColor            = color;
        shape.mDimensionId      = dim;
        shape.mExtraDataPayload = ArrowDataPayload{
            .mEndLocation     = end,
            .mArrowHeadLength = mArrowHeadLength,
            .mArrowHeadRadius = mArrowHeadRadius,
            .mNumSegments     = config.arrowSegments
        };
        packet->mShapes->emplace_back(std::move(shape));
        ll::thread::ServerThreadExecutor::getDefault().execute([packet, begin, dim, target] {
            sendToPlayerAt(dim, begin, *packet, target);
        });

        auto id = getNextGeoId();
        impl->addPacket(id, packet, dim, begin, target);
        return id;
    }

    int segmentNum  = static_cast<int>(len / static_cast<double>(shapeDisplayRadius)) + 1;
    offset         /= segmentNum;
    Vec3               currentPos = begin;
    Vec3               lastPos    = begin;
    std::vector<GeoId> ids;
    ids.reserve(segmentNum);
    for (int i = 0; i < segmentNum - 1; i++) {
        currentPos += offset;
        ids.emplace_back(line(dim, lastPos, currentPos, color, std::nullopt, target));
        lastPos = currentPos;
    }
    ids.emplace_back(
        arrow(dim, currentPos, end, color, mArrowHeadLength, mArrowHeadRadius, target)
    );
    return merge(ids);
}

GeometryGroup::GeoId DebugDrawingHandler::text(
    DimensionType        dim,
    Vec3 const&          pos,
    std::string const&   text,
    mce::Color const&    color,
    std::optional<float> scale,
    Target const&        target
) {
    auto packet = std::make_shared<PrimitiveShapesPacket>();
    packet->setSerializationMode(SerializationMode::CerealOnly);
    PrimitiveShapeDataPayload shape;
    shape.mNetworkId   = nextId_.fetch_sub(1);
    shape.mShapeType   = ::ScriptModuleMinecraft::ScriptPrimitiveShapeType::Text;
    shape.mLocation    = pos;
    shape.mScale       = scale;
    shape.mColor       = color;
    shape.mDimensionId = dim;
    TextDataPayload extraDataPayload;
    extraDataPayload.mText  = text;
    shape.mExtraDataPayload = std::move(extraDataPayload);
    packet->mShapes->emplace_back(std::move(shape));
    ll::thread::ServerThreadExecutor::getDefault().execute([packet, pos, dim, target] {
        sendToPlayerAt(dim, pos, *packet, target);
    });

    auto id = getNextGeoId();
    impl->addPacket(id, packet, dim, pos, target);
    return id;
}

bool DebugDrawingHandler::remove(GeoId id) {
    if (id.value == 0) {
        return false;
    }

    std::lock_guard l{impl->structureMutex};

    Target target = Target::All();
    impl->targets.erase_if(id, [&target](auto&& kv) {
        target = kv.second;
        return true;
    });

    std::vector<std::shared_ptr<PrimitiveShapesPacket>> removePackets;
    impl->geoPackets.erase_if(id, [&removePackets](auto&& kv) {
        removePackets = std::move(kv.second);
        return true;
    });

    for (auto& packet : removePackets) {
        impl->unindexPacket(id, packet);
        ll::thread::ServerThreadExecutor::getDefault().execute([packet, target] {
            if (!packet || packet->mShapes->empty()) return;
            for (auto& s : *packet->mShapes) s.mShapeType = std::nullopt;

            auto const& shape = (*packet->mShapes)[0];
            if (target.type() == Target::Type::Players) {
                if (shape.mLocation->has_value() && shape.mDimensionId->has_value()) {
                    sendToPlayerAt(
                        shape.mDimensionId->value(),
                        shape.mLocation->value(),
                        *packet,
                        target,
                        SendMode::Direct
                    );
                }
            } else {
                packet->sendToClients();
            }
        });
    }
    return true;
}

size_t DebugDrawingHandler::removeFor(mce::UUID const& uuid) {
    std::lock_guard l{impl->structureMutex};

    std::vector<GeoId> ids;
    for (auto const& [id, target] : impl->targets) {
        if (target.type() == Target::Type::Players && target.contains(uuid)) {
            ids.emplace_back(id);
        }
    }
    if (ids.empty()) return 0;

    Target const onlyPlayer{uuid};
    size_t       count = 0;

    for (auto id : ids) {
        std::vector<std::shared_ptr<PrimitiveShapesPacket>> packets;
        impl->geoPackets.modify_if(id, [&packets](auto&& kv) { packets = kv.second; });
        if (packets.empty()) {
            impl->targets.erase(id);
            continue;
        }

        bool const emptied = impl->targets.erase_if(id, [&uuid](auto&& kv) {
            kv.second.erase(uuid);
            return kv.second.empty();
        });
        if (emptied) impl->geoPackets.erase(id);

        for (auto& packet : packets) {
            if (emptied) impl->unindexPacket(id, packet);

            std::shared_ptr<PrimitiveShapesPacket> removal = packet;
            if (!emptied) removal = std::make_shared<PrimitiveShapesPacket>(*packet);
            if (!removal || removal->mShapes->empty()) continue;

            auto const& shape = (*removal->mShapes)[0];
            if (!shape.mLocation->has_value() || !shape.mDimensionId->has_value()) continue;

            auto const dim = shape.mDimensionId->value();
            auto const pos = shape.mLocation->value();
            for (auto& s : *removal->mShapes) s.mShapeType = std::nullopt;

            ll::thread::ServerThreadExecutor::getDefault().execute([removal, dim, pos, onlyPlayer] {
                sendToPlayerAt(dim, pos, *removal, onlyPlayer, SendMode::Direct);
            });
        }
        ++count;
    }
    return count;
}

GeometryGroup::GeoId DebugDrawingHandler::merge(std::span<GeoId> ids) {
    if (ids.empty()) {
        return GeoId::invalid();
    }
    std::lock_guard                                     l{impl->structureMutex};
    ll::ConcurrentDenseMap<std::pair<ChunkPos, int>,
                           std::vector<GeoId>>          temMap;     // 用来处理包的合并
    std::vector<std::shared_ptr<PrimitiveShapesPacket>> newPackets; // 合并后的包队列
    Target                                              mergedTarget = Target::None();

    // 移出旧id的geoPackets
    for (auto& id : ids) {
        impl->targets.erase_if(id, [&mergedTarget](auto&& kv) {
            mergedTarget = Target::merge(mergedTarget, kv.second);
            return true;
        });
        impl->geoPackets.erase_if(id, [id, &temMap, &newPackets](auto&& kv) {
            auto& packets = kv.second;
            // 处理包的合并
            std::erase_if(packets, [&temMap, id](auto&& packet) {
                if (packet && !packet->mShapes->empty()
                    && (*packet->mShapes)[0].mLocation->has_value()
                    && (*packet->mShapes)[0].mDimensionId->has_value()) {
                    auto chunkPos         = ChunkPos((*packet->mShapes)[0].mLocation->value());
                    auto dimId            = (int)(*packet->mShapes)[0].mDimensionId->value();
                    auto key              = std::make_pair(chunkPos, dimId);
                    auto [node, inserted] = temMap.try_emplace(key);

                    // 记录对应区块待合并的GeoId
                    node->second.emplace_back(id);
                    return false;
                } else return true;
            });
            newPackets.insert(
                newPackets.end(),
                std::make_move_iterator(packets.begin()),
                std::make_move_iterator(packets.end())
            );
            return true;
        });
    }

    if (newPackets.size() == 0) return GeoId::invalid();
    auto newId = getNextGeoId();
    impl->targets.emplace(newId, std::move(mergedTarget));

    // 处理chunkPackets
    for (auto& [key, data] : temMap) {
        impl->chunkPackets.modify_if(key, [&data, &newId](auto&& kv) {
            auto& packetPairs = kv.second;
            // 合并包
            std::vector<std::weak_ptr<PrimitiveShapesPacket>> pkts;
            for (auto& id : data) {
                auto it = std::lower_bound(
                    packetPairs.begin(),
                    packetPairs.end(),
                    id,
                    DebugDrawingHandler::Impl::compareByGeoId
                );
                if (it != packetPairs.end() && it->first.value == id.value) {
                    pkts.insert(
                        pkts.end(),
                        std::make_move_iterator(it->second.begin()),
                        std::make_move_iterator(it->second.end())
                    );
                    packetPairs.erase(it);
                }
            }
            // 添加新的包
            std::pair<GeoId, std::vector<std::weak_ptr<PrimitiveShapesPacket>>> newPair =
                std::make_pair(newId, std::move(pkts));
            auto it = std::lower_bound(
                packetPairs.begin(),
                packetPairs.end(),
                newId,
                DebugDrawingHandler::Impl::compareByGeoId
            );
            packetPairs.insert(it, std::move(newPair));
        });
    }

    // 添加新id的geoPackets
    impl->geoPackets.emplace(newId, std::move(newPackets));

    return newId;
}

bool DebugDrawingHandler::shift(GeoId id, Vec3 const& deltaPos) {
    if (id.value == 0) return false;

    std::lock_guard l{impl->structureMutex};

    Target target = Target::All();
    if (auto it = impl->targets.find(id); it != impl->targets.end()) {
        target = it->second;
    }

    return impl->geoPackets.modify_if(id, [this, id, deltaPos, target](auto&& kv) {
        auto& packets = kv.second;
        ll::ConcurrentDenseMap<
            std::pair<ChunkPos, int>,
            std::vector<std::weak_ptr<PrimitiveShapesPacket>>>
            temMap; // 用来处理shape跨区块

        std::erase_if(packets, [&temMap, &deltaPos](auto&& packet) {
            if (packet && !packet->mShapes->empty() && (*packet->mShapes)[0].mLocation->has_value()
                && (*packet->mShapes)[0].mDimensionId->has_value()) {
                auto chunkPos = ChunkPos((*packet->mShapes)[0].mLocation->value());
                auto dimId    = (int)(*packet->mShapes)[0].mDimensionId->value();

                // 插入空值，用于标记原区块
                temMap.try_emplace(std::make_pair(chunkPos, dimId));

                // 处理移动后的包
                for (auto& shape : *packet->mShapes) {
                    if (shape.mLocation->has_value()) {
                        shape.mLocation->value() += deltaPos;
                        if (std::holds_alternative<ArrowDataPayload>(*shape.mExtraDataPayload)) {
                            std::get<ArrowDataPayload>(*shape.mExtraDataPayload)
                                .mEndLocation->value() += deltaPos;
                        } else if (std::holds_alternative<LineDataPayload>(
                                       *shape.mExtraDataPayload
                                   )) {
                            *std::get<LineDataPayload>(*shape.mExtraDataPayload).mEndLocation +=
                                deltaPos;
                        }
                    }
                }
                auto newChunkPos    = ChunkPos((*packet->mShapes)[0].mLocation->value());
                auto [it, inserted] = temMap.try_emplace(std::make_pair(newChunkPos, dimId));
                it->second.emplace_back(std::weak_ptr<PrimitiveShapesPacket>(packet));
                return false;
            }
            return true;
        });

        // 此时，temMap中数据为空代表需要删除该区块中的对应geoId的数据；否则为增改
        for (auto& [key, data] : temMap) {
            if (data.empty()) {
                // 删
                impl->chunkPackets.erase_if(key, [&id](auto&& chunkKv) {
                    auto& packetPairs = chunkKv.second;
                    auto  it          = std::lower_bound(
                        packetPairs.begin(),
                        packetPairs.end(),
                        id,
                        DebugDrawingHandler::Impl::compareByGeoId
                    );
                    if (it != packetPairs.end() && it->first.value == id.value)
                        packetPairs.erase(it);
                    return packetPairs.empty();
                });
            }
            // 增改
            else {
                auto [it, inserted] = impl->chunkPackets.try_emplace(key);
                if (inserted) {
                    it->second.emplace_back(id, std::move(data));
                } else {
                    auto _it = std::lower_bound(
                        it->second.begin(),
                        it->second.end(),
                        id,
                        DebugDrawingHandler::Impl::compareByGeoId
                    );
                    if (_it != it->second.end() && _it->first.value == id.value)
                        _it->second = std::move(data);
                    else it->second.emplace(_it, id, std::move(data));
                }
            }
        }
        for (auto& pkt : packets) {
            ll::thread::ServerThreadExecutor::getDefault().execute([pkt, target] {
                if (!pkt || pkt->mShapes->empty()) return;
                auto const& shape = (*pkt->mShapes)[0];
                if (!shape.mLocation->has_value() || !shape.mDimensionId->has_value()) return;
                sendToPlayerAt(
                    shape.mDimensionId->value(),
                    shape.mLocation->value(),
                    *pkt,
                    target,
                    SendMode::Direct
                );
            });
        }
    });
}
} // namespace bsci