#pragma once

#include <cstddef>
#include <initializer_list>
#include <utility>
#include <vector>

#include <mc/world/actor/player/Player.h>

class Target {
public:
    enum class Type { None, All, Players };

    Target() = delete;

    Target(std::initializer_list<Player*> players) : mType(Type::Players), mPlayers(players) {
        checkAndRemoveInvalidPlayers();
        fillSubClientIds();
    }

    Target(std::vector<Player*> players) : mType(Type::Players), mPlayers(std::move(players)) {
        checkAndRemoveInvalidPlayers();
        fillSubClientIds();
    }

    Target(Player* player) : mType(Type::Players) {
        if (player) {
            this->mPlayers.emplace_back(player);
        } else {
            this->mType = Type::None;
        }
    }

    Target(std::nullptr_t) : mType(Type::None) {}

    [[nodiscard]] static Target None() { return Target(Type::None); }

    [[nodiscard]] static Target All() { return Target(Type::All); }

    [[nodiscard]] Type type() const noexcept { return mType; }

    [[nodiscard]] const std::vector<Player*>& players() const noexcept { return mPlayers; }

    [[nodiscard]] const std::vector<SubClientId>& subClientIds() const noexcept {
        return mSubClientIds;
    }

private:
    Target(Type type) : mType(type) {}

    Type                     mType         = Type::None;
    std::vector<Player*>     mPlayers      = {};
    std::vector<SubClientId> mSubClientIds = {};

    void checkAndRemoveInvalidPlayers() {
        if (mType != Type::Players) return;
        for (size_t i = 0; i < mPlayers.size(); i++) {
            auto* player = mPlayers[i];
            if (!player) {
                mPlayers.erase(mPlayers.begin() + static_cast<std::ptrdiff_t>(i));
                mSubClientIds.erase(mSubClientIds.begin() + static_cast<std::ptrdiff_t>(i));
                i--;
            }
        }
        if (mPlayers.empty()) {
            mType = Type::None;
        }
    }

    void fillSubClientIds() {
        if (mType != Type::Players) return;
        mSubClientIds.clear();
        for (auto* player : mPlayers) {
            if (player) {
                mSubClientIds.emplace_back(player->getClientSubId());
            }
        }
    }
};