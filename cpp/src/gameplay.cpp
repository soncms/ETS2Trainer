// gameplay.cpp —— 游戏选择状态与描述表（唯一来源）
#include "gameplay.h"

#include <atomic>

namespace ets2 {
namespace {

// 索引顺序必须与 GameId 一致。
const GameDescriptor kDescriptors[] = {
    {GameId::Ets2, "ets2", "Euro Truck Simulator 2", "ETS2", L"eurotrucks2.exe",
     "Euro Truck Simulator 2", "227300", 1u, true},
    {GameId::Ats, "ats", "American Truck Simulator", "ATS", L"amtrucks.exe",
     "American Truck Simulator", "270880", 2u, true},
};

size_t indexOf(GameId id) { return id == GameId::Ats ? 1u : 0u; }

std::atomic<int>& selectionSlot() {
    // 默认欧卡2，保持历史行为。
    static std::atomic<int> value{static_cast<int>(GameId::Ets2)};
    return value;
}

std::atomic<bool>& cliLockSlot() {
    static std::atomic<bool> locked{false};
    return locked;
}

}  // namespace

const GameDescriptor& gameDescriptor(GameId id) { return kDescriptors[indexOf(id)]; }

GameId selectedGame() {
    return static_cast<GameId>(selectionSlot().load(std::memory_order_acquire));
}

void setSelectedGame(GameId id) {
    selectionSlot().store(static_cast<int>(indexOf(id)), std::memory_order_release);
}

const GameDescriptor& selectedGameDescriptor() { return gameDescriptor(selectedGame()); }

void lockSelectionFromCli() { cliLockSlot().store(true, std::memory_order_release); }

bool selectionLockedFromCli() { return cliLockSlot().load(std::memory_order_acquire); }

const wchar_t* selectedProcessName() { return selectedGameDescriptor().processName; }

bool parseGameKey(const std::string& key, GameId* out) {
    if (key == "ets2") {
        if (out) *out = GameId::Ets2;
        return true;
    }
    if (key == "ats") {
        if (out) *out = GameId::Ats;
        return true;
    }
    return false;
}

GameId gameIdFromKey(const std::string& key, GameId fallback) {
    GameId id = fallback;
    return parseGameKey(key, &id) ? id : fallback;
}

const char* gameKey(GameId id) { return gameDescriptor(id).key; }

bool supportsVerifiedStructures(GameId id) { return gameDescriptor(id).verifiedLayouts; }

std::string structuralWriteBlockReason() {
    const GameDescriptor& d = selectedGameDescriptor();
    if (d.verifiedLayouts) return std::string();
    return std::string("ATS layout has not yet been verified:\nThe fixed address and structure offset of") + d.shortName +
           " are not checked according to this version, and structured writing has been rejected."
           "(cash/experience structured positioning, fixed oil volume, no damage to the vehicle, power adjustment)."
           "manual scanning, archive editing and console input are still available.";
}

}  // namespace ets2
