#include "bsci/particle/ParticleSpawner.h"
#include "BedrockServerClientInterface.h"
#include "bsci/utils/Utils.h"

#include <ll/api/base/Containers.h>
#include <ll/api/event/EventBus.h>
#include <ll/api/event/Listener.h>
#include <ll/api/event/world/ServerLevelTickEvent.h>
#include <ll/api/service/Bedrock.h>
#include <ll/api/service/GamingStatus.h>
#include <ll/api/thread/ServerThreadExecutor.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mc/deps/core/string/HashedString.h>
#include <mc/deps/core/threading/Threading.h>
#include <mc/legacy/ActorUniqueID.h>
#include <mc/network/packet/SpawnParticleEffectPacket.h>
#include <mc/platform/threading/AssignedThread.h>
#include <mc/util/MolangMemberArray.h>
#include <mc/util/MolangMemberVariable.h>
#include <mc/util/MolangScriptArg.h>
#include <mc/util/MolangStruct_RGBA.h>
#include <mc/util/MolangStruct_XY.h>
#include <mc/util/MolangStruct_XYZ.h>
#include <mc/util/MolangVariable.h>
#include <mc/util/MolangVariableMap.h>
#include <mc/util/MolangVariableSettings.h>
#include <mc/util/Timer.h>
#include <mc/world/level/BlockPos.h>
#include <mc/world/level/dimension/Dimension.h>


// MolangMemberArray::MolangMemberArray() = default;
MolangVariableMap::MolangVariableMap() : mHasPublicVariables(false) {}
// MolangScriptArg::MolangScriptArg() = default;
// MolangVariableMap::MolangVariableMap(MolangVariableMap const& rhs) {
//     mMapFromVariableIndexToVariableArrayOffset =
//     rhs.mMapFromVariableIndexToVariableArrayOffset; mVariables = {}; for (auto& ptr :
//     *rhs.mVariables) {
//         mVariables->push_back(std::make_unique<MolangVariable>(*ptr));
//     }
//     mHasPublicVariables = rhs.mHasPublicVariables;
// }

namespace bsci {
constexpr size_t PacketsMapN = 6;
constexpr size_t TablesCount = size_t{1} << PacketsMapN;

constexpr double TicksPerSecond    = 20.0;
constexpr double MinRefreshSeconds = 1.0 / TicksPerSecond;
constexpr double MaxRefreshSeconds = (double)TablesCount / TicksPerSecond;

static size_t submapsPerTick(double refreshInterval) {
    double const seconds = std::clamp(refreshInterval, MinRefreshSeconds, MaxRefreshSeconds);
    double const cycle   = seconds * TicksPerSecond;
    return std::clamp<size_t>(
        (size_t)std::llround((double)TablesCount / cycle),
        size_t{1},
        TablesCount
    );
}

static float particleLifetime(double refreshInterval, double extraTime) {
    double const period =
        (double)TablesCount / (double)submapsPerTick(refreshInterval) / TicksPerSecond;
    return (float)(period + extraTime);
}

static void addTime(MolangVariableMap& var) {
    auto& config = BedrockServerClientInterface::getInstance().getConfig().particle;
    var.setMolangVariable(
        "variable.bsci_particle_lifetime",
        particleLifetime(config.refreshInterval, config.extraTime)
    );
}
static void addSize(MolangVariableMap& var, Vec2 const& size) {
    var.setMolangVariable(
        "variable.bsci_particle_size",
        MolangMemberArray{MolangStruct_XY{}, size * 0.5f}
    );
}
static void addTint(MolangVariableMap& var, mce::Color const& tint) {
    var.setMolangVariable(
        "variable.bsci_particle_tint",
        MolangMemberArray{MolangStruct_RGBA{}, tint}
    );
}
static void addDirection(MolangVariableMap& var, Vec3 const& dir) {
    var.setMolangVariable(
        "variable.bsci_particle_direction",
        MolangMemberArray{MolangStruct_XYZ{}, dir}
    );
}

struct ParticleSpawner::Entry {
    std::shared_ptr<SpawnParticleEffectPacket> packet;
    std::shared_ptr<const Target>              target;
};

struct ParticleSpawner::Impl {
    using PacketsMap = ll::ConcurrentDenseMap<
        GeoId,
        Entry,
        ::phmap::priv::hash_default_hash<GeoId>,
        ::phmap::priv::hash_default_eq<GeoId>,
        ::std::allocator<::std::pair<GeoId const, Entry>>,
        PacketsMapN>;
    using GroupMap = ll::ConcurrentDenseMap<GeoId, std::vector<GeoId>>;

    std::atomic_bool       active{true};
    size_t                 nextSubMap{};
    ll::event::ListenerPtr listener;
    size_t                 id{};
    PacketsMap             geoPackets;
    GroupMap               geoGroup;

    void sendParticleImmediately(
        std::shared_ptr<SpawnParticleEffectPacket> const& packet,
        std::shared_ptr<const Target> const&              target
    ) {
        if (BedrockServerClientInterface::getInstance().getConfig().particle.delayUndate) return;
        if (!packet || !target) return;
        ll::thread::ServerThreadExecutor::getDefault().execute([packet, target] {
            sendToPlayerAt(packet->mVanillaDimensionId, *packet->mPos, *packet, *target);
        });
    }

    void tick() {
        if (!active.load(std::memory_order_acquire)) {
            return;
        }
        auto const perTick = submapsPerTick(
            BedrockServerClientInterface::getInstance().getConfig().particle.refreshInterval
        );
        for (size_t i = 0; i < perTick; i++) {
            geoPackets.with_submap(nextSubMap, [&](auto& map) {
                for (auto& [id, entry] : map) {
                    sendToPlayerAt(
                        entry.packet->mVanillaDimensionId,
                        *entry.packet->mPos,
                        *entry.packet,
                        *entry.target
                    );
                }
            });
            if (++nextSubMap == TablesCount) nextSubMap = 0;
        }
    }
};

ParticleSpawner::ParticleSpawner() : impl(std::make_shared<Impl>()) {
    impl->listener =
        ll::event::EventBus::getInstance().emplaceListener<ll::event::world::ServerLevelTickEvent>(
            [impl = impl](ll::event::world::ServerLevelTickEvent&) { impl->tick(); }
        );
}
ParticleSpawner::~ParticleSpawner() {
    if (impl) {
        impl->active.store(false, std::memory_order_release);
        if (impl->listener) {
            ll::event::EventBus::getInstance()
                .removeListener<ll::event::world::ServerLevelTickEvent>(impl->listener);
            impl->listener.reset();
        }
    }
}

GeometryGroup::GeoId ParticleSpawner::particle(
    DimensionType      dim,
    Vec3 const&        pos,
    std::string const& name,
    MolangVariableMap  var,
    Target const&      target
) const {
    addTime(var);
    auto packet =
        std::make_shared<SpawnParticleEffectPacket>(pos, name, (uchar)dim, std::move(var));
    auto tgt = std::make_shared<const Target>(target);
    impl->sendParticleImmediately(packet, tgt);
    auto id = GeometryGroup::getNextGeoId();
    impl->geoPackets.try_emplace(id, packet, tgt);
    return id;
}

GeometryGroup::GeoId ParticleSpawner::line(
    DimensionType        dim,
    Vec3 const&          begin,
    Vec3 const&          end,
    mce::Color const&    color,
    std::optional<float> thickness,
    Target const&        target
) {
    if (begin == end) return GeoId::invalid();
    MolangVariableMap var;
    addSize(
        var,
        {begin.distanceTo(end),
         thickness.value_or(
             BedrockServerClientInterface::getInstance().getConfig().particle.defaultThickness
         )}
    );
    addDirection(var, (end - begin).normalize());
    addTint(var, color);
    return particle(
        dim,
        (begin + end) * 0.5f,
        color.a == 1 ? "bsci:line" : "bsci:blend_line",
        var,
        target
    );
}

GeometryGroup::GeoId ParticleSpawner::point(
    DimensionType        dim,
    Vec3 const&          pos,
    mce::Color const&    color,
    std::optional<float> radius,
    Target const&        target
) {
    MolangVariableMap var;
    addSize(
        var,
        {radius.value_or(
            BedrockServerClientInterface::getInstance().getConfig().particle.defaultPointRadius
        )}
    );
    addTint(var, color);
    return particle(dim, pos, color.a == 1 ? "bsci:point" : "bsci:blend_point", var, target);
}

bool ParticleSpawner::remove(GeoId id) {
    if (id.value == 0) {
        return false;
    }
    if (!impl->geoGroup.erase_if(id, [this](auto&& kv) {
            for (auto& subId : kv.second) {
                impl->geoPackets.erase(subId);
            }
            return true;
        })) {
        return impl->geoPackets.erase(id);
    }
    return true;
}

GeometryGroup::GeoId ParticleSpawner::merge(std::span<GeoId> ids) {
    if (ids.empty()) {
        return GeoId::invalid();
    }
    auto               id = GeometryGroup::getNextGeoId();
    std::vector<GeoId> res;
    res.reserve(ids.size());
    for (auto const& sid : ids) {
        if (!impl->geoGroup.erase_if(sid, [&res](auto&& kv) {
                auto& subIds = kv.second;
                res.reserve(res.size() + subIds.size());
                res.insert(
                    res.end(),
                    std::make_move_iterator(subIds.begin()),
                    std::make_move_iterator(subIds.end())
                );
                return true;
            })) {
            res.push_back(sid);
        }
    }
    impl->geoGroup.try_emplace(id, std::move(res));
    return id;
}

bool ParticleSpawner::shift(GeoId id, Vec3 const& deltaPos) {
    if (!impl->geoGroup.modify_if(id, [this, &deltaPos](auto&& group) {
            for (auto& subId : group.second) {
                impl->geoPackets.modify_if(subId, [this, &deltaPos](auto&& kv) {
                    auto& entry = kv.second;
                    if (entry.packet) {
                        *entry.packet->mPos += deltaPos;
                        impl->sendParticleImmediately(entry.packet, entry.target);
                    }
                });
            }
        })) {
        return impl->geoPackets.modify_if(id, [this, &deltaPos](auto&& kv) {
            auto& entry = kv.second;
            if (entry.packet) {
                *entry.packet->mPos += deltaPos;
                impl->sendParticleImmediately(entry.packet, entry.target);
            }
        });
    }
    return true;
}

} // namespace bsci
