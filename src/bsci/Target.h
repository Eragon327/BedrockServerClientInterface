#pragma once

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <span>
#include <vector>

#include <mc/platform/UUID.h>
#include <mc/world/actor/player/Player.h>

class Target {
public:
    enum class Type { None, All, Players };

    Target() = delete;

    Target(Player const& player) : mType(Type::Players) { mIds.emplace_back(player.getUuid()); }

    Target(Player* player) : mType(Type::Players) {
        if (player) {
            mIds.emplace_back(player->getUuid());
        } else {
            mType = Type::None;
        }
    }

    Target(std::initializer_list<Player*> players) : mType(Type::Players) {
        for (auto* player : players) {
            if (player) mIds.emplace_back(player->getUuid());
        }
        if (mIds.empty()) mType = Type::None;
    }

    Target(std::vector<Player*> const& players) : mType(Type::Players) {
        for (auto* player : players) {
            if (player) mIds.emplace_back(player->getUuid());
        }
        if (mIds.empty()) mType = Type::None;
    }

    Target(std::nullptr_t) : mType(Type::None) {}

    [[nodiscard]] static Target None() { return Target(Type::None); }

    [[nodiscard]] static Target All() { return Target(Type::All); }

    [[nodiscard]] Type type() const noexcept { return mType; }

    [[nodiscard]] std::span<mce::UUID const> ids() const noexcept { return mIds; }

    [[nodiscard]] bool contains(Player const& player) const {
        if (mType == Type::All) return true;
        if (mType != Type::Players) return false;
        auto const& uuid = player.getUuid();
        return std::find(mIds.begin(), mIds.end(), uuid) != mIds.end();
    }

    [[nodiscard]] static Target unite(Target const& lhs, Target const& rhs) {
        if (lhs.mType == Type::All || rhs.mType == Type::All) return All();
        if (lhs.mType == Type::None) return rhs;
        if (rhs.mType == Type::None) return lhs;

        Target result = lhs;
        for (auto const& uuid : rhs.mIds) {
            if (std::find(result.mIds.begin(), result.mIds.end(), uuid) == result.mIds.end()) {
                result.mIds.emplace_back(uuid);
            }
        }
        return result;
    }

private:
    Target(Type type) : mType(type) {}

    Type                   mType = Type::None;
    std::vector<mce::UUID> mIds{};
};
