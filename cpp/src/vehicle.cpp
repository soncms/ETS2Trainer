// vehicle.cpp —— 按已核对版本选择固定入口与燃油校正偏移，再用遥测交叉验证。
#include "vehicle.h"

#include "gameio.h"
#include "gameplay.h"
#include "layout.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>

namespace ets2 {

namespace {

constexpr uint64_t kRootToDriver = 0x31B0;
constexpr uint64_t kDriverToContext = 0x18;
constexpr uint64_t kContextToTruck = 0x1F8;

constexpr uint64_t kTruckFuelCapacity = 0x190;
constexpr uint64_t kTruckFuelRatio = 0x1B8;
constexpr uint64_t kTruckChassisPermanent = 0x98;
constexpr uint64_t kTruckChassisWear = 0x9C;
constexpr uint64_t kTruckEnginePermanent = 0x1A0;
constexpr uint64_t kTruckTransmissionPermanent = 0x1A4;
constexpr uint64_t kTruckCabinPermanent = 0x1A8;
constexpr uint64_t kTruckEngineWear = 0x1AC;
constexpr uint64_t kTruckTransmissionWear = 0x1B0;
constexpr uint64_t kTruckCabinWear = 0x1B4;
constexpr uint64_t kTruckWheelWearArray = 0xA8;
constexpr uint64_t kTruckWheelWearCount = 0xB0;
constexpr uint64_t kTruckWheelPermanentArray = 0xD0;
constexpr uint64_t kTruckWheelPermanentCount = 0xD8;
constexpr uint64_t kTruckWheelWearCache = 0x168;
constexpr uint64_t kTruckWheelPermanentCache = 0x16C;
constexpr uint64_t kMaxWheelFields = 64;

bool isCanonicalPointer(uint64_t value) {
    return value >= 0x10000ull && value <= 0x00007FFFFFFFFFFFull;
}

bool readPointer(const ProcessMemory& memory, uint64_t address, uint64_t* value) {
    uint64_t local = 0;
    if (!memory.read(address, &local, sizeof(local)) || !isCanonicalPointer(local)) return false;
    *value = local;
    return true;
}

bool readFloat(const ProcessMemory& memory, uint64_t address, float* value) {
    float local = 0.0f;
    if (!memory.read(address, &local, sizeof(local)) || !std::isfinite(local)) return false;
    *value = local;
    return true;
}

// 保守范围：包含本程序的目标 [-6,-0.2] 与常规车辆米级重心偏移。
// 布局/对象不匹配时，即使是有限 float 也不能当作可修改的物理字段。


bool closeFloat(float a, float b, float tolerance) {
    return std::isfinite(a) && std::isfinite(b) && std::fabs(a - b) <= tolerance;
}

float clampWear(float value) {
    return (std::max)(0.0f, (std::min)(1.0f, value));
}

bool readWheelGroup(const ProcessMemory& memory, uint64_t truck, uint64_t pointerOffset,
                    uint64_t countOffset, uint64_t cacheOffset, float* value,
                    std::vector<uint64_t>* fields) {
    uint64_t count = 0;
    if (!memory.read(truck + countOffset, &count, sizeof(count)) || count > kMaxWheelFields) {
        return false;
    }
    uint64_t array = 0;
    if (count > 0 && !readPointer(memory, truck + pointerOffset, &array)) return false;

    float sum = 0.0f;
    for (uint64_t index = 0; index < count; ++index) {
        float item = 0.0f;
        const uint64_t address = array + index * sizeof(float);
        if (!readFloat(memory, address, &item) || item < -0.001f || item > 1.001f) return false;
        sum += item;
        if (fields) fields->push_back(address);
    }
    float cached = -1.0f;
    if (!readFloat(memory, truck + cacheOffset, &cached)) return false;
    *value = cached >= 0.0f ? cached : (count ? sum / static_cast<float>(count) : 0.0f);
    if (fields) fields->push_back(truck + cacheOffset);
    return true;
}

}  // namespace

// ===========================================================================
// ===========================================================================
bool VehicleLocker::bind(ProcessMemory* memory, DWORD pid, std::string* error) {
    stop();
    memory_ = memory;
    pid_ = pid;
    if (!memory_ || !memory_->isOpen()) {
        if (error) *error = "The game process has not been attached yet";
        return false;
    }
    if (!locateRootGlobal(error)) {
        memory_ = nullptr;
        pid_ = 0;
        return false;
    }
    setStatus("has been bound to the vehicle pointer of the verified version of the current game (no value scan)");
    return true;
}

bool VehicleLocker::locateRootGlobal(std::string* error) {
    uint64_t base = 0;
    const LayoutProfile* matched=nullptr;
    if (!verifyGameLayout(*memory_, &base, error, &matched)) return false;
    const auto& profile = *matched;
    fuelCorrectionOffset_=profile.fuelCorrectionOffset;
    truckTailShift_=profile.truckTailShift;
    int32_t displacement = 0;
    ::memcpy(&displacement, profile.rootInstruction.data() + 3, sizeof(displacement));
    rootGlobal_ = base + profile.rootRva + 7 + static_cast<int64_t>(displacement);
    return isCanonicalPointer(rootGlobal_);
}

bool VehicleLocker::resolveChainOnly(VehicleAddresses* addresses, std::string* error) const {
    if (!memory_ || !memory_->isOpen() || !rootGlobal_) {
        if (error) *error = "fixed vehicle pointer has not been bound yet";
        return false;
    }
    if (pid_ && (pid_ != memory_->pid() || !verifyGameLayout(*memory_, nullptr, error))) return false;
    uint64_t root = 0, driver = 0, context = 0, truck = 0;
    if (!readPointer(*memory_, rootGlobal_, &root) ||
        !readPointer(*memory_, root + kRootToDriver, &driver) ||
        !readPointer(*memory_, driver + kDriverToContext, &context) ||
        !readPointer(*memory_, context + kContextToTruck, &truck)) {
        if (error) *error = "There is currently no available vehicle (please enter the driving interface)";
        return false;
    }
    VehicleAddresses found;
    found.rootGlobal = rootGlobal_;
    found.context = context;
    found.truck = truck;
    if (addresses) *addresses = std::move(found);
    return true;
}

bool VehicleLocker::resolveAndValidate(VehicleAddresses* addresses,
                                       TelemetrySnapshot* telemetry,
                                       std::string* error, bool requireTelemetry) const {
    VehicleAddresses found;
    if (!resolveChainOnly(&found, error)) return false;
    const uint64_t truck = found.truck;
    const uint64_t context = found.context;

    TelemetrySnapshot live;
    std::string telemetryError;
    if (requireTelemetry && !readTelemetry(&live, &telemetryError)) {
        if (error) *error = "security verification requires telemetry data:" + telemetryError;
        return false;
    }

    float capacity = 0.0f, ratio = 0.0f, correction = 0.0f;
    std::array<float, 8> wearParts{};
    if (!readFloat(*memory_, truck + kTruckFuelCapacity, &capacity) ||
        !readFloat(*memory_, truck + kTruckFuelRatio + truckTailShift_, &ratio) ||
        !readFloat(*memory_, context + fuelCorrectionOffset_, &correction) ||
        !readFloat(*memory_, truck + kTruckEngineWear + truckTailShift_, &wearParts[0]) ||
        !readFloat(*memory_, truck + kTruckEnginePermanent + truckTailShift_, &wearParts[1]) ||
        !readFloat(*memory_, truck + kTruckTransmissionWear + truckTailShift_, &wearParts[2]) ||
        !readFloat(*memory_, truck + kTruckTransmissionPermanent + truckTailShift_, &wearParts[3]) ||
        !readFloat(*memory_, truck + kTruckCabinWear + truckTailShift_, &wearParts[4]) ||
        !readFloat(*memory_, truck + kTruckCabinPermanent + truckTailShift_, &wearParts[5]) ||
        !readFloat(*memory_, truck + kTruckChassisWear, &wearParts[6]) ||
        !readFloat(*memory_, truck + kTruckChassisPermanent, &wearParts[7])) {
        if (error) *error = "Failed to read fixed vehicle field";
        return false;
    }
    if (capacity <= 0.0f || capacity > 10000.0f || ratio < -0.01f || ratio > 1.10f ||
        correction < -1.10f || correction > 1.10f) {
        if (error) *error = "Fixed vehicle field range exception, writing has been refused";
        return false;
    }
    for (float value : wearParts) {
        if (value < -0.001f || value > 1.001f) {
            if (error) *error = "Fixed damage field range abnormality, write rejected";
            return false;
        }
    }

    found.fuel = truck + kTruckFuelRatio + truckTailShift_;
    found.fuelCorrection = context + fuelCorrectionOffset_;
    found.damageFields = {
        truck + kTruckEngineWear + truckTailShift_,        truck + kTruckEnginePermanent + truckTailShift_,
        truck + kTruckTransmissionWear + truckTailShift_,  truck + kTruckTransmissionPermanent + truckTailShift_,
        truck + kTruckCabinWear + truckTailShift_,         truck + kTruckCabinPermanent + truckTailShift_,
        truck + kTruckChassisWear,       truck + kTruckChassisPermanent,
    };

    float wheelWear = 0.0f, wheelPermanent = 0.0f;
    if (!readWheelGroup(*memory_, truck, kTruckWheelWearArray, kTruckWheelWearCount,
                        kTruckWheelWearCache, &wheelWear, &found.damageFields) ||
        !readWheelGroup(*memory_, truck, kTruckWheelPermanentArray,
                        kTruckWheelPermanentCount, kTruckWheelPermanentCache,
                        &wheelPermanent, &found.damageFields)) {
        if (error) *error = "Failed to read the wheel damage array, writing has been rejected";
        return false;
    }

    const float expectedFuel = capacity * (ratio + correction);
    std::array<float, 5> expectedWear = {
        clampWear(wearParts[0] + wearParts[1]),
        clampWear(wearParts[2] + wearParts[3]),
        clampWear(wearParts[4] + wearParts[5]),
        clampWear(wearParts[6] + wearParts[7]),
        clampWear(wheelWear + wheelPermanent),
    };
    if (requireTelemetry) {
        const bool fuelAlreadyLocked = fuelEnabled() && ratio >= 0.999f;
        const float fuelTolerance = (std::max)(2.0f, live.fuelCapacity * 0.005f);
        if (!closeFloat(capacity, live.fuelCapacity, 1.0f) ||
            (!fuelAlreadyLocked && !closeFloat(expectedFuel, live.fuel, fuelTolerance))) {
            if (error) {
                *error = fmt("Fixed fuel level field failed telemetry verification (memory %.2f/%.2f L, telemetry %.2f/%.2f L)",
                             expectedFuel, capacity, live.fuel, live.fuelCapacity);
            }
            return false;
        }
        for (size_t index = 0; index < expectedWear.size(); ++index) {
            const bool alreadyLocked = damageEnabled() && expectedWear[index] <= 0.002f;
            if (!alreadyLocked && !closeFloat(expectedWear[index], live.wear[index], 0.002f)) {
                if (error) {
                    *error = fmt("Fixed damage field item %llu failed telemetry verification (memory %.6f, telemetry %.6f)",
                                 (unsigned long long)(index + 1), expectedWear[index],
                                 live.wear[index]);
                }
                return false;
            }
        }
    }

    if (addresses) *addresses = std::move(found);
    if (telemetry) *telemetry = live;
    return true;
}

bool VehicleLocker::probe(VehicleAddresses* addresses, TelemetrySnapshot* telemetry,
                          std::string* error) {
    return resolveAndValidate(addresses, telemetry, error);
}

// 事务式写入：逐项「读取现值 → 写入 → 立即回读验证」；每一步都先把**确实写入并验证成功**
// 的字段记入恢复凭据（绝不用占位值覆盖已有凭据）。任一项失败则回滚本轮已写入项
// （先全部写回再统一回读验证）；回滚未成功的字段记为 NeedsRestore 并保留真实写入值，
// 交由后续周期安全重试 —— 绝不允许「报告失败却留下部分修改而没有恢复任务」。
bool VehicleLocker::applyTransactional(const VehicleAddresses& addresses, bool fuel, bool damage,
                                      uint64_t sequence, std::string* error) {
    struct Field { uint64_t address; float target; float before; };
    std::vector<Field> fields;
    if (fuel) {
        fields.push_back({addresses.fuel, 1.0f, 0.0f});
        fields.push_back({addresses.fuelCorrection, 0.0f, 0.0f});
    }
    if (damage) for (uint64_t address : addresses.damageFields)
        fields.push_back({address, 0.0f, 0.0f});
    // Read the entire transaction before changing any field.
    for (auto& field : fields) {
        if (!field.address || !readFloat(*memory_, field.address, &field.before)) {
            if (error) *error = "The vehicle field is not readable and no transaction writing is performed.";
            return false;
        }
    }
    auto rollback = [&](size_t count) {
        if (rollbackHookForTesting_) rollbackHookForTesting_();
        size_t failed = 0;
        for (size_t i = count; i > 0; --i) {
            const Field& field = fields[i - 1];
            float actual = 0.0f;
            // Do not overwrite values changed externally since our write.
            if (!readFloat(*memory_, field.address, &actual) ||
                (!closeFloat(actual, field.target, 1e-4f) &&
                 !closeFloat(actual, field.before, 1e-4f))) { ++failed; continue; }
            if (closeFloat(actual, field.before, 1e-4f)) continue;
            if (!memory_->write(field.address, &field.before, sizeof(float)) ||
                !readFloat(*memory_, field.address, &actual) ||
                !closeFloat(actual, field.before, 1e-4f)) ++failed;
        }
        return failed;
    };
    for (size_t i = 0; i < fields.size(); ++i) {
        const Field& field = fields[i];
        float actual = 0.0f;
        if (beforeFieldWrite_) beforeFieldWrite_(i);
        const bool stale = commandSequence() != sequence;
        if (stale || !memory_->write(field.address, &field.target, sizeof(float)) ||
            !readFloat(*memory_, field.address, &actual) ||
            !closeFloat(actual, field.target, 1e-4f)) {
            const size_t failed = rollback(stale ? i : i + 1);
            if (error) *error = failed
                ? fmt("Vehicle transaction failed, %zu item cannot confirm rollback; locking has stopped, please reload the archive", failed)
                : "Vehicle transaction cancellation or writing failure, this round of modifications has been rolled back";
            return false;
        }
    }
    return true;
}

bool VehicleLocker::setFuelEnabled(bool enabled, std::string* error, bool electric) {
    if (enabled) {
        if (const std::string reason = structuralWriteBlockReason(); !reason.empty()) {
            if (error) *error = reason;
            return false;
        }
    }
    const VehicleCommandState current = commands();
    applyCommand(enabled, current.damage, electric);
    if (!enabled) {
        std::lock_guard<std::mutex> writeLock(writeMutex_);
        setStatus(comboStatusText());
        return true;
    }
    if (!memory_ || !memory_->isOpen()) {
        applyCommand(false, current.damage);
        if (error) *error = "The game process has not been attached yet";
        return false;
    }
    VehicleAddresses addresses;
    TelemetrySnapshot telemetry;
    if (!resolveAndValidate(&addresses, &telemetry, error)) {
        applyCommand(false, current.damage);
        return false;
    }
    std::lock_guard<std::mutex> writeLock(writeMutex_);
    const CycleResult result = runCycleLocked(addresses, true, error);
    if (result.action != CycleAction::Applied) {
        applyCommand(false, current.damage);
        if (error && error->empty()) *error = result.note;
        setStatus(std::string(electric ? "Unlimited battery activation failed:" : "Unlimited fuel volume activation failed:") +
                  (error ? *error : std::string("write rejected")));
        return false;
    }
    setStatus(electric ? "unlimited power is locked (100%)" :
              fmt("unlimited oil volume locked (%.1f / %.1f L)", telemetry.fuel, telemetry.fuelCapacity));
    ensureThread();
    return true;
}

bool VehicleLocker::setDamageEnabled(bool enabled, std::string* error) {
    if (enabled) {
        if (const std::string reason = structuralWriteBlockReason(); !reason.empty()) {
            if (error) *error = reason;
            return false;
        }
    }
    const VehicleCommandState current = commands();
    applyCommand(current.fuel, enabled, current.electric);
    if (!enabled) {
        std::lock_guard<std::mutex> writeLock(writeMutex_);
        setStatus(comboStatusText());
        return true;
    }
    if (!memory_ || !memory_->isOpen()) {
        applyCommand(current.fuel, false, current.electric);
        if (error) *error = "The game process has not been attached yet";
        return false;
    }
    VehicleAddresses addresses;
    TelemetrySnapshot telemetry;
    if (!resolveAndValidate(&addresses, &telemetry, error)) {
        applyCommand(current.fuel, false, current.electric);
        return false;
    }
    std::lock_guard<std::mutex> writeLock(writeMutex_);
    const CycleResult result = runCycleLocked(addresses, true, error);
    if (result.action != CycleAction::Applied) {
        applyCommand(current.fuel, false, current.electric);
        if (error && error->empty()) *error = result.note;
        setStatus("Vehicle damage prevention activation failed:" + (error ? *error : std::string("write rejected")));
        return false;
    }
    setStatus("The five vehicle damages have been cleared and locked");
    ensureThread();
    return true;
}


// ===========================================================================
// 统一命令状态 / 单写者周期
// ===========================================================================
void VehicleLocker::applyCommand(bool fuel, bool damage, bool electric) {
    std::lock_guard<std::mutex> lock(commandMutex_);
    command_.fuel = fuel;
    command_.damage = damage;
    command_.electric = fuel && electric;
    ++command_.sequence;
}

VehicleCommandState VehicleLocker::commands() const {
    std::lock_guard<std::mutex> lock(commandMutex_);
    return command_;
}

uint64_t VehicleLocker::commandSequence() const {
    std::lock_guard<std::mutex> lock(commandMutex_);
    return command_.sequence;
}

bool VehicleLocker::fuelEnabled() const { return commands().fuel; }
bool VehicleLocker::damageEnabled() const { return commands().damage; }

void VehicleLocker::setWriteGuard(WriteGuard guard) {
    // 加锁安装：worker 线程可能正在运行并读取同一个 std::function（运行中替换守卫）
    std::lock_guard<std::mutex> lock(guardMutex_);
    guard_ = std::move(guard);
}

void VehicleLocker::setGuardIntervalMs(uint64_t milliseconds) {
    guardIntervalMs_ = milliseconds;
}

void VehicleLocker::setFuelWriteGuard(WriteGuard guard) {
    std::lock_guard<std::mutex> lock(guardMutex_);
    fuelGuard_ = std::move(guard);
}

void VehicleLocker::setCycleHookAfterSnapshot(std::function<void()> hook) {
    cycleHookAfterSnapshot_ = std::move(hook);
}

void VehicleLocker::setRollbackHookForTesting(std::function<void()> hook) {
    rollbackHookForTesting_ = std::move(hook);
}

bool VehicleLocker::bindForTesting(ProcessMemory* memory, uint64_t rootGlobal,uint64_t fuelCorrectionOffset) {
    stop();
    memory_ = memory;
    pid_ = 0;
    rootGlobal_ = rootGlobal;
    fuelCorrectionOffset_=fuelCorrectionOffset;
    if (!memory_ || !memory_->isOpen()) return false;
    return true;
}

void VehicleLocker::setCommandsForTesting(bool fuel, bool damage, bool electric) {
    applyCommand(fuel, damage, electric);
}
bool VehicleLocker::probeForTesting(VehicleAddresses* addresses,std::string* error) {
    return !pid_&&resolveAndValidate(addresses,nullptr,error,false);
}

std::string VehicleLocker::guardReason(const VehicleCommandState& state) {
    const uint64_t now = ::GetTickCount64();
    const uint64_t sequence = state.sequence;
    const bool sequenceChanged = sequence != guardCheckedSequence_;
    if (!sequenceChanged && now - guardCheckedAt_ < guardIntervalMs_) return std::string();
    guardCheckedAt_ = now;
    guardCheckedSequence_ = sequence;
    // 加锁复制守卫后在锁外调用：避免与运行中的 setWriteGuard 竞争，也不持锁执行外部代码
    WriteGuard guardSnapshot;
    {
        std::lock_guard<std::mutex> lock(guardMutex_);
        // Mixed fuel/damage commands always use the strict guard.
        guardSnapshot = state.fuel && !state.damage && fuelGuard_ ? fuelGuard_ : guard_;
    }
    if (!guardSnapshot) return std::string();
    return guardSnapshot();
}

// 单周期主逻辑（worker 线程与界面线程共用，调用方必须持有 writeMutex_）。
//   1) 命令快照 → 2) 写入闸门（联机保护）→ 3) 换车/备份 → 4) 事务式写入或还原
//   5) 写入前重新确认命令未变化（丢弃过期任务，避免旧状态覆盖新的关闭请求）
VehicleLocker::CycleResult VehicleLocker::runCycleLocked(const VehicleAddresses& addresses,
                                                        bool requireTelemetry, std::string* error) {
    (void)requireTelemetry; // Production callers resolve and cross-check telemetry first.
    if (!memory_ || !memory_->isOpen()) return {CycleAction::Idle, "The game process has not been bound yet"};
    const VehicleCommandState state = commands();
    if (cycleHookAfterSnapshot_) cycleHookAfterSnapshot_();
    if (commandSequence() != state.sequence) return {CycleAction::SkippedStale, "status has changed and will not be written in this round."};
    if (!state.fuel && !state.damage) return {CycleAction::Idle, "Vehicle lock is off"};
    std::string blocked = structuralWriteBlockReason();
    if (blocked.empty()) blocked = guardReason(state);
    if (commandSequence() != state.sequence) return {CycleAction::SkippedStale, "Status changed during gate inspection"};
    if (!blocked.empty()) {
        applyCommand(false, false);
        if (error) *error = blocked;
        return {CycleAction::Blocked, "Write rejected:" + blocked};
    }
    std::string detail;
    if (!applyTransactional(addresses, state.fuel, state.damage, state.sequence, &detail)) {
        if (commandSequence() == state.sequence) applyCommand(false, false);
        if (error) *error = detail;
        return {CycleAction::Failed, detail};
    }
    return {CycleAction::Applied, comboStatusText()};
}

VehicleLocker::CycleResult VehicleLocker::runMaintenanceCycle(const VehicleAddresses& addresses,
                                                             bool requireTelemetry,
                                                             std::string* error) {
    std::lock_guard<std::mutex> writeLock(writeMutex_);
    return runCycleLocked(addresses, requireTelemetry, error);
}

void VehicleLocker::ensureThread() {
    if (thread_.joinable()) return;
    stopping_.store(false);
    thread_ = std::thread([this]() { run(); });
}

// 单个 worker 迭代（真实共享实现；离线测试直接驱动它）。
// 关键：**链断恢复路径也必须持有 writeMutex_ 并复核最新命令** —— 否则它会绕过
// 「唯一写者」约束，与界面线程正在进行的启用/写入交错，误隔离有效记录或覆盖新写入。
VehicleLocker::CycleResult VehicleLocker::runWorkerIteration() {
    const VehicleCommandState state = commands();
    if (!state.fuel && !state.damage) return {CycleAction::Idle, "Vehicle lock is off"};
    VehicleAddresses addresses;
    TelemetrySnapshot telemetry;
    std::string error;
    if (!resolveAndValidate(&addresses, &telemetry, &error))
        return {CycleAction::Blocked, "Lock has been suspended:" + error};
    std::lock_guard<std::mutex> writeLock(writeMutex_);
    if (commandSequence() != state.sequence) return {CycleAction::SkippedStale, "command has changed"};
    return runCycleLocked(addresses, true, &error);
}

void VehicleLocker::run() {
    while (!stopping_.load()) {
        const CycleResult result = runWorkerIteration();
        setStatus(result.note.empty() ? comboStatusText() : result.note);
        ::Sleep(result.action == CycleAction::SkippedStale ? 10 : 250);
    }
}

void VehicleLocker::stop() {
    applyCommand(false, false);
    stopping_.store(true);
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> writeLock(writeMutex_);
    memory_ = nullptr;
    pid_ = 0;
    rootGlobal_ = 0;
    fuelCorrectionOffset_=0x1158;
    truckTailShift_=0;
    setStatus("not enabled");
}

std::string VehicleLocker::comboStatusText() const {
    std::vector<std::string> items;
    const auto state = commands();
    if (state.fuel) items.push_back(state.electric ? "Unlimited battery" : "Unlimited fuel");
    if (state.damage) items.push_back("damage prevention");
    if (items.empty()) return "Vehicle lock is off";
    std::string text;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) text += " + ";
        text += items[i];
    }
    text += "locked";
    return text;
}

void VehicleLocker::setStatus(const std::string& text) {
    std::lock_guard<std::mutex> lock(statusMutex_);
    status_ = text;
}

std::string VehicleLocker::status() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return status_;
}

}  // namespace ets2
