// ui.cpp —— ImGui 界面实现
#include "ui.h"
#include "homepreset.h"
#include "fueltank.h"
#include "ui_style.h"

#include "economy.h"
#include "layout.h"
#include "imgui.h"

#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace ets2 {

namespace {

bool parseNumber(const char* text, bool isFloat, double* out) {
    std::string s(text ? text : "");
    std::string cleaned;
    for (char c : s) {
        if (c == ',' || c == ' ' || c == '\t') continue;
        if (c == '\xEF' || (unsigned char)c >= 0x80) continue;  // 去掉中文逗号等
        cleaned += c;
    }
    if (cleaned.empty()) return false;
    try {
        size_t idx = 0;
        double v = std::stod(cleaned, &idx);
        if (idx != cleaned.size()) return false;
        if (!std::isfinite(v)) return false;
        if (!isFloat) {
            // 扫描 API 内部使用 double 传递输入。超过 2^53 会静默丢失整数精度，
            // 所以明确拒绝，而不是扫描/写入一个不同的值。
            constexpr double kMaxExactInteger = 9007199254740991.0;
            if (std::floor(v) != v || std::fabs(v) > kMaxExactInteger) return false;
        }
        *out = v;
        return true;
    } catch (...) {
        return false;
    }
}

size_t maxRegionBytes(const AppState& app) {
    if (app.maxRegionGb <= 0.0f) return 0;
    return (size_t)(app.maxRegionGb * 1073741824.0f);
}

ScanSession* sessionFor(AppState& app, PanelKind kind) {
    if (kind == PanelKind::Money) return app.moneySession.get();
    if (kind == PanelKind::Xp) return app.xpSession.get();
    if (kind == PanelKind::Fuel) return app.fuelSession.get();
    if (kind == PanelKind::Damage) return app.damageSession.get();
    return app.scanSession.get();
}

char* currentFor(AppState& app, PanelKind kind) {
    if (kind == PanelKind::Money) return app.moneyCurrent;
    if (kind == PanelKind::Xp) return app.xpCurrent;
    if (kind == PanelKind::Fuel) return app.fuelCurrent;
    if (kind == PanelKind::Damage) return app.damageCurrent;
    return app.scanValue;
}

char* targetFor(AppState& app, PanelKind kind) {
    if (kind == PanelKind::Money) return app.moneyTarget;
    if (kind == PanelKind::Xp) return app.xpTarget;
    if (kind == PanelKind::Fuel) return app.fuelTarget;
    if (kind == PanelKind::Damage) return app.damageTarget;
    return app.scanNewValue;
}

bool* lockFor(AppState& app, PanelKind kind) {
    if (kind == PanelKind::Money) return &app.moneyLock;
    if (kind == PanelKind::Xp) return &app.xpLock;
    if (kind == PanelKind::Fuel) return &app.fuelLock;
    if (kind == PanelKind::Damage) return &app.damageLock;
    return nullptr;
}

VType typeFor(AppState& app, PanelKind kind) {
    if (kind == PanelKind::Money) return app.moneyType;
    if (kind == PanelKind::Xp) return app.xpType;
    if (kind == PanelKind::Fuel) return app.fuelType;
    if (kind == PanelKind::Damage) return app.damageType;
    return app.scanType;
}

std::string& statusFor(AppState& app, PanelKind kind) {
    if (kind == PanelKind::Money) return app.moneyStatus;
    if (kind == PanelKind::Xp) return app.xpStatus;
    if (kind == PanelKind::Fuel) return app.fuelStatus;
    if (kind == PanelKind::Damage) return app.damageStatus;
    return app.scanStatus;
}

//: 浮点扫描容差输入框（整数类型不用）
char* toleranceFor(AppState& app, PanelKind kind) {
    if (kind == PanelKind::Money) return app.moneyTol;
    if (kind == PanelKind::Xp) return app.xpTol;
    if (kind == PanelKind::Fuel) return app.fuelTol;
    if (kind == PanelKind::Damage) return app.damageTol;
    return app.scanTolerance;
}

double toleranceValue(AppState& app, PanelKind kind) {
    const char* text = toleranceFor(app, kind);
    double v = ::atof(text ? text : "0");
    return v < 0 ? 0 : v;
}

const char* panelName(PanelKind kind) {
    if (kind == PanelKind::Money) return "Cash";
    if (kind == PanelKind::Xp) return "Experience";
    if (kind == PanelKind::Fuel) return "fuel level";
    if (kind == PanelKind::Damage) return "damaged";
    return "scanner";
}

void resetProcessState(AppState& app) {
    app.engineDraftSourceOption = -1;
    if (app.engineTuner) {
        // 分离前尽力写回原值，然后才放弃内存引用（避免再访问失效的 ProcessMemory）。
        app.engineTuner->restoreAndDetach();
        app.enginePowerOption = 0;
        app.engineRaiseLimit = false;
        app.engineStateText = app.engineTuner->exportState();
        app.engineStatus = "is not enabled. Power adjustment directly rewrites engine data without scanning the memory.";
    }
    if (app.vehicleLocker) app.vehicleLocker->stop();
    if (app.freezer) {
        app.freezer->stop();
        app.freezer->clear();
    }
    app.moneySession.reset();
    app.xpSession.reset();
    app.fuelSession.reset();
    app.damageSession.reset();
    app.scanSession.reset();
    app.scanSelection.clear();
    app.moneyLock = false;
    app.xpLock = false;
    app.fuelLock = false;
    app.damageLock = false;
    app.autoFuelLock = false;
    app.autoElectricLock = false;
    app.autoDamageLock = false;
    app.moneyStatus = "Not attached: Please rescan cash.";
    app.xpStatus = "is not attached: Please rescan the experience.";
    app.fuelStatus = "Not attached: Infinite fuel has been stopped.";
    app.electricStatus = "Not attached: Unlimited power has stopped.";
    app.damageStatus = "Not attached: The vehicle has stopped without damage.";
    app.scanStatus = "Not attached: Scan has failed.";
    // 结构化定位结果绑定的地址随进程失效，必须一起清掉
    app.locatedMoneyAddress = 0;
    app.locatedMoneyBank = 0;
    app.locatedMoneyValue = 0;
    app.locatedMoneyDetail.clear();
    app.locatedXpAddress = 0;
    app.locatedEconomy = 0;
    app.locatedXpValue = 0;
    app.locatedXpDetail.clear();
    app.locateStatus = "is not located. After filling in the current amount in the game, click \"Change Money Directly\", and the program will automatically locate and write it, without locking the entire process.";
    app.autoWriteMoneyAfterLocate = false;
    app.autoWriteXpAfterLocate = false;
    app.pendingMultiAction = 0;
    app.pendingMultiKind = -1;
    app.requestMultiConfirm = false;
    app.showCopyDialog = false;
    app.copyRequest = {};
    app.copyName[0] = 0;
    app.showTruckTransfer = false;
    app.showTruckTransferNotice = false;
    app.truckTransferNotice.clear();
    app.truckTransferRequest = {};
    app.transferSourceInventory = {};
    app.transferTargetInventory = {};
    app.transferTargets.clear();
    app.transferTargetIndex = -1;
    app.transferNote.clear();
    app.transferInspectBusy = false; app.transferTargetReady = false;
    ++app.transferInspectionGeneration;
    app.showFleet=false;app.fleetInspectBusy=false;app.fleetPreview=false;
    ++app.fleetGeneration;app.fleetRequest={};app.fleetInventory={};app.fleetCounts.clear();
    app.fleetIdleSelected.clear();app.fleetRepairCaches=false;
    app.fleetGarageCapacity.clear();app.fleetUnownedOnly=false;
    app.fleetName[0]=0;app.fleetSearch[0]=0;app.fleetNote.clear();
    app.showHomeAccessories = false;
    app.homeInspectBusy = false; app.homePreview = false;
    app.homePresetMode = 0;
    ++app.homeInspectionGeneration;
    app.homeRequest = {}; app.homeInventory = {};
    app.homeNote.clear(); app.homePath[0] = 0;
    app.showProfileSplit = false;
    app.splitRows.clear();
    app.splitUiFixture = false;
    { std::lock_guard<std::mutex> lock(app.resultMutex);
      app.pendingReady = false;
      app.pendingLog.clear();
      app.pendingStatus.clear();
      app.pendingProgressNote.clear();
      app.pendingEconomy = PendingEconomyResult{};
      app.pendingCopy = {};
      app.pendingCopyTruck = false;
      app.pendingCopyHome = false;
      app.pendingCopyFleet = false;
      app.pendingFleet = {};
      app.pendingHomeInspection = {};
      app.pendingCopyReady = false; }
    { std::lock_guard<std::mutex> lock(app.resultMutex);
      app.pendingSplit = {}; app.pendingSplitReady = false;
      app.pendingExport = {}; }
}

// ---------- 数值类型下拉框 ----------
bool typeCombo(const char* label, VType* type) {
    static const char* kNames[] = {"int32", "uint32", "int64", "uint64", "float", "double"};
    int current = (int)*type;
    bool changed = false;
    ::ImGui::SetNextItemWidth(90);
    if (::ImGui::BeginCombo(label, kNames[current])) {
        for (int i = 0; i < 6; ++i) {
            bool selected = (i == current);
            if (::ImGui::Selectable(kNames[i], selected)) {
                *type = (VType)i;
                changed = true;
            }
            if (selected) ::ImGui::SetItemDefaultFocus();
        }
        ::ImGui::EndCombo();
    }
    return changed;
}

}  // namespace

bool validatePanelBindings(std::string* error) {
    AppState app;
    const PanelKind kinds[] = {PanelKind::Money, PanelKind::Xp, PanelKind::Fuel,
                               PanelKind::Damage, PanelKind::Scanner};
    char* expectedCurrent[] = {app.moneyCurrent, app.xpCurrent, app.fuelCurrent,
                               app.damageCurrent, app.scanValue};
    char* expectedTarget[] = {app.moneyTarget, app.xpTarget, app.fuelTarget,
                              app.damageTarget, app.scanNewValue};
    bool* expectedLock[] = {&app.moneyLock, &app.xpLock, &app.fuelLock, &app.damageLock, nullptr};
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
        if (currentFor(app, kinds[i]) != expectedCurrent[i] ||
            targetFor(app, kinds[i]) != expectedTarget[i] ||
            lockFor(app, kinds[i]) != expectedLock[i]) {
            if (error) *error = fmt("input/lock binding error for panel %d", (int)kinds[i]);
            return false;
        }
    }
    double parsed = 0;
    if (!parseNumber("9007199254740991", false, &parsed) ||
        parseNumber("9007199254740992", false, &parsed) || parseNumber("12.5", false, &parsed)) {
        if (error) *error = "Integer precision boundary check failed";
        return false;
    }
    return true;
}

// ========================================================================== 
// 工作线程 -> 界面线程 的消息
// ========================================================================== 
namespace {

void postResult(AppState& app, PanelKind kind, const std::string& status,
                const std::string& log = std::string(),
                const std::string& progressNote = std::string()) {
    std::lock_guard<std::mutex> lock(app.resultMutex);
    app.pendingStatus = status;
    app.pendingLog = log;
    app.pendingProgressNote = progressNote;
    app.pendingKind = (int)kind;
    app.pendingReady = true;
}

void applyPending(AppState& app) {
    std::string status, log, progressNote;
    int kind = 0;
    {
        std::lock_guard<std::mutex> lock(app.resultMutex);
        if (!app.pendingReady) return;
        status = app.pendingStatus;
        log = app.pendingLog;
        progressNote = app.pendingProgressNote;
        kind = app.pendingKind;
        app.pendingStatus.clear();
        app.pendingLog.clear();
        app.pendingProgressNote.clear();
        app.pendingReady = false;
    }
    if (!status.empty()) statusFor(app, (PanelKind)kind) = status;
    if (!log.empty()) logLine(log);
    if (!progressNote.empty()) app.progressNote = progressNote;
}

void joinWorker(AppState& app) {
    if (app.worker.joinable()) app.worker.join();
}

bool ensureAttached(AppState& app) {
    if (app.mem.isOpen() && app.mem.alive()) return true;
    attachGame(app);
    return app.mem.isOpen();
}

}  // namespace

// ==========================================================================
// 进程附加 / 分离
// ==========================================================================
bool validateAttachedSessionReuse(std::string* error) {
    AppState app;
    if (!app.mem.open(::GetCurrentProcessId(), "attached-session-test", error)) return false;
    // 失败时直接返回，绝不让离线测试落入查找/附加真实游戏的分支。
    if (!app.mem.alive()) {
        if (error) *error = "The live process is misjudged as exiting; continuous activation will reattach and reset the function";
        return false;
    }
    app.pid = ::GetCurrentProcessId();
    app.autoFuelLock = app.autoDamageLock = true;
    app.moneyLock = app.xpLock = true;
    app.enginePowerOption = 3;
    app.engineRaiseLimit = true;
    app.locatedMoneyAddress = 123;
    for (int i = 0; i < 10; ++i) {
        if (!ensureAttached(app) || !app.autoFuelLock || !app.autoDamageLock ||
            !app.moneyLock || !app.xpLock ||
            app.enginePowerOption != 3 || !app.engineRaiseLimit ||
            app.locatedMoneyAddress != 123 || app.mem.pid() != ::GetCurrentProcessId()) {
            if (error) *error = "Existing function/positioning status is reset when reusing the current attached session";
            return false;
        }
    }
    return true;
}

static std::string serializeSpots(const AppState& app);
static void parseSpotsInto(const std::string& value, AppState& app);

// 切换目标游戏：必须先停下所有写入线程并恢复现场，再改变选择。
// 任何未确认的发动机恢复凭据都会阻止切换，绝不静默丢弃。
bool selectGame(AppState& app, GameId id, const std::wstring& settingsPath) {
    if (id == selectedGame()) return true;
    app.cancel.store(true);
    joinWorker(app);
    resetProcessState(app);   // 先停动力/车辆/冻结器并清空扫描与地址，才允许换选择
    if (app.engineTuner && (app.engineTuner->active() ||
                            app.engineTuner->hasInvalidatedRecord() ||
                            !app.engineTuner->exportState().empty())) {
        app.procStatus = "Switching canceled: The original engine value has not been restored yet. Please complete the restoration before switching games.";
        logLine(app.procStatus);
        return false;
    }
    // 先把当前收藏清单归档到所属游戏，再切换选择（设置保存也按游戏分区）
    if (selectedGame() == GameId::Ets2) {
        app.teleportSpotsEts2 = serializeSpots(app);
        app.engineStateEts2 = app.engineStateText;
    } else {
        app.teleportSpotsAts = serializeSpots(app);
        app.engineStateAts = app.engineStateText;
    }
    detachGame(app);
    app.engineStateText.clear();
    setSelectedGame(id);
    app.engineStateText = id == GameId::Ets2 ? app.engineStateEts2 : app.engineStateAts;
    if (app.engineTuner) app.engineTuner->importState(app.engineStateText);
    // 再装入目标游戏的收藏清单
    app.teleportSpots.clear();
    parseSpotsInto(id == GameId::Ets2 ? app.teleportSpotsEts2 : app.teleportSpotsAts, app);
    // 存档/备份/选择等都不允许带进新游戏，否则可能在旧存档上执行新操作
    app.slots.clear();
    app.saveProfiles.clear();
    app.selectedSaveProfile = -1;
    app.visibleSaveSlotCount = 0;
    app.backups.clear();
    app.selectedSlot = -1;
    app.selectedBackup = -1;
    app.slotValues = {};
    app.telemetry = {};
    app.telemetryAvailable = false;
    app.telemetryCheckedAt = 0;
    app.telemetryError.clear();
    app.saveMoney[0] = app.saveXp[0] = '\0';
    app.saveNote = "Please select the archive of the current game.";
    app.requestRestoreConfirm = app.showBackupDialog = false;
    app.camera = {};
    app.cameraLoaded = false;
    app.flyDeveloper = app.flyConsole = -1;
    app.flySpeedSaved.clear();
    ::strcpy_s(app.flySpeed, "100");
    app.flyNote = "Please recheck the config.cfg and camera buttons of the current game.";
    app.consoleKey = 0xC0;
    ::strcpy_s(app.consoleCmd, "g_set_time 12 0");
    app.consoleNote.clear();
    app.convoy = {};
    app.convoyNote = "Please refresh the online status of the current game.";
    app.cityFilter[0] = app.cityInput[0] = app.coordInput[0] = app.spotName[0] = '\0';
    app.teleportNote = "Please read the city list of the current game.";
    app.lastEngineStateRevision = app.lastEngineSaveTick = 0;
    app.mapCities.clear();
    app.mapCitySelected = -1;
    app.mpHits.clear();
    app.memInfo.clear();
    app.procStatus = fmt("has been switched to %s: please append %s; archives/configurations/backups are all read according to the game path.",
                         selectedGameDescriptor().shortName,
                         W2U(selectedProcessName()).c_str());
    logLine(app.procStatus);
    if (!supportsVerifiedStructures(selectedGame())) logLine(structuralWriteBlockReason());
    const bool saved = settingsPath.empty() ? saveSettings(app) : saveSettingsTo(app, settingsPath);
    if (!saved) app.procStatus += "(Failed to save settings, current selection only takes effect in this run)";
    return true;
}

void attachGame(AppState& app) {
    app.cancel.store(true);
    joinWorker(app);
    DWORD pid = 0;
    std::wstring exe;
    if (!findProcess(selectedProcessName(), &pid, &exe)) {
        resetProcessState(app);
        app.mem.close();
        app.procStatus = fmt("%s not found, please start the game first", W2U(selectedProcessName()).c_str());
        logLine(app.procStatus);
        app.attached = false;
        // 清掉上一轮的进程/联机痕迹，避免界面显示与当前状态矛盾
        app.pid = 0;
        app.exeName.clear();
        app.mpHits.clear();
        app.memInfo.clear();
        return;
    }
    // 每次重新附加都作为一个新的地址空间处理。即使 PID 相同，也不复用旧扫描与锁定地址。
    resetProcessState(app);
    enableDebugPrivilege();
    std::string err;
    if (!app.mem.open(pid, W2U(exe), &err)) {
        app.procStatus = "Attach failed:" + err;
        logLine(app.procStatus);
        app.attached = false;
        app.pid = 0;
        app.exeName.clear();
        app.mpHits.clear();
        app.memInfo.clear();
        return;
    }
    app.pid = pid;
    app.exeName = exe;
    app.attached = true;
    if (!app.freezer) app.freezer = std::make_unique<Freezer>();
    // 锁定（可选保险）同样要过联机闸门：进入联机后不得继续每 250ms 写入
    app.freezer->setWriteGuard([]() { return writeGateReasonFromPolicy(); });
    app.freezer->start(&app.mem, 250);
    if (!app.vehicleLocker) app.vehicleLocker = std::make_unique<VehicleLocker>();
    // 写入闸门：油量/无损的后台周期在真正写入前都会调用它（联机保护）。
    // 不捕获 AppState：worker 线程只读线程安全的策略快照。
    app.vehicleLocker->setWriteGuard([]() { return writeGateReasonFromPolicy(); });
    app.vehicleLocker->setFuelWriteGuard([]() { return fuelWriteGateReasonFromPolicy(); });
    app.procStatus = fmt("Attached: %s (PID %u)", W2U(exe).c_str(), pid);
    app.mpHits = detectMultiplayer(pid);
    MultiplayerStatus mp = checkMultiplayer(pid, true);
    app.mpBlocks = mp.blocksWrite();
    app.mpDetail = mp.detail;
    if (mp.blocksWrite()) {
        logLine("online protection:" + mp.detail + "; TruckersMP only allows automatic unlimited fuel/power, and other memory writes are still limited.");
    }
    std::vector<Region> regions = app.mem.regions(maxRegionBytes(app));
    uint64_t total = 0;
    for (const auto& r : regions) total += r.size;
    app.memInfo = fmt("Writable memory: %d areas, total %s\nOnline detection after", (int)regions.size(),
                      formatSize(total).c_str());
    logLine(app.procStatus);
    logLine(app.memInfo);
    logLine("is attached:" + mp.detail);
    TelemetrySnapshot telemetry;
    std::string telemetryError;
    if (readTelemetry(&telemetry, &telemetryError))
        logLine(fmt("attached post telemetry: game=%u revision=%u fuel=%.2f/%.2f L", telemetry.game,
            telemetry.pluginRevision, telemetry.fuel, telemetry.fuelCapacity));
    else logLine("Telemetry not available after attachment:" + telemetryError);
}

void detachGame(AppState& app) {
    app.cancel.store(true);
    joinWorker(app);
    resetProcessState(app);
    app.mem.close();
    app.attached = false;
    app.pid = 0;
    app.mpHits.clear();
    app.procStatus = "is not attached: please start the game first, then click \"Attach Game\"";
    app.memInfo.clear();
    logLine("Process detached");
}

void initFonts(AppState& app) {
    (void)app;
    ImGuiIO& io = ::ImGui::GetIO();
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    const char* kFonts[] = {"C:\\Windows\\Fonts\\msyh.ttc", "C:\\Windows\\Fonts\\msyh.ttf",
                            "C:\\Windows\\Fonts\\simhei.ttf", "C:\\Windows\\Fonts\\Deng.ttf",
                            "C:\\Windows\\Fonts\\simsun.ttc"};
    ImFont* font = nullptr;
    for (const auto* path : kFonts) {
        if (::GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;
        font = io.Fonts->AddFontFromFileTTF(path, 17.0f, &cfg,
                                           io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        if (font) break;
    }
    if (!font) io.Fonts->AddFontDefault();
}

void loadSettings(AppState& app) {
    loadSettingsFrom(app, applicationDir() + L"\\settings.ini");
}

// 收藏点序列化/解析（切换游戏时按游戏分区保存，互不覆盖）
static std::string serializeSpots(const AppState& app) {
    std::string spots;
    for (const auto& spot : app.teleportSpots) {
        if (!spots.empty()) spots += ",";
        spots += spot.first + "|" + spot.second;
    }
    return spots;
}

static void parseSpotsInto(const std::string& value, AppState& app) {
    app.teleportSpots.clear();
    char separator = ',';
    if (value.find(',') == std::string::npos && value.find(';') != std::string::npos &&
        value.find('|') != value.rfind('|')) {
        separator = ';';
    }
    size_t pos = 0;
    while (pos <= value.size()) {
        const size_t sep = value.find(separator, pos);
        const std::string item =
            value.substr(pos, (sep == std::string::npos) ? std::string::npos : sep - pos);
        const size_t bar = item.find('|');
        if (bar != std::string::npos && bar > 0 && bar + 1 < item.size()) {
            app.teleportSpots.emplace_back(item.substr(0, bar), item.substr(bar + 1));
        }
        if (sep == std::string::npos) break;
        pos = sep + 1;
    }
}

void loadSettingsFrom(AppState& app, const std::wstring& path) {
    std::ifstream in{std::filesystem::path(path)};
    if (!in) return;
    std::vector<std::string> lines;
    {
        std::string raw;
        while (std::getline(in, raw)) lines.push_back(raw);
    }
    // 先确定游戏选择，之后的存档目录/收藏等按键都按它解析
    for (const auto& probe : lines) {
        const size_t eqPos = probe.find('=');
        if (eqPos == std::string::npos) continue;
        if (trim(probe.substr(0, eqPos)) == "game") {
            GameId parsed;
            // --game=... 在命令行明确指定时，settings.ini 不得覆盖
            if (!selectionLockedFromCli() &&
                parseGameKey(trim(probe.substr(eqPos + 1)), &parsed)) {
                setSelectedGame(parsed);
            }
        }
    }
    for (const std::string& line : lines) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq + 1));
        if (key == "money_type") vtypeFromName(value.c_str(), &app.moneyType);
        else if (key == "xp_type") vtypeFromName(value.c_str(), &app.xpType);
        else if (key == "fuel_type") vtypeFromName(value.c_str(), &app.fuelType);
        else if (key == "damage_type") vtypeFromName(value.c_str(), &app.damageType);
        else if (key == "money_target") ::strncpy_s(app.moneyTarget, value.c_str(), _TRUNCATE);
        else if (key == "xp_target") ::strncpy_s(app.xpTarget, value.c_str(), _TRUNCATE);
        else if (key == "fuel_target") ::strncpy_s(app.fuelTarget, value.c_str(), _TRUNCATE);
        else if (key == "fuel_tol") ::strncpy_s(app.fuelTol, value.c_str(), _TRUNCATE);
        else if (key == "damage_target") ::strncpy_s(app.damageTarget, value.c_str(), _TRUNCATE);
        else if (key == "damage_tol") ::strncpy_s(app.damageTol, value.c_str(), _TRUNCATE);
        else if (key == "console_key") app.consoleKey = ::atoi(value.c_str());
        else if (key == "max_region_gb") {
            const float parsed = (float)::atof(value.c_str());
            app.maxRegionGb = (std::isfinite(parsed) && parsed >= 0.0f && parsed <= 64.0f)
                                  ? parsed : 1.0f;
        }
        else if (key == "workers") app.workers = ::atoi(value.c_str());
        else if (key == "engine_state" || key == "engine_state_ats") {
            const bool ats = key == "engine_state_ats";
            (ats ? app.engineStateAts : app.engineStateEts2) = value;
            if (ats == (selectedGame() == GameId::Ats)) {
                app.engineStateText = value;
                if (app.engineTuner) app.engineTuner->importState(value);
            }
        }
        else if (key == "fly_speed") ::strncpy_s(app.flySpeed, value.c_str(), _TRUNCATE);
        else if (key == "mp_policy") app.mpPolicy = ::atoi(value.c_str()) == 1 ? 1 : 0;
        else if (key == "check_convoy") app.checkConvoy = (::atoi(value.c_str()) != 0);
        else if (key == "hotkeys_enabled") app.hotkeysEnabled = (::atoi(value.c_str()) != 0);
        else if (key == "teleport_spots" || key == "teleport_spots_ats") {
            // 收藏点按游戏分区：只加载当前选择游戏的键，避免把欧卡2 的收藏带进美卡
            if ((key == "teleport_spots_ats") != (selectedGame() == GameId::Ats)) {
                if (key == "teleport_spots_ats") {
                    app.teleportSpotsAts = value;
                } else {
                    app.teleportSpotsEts2 = value;
                }
                continue;
            }
            // 格式：名字|目标,名字|目标（分隔符必须是 sanitize 白名单外的字符，
            // 否则坐标里的 ';' 会把一个收藏拆碎）
            // 兼容旧版：旧版用 ';' 分隔多条，仅当出现多个 '|' 且没有新分隔符时才按旧规则解析。
            char separator = ',';
            if (value.find(',') == std::string::npos &&
                value.find(';') != std::string::npos && value.find('|') != value.rfind('|')) {
                separator = ';';
            }
            app.teleportSpots.clear();
            size_t pos = 0;
            while (pos <= value.size()) {
                const size_t sep = value.find(separator, pos);
                const std::string item =
                    value.substr(pos, (sep == std::string::npos) ? std::string::npos : sep - pos);
                const size_t bar = item.find('|');
                if (bar != std::string::npos && bar > 0 && bar + 1 < item.size()) {
                    app.teleportSpots.emplace_back(item.substr(0, bar), item.substr(bar + 1));
                }
                if (sep == std::string::npos) break;
                pos = sep + 1;
            }
        }
    }
    setConvoyCheckEnabled(app.checkConvoy);
}

std::string settingsText(const AppState& app) {
    std::string text;
    text += "money_type=" + std::string(vtypeName(app.moneyType)) + "\n";
    text += "xp_type=" + std::string(vtypeName(app.xpType)) + "\n";
    text += "fuel_type=" + std::string(vtypeName(app.fuelType)) + "\n";
    text += "damage_type=" + std::string(vtypeName(app.damageType)) + "\n";
    text += "money_target=" + std::string(app.moneyTarget) + "\n";
    text += "xp_target=" + std::string(app.xpTarget) + "\n";
    text += "fuel_target=" + std::string(app.fuelTarget) + "\n";
    text += "fuel_tol=" + std::string(app.fuelTol) + "\n";
    text += "damage_target=" + std::string(app.damageTarget) + "\n";
    text += "damage_tol=" + std::string(app.damageTol) + "\n";
    text += fmt("console_key=%d\n", app.consoleKey);
    text += fmt("max_region_gb=%g\n", (double)app.maxRegionGb);
    text += fmt("workers=%d\n", app.workers);
    const std::string currentEngineState = app.engineTuner ? app.engineTuner->exportState() : app.engineStateText;
    text += "engine_state=" + (selectedGame() == GameId::Ets2 ? currentEngineState : app.engineStateEts2) + "\n";
    text += "engine_state_ats=" + (selectedGame() == GameId::Ats ? currentEngineState : app.engineStateAts) + "\n";
    text += "fly_speed=" + std::string(app.flySpeed) + "\n";
    text += std::string("game=") + gameKey(selectedGame()) + "\n";
    text += fmt("mp_policy=%d\n", app.mpPolicy);
    text += fmt("check_convoy=%d\n", app.checkConvoy ? 1 : 0);
    text += fmt("hotkeys_enabled=%d\n", app.hotkeysEnabled ? 1 : 0);
    std::string spots;
    for (const auto& spot : app.teleportSpots) {
        // ',' 不在传送目标白名单内，不会被坐标里的 ';' 破坏
        if (!spots.empty()) spots += ",";
        spots += spot.first + "|" + spot.second;
    }
    // 收藏点按游戏分区保存：美卡写 teleport_spots_ats，不覆盖欧卡2 的收藏
    // 两个游戏的收藏都写入；非当前游戏的原样保留，切游戏不会删掉对方的收藏
    if (selectedGame() == GameId::Ats) {
        text += "teleport_spots_ats=" + spots + "\n";
        text += "teleport_spots=" + app.teleportSpotsEts2 + "\n";
    } else {
        text += "teleport_spots=" + spots + "\n";
        text += "teleport_spots_ats=" + app.teleportSpotsAts + "\n";
    }
    return text;
}

bool writeTextFileAtomic(const std::wstring& path, const std::string& text) {
    // 原子写入：先写同目录临时文件再替换，避免中途崩溃/断电留下半截文件。
    const std::wstring tempPath = path + L".tmp";
    {
        std::ofstream out{std::filesystem::path(tempPath), std::ios::trunc | std::ios::binary};
        if (!out) return false;
        out.write(text.data(), (std::streamsize)text.size());
        out.flush();
        if (!out.good()) {
            ::DeleteFileW(tempPath.c_str());
            return false;
        }
    }
    if (!::MoveFileExW(tempPath.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ::DeleteFileW(tempPath.c_str());
        return false;
    }
    return true;
}

bool saveSettingsTo(const AppState& app, const std::wstring& path) {
    // 内容格式与旧版完全一致（键名/顺序不变），可直接覆盖旧文件，无需迁移。
    return writeTextFileAtomic(path, settingsText(app));
}

bool saveSettings(const AppState& app) {
    return saveSettingsTo(app, applicationDir() + L"\\settings.ini");
}

// ==========================================================================
// 写入闸门 + 目标值校验（所有真正写入游戏内存的入口共用）
// ==========================================================================
namespace {

// 测试注入的闸门覆盖实现（默认空：走真实联机检测）
std::mutex       g_writeGateOverrideMutex;
WriteGateOverride g_writeGateOverride;

// 闸门策略快照：只由界面线程写入，工作线程只读原子量 —— 保证 worker 侧的
// 守卫回调不再触碰 AppState 的普通字段（消除跨线程读写竞争）。
struct WriteGatePolicy {
    std::atomic<DWORD> pid{0};
    std::atomic<int>   mpPolicy{1};
};
WriteGatePolicy g_writeGatePolicy;

// 经验业务上限（与 economy.cpp 的写入安全范围一致）
constexpr int32_t kXpBusinessMax = 2000000000;
// 金额安全范围（允许负值＝欠款）
constexpr int64_t kMoneyLimit = 1000000000000LL;

}  // namespace

void setWriteGateOverride(WriteGateOverride override_fn) {
    std::lock_guard<std::mutex> lock(g_writeGateOverrideMutex);
    g_writeGateOverride = std::move(override_fn);
}

void updateWriteGatePolicy(const AppState& app) {
    // 仅界面线程调用：把守卫需要的策略数据放进原子快照
    g_writeGatePolicy.pid.store(app.pid);
    g_writeGatePolicy.mpPolicy.store(app.mpPolicy);
}

namespace {

std::string writeGateReasonInternal(bool forceRefresh, bool fuelOnly = false) {
    {
        WriteGateOverride override_fn;
        {
            std::lock_guard<std::mutex> lock(g_writeGateOverrideMutex);
            override_fn = g_writeGateOverride;
        }
        if (override_fn) return override_fn();  // 锁外调用，避免持锁执行外部代码
    }
    DWORD pid = g_writeGatePolicy.pid.load();
    if (!pid) {
        std::wstring exe;
        if (!findProcess(selectedProcessName(), &pid, &exe)) {
            // 没有游戏进程：此处不拦（写入本身会以"尚未附加"失败），避免给出误导性的联机拒绝
            return std::string();
        }
    }
    const MultiplayerStatus status = checkMultiplayer(pid, forceRefresh);
    // 通用写入闸门拒绝 TruckersMP；自动油量使用独立例外。无法确认（Unknown，含模块枚举失败与
    // 读不到 game.log.txt）一律拒绝；Convoy（检测开启时）一律拒绝 —— 与联运模式策略
    // 无关（"不写内存的功能"才受 mpPolicy 影响）。
    if (fuelOnly ? status.blocksFuelWrite() : status.blocksWrite()) return status.detail;
    return std::string();
}

}  // namespace

std::string writeGateReasonFromPolicy() {
    // 任意线程可调用（含 worker 线程）：只读原子快照 + 既有全局开关
    return writeGateReasonInternal(/*forceRefresh=*/false);
}

std::string fuelWriteGateReasonFromPolicy() {
    return writeGateReasonInternal(/*forceRefresh=*/false, /*fuelOnly=*/true);
}

std::string writeGateReason(AppState& app) {
    // 界面线程的用户操作路径：先同步快照，再强制刷新一次检测结果
    updateWriteGatePolicy(app);
    return writeGateReasonInternal(/*forceRefresh=*/true);
}

bool parseXpTarget(const char* text, int32_t* out, std::string* error) {
    double value = 0.0;
    if (!parseNumber(text, false, &value)) {
        if (error) *error = "\"The experience to be changed to\" is not a valid number";
        return false;
    }
    // 先做范围判断，再转换：越界转换是未定义行为，绝不允许
    if (value < (double)INT32_MIN || value > (double)INT32_MAX) {
        if (error) *error = fmt("The target experience exceeds the 32-bit integer range (%lld ~ %lld)", (long long)INT32_MIN,
                                (long long)INT32_MAX);
        return false;
    }
    const int32_t converted = (int32_t)value;
    if ((double)converted != value) {
        if (error) *error = "Target experience must be an integer";
        return false;
    }
    if (converted < 0) {
        if (error) *error = "target experience cannot be a negative number";
        return false;
    }
    if (converted > kXpBusinessMax) {
        if (error) {
            *error = fmt("The target experience exceeds the safe upper limit (0 ~ %lld)", (long long)kXpBusinessMax);
        }
        return false;
    }
    if (out) *out = converted;
    if (error) error->clear();
    return true;
}

bool parseMoneyTarget(const char* text, int64_t* out, std::string* error) {
    double value = 0.0;
    if (!parseNumber(text, false, &value)) {
        if (error) *error = "\"The amount to be changed\" is not a valid number";
        return false;
    }
    if (value < -(double)kMoneyLimit || value > (double)kMoneyLimit) {
        if (error) {
            *error = fmt("The target amount exceeds the safe range (-%lld ~ %lld)", (long long)kMoneyLimit,
                         (long long)kMoneyLimit);
        }
        return false;
    }
    if (out) *out = (int64_t)value;
    if (error) error->clear();
    return true;
}

bool writeBankMoneyGuarded(AppState& app, uint64_t moneyAddress, uint64_t expectedBank,
                           int64_t value, std::string* error) {
    const std::string gate = writeGateReason(app);  // 写入前一刻复核联机状态
    if (!gate.empty()) {
        if (error) *error = "online protection:" + gate;
        return false;
    }
    return writeBankMoneyVerified(app.mem, moneyAddress, expectedBank, value, error);
}

bool writeXpGuarded(AppState& app, uint64_t xpAddress, uint64_t expectedEconomy, int32_t value,
                    std::string* error) {
    const std::string gate = writeGateReason(app);
    if (!gate.empty()) {
        if (error) *error = "online protection:" + gate;
        return false;
    }
    return writeEconomyExperienceVerified(app.mem, xpAddress, expectedEconomy, value, error);
}

// ==========================================================================
// 扫描 / 写入 / 锁定
// ==========================================================================
void startFirstScan(AppState& app, PanelKind kind) {
    if (app.busy.load()) {
        statusFor(app, kind) = "A scan is in progress, please wait for it to end or click \"Cancel\"";
        return;
    }
    if (!ensureAttached(app)) {
        statusFor(app, kind) = "has not been attached to the game process yet";
        return;
    }
    joinWorker(app);
    applyPending(app);  // 先消费上一轮未显示的结果，防止单槽覆盖
    // 新扫描会使旧候选地址失去语义，先解锁它们，避免后台继续写旧地址。
    if (ScanSession* previous = sessionFor(app, kind); previous && app.freezer) {
        for (uint64_t addr : previous->addresses()) app.freezer->remove(addr);
    }
    if (bool* state = lockFor(app, kind)) *state = false;
    if (kind == PanelKind::Scanner) app.scanSelection.clear();
    VType type = typeFor(app, kind);
    const bool isFloat = vtypeInfo(type).isFloat;
    const char* text = currentFor(app, kind);
    double value = 0.0;
    if (!parseNumber(text, isFloat, &value)) {
        statusFor(app, kind) = "Please fill in a valid number (for example 45678) in \"Current Value\"";
        return;
    }
    ScanOptions opt;
    opt.workers = app.workers;
    opt.maxRegionSize = (kind == PanelKind::Scanner && app.scanIncludeBig)
                            ? 0
                            : maxRegionBytes(app);
    const int alignment = (kind == PanelKind::Scanner) ? app.scanAlign : 0;
    auto session =
        std::make_unique<ScanSession>(app.mem, type, alignment, toleranceValue(app, kind));
    ScanSession* raw = session.get();
    if (kind == PanelKind::Money) app.moneySession = std::move(session);
    else if (kind == PanelKind::Xp) app.xpSession = std::move(session);
    else if (kind == PanelKind::Fuel) app.fuelSession = std::move(session);
    else if (kind == PanelKind::Damage) app.damageSession = std::move(session);
    else app.scanSession = std::move(session);

    statusFor(app, kind) =
        fmt("first scan %s = %s in progress... (about 10~25 seconds)", vtypeName(type), text);
    logLine(fmt("[%s] First scan %s = %s ...", panelName(kind), vtypeName(type), text));
    app.busy.store(true);
    app.cancel.store(false);
    app.progressDone.store(0);
    app.progressTotal.store(1);
    app.progressNote = "Scanning for the first time";
    const double scanValue = value;
    app.worker = std::thread([&app, raw, opt, scanValue, kind, type]() {
        uint64_t hits = raw->firstScan(
            scanValue, opt,
            [&app](uint64_t done, uint64_t total) {
                app.progressDone.store(done);
                app.progressTotal.store(total ? total : 1);
            },
            [&app]() { return app.cancel.load(); });
        const bool cancelled = app.cancel.load();
        std::string tip;
        if (cancelled) {
            raw->clear();
            hits = 0;
            tip = "has been canceled and incomplete candidate addresses will not be retained.";
        } else if (raw->limitReached()) {
            tip = "Too many hits (over 2 million), incomplete results discarded; please narrow the scan or avoid common values.";
        } else if (hits == 0) {
            tip = "not found. Please make sure the values ​​are correct; you can also change the values ​​in the game and scan again.";
        } else if (hits == 1) {
            tip = "has converged to 1 candidate address and can be written.";
        } else {
            tip = ": it is recommended to continue narrowing; it will be confirmed again before forcibly writing.";
        }
        std::string status = fmt("hits %llu (%s). %s", (unsigned long long)hits,
                                 vtypeName(type), tip.c_str());
        postResult(app, kind, status,
                   fmt("[%s] First scan completed: hit %llu", panelName(kind),
                       (unsigned long long)hits),
                   cancelled ? "First scan canceled" : "first scan completed");
        app.busy.store(false);
    });
}

void startNextScan(AppState& app, PanelKind kind, ScanMode mode) {
    if (app.busy.load()) {
        statusFor(app, kind) = "is already scanning in progress";
        return;
    }
    ScanSession* session = sessionFor(app, kind);
    if (!session || session->empty()) {
        statusFor(app, kind) = "Please do a \"first scan\" first";
        return;
    }
    joinWorker(app);
    applyPending(app);  // 先消费上一轮未显示的结果
    if (kind == PanelKind::Scanner) app.scanSelection.clear();
    const bool isFloat = vtypeInfo(session->type()).isFloat;
    const char* text = currentFor(app, kind);
    double value = 0.0;
    if (mode == ScanMode::Exact && !parseNumber(text, isFloat, &value)) {
        statusFor(app, kind) = "Please first fill in the new value in the game in \"Current Value\"";
        return;
    }
    app.busy.store(true);
    app.cancel.store(false);
    app.progressDone.store(0);
    app.progressTotal.store(1);
    app.progressNote = "is narrowing";
    app.worker = std::thread([&app, session, mode, value, kind]() {
        uint64_t hits = session->nextScan(
            mode, value,
            [&app](uint64_t done, uint64_t total) {
                app.progressDone.store(done);
                app.progressTotal.store(total ? total : 1);
            },
            [&app]() { return app.cancel.load(); });
        const bool cancelled = app.cancel.load();
        std::string status = cancelled
                                 ? "narrowing has been cancelled, and the original candidate address has been retained."
                                 : fmt("remains at %llu after narrowing.", (unsigned long long)hits);
        if (!cancelled && hits == 0) status += "The value may have changed. It is recommended to scan again.";
        else if (!cancelled && hits == 1) status += "has converged to 1 candidate address.";
        else if (!cancelled && hits > 1) status += "It is recommended to continue narrowing.";
        postResult(app, kind, status,
                   fmt("[%s] narrows: remaining at %llu", panelName(kind), (unsigned long long)hits),
                   cancelled ? "narrowing has been cancelled" : "narrowing completed");
        app.busy.store(false);
    });
}

void writePanelValues(AppState& app, PanelKind kind, bool confirmed) {
    if (app.busy.load()) {
        statusFor(app, kind) = "Please wait until the current operation is completed";
        return;
    }
    ScanSession* session = sessionFor(app, kind);
    if (!session || session->empty()) {
        statusFor(app, kind) = "Please scan the address first and then write it";
        return;
    }
    const bool isFloat = vtypeInfo(session->type()).isFloat;
    const char* text = targetFor(app, kind);
    double value = 0.0;
    if (!parseNumber(text, isFloat, &value)) {
        statusFor(app, kind) = "Enter the target value in New value";
        return;
    }
    const size_t total = session->count();
    if (total > 1 && !confirmed) {
        app.pendingMultiAction = 1;
        app.pendingMultiKind = (int)kind;
        app.requestMultiConfirm = true;
        statusFor(app, kind) =
            fmt("%d candidate addresses remain. Narrow the results or confirm a batch write.",
                (int)total);
        return;
    }
    const size_t written =
        isFloat ? session->writeAllNumber(value) : session->writeAllInt((int64_t)value);
    statusFor(app, kind) = fmt("Wrote %d/%d addresses = %s (applied in game)", (int)written,
                               (int)total, text);
    logLine(fmt("[%s] Write %s: %d/%d succeeded", panelName(kind), text, (int)written, (int)total));
    bool* lockState = lockFor(app, kind);
    const bool locked = lockState && *lockState;
    if (locked && app.freezer) {
        for (uint64_t addr : session->addresses()) {
            app.freezer->add(addr, session->type(), value);
        }
    }
}

void togglePanelLock(AppState& app, PanelKind kind, bool lock, bool confirmed) {
    ScanSession* session = sessionFor(app, kind);
    if (!session || session->empty() || !app.freezer) {
        if (bool* state = lockFor(app, kind)) *state = false;
        statusFor(app, kind) = "Scan for an address before freezing";
        return;
    }
    if (!lock) {
        for (uint64_t addr : session->addresses()) app.freezer->remove(addr);
        statusFor(app, kind) = "Values unfrozen.";
        logLine(fmt("[%s] Values unfrozen", panelName(kind)));
        return;
    }
    if (session->count() > 1 && !confirmed) {
        if (bool* state = lockFor(app, kind)) *state = false;
        app.pendingMultiAction = 2;
        app.pendingMultiKind = (int)kind;
        app.requestMultiConfirm = true;
        statusFor(app, kind) =
            fmt("%d candidate addresses remain; confirm before freezing all.", (int)session->count());
        return;
    }
    const bool isFloat = vtypeInfo(session->type()).isFloat;
    const char* text = targetFor(app, kind);
    double value = 0.0;
    if (!parseNumber(text, isFloat, &value)) {
        if (bool* state = lockFor(app, kind)) *state = false;
        statusFor(app, kind) = "Enter a target value before freezing";
        return;
    }
    for (uint64_t addr : session->addresses()) app.freezer->add(addr, session->type(), value);
    statusFor(app, kind) = fmt("locked %d addresses = %s (writeback every 0.25 seconds)",
                               (int)session->count(), text);
    logLine(fmt("[%s] %d addresses locked", panelName(kind), (int)session->count()));
}

// ==========================================================================
// 游戏内功能（控制台命令）
// ==========================================================================
void sendConsole(AppState& app, const std::string& command) {
    if (command.empty()) return;
    if (app.busy.load()) {
        app.consoleNote = "Please wait for the current scan to end or cancel the scan.";
        return;
    }
    const std::string gate = multiplayerGateForSafeFeature(app);
    if (!gate.empty()) {
        app.consoleNote = gate;
        app.teleportNote = gate;
        logLine("console command is intercepted:" + gate);
        return;
    }
    ::strncpy_s(app.consoleCmd, command.c_str(), _TRUNCATE);
    const int vk = app.consoleKey;
    DWORD pid = app.pid;
    if (!pid || !app.mem.alive()) {
        std::wstring exe;
        if (!findProcess(selectedProcessName(), &pid, &exe)) {
            app.consoleNote = fmt("%s not found, please start the game first.", W2U(selectedProcessName()).c_str());
            return;
        }
    }
    logLine("is sending commands to the game:" + command);
    joinWorker(app);
    // 上一个工作任务可能已完成但结果尚未在 renderApp 消费，先交付再复用单槽。
    applyPending(app);
    app.busy.store(true);
    app.progressDone.store(0);
    app.progressTotal.store(1);
    app.progressNote = "is sending console command";
    app.worker = std::thread([&app, command, vk, pid]() {
        SendResult result = sendConsoleCommand(command, vk, pid, true);
        {
            std::lock_guard<std::mutex> lock(app.resultMutex);
            app.pendingLog = result.message;
            app.pendingStatus.clear();
            app.pendingProgressNote = result.ok ? "console command sent" : "Console command failed to send";
            app.pendingKind = (int)PanelKind::Money;
            app.pendingReady = true;
        }
        app.progressDone.store(1);
        app.busy.store(false);
    });
}

void handleGlobalHotkeys(AppState& app) {
    if (!app.hotkeysEnabled) return;
    // 焦点在任意文本输入框（游戏内或修改器内）时不响应，避免误触发
    if (::ImGui::GetIO().WantTextInput) return;

    static DWORD lastPressTime = 0;
    DWORD now = ::GetTickCount();
    if (now - lastPressTime < 300) return; // 防抖

    bool ctrlDown = (::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    if (!ctrlDown) return;

    // 边沿触发：每帧无条件刷新按键状态缓存，只有"松开→按下"的那一帧才触发，
    // 长按不会反复翻转开关
    static const int kHotkeys[] = {VK_F1, VK_F2, VK_F3, VK_F4, VK_F5, VK_F6, VK_BACK};
    static bool prevDown[sizeof(kHotkeys) / sizeof(kHotkeys[0])] = {};
    int fired = -1;
    for (size_t i = 0; i < sizeof(kHotkeys) / sizeof(kHotkeys[0]); ++i) {
        const bool down = (::GetAsyncKeyState(kHotkeys[i]) & 0x8000) != 0;
        if (down && !prevDown[i] && fired < 0) fired = (int)i;  // 每帧最多触发一个
        prevDown[i] = down;
    }
    if (fired < 0) return;
    lastPressTime = now;

    switch (kHotkeys[fired]) {
    case VK_F1:  // Ctrl + F1: 传送到导航终点（游戏内再按 Ctrl+F9 落车）
        teleportTo(app, "nav_end");
        logLine("[Hotkey] Teleport to the navigation end point (goto nav_end); press Ctrl+F9 to exit the vehicle after arriving at the location");
        break;
    case VK_F2:  // Ctrl + F2: 切换无限油量
        toggleAutoVehicleLock(app, true, !app.autoFuelLock);
        logLine(std::string("[Hotkey] Unlimited fuel volume: ") + (app.autoFuelLock ? "on" : "Close"));
        break;
    case VK_F6:  // Ctrl + F6: 切换无限电量
        toggleAutoVehicleLock(app, true, !app.autoElectricLock, true);
        logLine(std::string("[Hotkey] Unlimited battery: ") + (app.autoElectricLock ? "on" : "Close"));
        break;
    case VK_F3:  // Ctrl + F3: 切换车辆无损
        toggleAutoVehicleLock(app, false, !app.autoDamageLock);
        logLine(std::string("[Hotkey] Vehicle intact: ") + (app.autoDamageLock ? "on" : "Close"));
        break;
    case VK_F4:  // Ctrl + F4: 切换发动机动力
    {
        int nextOpt = (app.enginePowerOption == 0) ? 3 : 0;
        applyEnginePower(app, nextOpt, nextOpt > 0);
        logLine(std::string("[Hotkey] Engine power switch is: ") + (nextOpt == 0 ? "Stock" : "2.0x Cyclonus"));
        break;
    }
    case VK_F5:  // Ctrl + F5: 快速把游戏时间设为 09:00
        sendConsole(app, "g_set_time 9 0");
        logLine("[Hotkey] Game time has been set to 09:00");
        break;
    case VK_BACK:  // Ctrl + Backspace: 撤销传送
        teleportTo(app, "back");
        logLine("[Hotkey] Undo transmission (goto back)");
        break;
    default:
        break;
    }
}

void syncAutoVehicleLocks(AppState& app) {
    if (!app.vehicleLocker) return;
    const auto commands = app.vehicleLocker->commands();
    const auto status = app.vehicleLocker->status();
    if (app.autoFuelLock) app.fuelStatus = status;
    if (app.autoElectricLock) app.electricStatus = status;
    if (app.autoDamageLock) app.damageStatus = status;
    app.autoFuelLock = commands.fuel && !commands.electric;
    app.autoElectricLock = commands.fuel && commands.electric;
    app.autoDamageLock = commands.damage;
}

void toggleAutoVehicleLock(AppState& app, bool fuel, bool enabled, bool electric) {
    bool& state = fuel ? (electric ? app.autoElectricLock : app.autoFuelLock) : app.autoDamageLock;
    std::string& status = fuel ? (electric ? app.electricStatus : app.fuelStatus) : app.damageStatus;
    if (!enabled) {
        if (app.vehicleLocker) {
            std::string ignored;
            if (fuel) app.vehicleLocker->setFuelEnabled(false, &ignored);
            else app.vehicleLocker->setDamageEnabled(false, &ignored);
        }
        state = false;
        if (fuel) app.autoFuelLock = app.autoElectricLock = false;
        status = fuel ? (electric ? "unlimited power is turned off." : "Unlimited fuel is turned off.") : "Lossless locking is turned off.";
        return;
    }
    if (const std::string reason = structuralWriteBlockReason(); !reason.empty()) {
        state = false;
        status = reason;
        logLine(reason);
        return;
    }
    if (!ensureAttached(app)) {
        state = false;
        status = "has not been attached to the game process yet";
        return;
    }
    updateWriteGatePolicy(app);
    if (!fuel && !app.mpHits.empty()) {
        state = false;
        status = "The online feature has been detected and vehicle locking has been refused; please only use it on a single-player.";
        return;
    }
    {
        MultiplayerStatus mp = checkMultiplayer(app.pid, true);
        app.mpBlocks = mp.blocksWrite();
        app.mpDetail = mp.detail;
        if (fuel ? mp.blocksFuelWrite() : mp.blocksWrite()) {
            state = false;
            status = "The current vehicle lock is intercepted by online protection:" + mp.detail;
            logLine(status);
            return;
        }
    }
    bool other = app.autoFuelLock || app.autoElectricLock || app.autoDamageLock;
    if (!app.vehicleLocker) app.vehicleLocker = std::make_unique<VehicleLocker>();
    // 写入闸门：油量/无损的后台周期在真正写入前都会调用它（联机保护）。
    // 不捕获 AppState：worker 线程只读线程安全的策略快照。
    app.vehicleLocker->setWriteGuard([]() { return writeGateReasonFromPolicy(); });
    app.vehicleLocker->setFuelWriteGuard([]() { return fuelWriteGateReasonFromPolicy(); });
    std::string error;
    // 已有另一项锁定时沿用同一条固定指针链；每轮仍会读取当前车辆并做遥测校验。
    if (!other) {
        if (!app.vehicleLocker->bind(&app.mem, app.pid, &error)) {
            state = false;
            status = "Automatic positioning failed:" + error;
            logLine(status);
            return;
        }
    }
    bool ok = fuel ? app.vehicleLocker->setFuelEnabled(true, &error, electric)
                   : app.vehicleLocker->setDamageEnabled(true, &error);
    syncAutoVehicleLocks(app);
    state = ok;
    if (fuel && ok) {
        app.autoFuelLock = !electric;
        app.autoElectricLock = electric;
        if (electric) app.fuelStatus = "Unlimited fuel is turned off (switched to unlimited power).";
        else app.electricStatus = "Unlimited power is turned off (switched to unlimited fuel).";
    }
    status = ok ? app.vehicleLocker->status() : ("Automatic positioning security check failed:" + error);
    logLine(status);
}


// ---------- 发动机动力调节 ----------
namespace {

constexpr float kEngineScales[] = {1.0f, 1.25f, 1.50f, 2.00f};

// 创建（并按需恢复上次运行的记录）动力调节器。
EngineTuner& ensureEngineTuner(AppState& app) {
    if (!app.engineTuner) {
        app.engineTuner = std::make_unique<EngineTuner>();
        if (!app.engineStateText.empty()) app.engineTuner->importState(app.engineStateText);
    }
    return *app.engineTuner;
}

// 运行期间每 5 秒复查一次联机状态；命中即让保持线程恢复原值并停止。
EngineTuner::Guard engineGuardFor(DWORD pid) {
    (void)pid;
    // 与油量/无损/经济写入共用同一套写入闸门（线程安全快照）
    return [](std::string* reason) {
        const std::string blocked = writeGateReasonFromPolicy();
        if (!blocked.empty()) {
            if (reason) *reason = blocked;
            return false;
        }
        return true;
    };
}

std::string multiplayerSummary() {
    if (!isConvoyCheckEnabled()) {
        return "Online protection: TruckersMP only releases automatic unlimited fuel/power (intermodal detection is turned off).";
    }
    return "online protection: TruckersMP only releases automatic unlimited fuel/power; official Convoy and unconfirmed status refuse to write.";
}

}  // namespace

void applyEnginePower(AppState& app, int option, bool raiseLimit) {
    if (!supportsVerifiedStructures(selectedGame())) {
        app.enginePowerOption = 0;
        app.engineRaiseLimit = false;
        app.engineStatus = structuralWriteBlockReason();
        logLine(app.engineStatus);
        return;
    }
    const int index = (option < 0 || option > 3) ? 0 : option;
    EngineTuner& tuner = ensureEngineTuner(app);

    if (index == 0 && !raiseLimit) {
        std::string error;
        const bool ok = tuner.release(&error);
        app.enginePowerOption = 0;
        app.engineRaiseLimit = false;
        app.engineStateText = tuner.exportState();
        app.engineStatus = ok ? "has restored its original power" : ("Power adjustment is turned off (" + error + "）");
        saveSettings(app);
        logLine(app.engineStatus);
        return;
    }

    if (!ensureAttached(app)) {
        app.enginePowerOption = 0;
        app.engineRaiseLimit = false;
        app.engineStatus = "has not been attached to the game process yet";
        return;
    }
    // 用户操作前用最新状态复核联机（不依赖缓存）
    MultiplayerStatus mp = checkMultiplayer(app.pid, true);
    app.mpBlocks = mp.blocksWrite();
    app.mpDetail = mp.detail;
    if (mp.blocksWrite()) {
        app.enginePowerOption = 0;
        app.engineRaiseLimit = false;
        app.engineStatus = "has refused to modify the engine data:" + mp.detail;
        logLine(app.engineStatus);
        return;
    }

    const bool rebind = !tuner.bound();
    if (rebind) {
        // 只有在没有绑定时才重新绑定，避免丢掉旧对象的原值记录
        tuner.attach(std::make_unique<ProcessMemoryAdapter>(&app.mem),
                     std::make_unique<GameEngineResolver>(&app.mem, app.pid),
                     engineGuardFor(app.pid));
    }
    std::string error;
    const bool ok = tuner.apply(kEngineScales[index], raiseLimit, &error);    app.enginePowerOption = ok ? index : 0;
    app.engineRaiseLimit = ok ? raiseLimit : false;
    app.engineStateText = tuner.exportState();
    app.engineStatus = ok ? tuner.status() : ("Power adjustment failed:" + error);
    if (ok && mp.kind == MultiplayerKind::Unknown) {
        app.engineStatus += "; Note: Unable to confirm whether it is in the official Convoy";
    }
    saveSettings(app);
    logLine(app.engineStatus);
}

void detectConsoleKey(AppState& app) {
    std::vector<SaveSlot> slots = app.slots.empty() ? listSlots() : app.slots;
    if (slots.empty()) {
        app.consoleNote = fmt("The profile of %s was not found and the keys could not be automatically recognized.\nThere is no `mix console` binding in", selectedGameDescriptor().shortName);
        return;
    }
    int vk = consoleKeyFromControls(slots.front().profileDir + L"\\controls.sii");
    if (!vk) {
        app.consoleNote = "controls.sii, so the default ~` key is used.";
        return;
    }
    app.consoleKey = vk;
    app.consoleNote = fmt("Console key recognized: 0x%02X", vk);
    logLine(app.consoleNote);
    saveSettings(app);
}

void checkConsoleConfig(AppState& app) {
    DWORD runningPid = 0;
    std::wstring runningExe;
    if (findProcess(selectedProcessName(), &runningPid, &runningExe)) {
        app.consoleNote =
            "Please exit the game completely first: config.cfg will be rewritten when the game exits, and modifications during operation will be overwritten (without any changes).";
        logLine(app.consoleNote);
        return;
    }
    std::string text, error;
    if (!readGameConfigText(&text, &error)) {
        app.consoleNote = error;
        return;
    }
    bool changed = false, any = false;
    rewriteConfigKey(&text, "g_console", "1", &changed);
    any |= changed;
    rewriteConfigKey(&text, "g_developer", "1", &changed);
    any |= changed;
    if (!any) {
        app.consoleNote = "config.cfg check: g_console=1, g_developer=1, console is available.";
        logLine(app.consoleNote);
        return;
    }
    std::wstring backup;
    if (!writeGameConfigText(text, &error, &backup)) {
        app.consoleNote = error;
        logLine(app.consoleNote);
        return;
    }
    app.consoleNote = "has been written into config.cfg (the original file backup is config.cfg.trainer.bak), and it will take effect after restarting the game.";
    logLine(app.consoleNote);
    refreshFlyMode(app);
}

// ==========================================================================
// 飞行模式（自由相机）
// ==========================================================================
namespace {

std::string keyLabel(int vk) {
    if (vk >= '0' && vk <= '9') return fmt("numeric key %c", (char)vk);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return fmt("small keyboard %c", (char)('0' + (vk - VK_NUMPAD0)));
    for (const auto& option : consoleKeyOptions()) {
        if (option.vk == vk) return option.label;
    }
    return fmt("0x%02X", vk);
}

}  // namespace

void refreshFlyMode(AppState& app) {
    std::string text, error;
    if (!readGameConfigText(&text, &error)) {
        app.flyDeveloper = -1;
        app.flyConsole = -1;
        app.flySpeedSaved.clear();
        app.flyNote = error;
        return;
    }
    std::string value;
    app.flyDeveloper = readConfigKey(text, "g_developer", &value) ? ::atoi(value.c_str()) : -1;
    app.flyConsole = readConfigKey(text, "g_console", &value) ? ::atoi(value.c_str()) : -1;
    const bool hasSpeed = readConfigKey(text, "g_flyspeed", &value);
    app.flySpeedSaved = hasSpeed ? value : std::string("(not set)\nThere is no g_flyspeed in");
    if (!hasSpeed) {
        app.flyNote = "config.cfg. Click \"Write Flight Mode Settings\" to fill it in.";
    } else if (app.flyDeveloper == 1 && app.flyConsole == 1) {
        app.flyNote = fmt("config.cfg is ready: g_developer=1, g_console=1, g_flyspeed=%s.",
                          app.flySpeedSaved.c_str());
    } else {
        app.flyNote = fmt("config.cfg：g_developer=%d、g_console=%d、g_flyspeed=%s；"
                          "needs to be turned on before you can use the free camera.",
                          app.flyDeveloper, app.flyConsole, app.flySpeedSaved.c_str());
    }
    if (hasSpeed) {
        ::strncpy_s(app.flySpeed, value.c_str(), _TRUNCATE);
    }
}

void detectCameraKeys(AppState& app) {
    std::vector<SaveSlot> slots = app.slots.empty() ? listSlots() : app.slots;
    if (slots.empty()) {
        app.flyNote = fmt("The profile of %s was not found and the camera button could not be recognized.\nThe mix cam* binding was not found in", selectedGameDescriptor().shortName);
        return;
    }
    CameraBindings bindings;
    if (!cameraKeyBindings(slots.front().profileDir + L"\\controls.sii", &bindings)) {
        app.flyNote = "controls.sii (default is used: numeric key 0 switch, small keyboard 8/5/4/6/9/3)";
        return;
    }
    app.camera = bindings;
    app.cameraLoaded = true;
    app.flyNote = fmt("Recognized camera buttons: switch %s, front and rear %s/%s, left and right %s/%s, up and down %s/%s",
                      keyLabel(bindings.toggle).c_str(), keyLabel(bindings.forward).c_str(),
                      keyLabel(bindings.back).c_str(), keyLabel(bindings.left).c_str(),
                      keyLabel(bindings.right).c_str(), keyLabel(bindings.up).c_str(),
                      keyLabel(bindings.down).c_str());
    logLine(app.flyNote);
}

void enableFlyMode(AppState& app) {
    DWORD runningPid = 0;
    std::wstring runningExe;
    if (findProcess(selectedProcessName(), &runningPid, &runningExe)) {
        app.flyNote = "Please exit the game completely first: config.cfg will be rewritten when the game exits, and modifications during operation will be overwritten (without any changes).";
        logLine(app.flyNote);
        return;
    }
    std::string text, error;
    if (!readGameConfigText(&text, &error)) {
        app.flyNote = error;
        return;
    }
    const std::string speed = app.flySpeed[0] ? app.flySpeed : "100";
    bool changed = false, any = false;
    rewriteConfigKey(&text, "g_developer", "1", &changed);
    any |= changed;
    rewriteConfigKey(&text, "g_console", "1", &changed);
    any |= changed;
    rewriteConfigKey(&text, "g_flyspeed", speed, &changed);
    any |= changed;
    if (!any) {
        app.flyNote = "config.cfg already meets the flight mode requirements and does not need to be modified.";
        refreshFlyMode(app);
        return;
    }
    std::wstring backup;
    if (!writeGameConfigText(text, &error, &backup)) {
        app.flyNote = error;
        logLine(app.flyNote);
        return;
    }
    app.flyNote = fmt("has been written to config.cfg (backup %s). Restart the game and enter the driving interface: press %s to switch the free camera on and off.",
                      W2U(backup).c_str(),
                      app.cameraLoaded ? keyLabel(app.camera.toggle).c_str() : "numeric key 0");
    logLine(app.flyNote);
    refreshFlyMode(app);
}

void setFlySpeed(AppState& app, const char* value) {
    if (!value || !value[0]) return;
    ::strncpy_s(app.flySpeed, value, _TRUNCATE);
    sendConsole(app, std::string("g_flyspeed ") + value);
}

void toggleFreeCamera(AppState& app) {
    const std::string gate = multiplayerGateForSafeFeature(app);
    if (!gate.empty()) {
        app.flyNote = gate;
        logLine("free camera intercepted:" + gate);
        return;
    }
    DWORD pid = app.pid;
    if (!pid) {
        std::wstring exe;
        if (!findProcess(selectedProcessName(), &pid, &exe)) {
            app.flyNote = fmt("%s not found, please start the game first.", W2U(selectedProcessName()).c_str());
            return;
        }
    }
    const int vk = app.cameraLoaded && app.camera.toggle ? app.camera.toggle : '0';
    const SendResult result = sendKeyTap(vk, pid);
    app.flyNote = result.message + (result.ok ? "(free camera switch)" : "");
    logLine(app.flyNote);
}

// ==========================================================================
// 存档工具
// ==========================================================================
void clearSaveSlotSelection(AppState& app) {
    app.selectedSlot=-1;app.slotValues={};
    app.saveMoney[0]=0;app.saveXp[0]=0;
}

void selectSaveProfile(AppState& app,int index) {
    if(index<0 || index>=(int)app.saveProfiles.size()) return;
    if(index==app.selectedSaveProfile) return;
    app.selectedSaveProfile=index;
    clearSaveSlotSelection(app);
    app.showCopyDialog=false;app.copyRequest={};
    app.saveNote="has switched to the file \""+app.saveProfiles[index].name+"\". Please select an archive for this file.";
}

void selectSlot(AppState& app,int index);

void refreshSaves(AppState& app) {
    std::wstring profileDir,slotDir;bool cloud=false;
    if(app.selectedSaveProfile>=0 && app.selectedSaveProfile<(int)app.saveProfiles.size()) {
        const auto& profile=app.saveProfiles[app.selectedSaveProfile];
        profileDir=profile.profileDir;cloud=profile.cloud;
    }
    if(app.selectedSlot>=0 && app.selectedSlot<(int)app.slots.size()) {
        const auto& slot=app.slots[app.selectedSlot];
        profileDir=slot.profileDir;cloud=slot.cloud;slotDir=slot.slotDir;
    }
    app.slots = listSlots();
    app.saveProfiles=groupSaveProfiles(app.slots);
    const auto selection=restoreSaveProfileSelection(app.slots,app.saveProfiles,profileDir,cloud,slotDir);
    app.selectedSaveProfile=selection.profile;
    clearSaveSlotSelection(app);
    app.backups = listBackups();
    int scs = 0, text = 0;
    for (const auto& s : app.slots) {
        if (s.format == "scs") ++scs;
        else if (s.format == "text") ++text;
    }
    app.saveNote = fmt("Found a total of %d files and %d archive slots (%d encrypted, %d plaintext). Select the file first, then the archive.",
                       (int)app.saveProfiles.size(),(int)app.slots.size(), scs, text);
    if(selection.slot>=0) selectSlot(app,selection.slot);
    logLine(fmt("archive list has been refreshed, with a total of %d archive slots", (int)app.slots.size()));
}

void selectSlot(AppState& app, int index) {
    if (index < 0 || index >= (int)app.slots.size()) return;
    app.selectedSlot = index;
    app.selectedSaveProfile=saveProfileForSlot(app.saveProfiles,index);
    const SaveSlot& slot = app.slots[(size_t)index];
    app.slotValues = {};
    app.saveMoney[0]=0;app.saveXp[0]=0;
    if (slot.format != "text") {
        app.saveMoney[0] = 0;
        app.saveXp[0] = 0;
        app.saveNote = fmt("%s is the %s format. This tool does not write encrypted/binary archives; if you want to change the money, please go to \"Real-time Money Change\""
                           "Change the memory and let the game save automatically.",
                           slot.label.c_str(), slot.formatText.c_str());
        return;
    }
    TextValues values;
    std::string err;
    if (readTextValues(slot.gameSii, &values, &err)) {
        app.slotValues = values;
        if (values.hasMoney) ::strncpy_s(app.saveMoney, formatInt(values.money).c_str(), _TRUNCATE);
        if (values.hasExperience) {
            ::strncpy_s(app.saveXp, formatInt(values.experience).c_str(), _TRUNCATE);
        }
        app.saveNote = fmt("%s: Clear text archive, read cash=%s, experience=%s. After changing, click \"Write to clear text archive\".",
                           slot.label.c_str(),
                           values.hasMoney ? formatInt(values.money).c_str() : "None",
                           values.hasExperience ? formatInt(values.experience).c_str() : "None");
    } else {
        app.saveNote = fmt("%s: Failed to read value (%s)", slot.label.c_str(), err.c_str());
    }
}

void consumePendingCopy(AppState& app) {
    SaveCopyResult result;
    bool truck = false, home = false, trailer = false, fleet = false;
    {
        std::lock_guard<std::mutex> lock(app.resultMutex);
        if (!app.pendingCopyReady) return;
        result = std::move(app.pendingCopy);
        truck = app.pendingCopyTruck;
        home = app.pendingCopyHome;
        trailer = app.pendingCopyTrailer;
        fleet = app.pendingCopyFleet;
        app.pendingCopyTruck = false;
        app.pendingCopyHome = false;
        app.pendingCopyTrailer = false;
        app.pendingCopyFleet = false;
        app.pendingCopyReady = false;
    }
    if (result.game != selectedGame()) return;
    if (result.ok) {
        refreshSaves(app);
        for (int i = 0; i < (int)app.slots.size(); ++i)
            if (::_wcsicmp(app.slots[i].slotDir.c_str(), result.slotDir.c_str()) == 0) { selectSlot(app, i); break; }
        app.saveNote = std::string(fleet ? "fleet has been expanded and saved as \"" : trailer ? "trailer has been replaced and saved as \"" : home ? "Home accessories have been modified and saved as \"" : truck ? "truck has been imported into the new archive \"" : "has been copied as \"") + result.displayName + "\", new slot:" + W2U(result.slotName) +
            ". After launching the game, check the loading list of the same file; for cloud saves, please wait for Steam synchronization to complete.";
        if(fleet) app.saveNote += " Please check the working status and income settlement of the newly added driver in the game.";
        if(!result.note.empty())app.saveNote+=" "+result.note;
    } else app.saveNote = std::string(fleet ? "Fleet expansion failed:" : trailer ? "Failed to replace trailer:" : home ? "Home accessory modification failed:" : truck ? "Vehicle import failed:" : "Copy failed:") + result.error;
    logLine(app.saveNote);
}

void consumePendingSplit(AppState& app) {
    ProfileSplitResult result;
    {
        std::lock_guard<std::mutex> lock(app.resultMutex);
        if (!app.pendingSplitReady) return;
        result = std::move(app.pendingSplit); app.pendingSplitReady = false;
    }
    if (result.game != selectedGame()) return;
    if (!result.created.empty()) refreshSaves(app);
    std::string names;
    for (const auto& profile : result.created) {
        if (!names.empty()) names += "、";
        names += "「" + profile.name + "」";
    }
    if (result.ok) app.saveNote = "An independent local file has been generated:" + names +
        ". After launching the game, select the corresponding file, check the Mod Manager, and then load the saved save.";
    else app.saveNote = "split is not fully completed:" + result.error +
        (names.empty() ? ". No new file generated." : ". Generated and retained:" + names + "; Please change the names of other items and try again.");
    logLine(app.saveNote);
}

void openProfileSplit(AppState& app) {
    if (app.busy || app.selectedSlot < 0 || app.selectedSlot >= (int)app.slots.size()) {
        app.saveNote = "Please select an archive of the TMP, ProMods or intermodal file first"; return;
    }
    std::string error;
    if (copyBlockedByRunningGame(selectedGame(), &error)) { app.saveNote = error; return; }
    const auto source = app.slots[app.selectedSlot];
    app.splitRows.clear(); app.splitGame = selectedGame(); app.splitUiFixture = false;
    for (const auto& slot : app.slots) if (::_wcsicmp(slot.profileDir.c_str(), source.profileDir.c_str()) == 0) {
        ProfileSplitRow row; row.source = slot;
        row.selected = ::_wcsicmp(slot.slotDir.c_str(), source.slotDir.c_str()) == 0;
        const auto name = slot.displayName.empty() ? W2U(slot.profileName) + "-" + W2U(slot.slotName) : slot.displayName;
        ::strncpy_s(row.name, name.c_str(), _TRUNCATE); app.splitRows.push_back(row);
    }
    app.showProfileSplit = true;
}

void startProfileSplit(AppState& app) {
    if (app.busy || app.splitUiFixture) return;
    joinWorker(app); consumePendingCopy(app); consumePendingSplit(app);
    ProfileSplitRequest request; request.game = app.splitGame;
    for (const auto& row : app.splitRows) if (row.selected) request.items.push_back({row.source, row.name});
    app.busy = true; app.cancel = false; app.saveNote = "is preparing an independent archive and verifying the complete archive...";
    app.worker = std::thread([&app, request] {
        ProfileSplitResult result;
        try { result = splitSelectedProfiles(request, [&app] { return app.cancel.load(); }); }
        catch (const std::exception& e) { result.game = request.game; result.error = e.what(); }
        { std::lock_guard<std::mutex> lock(app.resultMutex);
          app.pendingSplit = std::move(result); app.pendingSplitReady = true; }
        app.busy = false;
    });
}

void renderProfileSplitDialog(AppState& app) {
    if (app.showProfileSplit) { ::ImGui::OpenPopup("split into independent file ##split"); app.showProfileSplit = false; }
    ::ImGui::SetNextWindowSize(ImVec2(780, 0), ImGuiCond_Appearing);
    if (!::ImGui::BeginPopupModal("split into independent file ##split", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    if (app.splitRows.empty() || app.splitGame != selectedGame()) {
        ::ImGui::CloseCurrentPopup(); ::ImGui::EndPopup(); return;
    }
    ::ImGui::Text("%s / Source file: %s", gameDescriptor(app.splitGame).shortName,
                  W2U(app.splitRows.front().source.profileName).c_str());
    ::ImGui::TextWrapped("Check the archives that need to be separated, and fill in a different new file name for each archive (such as TMP, ProMods, Intermodal). Select up to 16.");
    ::ImGui::TextWrapped("Each new archive retains only the selected archive, inheriting current settings, controller and module configurations. Historical module order cannot be automatically restored; please check the module manager before loading for the first time.");
    ::ImGui::TextWrapped("is generated to local profiles; encryption progress is supported and the original files are retained. Local controller settings from Steam Cloud sources are copied along.");
    ::ImGui::BeginChild("##split-list", ImVec2(0, 270), ImGuiChildFlags_Borders);
    if (::ImGui::BeginTable("##split-rows", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ::ImGui::TableSetupColumn("Select", ImGuiTableColumnFlags_WidthFixed, 45);
        ::ImGui::TableSetupColumn("slot", ImGuiTableColumnFlags_WidthFixed, 105);
        ::ImGui::TableSetupColumn("archive name"); ::ImGui::TableSetupColumn("New file name"); ::ImGui::TableHeadersRow();
        for (int i = 0; i < (int)app.splitRows.size(); ++i) {
            auto& row = app.splitRows[i]; ::ImGui::PushID(i); ::ImGui::TableNextRow();
            ::ImGui::TableSetColumnIndex(0); ::ImGui::Checkbox("##enabled", &row.selected);
            ::ImGui::TableSetColumnIndex(1); ::ImGui::TextUnformatted(W2U(row.source.slotName).c_str());
            ::ImGui::TableSetColumnIndex(2); ::ImGui::TextUnformatted(row.source.displayName.c_str());
            ::ImGui::TableSetColumnIndex(3); ::ImGui::BeginDisabled(!row.selected);
            ::ImGui::SetNextItemWidth(-1); ::ImGui::InputText("##name", row.name, sizeof(row.name));
            ::ImGui::EndDisabled(); ::ImGui::PopID();
        }
        ::ImGui::EndTable();
    }
    ::ImGui::EndChild();
    size_t count = 0; std::set<std::wstring> names; std::string error;
    for (const auto& row : app.splitRows) if (row.selected) {
        ++count; std::wstring folder; std::string why;
        if (!profileFolderName(row.name, &folder, &why)) { if (error.empty()) error = why; }
        else if (!names.insert(folder).second) error = "The new file name cannot be repeated";
    }
    if (!count || count > 16) error = "Please select 1 to 16 archives";
    ::ImGui::Text("will create %zu independent files\nFiles with the same name as", count);
    if (!error.empty()) ::ImGui::TextWrapped("%s", error.c_str());
    ::ImGui::TextDisabled("will be refused to be overwritten. If it fails halfway, the generated files will be listed as a result.");
    ::ImGui::BeginDisabled(!error.empty() || app.splitUiFixture);
    if (::ImGui::Button("generate independent files")) { startProfileSplit(app); ::ImGui::CloseCurrentPopup(); }
    ::ImGui::EndDisabled(); ::ImGui::SameLine();
    if (::ImGui::Button("Cancel")) ::ImGui::CloseCurrentPopup();
    ::ImGui::EndPopup();
}

void startSaveCopy(AppState& app) {
    if (app.busy) return;
    joinWorker(app);
    consumePendingCopy(app);
    SaveCopyRequest request = app.copyRequest;
    request.displayName = app.copyName;
    app.cancel = false;
    app.busy = true;
    app.saveNote = "Copying and verifying archive files...";
    app.worker = std::thread([&app, request] {
        SaveCopyResult result;
        try { result = copySlotToNewSave(request, [&app] { return app.cancel.load(); }); }
        catch (const std::exception& e) { result.game = request.game; result.error = e.what(); }
        {
            std::lock_guard<std::mutex> lock(app.resultMutex);
            app.pendingCopy = std::move(result);
            app.pendingCopyTruck = false;
            app.pendingCopyReady = true;
        }
        app.busy = false;
    });
}


#include "fleet_ui.inc"

void consumePendingTransferInspection(AppState& app) {
    PendingTransferInspection result;
    {
        std::lock_guard<std::mutex> lock(app.resultMutex);
        if(!app.pendingTransferInspection.ready) return;
        result=std::move(app.pendingTransferInspection);
        app.pendingTransferInspection={};
    }
    if(result.game!=selectedGame() || result.generation!=app.transferInspectionGeneration) return;
    app.transferInspectBusy=false;
    app.progressDone=1;
    if(result.target) {
        app.transferTargetReady=result.ok;
        app.transferTargetInventory=result.ok ? std::move(result.inventory) : TransferInventory{};
        app.transferNote=!result.ok ? "target cannot be used:"+result.error :
            app.transferTargetInventory.garages.empty() ? "The target has a garage location with both vehicles and drivers empty." :
            "Target version and dependency check passed. Please select a garage and new save name.";
    } else {
        app.transferSourceInventory=result.ok ? std::move(result.inventory) : TransferInventory{};
        app.transferNote=!result.ok ? "Unable to read source vehicle:"+result.error :
            app.transferSourceInventory.trucks.empty() ? "The source archive has no own trucks to copy." :
            app.transferTargets.empty() ? "There is no other target archive. Please create an available archive in the target archive first." :
            "source vehicle has been read. Please select a truck and target archive.";
    }
}

void startTransferInspection(AppState& app,bool target) {
    if(app.busy) return;
    joinWorker(app);
    const auto request=app.truckTransferRequest;
    const uint64_t generation=++app.transferInspectionGeneration;
    app.transferInspectBusy=true; app.cancel=false; app.busy=true;
    app.progressDone=0; app.progressTotal=1;
    app.progressNote=target ? "Read target garage and check compatibility" : "read source vehicle";
    app.transferNote=target ? "is automatically decrypting and converting the target archive, and checking versions and dependencies..." :
                             "is automatically decrypting and converting the source archive, please wait...";
    app.worker=std::thread([&app,request,generation,target]{
        PendingTransferInspection result;
        result.game=request.game; result.generation=generation; result.target=target;
        auto canceled=[&app]{return app.cancel.load();};
        result.ok=target ? inspectTruckTransferTarget(request.source,request.target,&result.inventory,&result.error,canceled) :
                           inspectTruckTransferSlot(request.source,&result.inventory,&result.error,canceled);
        result.ready=true;
        {
            std::lock_guard<std::mutex> lock(app.resultMutex);
            app.pendingTransferInspection=std::move(result);
        }
        app.busy=false;
    });
}

void openTruckTransfer(AppState& app) {
    auto explain=[&](const std::string& message) {
        app.saveNote=message;
        app.truckTransferNotice=message;
        app.showTruckTransferNotice=true;
        logLine("Copy truck across archives:"+message);
    };
    if(app.busy) {explain("The current task is being processed, please complete or cancel before copying the truck.");return;}
    std::string error;
    if(copyBlockedByRunningGame(selectedGame(),&error)){explain(error);return;}
    if(app.selectedSlot<0 || app.selectedSlot>=(int)app.slots.size()) {
        explain("Please select the source archive of the truck in the archive list below, and then click \"Copy Truck Across Archives\".");return;
    }
    app.truckTransferRequest={};
    app.truckTransferRequest.game=selectedGame();
    app.truckTransferRequest.source=app.slots[app.selectedSlot];
    app.transferSourceInventory={};
    app.transferTargets.clear();
    for(const auto& slot:app.slots)
        if(::_wcsicmp(slot.slotDir.c_str(),app.truckTransferRequest.source.slotDir.c_str())!=0)
            app.transferTargets.push_back(slot);
    app.transferTruckIndex=0;app.transferTargetIndex=-1;app.transferGarageIndex=0;
    app.transferTargetInventory={};
    app.transferTargetReady=false;
    ::strncpy_s(app.transferName,"vehicle import copy",_TRUNCATE);
    app.showTruckTransfer=true;
    logLine("Copy a truck across archives: Open the selection window and read the source vehicle.");
    startTransferInspection(app,false);
}

void startTruckTransfer(AppState& app) {
    if(app.busy || !app.transferTargetReady || app.transferTruckIndex<0 ||
       app.transferTruckIndex>=(int)app.transferSourceInventory.trucks.size() ||
       app.transferGarageIndex<0 || app.transferGarageIndex>=(int)app.transferTargetInventory.garages.size()) return;
    joinWorker(app);consumePendingCopy(app);
    TruckTransferRequest request=app.truckTransferRequest;
    request.truckId=app.transferSourceInventory.trucks.at(app.transferTruckIndex).id;
    request.garageId=app.transferTargetInventory.garages.at(app.transferGarageIndex).id;
    request.displayName=app.transferName;
    app.busy=true;app.cancel=false;
    app.progressDone=0;app.progressTotal=1;app.progressNote="Convert and copy truck to new save";
    app.saveNote="Verifying truck accessories and target archives, and creating a copy of the vehicle import...";
    app.worker=std::thread([&app,request]{
        auto result=transferTruckToNewSave(request,[&app]{return app.cancel.load();});
        {
            std::lock_guard<std::mutex> lock(app.resultMutex);
            app.pendingCopy=std::move(result);app.pendingCopyTruck=true;app.pendingCopyReady=true;
        }
        app.busy=false;
    });
}

void renderTruckTransferDialog(AppState& app) {
    const char* notice="Cross-save truck copy prompt ##transfer_notice";
    if(app.showTruckTransferNotice) {::ImGui::OpenPopup(notice);app.showTruckTransferNotice=false;}
    ::ImGui::SetNextWindowSize(ImVec2(560,0),ImGuiCond_Appearing);
    if(::ImGui::BeginPopupModal(notice,nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        ::ImGui::TextWrapped("%s",app.truckTransferNotice.c_str());
        if(app.truckTransferUiProbe.capture) app.truckTransferUiProbe.noticeVisible=true;
        if(::ImGui::Button("Got it")) ::ImGui::CloseCurrentPopup();
        ::ImGui::EndPopup();
    }
    if(app.showTruckTransfer){::ImGui::OpenPopup("Cross-save copy truck ##transfer");app.showTruckTransfer=false;}
    ::ImGui::SetNextWindowSize(ImVec2(740,0),ImGuiCond_Appearing);
    if(!::ImGui::BeginPopupModal("Cross-save copy truck ##transfer",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) return;
    if(app.truckTransferRequest.source.slotDir.empty() ||
       app.truckTransferRequest.game!=selectedGame()){
        ::ImGui::CloseCurrentPopup();::ImGui::EndPopup();return;
    }
    ::ImGui::TextWrapped("Source: %s / %s",gameDescriptor(app.truckTransferRequest.game).shortName,
                        app.truckTransferRequest.source.label.c_str());
    ::ImGui::TextWrapped("automatically decrypts and reads existing archives without setting the game archive format. Copy to the new save slot of the target file, keeping two copies of the original file. Requires the same game, the same save version and compatible DLC/modules.");
    ::ImGui::BeginDisabled(app.busy);
    ::ImGui::SetNextItemWidth(650);
    const auto& trucks=app.transferSourceInventory.trucks;
    if(::ImGui::BeginCombo("truck",trucks.empty()?"No vehicle":trucks[app.transferTruckIndex].label.c_str())){
        for(int i=0;i<(int)trucks.size();++i){
            ::ImGui::PushID(i);
            auto label=trucks[i].label+" / "+trucks[i].id+" / "+std::to_string(trucks[i].accessories)+" accessories";
            if(::ImGui::Selectable(label.c_str(),i==app.transferTruckIndex))app.transferTruckIndex=i;
            ::ImGui::PopID();
        }
        ::ImGui::EndCombo();
    }
    ::ImGui::SetNextItemWidth(650);
    if(::ImGui::BeginCombo("target archive",app.transferTargetIndex<0?"Please select the target archive":app.transferTargets[app.transferTargetIndex].label.c_str())){
        for(int i=0;i<(int)app.transferTargets.size();++i){
            ::ImGui::PushID(i);
            if(::ImGui::Selectable(app.transferTargets[i].label.c_str(),i==app.transferTargetIndex)){
                app.transferTargetIndex=i;app.transferGarageIndex=0;
                app.truckTransferRequest.target=app.transferTargets[i];
                app.transferTargetInventory={}; app.transferTargetReady=false;
                startTransferInspection(app,true);
            }
            ::ImGui::PopID();
        }
        ::ImGui::EndCombo();
    }
    const auto& garages=app.transferTargetInventory.garages;
    ::ImGui::SetNextItemWidth(650);
    if(::ImGui::BeginCombo("Target Garage",garages.empty()?"No available space":garages[app.transferGarageIndex].id.c_str())){
        for(int i=0;i<(int)garages.size();++i){
            const auto label=garages[i].id+"（"+std::to_string(garages[i].freeSlots)+" vacancies)";
            if(::ImGui::Selectable(label.c_str(),i==app.transferGarageIndex))app.transferGarageIndex=i;
        }
        ::ImGui::EndCombo();
    }
    ::ImGui::SetNextItemWidth(500);
    ::ImGui::InputText("New archive name",app.transferName,sizeof(app.transferName));
    ::ImGui::EndDisabled();
    ::ImGui::TextWrapped("%s",app.transferNote.c_str());
    std::string escaped,error;
    bool valid=siiEscapeName(app.transferName,&escaped,&error);
    if(!valid)::ImGui::TextWrapped("%s",error.c_str());
    ::ImGui::BeginDisabled(!valid || garages.empty() || trucks.empty() || app.transferTargetIndex<0 ||
                           !app.transferTargetReady || app.transferInspectBusy || app.busy);
    if(::ImGui::Button("Duplicate truck and save as new slot")){startTruckTransfer(app);::ImGui::CloseCurrentPopup();}
    ::ImGui::EndDisabled();
    ::ImGui::SameLine();
    if(::ImGui::Button(app.transferInspectBusy ? "Cancel reading and close" : "Close")) {
        if(app.transferInspectBusy) app.cancel=true;
        app.transferInspectBusy=false; app.transferTargetReady=false;
        ++app.transferInspectionGeneration;
        ::ImGui::CloseCurrentPopup();
    }
    ::ImGui::EndPopup();
}


void selectHomeAccessory(AppState& app,int index) {
    app.homeAccessoryIndex=index;
    app.homePreview=false;
    app.homePath[0]=0;
    if(index>=0 && index<(int)app.homeInventory.entries.size())
        ::strncpy_s(app.homePath,app.homeInventory.entries[index].dataPath.c_str(),_TRUNCATE);
}

void consumePendingHomeInspection(AppState& app) {
    PendingHomeAccessoryInspection result;
    {
        std::lock_guard<std::mutex> lock(app.resultMutex);
        if(!app.pendingHomeInspection.ready) return;
        result=std::move(app.pendingHomeInspection); app.pendingHomeInspection={};
    }
    if(result.game!=selectedGame() || result.generation!=app.homeInspectionGeneration) return;
    app.homeInspectBusy=false; app.progressDone=1;
    app.homeInventory=result.ok ? std::move(result.inventory) : HomeAccessoryInventory{};
    app.homeTruckIndex=0;
    selectHomeAccessory(app,app.homeInventory.entries.empty() ? -1 : 0);
    app.homeNote=!result.ok ? "Failed to read accessories:"+result.error :
        app.homeInventory.entries.empty() ? "This archive has no own truck accessories to read." :
        app.homePresetMode==2 ? "Select a truck and target capacity, then preview permanent expansion." :
        "Select the truck and existing accessories, paste the new /def/... path, and preview.";
}

void openHomeAccessories(AppState& app,int mode=0) {
    if(app.busy) return;
    if(selectedGame()!=GameId::Ets2) {app.saveNote="Home accessory code currently only supports ETS2";return;}
    if(app.selectedSlot<0 || app.selectedSlot>=(int)app.slots.size()) {
        app.saveNote="Please select the archive you want to modify first";return;
    }
    joinWorker(app); consumePendingCopy(app);
    app.homeRequest={}; app.homeRequest.game=selectedGame();
    app.homeRequest.source=app.slots[app.selectedSlot];
    app.homeInventory={}; app.homeTruckIndex=0; selectHomeAccessory(app,-1);
    app.homePresetMode=mode;app.tankCapacity=5000;
    ::strncpy_s(app.homeName,mode==2?"permanent capacity expansion copy":"Home Accessories Copy",_TRUNCATE);
    app.homeNote="is automatically decrypting and reading its own truck accessories...";
    app.showHomeAccessories=true; app.homeInspectBusy=true;
    app.busy=true; app.cancel=false;
    app.progressDone=0; app.progressTotal=1; app.progressNote=mode==2?"reads vehicle chassis":"Read Home Accessories";
    const auto request=app.homeRequest;
    const uint64_t generation=++app.homeInspectionGeneration;
    app.worker=std::thread([&app,request,generation]{
        PendingHomeAccessoryInspection result;
        result.game=request.game; result.generation=generation;
        result.ok=inspectHomeAccessoriesSlot(request.source,&result.inventory,&result.error,
                                             [&app]{return app.cancel.load();});
        result.ready=true;
        {std::lock_guard<std::mutex> lock(app.resultMutex);app.pendingHomeInspection=std::move(result);}
        app.busy=false;
    });
}

bool startHomeAccessoryCopy(AppState& app) {
    if(app.busy || !app.homePreview || app.homeAccessoryIndex<0 ||
       app.homeAccessoryIndex>=(int)app.homeInventory.entries.size()) return false;
    std::string error;
    if(copyBlockedByRunningGame(selectedGame(),&error)) {app.homeNote=error;return false;}
    const auto& entry=app.homeInventory.entries[app.homeAccessoryIndex];
    if(!app.homePresetMode && entry.shared) {app.homeNote="shared accessories cannot be modified, please choose other accessories.";return false;}
    HomeAccessoryRequest request=app.homeRequest;
    request.truckId=entry.truckId; request.unitId=entry.unitId; request.originalPath=entry.dataPath;
    request.newPath=app.homePath; request.displayName=app.homeName;
    request.scaniaTvPreset=app.homePresetMode==1;
    request.tankCapacity=app.homePresetMode==2?app.tankCapacity:0;
    joinWorker(app); consumePendingCopy(app);
    app.busy=true; app.cancel=false;
    app.progressDone=0;app.progressTotal=1;app.progressNote=request.tankCapacity?"Generate Capacity Mod and Archive Copy":request.scaniaTvPreset ? "Install three-screen TV presets and save a copy" : "Generate archive copy of Home accessories";
    app.saveNote=request.tankCapacity ? "Generating permanent tank/battery expansion mod and archive copy..." : request.scaniaTvPreset ? "is verifying built-in model resources and hangpoints, and generating a three-screen TV archive copy..." : "is verifying accessories and generating a new archive. The original archive is retained...";
    app.worker=std::thread([&app,request]{
        auto result=homeAccessoryToNewSave(request,[&app]{return app.cancel.load();});
        {
            std::lock_guard<std::mutex> lock(app.resultMutex);
            app.pendingCopy=std::move(result);app.pendingCopyTruck=false;
            app.pendingCopyHome=true;app.pendingCopyReady=true;
        }
        app.busy=false;
    });
    return true;
}

void renderFuelTankControls(AppState& app) {
    ::ImGui::TextWrapped("adds an independent expansion chassis for a single truck: diesel fuel tank by liter, battery by kWh. Supports built-in Oka 1.61 original diesel chassis and Scania S 2024 electric 4×2; Mods that modify the chassis definition are not supported.\nThe");
    ::ImGui::TextWrapped("Mod will be installed into the mod folder of the game's documentation. After generating, enable it in the game Mod Manager first, and then load the copy; the capacity will still take effect after saving and restarting, and you need to keep the Mod enabled while using the copy.");
    ::ImGui::Separator();
    std::vector<int> chassis;
    for(int i=0;i<(int)app.homeInventory.entries.size();++i) {
        const auto& entry=app.homeInventory.entries[i];
        if(entry.type=="vehicle_accessory"&&entry.dataPath.find("/chassis/")!=std::string::npos)chassis.push_back(i);
    }
    if(std::find(chassis.begin(),chassis.end(),app.homeAccessoryIndex)==chassis.end()) {
        int preferred=chassis.empty()?-1:chassis.front();
        for(int index:chassis) {
            FuelTankMod probe;std::string error;
            const auto& entry=app.homeInventory.entries[index];
            if(!entry.shared&&buildFuelTankMod(entry.dataPath,app.tankCapacity,&probe,&error)){preferred=index;break;}
        }
        selectHomeAccessory(app,preferred);
    }
    ::ImGui::BeginDisabled(app.busy||app.homeInspectBusy);
    const auto* entry=app.homeAccessoryIndex>=0&&app.homeAccessoryIndex<(int)app.homeInventory.entries.size()?
        &app.homeInventory.entries[app.homeAccessoryIndex]:nullptr;
    const auto selectedLabel=entry?entry->truckLabel+" / "+entry->truckId:"No readable chassis";
    ::ImGui::SetNextItemWidth(-100);
    if(::ImGui::BeginCombo("Select truck",selectedLabel.c_str())) {
        for(int index:chassis) {
            const auto& item=app.homeInventory.entries[index];
            const auto label=item.truckLabel+" / "+item.truckId+(item.shared?" [Shared, cannot be modified]":"");
            if(::ImGui::Selectable(label.c_str(),index==app.homeAccessoryIndex))selectHomeAccessory(app,index);
        }
        ::ImGui::EndCombo();
    }
    entry=app.homeAccessoryIndex>=0&&app.homeAccessoryIndex<(int)app.homeInventory.entries.size()?
        &app.homeInventory.entries[app.homeAccessoryIndex]:nullptr;
    if(entry)::ImGui::TextWrapped("Current chassis: %s",entry->dataPath.c_str());
    const bool electric=entry&&fuelTankIsElectric(entry->dataPath);
    const char* unit=electric?"kWh":"Liter";
    ::ImGui::SetNextItemWidth(200);
    if(::ImGui::InputInt(electric?"target battery capacity (kWh)":"target fuel tank capacity (liters)",&app.tankCapacity,100,500))app.homePreview=false;
    ::ImGui::TextDisabled("supports 100–10000 %s, the target must be larger than the original capacity.",unit);
    ::ImGui::TextDisabled("retains the archived %s ratio, and the actual %s is calculated based on the new capacity.",electric?"power":"fuel level",electric?"power":"Liters");
    ::ImGui::SetNextItemWidth(-100);
    if(::ImGui::InputText("New archive name",app.homeName,sizeof(app.homeName)))app.homePreview=false;
    FuelTankMod mod;std::string error,escaped;
    bool valid=entry&&!entry->shared&&buildFuelTankMod(entry->dataPath,app.tankCapacity,&mod,&error);
    if(entry&&entry->shared)error="chassis accessories are shared by multiple vehicles and cannot be modified individually.";
    if(valid) {
        std::vector<std::string> engines;
        for(const auto& item:app.homeInventory.entries)if(item.truckId==entry->truckId&&item.dataPath.find("/engine/")!=std::string::npos)
            engines.push_back(item.dataPath);
        valid=validateFuelTankPowertrain(mod,engines,&error);
    }
    if(valid)valid=siiEscapeName(app.homeName,&escaped,&error);
    if(valid)::ImGui::Text("%s: original %d %s → new capacity %d %s",electric?"battery":"fuel tank",mod.originalCapacity,unit,mod.capacity,unit);
    if(!error.empty())::ImGui::TextWrapped("%s",error.c_str());
    ::ImGui::BeginDisabled(!valid);
    if(::ImGui::Button("preview permanent expansion"))app.homePreview=true;
    ::ImGui::EndDisabled();
    if(app.homePreview&&valid) {
        app.homePreviewVisible=true;
        ::ImGui::TextWrapped("Truck: %s / %s\n Original %d → %d %s\nMod: %s\n New Chassis: %s\n The original save is retained with other trucks; please exit the game first and then generate again.",
            entry->truckLabel.c_str(),entry->truckId.c_str(),mod.originalCapacity,mod.capacity,unit,mod.displayName.c_str(),mod.chassisPath.c_str());
    }
    app.homeApplyEnabled=valid&&app.homePreview&&!app.busy;
    ::ImGui::BeginDisabled(!valid||!app.homePreview);
    if(ui::primaryButton(electric?"Generate Mod and Battery Archive Copy":"Generate Mod and Tank Archive Copy"))if(startHomeAccessoryCopy(app))::ImGui::CloseCurrentPopup();
    ::ImGui::EndDisabled();::ImGui::EndDisabled();
    ::ImGui::Spacing();::ImGui::TextWrapped("%s",app.homeNote.c_str());
    if(::ImGui::Button(app.homeInspectBusy?"Cancel reading and close":"Close")) {
        if(app.homeInspectBusy)app.cancel=true;
        app.homeInspectBusy=false;app.homePreview=false;++app.homeInspectionGeneration;::ImGui::CloseCurrentPopup();
    }
}

void renderHomeAccessoriesDialog(AppState& app) {
    app.homeDialogVisible=false; app.homePreviewVisible=false; app.homeApplyEnabled=false;
    const char* title="Vehicle accessories and capacity expansion ##home_accessories";
    if(app.showHomeAccessories){::ImGui::OpenPopup(title);app.showHomeAccessories=false;}
    ::ImGui::SetNextWindowSize(ImVec2(900,(std::min)(740.f,::ImGui::GetIO().DisplaySize.y-40.f)),ImGuiCond_Appearing);
    if(!::ImGui::BeginPopupModal(title,nullptr,ImGuiWindowFlags_None)) return;
    if(selectedGame()!=GameId::Ets2 || app.homeRequest.source.slotDir.empty()) {
        ::ImGui::CloseCurrentPopup();::ImGui::EndPopup();return;
    }
    app.homeDialogVisible=true;
    ui::textf(ui::muted,"Source file: %s / Archive: %s",W2U(app.homeRequest.source.profileName).c_str(),
              app.homeRequest.source.displayName.c_str());
    if(app.homePresetMode==2){renderFuelTankControls(app);::ImGui::EndPopup();return;}
    ::ImGui::TextWrapped("Select path replacement or built-in Home preset to generate a named copy in the same file. Please exit Oka 2 before generating a copy.");
    const char* modes[]={"Manually replace /def/... accessory path","three-screen TV + blind mirror + driving information (Scania S 2016)"};
    ::ImGui::BeginDisabled(app.busy);
    ::ImGui::SetNextItemWidth(-100);
    if(::ImGui::Combo("modification method",&app.homePresetMode,modes,2)) {
        app.homePreview=false;
        ::strncpy_s(app.homeName,app.homePresetMode ? "triple screen TV copy" : "Home Accessories Copy",_TRUNCATE);
    }
    ::ImGui::EndDisabled();
    if(app.homePresetMode) {
        ::ImGui::TextWrapped("This experimental preset comes from the MoRan zip package you provided. Requires Cabin Accessories DLC; older models have not yet been verified for display in current game and TMP.");
        ::ImGui::TextWrapped("installs 14 model resource files; use slot_1 to automatically complete the installation base. Mount points that already have different resources or occupies different resources will be retained and a conflict will be prompted.");
    } else ::ImGui::TextWrapped("only checks path format, does not verify resource existence or TruckersMP synchronization. Please use the accessory path that matches the game version and car model.");
    ::ImGui::Separator();
    std::vector<const HomeAccessoryEntry*> trucks;
    std::set<std::string> truckIds;
    for(const auto& entry:app.homeInventory.entries)
        if(truckIds.insert(entry.truckId).second) trucks.push_back(&entry);
    ::ImGui::BeginDisabled(app.busy || trucks.empty());
    if(!trucks.empty()) {
        app.homeTruckIndex=std::clamp(app.homeTruckIndex,0,(int)trucks.size()-1);
        const auto* truck=trucks[app.homeTruckIndex];
        ::ImGui::SetNextItemWidth(-100);
        if(::ImGui::BeginCombo("truck",truck->truckLabel.c_str())) {
            for(int i=0;i<(int)trucks.size();++i) {
                ::ImGui::PushID(i);
                if(::ImGui::Selectable(trucks[i]->truckLabel.c_str(),app.homeTruckIndex==i)) {
                    app.homeTruckIndex=i;
                    for(int j=0;j<(int)app.homeInventory.entries.size();++j)
                        if(app.homeInventory.entries[j].truckId==trucks[i]->truckId) {selectHomeAccessory(app,j);break;}
                }
                ::ImGui::PopID();
            }
            ::ImGui::EndCombo();
        }
        const std::string truckId=trucks[app.homeTruckIndex]->truckId;
        std::string accessoryLabel="Please select accessories";
        if(app.homeAccessoryIndex>=0 && app.homeAccessoryIndex<(int)app.homeInventory.entries.size()) {
            const auto& entry=app.homeInventory.entries[app.homeAccessoryIndex];
            accessoryLabel=entry.dataPath+(entry.shared ? " [Shared, cannot be modified]" : "");
        }
        ::ImGui::SetNextItemWidth(-100);
        if(!app.homePresetMode && ::ImGui::BeginCombo("already has accessories",accessoryLabel.c_str())) {
            for(int i=0;i<(int)app.homeInventory.entries.size();++i) {
                const auto& entry=app.homeInventory.entries[i];
                if(entry.truckId!=truckId) continue;
                ::ImGui::PushID(i);
                const std::string label=entry.dataPath+(entry.shared ? " [Shared, cannot be modified]" : "");
                if(::ImGui::Selectable(label.c_str(),app.homeAccessoryIndex==i)) selectHomeAccessory(app,i);
                ::ImGui::PopID();
            }
            ::ImGui::EndCombo();
        }
    }
    const HomeAccessoryEntry* entry=app.homeAccessoryIndex>=0 &&
        app.homeAccessoryIndex<(int)app.homeInventory.entries.size() ?
        &app.homeInventory.entries[app.homeAccessoryIndex] : nullptr;
    if(entry && !app.homePresetMode) {
        ui::textf(ui::muted,"Accessory type: %s / Sub-type: %s",entry->type.c_str(),entry->subtype.empty() ? "—" : entry->subtype.c_str());
        ::ImGui::TextWrapped("Current path: %s",entry->dataPath.c_str());
        if(entry->shared) ui::text(ui::accent,"This accessory or parent accessory is shared and cannot be modified.");
    }
    if(!app.homePresetMode) {
        ::ImGui::SetNextItemWidth(-100);
        if(::ImGui::InputText("new accessory path",app.homePath,sizeof(app.homePath))) app.homePreview=false;
    }
    ::ImGui::SetNextItemWidth(-100);
    if(::ImGui::InputText("New archive name",app.homeName,sizeof(app.homeName))) app.homePreview=false;
    std::string error,escaped;
    bool valid=false;
    if(app.homePresetMode) {
        valid=entry && entry->truckLabel=="scania.s_2016" && scaniaTvPresetAvailable();
        if(!scaniaTvPresetAvailable()) error="This version does not contain the complete three-screen TV preset resources.";
        else if(!entry || entry->truckLabel!="scania.s_2016") error="Please choose Scania S 2016 own truck.";
    } else valid=entry && !entry->shared && validateHomeAccessoryPath(app.homePath,&error);
    if(valid) valid=siiEscapeName(app.homeName,&escaped,&error);
    if(valid && !app.homePresetMode && entry->dataPath==app.homePath){valid=false;error="The new path is the same as the current path, please fill in the path to be replaced.";}
    if(entry && !error.empty()) ::ImGui::TextWrapped("%s",error.c_str());
    ::ImGui::BeginDisabled(!valid);
    if(::ImGui::Button(app.homePresetMode ? "preview import" : "preview replacement")) app.homePreview=true;
    ::ImGui::EndDisabled();
    if(app.homePreview && valid) {
        app.homePreviewVisible=true;
        if(app.homePresetMode) {
            ui::text(ui::accent,"will import the three-screen TV presets and keep the original save:");
            ::ImGui::TextWrapped("Truck: %s\n New archive: %s\n Resource location: %s/MoRan/scania/TV",entry->truckLabel.c_str(),app.homeName,W2U(documentsDir()).c_str());
        } else {
            ui::text(ui::accent,"will replace only the path of the selected accessory:");
            ::ImGui::TextWrapped("Original: %s",entry->dataPath.c_str());
            ::ImGui::TextWrapped("New: %s",app.homePath);
        }
    }
    app.homeApplyEnabled=valid && app.homePreview && !app.busy;
    ::ImGui::BeginDisabled(!valid || !app.homePreview);
    if(ui::primaryButton(app.homePresetMode ? "One-click import and generate archive copy" : "Modify and generate archive copy"))
        if(startHomeAccessoryCopy(app)) ::ImGui::CloseCurrentPopup();
    ::ImGui::EndDisabled();
    ::ImGui::EndDisabled();
    ::ImGui::Spacing();
    ::ImGui::TextWrapped("%s",app.homeNote.c_str());
    if(::ImGui::Button(app.homeInspectBusy ? "Cancel reading and close" : "Close")) {
        if(app.homeInspectBusy) app.cancel=true;
        app.homeInspectBusy=false;app.homePreview=false;++app.homeInspectionGeneration;
        ::ImGui::CloseCurrentPopup();
    }
    ::ImGui::EndPopup();
}

void consumePendingTrailerSwapInspection(AppState& app) {
    PendingTrailerSwapInspection result;
    {
        std::lock_guard<std::mutex> lock(app.resultMutex);
        if(!app.pendingTrailerSwap.ready) return;
        result=std::move(app.pendingTrailerSwap);
        app.pendingTrailerSwap={};
    }
    if(result.game!=selectedGame() || result.generation!=app.trailerSwapGeneration) return;
    app.trailerSwapBusy=false;
    app.progressDone=1;
    if(!result.ok) {
        app.trailerSwapInventory={};
        app.trailerSwapIndex=-1;
        app.trailerSwapNote="Unable to read trailer:"+result.error;
        return;
    }
    app.trailerSwapInventory=std::move(result.inventory);
    app.trailerSwapIndex=-1;
    for(int i=0;i<(int)app.trailerSwapInventory.entries.size();++i) {
        const auto& entry=app.trailerSwapInventory.entries[i];
        if(entry.current || entry.blocked) continue;
        app.trailerSwapIndex=i; break;
    }
    std::size_t owned=0,blockedCount=0;
    for(const auto& entry:app.trailerSwapInventory.entries) {
        if(entry.owned) ++owned;
        if(entry.blocked) ++blockedCount;
    }
    if(!app.trailerSwapInventory.hasCurrent)
        app.trailerSwapNote=app.trailerSwapInventory.swapBlockReason;
    else {
        app.trailerSwapNote="total "+std::to_string(app.trailerSwapInventory.entries.size())+
            " trailers (owned "+std::to_string(owned)+" vehicles"+
            (app.trailerSwapInventory.onJob ? ", mission in progress" : "")+
            (app.trailerSwapInventory.onlineJob ? ", online task" : "")+"）。";
        if(!app.trailerSwapInventory.swapBlockReason.empty())
            app.trailerSwapNote+="\n"+app.trailerSwapInventory.swapBlockReason;
    }
}

void startTrailerSwapInspection(AppState& app) {
    if(app.busy) return;
    joinWorker(app);consumePendingCopy(app);
    const auto request=app.trailerSwapRequest;
    const uint64_t generation=++app.trailerSwapGeneration;
    app.trailerSwapBusy=true; app.cancel=false; app.busy=true;
    app.progressDone=0; app.progressTotal=1;
    app.progressNote="Read the trailer in the archive";
    app.trailerSwapNote="is automatically decrypting and converting the archive, please wait...";
    app.trailerSwapInventory={}; app.trailerSwapIndex=-1;
    app.worker=std::thread([&app,request,generation]{
        PendingTrailerSwapInspection result;
        result.game=request.game; result.generation=generation;
        result.ok=inspectTrailerSwapSlot(request.source,&result.inventory,&result.error,
                                        [&app]{return app.cancel.load();});
        result.ready=true;
        {
            std::lock_guard<std::mutex> lock(app.resultMutex);
            app.pendingTrailerSwap=std::move(result);
        }
        app.busy=false;
    });
}

void openTrailerSwap(AppState& app) {
    auto explain=[&](const std::string& message) {
        app.saveNote=message; logLine("replacement trailer:"+message);
    };
    if(app.busy) {explain("The current task is being processed. Please complete or cancel before changing the trailer.");return;}
    std::string error;
    if(copyBlockedByRunningGame(selectedGame(),&error)){explain(error);return;}
    if(app.selectedSlot<0 || app.selectedSlot>=(int)app.slots.size()) {
        explain("Please first select the archive you want to change the trailer in the archive list below, and then click \"Change Trailer\".");return;
    }
    app.trailerSwapRequest={};
    app.trailerSwapRequest.game=selectedGame();
    app.trailerSwapRequest.source=app.slots[app.selectedSlot];
    const auto& name=app.trailerSwapRequest.source.displayName;
    ::strncpy_s(app.trailerSwapName,(name.empty()?std::string("trailer replacement copy"):name+" replacement trailer").c_str(),_TRUNCATE);
    app.showTrailerSwap=true;
    logLine("Changing a trailer: Open the selection window and read the trailer list.");
    startTrailerSwapInspection(app);
}

bool startTrailerSwap(AppState& app) {
    if(app.busy || app.trailerSwapIndex<0 ||
       app.trailerSwapIndex>=(int)app.trailerSwapInventory.entries.size()) return false;
    const auto& entry=app.trailerSwapInventory.entries[app.trailerSwapIndex];
    if(entry.blocked || entry.current || !app.trailerSwapInventory.hasCurrent ||
       !app.trailerSwapInventory.swapBlockReason.empty()) return false;
    joinWorker(app);consumePendingCopy(app);
    TrailerSwapRequest request=app.trailerSwapRequest;
    request.targetTrailerId=entry.id;
    request.displayName=app.trailerSwapName;
    app.busy=true;app.cancel=false;
    app.progressDone=0;app.progressTotal=1;app.progressNote="Change trailer and save new file";
    app.saveNote="is verifying the trailer and generating a new archive. The original archive is retained...";
    app.worker=std::thread([&app,request]{
        auto result=trailerSwapToNewSave(request,[&app]{return app.cancel.load();});
        {
            std::lock_guard<std::mutex> lock(app.resultMutex);
            app.pendingCopy=std::move(result);app.pendingCopyTrailer=true;app.pendingCopyReady=true;
        }
        app.busy=false;
    });
    return true;
}

std::string trailerSwapEntryLabel(const TrailerSwapEntry& entry) {
    std::string label=entry.label;
    if(!entry.plate.empty()) label+=" / "+entry.plate;
    if(entry.cargoMass>0.5f) {
        char mass[32];::snprintf(mass,sizeof(mass),"%dt",(int)(entry.cargoMass/1000.f+0.5f));
        label+=" / load "; label+=mass;
    }
    label+=entry.current ? " 【Current】" : entry.owned ? "(own)" : "(Task/Market)";
    if(entry.blocked) label+=" [not optional]";
    return label;
}

void renderTrailerSwapDialog(AppState& app) {
    const char* title="replacement trailer ##trailer_swap";
    if(app.showTrailerSwap){::ImGui::OpenPopup(title);app.showTrailerSwap=false;}
    ::ImGui::SetNextWindowSize(ImVec2(760,0),ImGuiCond_Appearing);
    if(!::ImGui::BeginPopupModal(title,nullptr,ImGuiWindowFlags_AlwaysAutoResize)) return;
    app.trailerSwapDialogVisible=true;
    if(app.trailerSwapRequest.source.slotDir.empty() ||
       app.trailerSwapRequest.game!=selectedGame()){
        ::ImGui::CloseCurrentPopup();::ImGui::EndPopup();return;
    }
    ui::textf(ui::muted,"Source file: %s / Archive: %s",
              W2U(app.trailerSwapRequest.source.profileName).c_str(),
              app.trailerSwapRequest.source.displayName.c_str());
    ::ImGui::TextWrapped("Change the currently connected trailer to another one in the archive. There is no need to drive back to the garage first, and the trailer space will remain in place. The cargo and tasks continue with the new trailer; it is saved as a new archive by default and takes effect immediately after loading into the game.");
    const auto& inv=app.trailerSwapInventory;
    ::ImGui::Separator();
    if(inv.hasCurrent) {
        ::ImGui::TextWrapped("Current truck: %s",inv.truckLabel.c_str());
        ::ImGui::TextWrapped("Current trailer: %s%s / %s",
                             inv.currentPlate.empty()?"":(inv.currentPlate+" / ").c_str(),
                             inv.currentLabel.c_str(),inv.currentId.c_str());
        if(inv.onJob)
            ui::textf(ui::muted,"mission %s (cargo: %s) is in progress. After changing the vehicle, the goods are hung on the new trailer.",
                      inv.onlineJob?", and it is an online task\nWhich trailer should":"",inv.jobCargo.c_str());
    } else ::ImGui::TextWrapped("%s",inv.swapBlockReason.c_str());
    ::ImGui::Separator();
    ::ImGui::BeginDisabled(app.busy || app.trailerSwapBusy);
    if(!inv.entries.empty()) {
        app.trailerSwapIndex=std::clamp(app.trailerSwapIndex,0,(int)inv.entries.size()-1);
        const auto& selected=inv.entries[app.trailerSwapIndex];
        ::ImGui::SetNextItemWidth(-100);
        if(::ImGui::BeginCombo("be replaced with?",trailerSwapEntryLabel(selected).c_str())) {
            for(int i=0;i<(int)inv.entries.size();++i) {
                const auto& entry=inv.entries[i];
                ::ImGui::PushID(i);
                if(::ImGui::Selectable(trailerSwapEntryLabel(entry).c_str(),app.trailerSwapIndex==i))
                    app.trailerSwapIndex=i;
                if(entry.blocked && ::ImGui::IsItemHovered())
                    ::ImGui::SetTooltip("%s",entry.blockReason.c_str());
                ::ImGui::PopID();
            }
            ::ImGui::EndCombo();
        }
        if(selected.blocked) ui::text(ui::accent,("cannot be switched:"+selected.blockReason).c_str());
        else if(selected.current) ui::text(ui::muted,"This is the current trailer, please choose another one.");
        else {
            std::string detail="new car:"+selected.label;
            if(!selected.plate.empty()) detail+=" / license plate "+selected.plate;
            if(selected.cargoMass>0.5f) {
                char mass[32];::snprintf(mass,sizeof(mass),"%d tons",(int)(selected.cargoMass/1000.f+0.5f));
                detail+=" / load "; detail+=mass;
            }
            ::ImGui::TextWrapped("%s",detail.c_str());
        }
    } else if(!app.trailerSwapBusy) ::ImGui::TextWrapped("There is no trailer in the archive.");
    ::ImGui::SetNextItemWidth(-100);
    ::ImGui::InputText("New archive name",app.trailerSwapName,sizeof(app.trailerSwapName));
    ::ImGui::EndDisabled();
    std::string escaped,error;
    const bool nameValid=siiEscapeName(app.trailerSwapName,&escaped,&error);
    if(!nameValid) ::ImGui::TextWrapped("%s",error.c_str());
    const bool canApply=inv.hasCurrent && inv.swapBlockReason.empty() && !inv.entries.empty() &&
                         app.trailerSwapIndex>=0 && nameValid &&
                         app.trailerSwapIndex<(int)inv.entries.size() &&
                         !inv.entries[app.trailerSwapIndex].blocked &&
                         !inv.entries[app.trailerSwapIndex].current &&
                         !app.busy && !app.trailerSwapBusy;
    ::ImGui::BeginDisabled(!canApply);
    if(ui::primaryButton("Change trailer and save as new archive"))
        if(startTrailerSwap(app)) ::ImGui::CloseCurrentPopup();
    ::ImGui::EndDisabled();
    ::ImGui::SameLine();
    if(::ImGui::Button(app.trailerSwapBusy ? "Cancel reading and close" : "Close")) {
        if(app.trailerSwapBusy) app.cancel=true;
        app.trailerSwapBusy=false;++app.trailerSwapGeneration;
        ::ImGui::CloseCurrentPopup();
    }
    ::ImGui::Spacing();
    if(app.trailerSwapBusy) ui::text(ui::muted,"is reading the trailer list...");
    ::ImGui::TextWrapped("%s",app.trailerSwapNote.c_str());
    ::ImGui::EndPopup();
}

void backupSelectedSlot(AppState& app) {
    if (app.selectedSlot < 0 || app.selectedSlot >= (int)app.slots.size()) {
        app.saveNote = "Please select an archive in the list first";
        return;
    }
    const SaveSlot& slot = app.slots[(size_t)app.selectedSlot];
    std::string err;
    std::wstring dir = backupSlot(slot, "manual", &err);
    if (dir.empty()) {
        app.saveNote = "Backup failed:" + err;
        logLine(app.saveNote);
        return;
    }
    app.saveNote = "has been backed up to:" + W2U(dir);
    logLine("has been backed up " + slot.label + " -> " + W2U(dir));
    app.backups = listBackups();
}

void restoreSelectedBackup(AppState& app) {
    if (app.selectedBackup < 0 || app.selectedBackup >= (int)app.backups.size()) {
        app.saveNote = "Please select a backup in the \"Backup List\" first";
        return;
    }
    app.requestRestoreConfirm = true;
    app.saveNote = "Restore will overwrite the current archive, please check in the confirmation box.";
}

namespace {

void performRestoreSelectedBackup(AppState& app) {
    if (app.selectedBackup < 0 || app.selectedBackup >= (int)app.backups.size()) {
        app.saveNote = "The selected backup has expired, please select again.";
        return;
    }
    DWORD gamePid = 0;
    if (findProcess(selectedProcessName(), &gamePid, nullptr)) {
        app.saveNote = fmt("Restore canceled: Please exit %s first to avoid overwriting files by games or Steam cloud synchronization.", selectedGameDescriptor().shortName);
        return;
    }
    const BackupInfo info = app.backups[(size_t)app.selectedBackup];
    std::vector<SaveSlot> currentSlots = listSlots();
    const SaveSlot* current = nullptr;
    for (const auto& slot : currentSlots) {
        if (_wcsicmp(slot.slotDir.c_str(), info.originalDir.c_str()) == 0) {
            current = &slot;
            break;
        }
    }

    std::string err;
    std::wstring safetyBackup;
    if (current) {
        safetyBackup = backupSlot(*current, "before_restore", &err);
        if (safetyBackup.empty()) {
            app.saveNote = "Restore canceled: Unable to back up current archive first (" + err + "）";
            logLine(app.saveNote);
            return;
        }
    } else if (::GetFileAttributesW(info.originalDir.c_str()) != INVALID_FILE_ATTRIBUTES) {
        app.saveNote = fmt("Restore canceled: The target directory is not a currently recognized %s archive.", selectedGameDescriptor().shortName);
        logLine(app.saveNote);
        return;
    }
    if (!restoreBackup(info, &err)) {
        app.saveNote = "restore failed:" + err;
        logLine(app.saveNote);
        return;
    }
    app.saveNote = fmt("has restored %s / %s to %s%s", W2U(info.profile).c_str(),
                       W2U(info.slot).c_str(), W2U(info.originalDir).c_str(),
                       safetyBackup.empty()
                           ? ""
                           : ("; The state before restoration has been backed up to " + W2U(safetyBackup)).c_str());
    logLine(app.saveNote);
    app.slots = listSlots();
    app.backups = listBackups();
    app.selectedBackup = -1;
}

}  // namespace

namespace {

bool isDirSegment(const std::wstring& path) {
    const DWORD attr = ::GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool exportFilePathExists(const std::wstring& path) {
    return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring sanitizeExportPart(const std::wstring& raw) {
    std::wstring out;
    for (wchar_t ch : raw) {
        if (ch < 0x20 || ch == 0x7F) continue;
        if (wcschr(L"<>:\"/\\|?*", ch) != nullptr) continue;
        out.push_back(ch);
    }
    while (!out.empty() && (out.back() == L'.' || out.back() == L' ')) out.pop_back();
    while (!out.empty() && out.front() == L' ') out.erase(out.begin());
    if (out.empty()) out = L"unknown";
    if (out == L"." || out == L"..") out = L"unknown";
    if (out.size() > 40) out.resize(40);
    return out;
}

std::wstring exportTimestamp() {
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    wchar_t buf[32];
    swprintf_s(buf, L"%04u%02u%02u_%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour,
               st.wMinute, st.wSecond);
    return buf;
}

}  // namespace

namespace {

void consumePendingExport(AppState& app) {
    PendingSaveExport result;
    {
        std::lock_guard<std::mutex> lock(app.resultMutex);
        if (!app.pendingExport.ready) return;
        result = std::move(app.pendingExport);
        app.pendingExport = {};
    }
    if (result.game != selectedGame()) return;
    app.saveNote = std::move(result.note);
    logLine(app.saveNote);
}

void startSelectedExport(AppState& app, bool readable) {
    if (app.busy) {
        app.saveNote = "There is already a background task in progress. Please wait for completion or cancellation before exporting.";
        return;
    }
    if (app.selectedSlot < 0 || app.selectedSlot >= (int)app.slots.size()) {
        app.saveNote = "Please select an archive in the list first";
        return;
    }
    joinWorker(app);
    consumePendingExport(app);
    const SaveSlot slot = app.slots[(size_t)app.selectedSlot];
    const GameId game = selectedGame();
    const std::wstring exports = documentsDir() + L"\\exports";
    if (!isDirSegment(exports) && !::CreateDirectoryW(exports.c_str(), nullptr)) {
        const DWORD e = ::GetLastError();
        if (e != ERROR_ALREADY_EXISTS || !isDirSegment(exports)) {
            app.saveNote = "Export failed: Unable to create the export directory (may be insufficient permissions or the path is occupied)";
            logLine(app.saveNote);
            return;
        }
    }
    const std::wstring base = (readable ? L"readable_" : L"save_") + sanitizeExportPart(slot.profileName) + L"_" +
                              sanitizeExportPart(slot.slotName) + L"_" + exportTimestamp();
    std::wstring outPath;
    for (int i = 0; i < 1000; ++i) {
        std::wstring candidate = exports + L"\\" + base + (i == 0 ? L"" : L"_" + std::to_wstring(i)) + L".sii";
        if (!exportFilePathExists(candidate)) { outPath = candidate; break; }
    }
    if (outPath.empty()) {
        app.saveNote = "Export failed: There are too many files with the same name. Please clear the export directory and try again.";
        logLine(app.saveNote);
        return;
    }
    app.cancel = false;
    app.progressDone = 0;
    app.progressTotal = 1;
    app.progressNote = readable ? "exports readable SII" : "Decrypted export archive";
    app.saveNote = readable ? "is decrypting and converting readable SII text in the background..." : "is decrypting and exporting archives in the background...";
    app.busy = true;
    try {
        app.worker = std::thread([&app, slot, outPath, game, readable]() {
            PendingSaveExport result;
            result.game = game;
            result.ready = true;
            try {
                const auto canceled = [&app] { return app.cancel.load(); };
                std::string err;
                if (readable) {
                    ReadableExportInfo info;
                    if (!exportReadableSii(slot, outPath, &info, &err, canceled)) {
                        result.note = "Readable export failed:" + (err.empty() ? "unknown error" : err);
                    } else {
                        result.note = fmt("Exported readable SII (%s, %s) -> %s. Repeated export will not overwrite old files",
                            info.innerFormat == "bsii" ? "BSII v3 converted to text" : "SII text remains as is",
                            formatSize(info.textSize).c_str(), W2U(outPath).c_str());
                    }
                } else {
                    DecryptInfo info;
                    if (!exportDecrypted(slot, outPath, &info, &err, canceled)) {
                        result.note = "Decryption and export failed:" + (err.empty() ? "unknown error" : err);
                    } else {
                        const char* kind = info.innerFormat == "text" ? "SII text" :
                                           info.innerFormat == "bsii" ? "BSII binary, retain the original format" :
                                           "3nK binary, retain the original format";
                        result.note = fmt("decrypted and exported (%s, %s) -> %s. Repeated export will not overwrite old files",
                            kind, formatSize(info.innerSize).c_str(), W2U(outPath).c_str());
                    }
                }
            } catch (const std::exception& e) {
                result.note = std::string("Export failed:") + e.what();
            } catch (...) {
                result.note = "Export failed: Unknown exception occurred";
            }
            {
                std::lock_guard<std::mutex> lock(app.resultMutex);
                app.pendingExport = std::move(result);
            }
            app.progressDone = 1;
            app.busy = false;
        });
    } catch (const std::exception& e) {
        app.busy = false;
        app.saveNote = std::string("Unable to start the export task:") + e.what();
        logLine(app.saveNote);
    }
}

}  // namespace

void exportSelectedSlot(AppState& app) { startSelectedExport(app, false); }
void exportReadableSelectedSlot(AppState& app) { startSelectedExport(app, true); }

void applyTextPatch(AppState& app) {
    // 存档编辑仅在游戏完全退出后允许：避免游戏自动存档覆盖新值、读写文件竞争，
    // 同时杜绝在线会话中通过存档编辑绕过进程内存写入闸门。
    DWORD runningPid = 0;
    if (findProcess(selectedProcessName(), &runningPid, nullptr)) {
        app.saveNote = "Please exit the game completely before modifying the plain text archive (the file may be automatically saved and overwritten while the game is running).";
        logLine(app.saveNote);
        return;
    }
    if (app.selectedSlot < 0 || app.selectedSlot >= (int)app.slots.size()) {
        app.saveNote = "Please select an archive in the list first";
        return;
    }
    SaveSlot slot = app.slots[(size_t)app.selectedSlot];
    if (slot.format != "text") {
        app.saveNote = "is only valid for \"plain text (SiiNunit)\" archives; encrypted/binary archives are not written.";
        return;
    }
    double money = 0, xp = 0;
    bool setMoney = parseNumber(app.saveMoney, false, &money);
    bool setXp = parseNumber(app.saveXp, false, &xp);
    if (!setMoney && !setXp) {
        app.saveNote = "Please fill in the cash or experience to be written";
        return;
    }
    std::string err;
    std::wstring backup = backupSlot(slot, "before_text_edit", &err);
    if (backup.empty()) {
        app.saveNote = "Backup failed before writing:" + err;
        logLine(app.saveNote);
        return;
    }
    if (!patchTextSave(slot.gameSii, setMoney, (int64_t)money, setXp, (int64_t)xp, &err)) {
        app.saveNote = "writing failure:" + err;
        logLine(app.saveNote);
        return;
    }
    app.saveNote = fmt("has been written to the plaintext archive (backup: %s). Enter the game and load the archive \"%s\" to see the new values.",
                       W2U(backup).c_str(), W2U(slot.slotName).c_str());
    logLine(app.saveNote);
    app.backups = listBackups();
    selectSlot(app, app.selectedSlot);
}

// ==========================================================================
// 界面渲染
// ==========================================================================
namespace {

void renderValuePanel(AppState& app, PanelKind kind, const char* title, const char* hint,
                      VType* ptype, char* current, char* target, bool* lockVar) {
    const char* const titleId = title;
    ::ImGui::PushID((int)kind);
    ::ImGui::BeginChild(titleId, ImVec2(0, 0),
                        ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders,
                        ImGuiWindowFlags_None);

    // Card Header Bar
    ui::textf(ui::accent, "◆ %s", title);
    if (lockVar && *lockVar) {
        ui::next(220);
        ui::textf(ImVec4(0.22f, 0.85f, 0.52f, 1.0f), "[Lock held]");
    }
    ::ImGui::Separator();
    ::ImGui::Spacing();

    // 步骤 1: 扫描定位
    ui::textf(ImVec4(0.95f, 0.75f, 0.30f, 1.0f), "① Scan");
    ui::next(220);
    ::ImGui::TextWrapped("%s", "type");
    ui::next(220);
    typeCombo("##type", ptype);
    if (vtypeInfo(*ptype).isFloat) {
        ui::next(220);
        ::ImGui::TextWrapped("%s", "tolerance");
        ui::next(220);
        ::ImGui::SetNextItemWidth(70);
        ::ImGui::InputText("##tol", toleranceFor(app, kind), 16, ImGuiInputTextFlags_CharsDecimal);
    }

    ::ImGui::TextWrapped("%s", "Game current value");
    ui::next(220);
    ::ImGui::SetNextItemWidth(170);
    ::ImGui::InputTextWithHint("##current", "current value", current, 32, ImGuiInputTextFlags_CharsDecimal);
    ui::next(220);
    ::ImGui::BeginDisabled(app.busy.load());
    if (::ImGui::Button(" first scan ", ImVec2(120, 26))) startFirstScan(app, kind);
    ui::next(220);
    if (::ImGui::Button(" narrowed after changes ", ImVec2(120, 26))) startNextScan(app, kind, ScanMode::Exact);
    ui::next(220);
    if (::ImGui::Button(" has not changed and narrowed ", ImVec2(110, 26))) startNextScan(app, kind, ScanMode::Unchanged);
    ::ImGui::EndDisabled();

    ::ImGui::Spacing();

    // 步骤 2: 修改生效
    ui::textf(ui::accent, "② Write/Lock");
    ::ImGui::TextWrapped("%s", "target modification value");
    ui::next(220);
    ::ImGui::SetNextItemWidth(170);
    ::ImGui::InputTextWithHint("##target", "target value", target, 32, ImGuiInputTextFlags_CharsDecimal);
    ui::next(220);
    ::ImGui::BeginDisabled(app.busy.load());
    if (::ImGui::Button(" write modification (effective immediately) ", ImVec2(170, 26))) writePanelValues(app, kind);
    ui::next(220);
    if (::ImGui::Checkbox("Lock this value", lockVar)) togglePanelLock(app, kind, *lockVar);
    ui::next(220);
    if (::ImGui::Button("Unlock", ImVec2(80, 26))) {
        *lockVar = false;
        togglePanelLock(app, kind, false);
    }
    ::ImGui::EndDisabled();

    // 快速预设按钮
    if (kind == PanelKind::Money) {
        ui::textf(ImVec4(0.55f, 0.60f, 0.72f, 1.0f), "preset");
        ui::next(220);
        if (::ImGui::Button("+1 million")) {
            double cur = 0;
            parseNumber(target, false, &cur);
            if (cur <= 0) parseNumber(current, false, &cur);
            cur += 1000000.0;
            char buf[32];
            ::snprintf(buf, sizeof(buf), "%.0f", cur);
            ::strncpy_s(target, 32, buf, _TRUNCATE);
        }
        ui::next(220);
        if (::ImGui::Button("10 million")) ::strncpy_s(target, 32, "10000000", _TRUNCATE);
        ui::next(220);
        if (::ImGui::Button("50 million")) ::strncpy_s(target, 32, "50000000", _TRUNCATE);
        ui::next(220);
        if (::ImGui::Button("100 million")) ::strncpy_s(target, 32, "100000000", _TRUNCATE);
        ui::next(220);
        if (::ImGui::Button("999 million (upper limit)")) ::strncpy_s(target, 32, "999999999", _TRUNCATE);
    } else if (kind == PanelKind::Xp) {
        ui::textf(ImVec4(0.55f, 0.60f, 0.72f, 1.0f), "preset");
        ui::next(220);
        if (::ImGui::Button("5,000 (about level 10)")) ::strncpy_s(target, 32, "5000", _TRUNCATE);
        ui::next(220);
        if (::ImGui::Button("50,000 (about level 35)")) ::strncpy_s(target, 32, "50000", _TRUNCATE);
        ui::next(220);
        if (::ImGui::Button("150,000 (full level skill)")) ::strncpy_s(target, 32, "150000", _TRUNCATE);
        ui::next(220);
        if (::ImGui::Button("1,000,000 (one million experience points)")) ::strncpy_s(target, 32, "1000000", _TRUNCATE);
    }

    ::ImGui::Spacing();
    const std::string& st = statusFor(app, kind);
    if (!st.empty()) {
        ui::textf(ui::accent, "status: %s", st.c_str());
    } else {
        ui::textf(ui::muted, "Tip: %s", hint);
    }
    ::ImGui::EndChild();
    ::ImGui::PopID();
}

// ==========================================================================
// 金钱 / 经验：结构化定位（不依赖手动收窄）
// ==========================================================================
namespace {

// 结构校验时最多考察的候选数量（与旧版一致的上限）
constexpr size_t kEconomyProbeCap = 200000;

}  // namespace

// ---------------------------------------------------------------------------
// 工作线程结果 -> 界面状态。只在界面线程调用；锁内仅做拷贝/清空，不阻塞渲染。
// ---------------------------------------------------------------------------
namespace {

void consumePendingEconomy(AppState& app) {
    PendingEconomyResult result;
    {
        std::lock_guard<std::mutex> lock(app.resultMutex);
        if (!app.pendingEconomy.ready) return;
        result = std::move(app.pendingEconomy);
        app.pendingEconomy = PendingEconomyResult{};
    }
    if (!result.log.empty()) logLine(result.log);
    if (result.cancelled) {
        app.locateStatus = "Structured positioning has been cancelled.";
        return;
    }
    // 结构化数据统一在这里落地（工作线程不再直接写 located* / locateStatus）
    app.locatedMoneyAddress = result.moneyLocated ? result.moneyAddress : 0;
    app.locatedMoneyBank = result.moneyLocated ? result.moneyBank : 0;
    app.locatedMoneyValue = result.moneyLocated ? result.moneyValue : 0;
    app.locatedMoneyDetail = result.moneyLocated ? result.moneyDetail : std::string();
    app.locatedXpAddress = result.xpLocated ? result.xpAddress : 0;
    app.locatedEconomy = result.xpLocated ? result.economyObject : 0;
    app.locatedXpValue = result.xpLocated ? result.xpValue : 0;
    app.locatedXpDetail = result.xpLocated ? result.xpDetail : std::string();

    // 组装状态文案
    std::string status;
    if (result.moneyTruncated) {
        status = "positioning failed (safely closed):" + result.moneyDetail + "。";
    } else if (result.moneyAmbiguous) {
        status = "positioning failed (safely closed):" + result.moneyDetail +
                 ". There are multiple candidates for the same bank, which cannot be uniquely determined, and automatic writing has been rejected;"
                 "Please allow the amount to change before repositioning, or use \"Advanced: Manual Scan\" to manually filter.";
    } else if (result.moneyLocated) {
        status = fmt("has been positioned: money %s", formatInt(result.moneyValue).c_str());
        if (result.xpTruncated) {
            status += fmt("(Too many experience candidates and incomplete check: %s)", result.xpDetail.c_str());
        } else if (result.xpAmbiguous) {
            status += fmt("(There is ambiguity in experience positioning: %s, automatic writing of experience has been refused)",
                          result.xpDetail.c_str());
        } else if (result.xpLocated) {
            status += fmt(", experience %s", formatInt(result.xpValue).c_str());
        } else {
            status += "(experience not located)";
        }
    } else {
        status = "not located:" + result.moneyDetail;
        if (result.xpTruncated) status += ";Experience:" + result.xpDetail;
    }

    if (result.autoWriteMoney) {
        if (result.moneyWriteOk) {
            app.locatedMoneyValue = result.moneyWritten;
            status += fmt(". Money has been written directly = %s (readback consistent), no lock required.",
                          formatInt(result.moneyWritten).c_str());
        } else {
            status += ". Money is not written:" +
                      (result.moneyWriteError.empty()
                           ? std::string("The target amount is invalid")
                           : result.moneyWriteError);
        }
    }
    if (result.autoWriteXp) {
        if (result.xpWriteOk) {
            app.locatedXpValue = result.xpWritten;
            status += fmt(". Experience = %s has been written directly (readback consistent), no lock required.",
                          formatInt(result.xpWritten).c_str());
        } else {
            status += ". Experience not written:" +
                      (result.xpWriteError.empty()
                           ? std::string("Target experience must be a 32-bit integer")
                           : result.xpWriteError);
        }
    }
    app.locateStatus = std::move(status);
    logLine(app.locateStatus);
}

}  // namespace

}  // namespace

// ---------------------------------------------------------------------------
// 经济定位 / 直接写入（供界面与离线测试共用；声明见 ui.h）
// ---------------------------------------------------------------------------
void clearLocatedEconomy(AppState& app) {
    // 清除定位结果时必须一并解除锁定，否则后台保持线程会继续写这两个地址
    if (app.freezer) {
        if (app.locatedMoneyAddress) app.freezer->remove(app.locatedMoneyAddress);
        if (app.locatedXpAddress) app.freezer->remove(app.locatedXpAddress);
    }
    app.locatedMoneyAddress = 0;
    app.locatedMoneyBank = 0;
    app.locatedMoneyValue = 0;
    app.locatedMoneyDetail.clear();
    app.locatedXpAddress = 0;
    app.locatedEconomy = 0;
    app.locatedXpValue = 0;
    app.locatedXpDetail.clear();
    app.locateStatus = "Positioning results have been cleared.";
}

void startLocateEconomy(AppState& app) {
    if (const std::string reason = structuralWriteBlockReason(); !reason.empty()) {
        app.autoWriteMoneyAfterLocate = app.autoWriteXpAfterLocate = false;
        app.locateStatus = reason;
        logLine(reason);
        return;
    }
    if (app.busy.load()) {
        app.locateStatus = "A task is running. Wait for it to finish.";
        return;
    }
    if (!ensureAttached(app)) {
        app.autoWriteMoneyAfterLocate = app.autoWriteXpAfterLocate = false;
        app.locateStatus = "Not attached to game.";
        return;
    }
    const bool atsAuto = selectedGame() == GameId::Ats;
    double moneyValue = 0.0;
    if (atsAuto) {
        uint64_t imageBase = 0;
        const LayoutProfile* profile = nullptr;
        if (!verifyGameLayout(app.mem, &imageBase, &app.locateStatus, &profile)) {
            app.autoWriteMoneyAfterLocate = app.autoWriteXpAfterLocate = false;
            logLine(app.locateStatus);
            return;
        }
        if (!profile->bankVtableRva || !profile->economyVtableRva) {
            app.autoWriteMoneyAfterLocate = app.autoWriteXpAfterLocate = false;
            app.locateStatus = "Automatic money/XP editing is not verified for ATS 1.62. Engine tuning is supported.";
            logLine(app.locateStatus);
            return;
        }
        moneyValue = static_cast<double>(imageBase + profile->bankVtableRva);
    } else if (!parseNumber(app.moneyCurrent, false, &moneyValue)) {
        app.locateStatus = "Enter your current in-game money in Current game value.";
        return;
    }
    double xpValue = 0.0;
    const bool hasXp = parseNumber(app.xpCurrent, false, &xpValue);

    joinWorker(app);
    applyPending(app);
    consumePendingEconomy(app);  // 先消费上一轮未显示的结果，防止覆盖
    const int workers = app.workers;
    const size_t regionCap = maxRegionBytes(app);
    // 「直接改钱/直接改经验」的一键流程：把自动写入意图拍成快照带进工作线程。
    // 捕获后立刻清掉界面侧标志，避免上一轮的意图残留到下一次手动定位。
    const bool autoWriteMoney = app.autoWriteMoneyAfterLocate;
    const bool autoWriteXp = app.autoWriteXpAfterLocate;
    app.autoWriteMoneyAfterLocate = false;
    app.autoWriteXpAfterLocate = false;
    // 输入快照：工作线程绝不读取界面可编辑的字符缓冲区（moneyTarget/xpTarget 等），
    // 全部输入参数在启动线程前于界面线程复制成值；越界/非法值在此就判定并带进线程，
    // 绝不在工作线程里做越界转换。
    int64_t moneyTargetValue = 0;
    std::string moneyTargetError;
    const bool moneyTargetValid = parseMoneyTarget(app.moneyTarget, &moneyTargetValue,
                                                   &moneyTargetError);
    int32_t xpTargetValue = 0;
    std::string xpTargetError;
    const bool xpTargetValid = parseXpTarget(app.xpTarget, &xpTargetValue, &xpTargetError);
    // 写入闸门快照在真正写入前重新评估（见 worker 内的 gateAtWrite）
    app.busy.store(true);
    app.cancel.store(false);
    app.progressDone.store(0);
    app.progressTotal.store(2);
    app.progressNote = "Structured search: money";
    app.locateStatus = atsAuto ? "Searching for the ATS bank object..." : "Searching memory for a bank object matching your current money...";
    logLine("Starting structured money/XP search for the selected game");

    app.worker = std::thread([&app, moneyValue, atsAuto, xpValue, hasXp, workers, regionCap,
                              autoWriteMoney, autoWriteXp, moneyTargetValue, moneyTargetValid,
                              moneyTargetError, xpTargetValue, xpTargetValid,
                              xpTargetError]() {
        // 线程安全约定：本线程只触碰 app 的原子量（progressDone/progressTotal/cancel/
        // busy）与 resultMutex 保护的 pending 通道；对游戏内存只做定位线程内的读写。
        ScanOptions opt;
        opt.workers = workers;
        opt.maxRegionSize = regionCap;
        auto progressFn = [&app](uint64_t done, uint64_t total) {
            app.progressDone.store(done);
            app.progressTotal.store(total ? total : 1);
        };
        auto cancelFn = [&app]() { return app.cancel.load(); };

        PendingEconomyResult result;

        // 1) 按金额找 bank 对象（最高分候选唯一且候选完整才可用；歧义/截断一律失败关闭）
        ScanSession moneyScan(app.mem, VType::Int64, 8, 0.0);
        const uint64_t moneyHits =
            moneyScan.firstScan(moneyValue, opt, progressFn, cancelFn);
        if (!app.cancel.load() && moneyHits > 0) {
            std::vector<uint64_t> moneyFields = moneyScan.addresses();
            if (atsAuto) for (auto& address : moneyFields) address += ats_economy_offsets::kMoney;
            const CandidatePick pick = pickBestBankCandidate(app.mem, moneyFields, kEconomyProbeCap);
            if (pick.truncated) {
                result.moneyTruncated = true;
                result.moneyDetail =
                    fmt("%llu money candidates exceed the limit of %llu (checked only %llu): "
                        "Cannot exclude equally or better matching bank objects; write refused. "
                        "Reduce the scan range or narrow the candidates and try again",
                        (unsigned long long)moneyHits, (unsigned long long)kEconomyProbeCap,
                        (unsigned long long)pick.examined);
            } else if (pick.unique) {
                result.moneyLocated = true;
                result.moneyAddress = pick.fieldAddress;
                result.moneyBank = pick.object;
                result.moneyValue = pick.value;
                result.moneyDetail = fmt("%llu money candidates; one verified bank object (score %d)",
                                         (unsigned long long)moneyHits, pick.score);
            } else if (pick.probed) {
                result.moneyAmbiguous = true;
                result.moneyDetail =
                    fmt("%llu money candidates; %d verified objects tied for best score",
                        (unsigned long long)moneyHits, pick.tied);
            } else {
                result.moneyDetail = fmt("%llu money candidates; none passed bank validation",
                                         (unsigned long long)moneyHits);
                if (moneyFields.size() == 1) {
                    ProbeResult rejected;
                    if (!probeBankObject(app.mem, moneyFields.front(), &rejected) && !rejected.detail.empty())
                        result.moneyDetail += "：" + rejected.detail;
                }
            }
        } else if (!app.cancel.load()) {
            result.moneyDetail = atsAuto ? "ATS bank object not found. Load a profile and try again." : "Current money not found. Change your balance in game and try again.";
        }

        app.progressDone.store(1);
        // 阶段性进度说明走 pending 通道（旧实现直接写 app.progressNote，属数据竞争）
        postResult(app, PanelKind::Money, std::string(), std::string(),
                   "Structured search: economy object from bank pointer");

        // 2) 由「指向 bank 的指针」反查经济对象（经验地址随之确定）。
        //    候选必须真的引用已定位的 bank，且唯一同分才可用。
        if (result.moneyLocated && !app.cancel.load()) {
            ScanSession bankRefScan(app.mem, VType::Int64, 8, 0.0);
            const uint64_t refHits =
                bankRefScan.firstScan((double)result.moneyBank, opt, progressFn, cancelFn);
            if (!app.cancel.load() && refHits > 0) {
                const CandidatePick pick = pickBestEconomyFromBankRefs(
                    app.mem, bankRefScan.addresses(), result.moneyBank, kEconomyProbeCap);
                if (pick.truncated) {
                    result.xpTruncated = true;
                    result.xpDetail =
                        fmt("bank refers to %llu, which exceeds the upper limit of %llu that can be checked this time (only the first %llu were checked):"
                            "There is no guarantee that there is no economic object with the same score or higher, and experience writing has been refused.",
                            (unsigned long long)refHits, (unsigned long long)kEconomyProbeCap,
                            (unsigned long long)pick.examined);
                } else if (pick.unique) {
                    result.xpLocated = true;
                    result.xpAddress = pick.fieldAddress;
                    result.economyObject = pick.object;
                    result.xpValue = pick.value;
                    result.xpDetail =
                        fmt("is referenced by the bank at %llu to confirm the economic object (check %d points, the references are consistent)",
                            (unsigned long long)refHits, pick.score);
                } else if (pick.probed) {
                    result.xpAmbiguous = true;
                    result.xpDetail =
                        fmt("bank refers to %llu, and there are %d candidates for the same economic object.",
                            (unsigned long long)refHits, pick.tied);
                } else {
                    result.xpDetail = "The bank reference was found, but the economic object structure check failed.";
                }
            } else if (!app.cancel.load()) {
                result.xpDetail = "No reference to bank found (economic object may not have been loaded yet)";
            }
        }

        // 3) 若仍没有经验地址，且用户填了经验值，则按经验值再定位一次。
        //    xpScan.addresses()[i] 本身就是经验字段地址（economy + 0x780），
        //    直接传给 probe —— 这里不能再加 kEconomyExperiencePoints（历史缺陷：
        //    会让对象基址错位）。已定位银行对象时还必须验证经济对象引用它。
        if (!result.xpLocated && hasXp && !app.cancel.load()) {
            ScanSession xpScan(app.mem, VType::Int32, 4, 0.0);
            const uint64_t xpHits = xpScan.firstScan(xpValue, opt, progressFn, cancelFn);
            if (!app.cancel.load() && xpHits > 0) {
                const uint64_t requiredBank = result.moneyLocated ? result.moneyBank : 0;
                const CandidatePick pick = pickBestEconomyFromXpFields(
                    app.mem, xpScan.addresses(), requiredBank, kEconomyProbeCap);
                if (pick.truncated) {
                    result.xpTruncated = true;
                    result.xpDetail =
                        fmt("There are %llu experience value candidates, exceeding the upper limit of %llu that can be checked this time (only the first %llu were checked):"
                            "There is no guarantee that there is no economic object with the same score or higher, and experience writing has been refused.",
                            (unsigned long long)xpHits, (unsigned long long)kEconomyProbeCap,
                            (unsigned long long)pick.examined);
                } else if (pick.unique) {
                    result.xpLocated = true;
                    result.xpAddress = pick.fieldAddress;
                    result.economyObject = pick.object;
                    result.xpValue = pick.value;
                    result.xpDetail =
                        fmt("is positioned according to experience value: %llu candidates, the only trusted economic object (check %d points %s)",
                            (unsigned long long)xpHits, pick.score,
                            requiredBank ? ", confirmed reference to the located bank object" : "");
                } else if (pick.probed) {
                    result.xpAmbiguous = true;
                    result.xpDetail =
                        fmt("is positioned based on experience value: %llu candidates, %d candidates for the same economic object",
                            (unsigned long long)xpHits, pick.tied);
                } else {
                    result.xpDetail =
                        fmt("experience value candidates %llu, but the structure verification failed %s",
                            (unsigned long long)xpHits,
                            requiredBank ? "(or the located bank object is not referenced)" : "");
                }
            } else if (!app.cancel.load()) {
                result.xpDetail = "No value consistent with the current experience was found in the memory.";
            }
        }

        // 4) 「直接改钱 / 直接改经验」：用启动前复制的目标值快照做验证写入。
        // 4) 「直接改钱 / 直接改经验」：用启动前复制的目标值快照做验证写入。
        //    扫描可能持续数十秒，因此在这里（真正写入前一刻）重新评估写入闸门。
        const std::string gateAtWrite = app.cancel.load() ? std::string() : writeGateReason(app);
        if (!app.cancel.load() && autoWriteMoney && result.moneyLocated) {
            result.autoWriteMoney = true;
            // 扫描可能持续数十秒：真正写入前用最新状态复核写入闸门
            if (!gateAtWrite.empty()) {
                result.moneyWriteError = "online protection:" + gateAtWrite + "(not written)";
            } else if (moneyTargetValid) {
                std::string error;
                if (writeBankMoneyGuarded(app, result.moneyAddress, result.moneyBank,
                                          (int64_t)moneyTargetValue, &error)) {
                    result.moneyWriteOk = true;
                    result.moneyWritten = (int64_t)moneyTargetValue;
                } else {
                    result.moneyWriteError = error;
                }
            } else {
                result.moneyWriteError = moneyTargetError.empty()
                                             ? std::string("\"The amount to be changed\" is not a valid number")
                                             : moneyTargetError;
            }
        }
        if (!app.cancel.load() && autoWriteXp && result.xpLocated) {
            result.autoWriteXp = true;
            if (!gateAtWrite.empty()) {
                result.xpWriteError = "online protection:" + gateAtWrite + "(not written)";
            } else if (xpTargetValid) {
                std::string error;
                if (writeXpGuarded(app, result.xpAddress, result.economyObject,
                                   (int32_t)xpTargetValue, &error)) {
                    result.xpWriteOk = true;
                    result.xpWritten = (int32_t)xpTargetValue;
                } else {
                    result.xpWriteError = error;
                }
            } else {
                result.xpWriteError = xpTargetError.empty()
                                          ? std::string("\"Experience to be changed to\" is not a valid 32-bit integer")
                                          : xpTargetError;
            }
        }

        result.cancelled = app.cancel.load();
        result.log = "Structured Positioning: Money " +
                     (result.moneyLocated
                          ? fmt("0x%llX", (unsigned long long)result.moneyAddress)
                          : std::string("not located")) +
                     ";Experience " +
                     (result.xpLocated ? fmt("0x%llX", (unsigned long long)result.xpAddress)
                                       : std::string("not located"));
        {
            std::lock_guard<std::mutex> lock(app.resultMutex);
            result.ready = true;
            app.pendingEconomy = std::move(result);
        }
        app.progressDone.store(app.progressTotal.load());
        app.busy.store(false);
    });
}

// ---------------------------------------------------------------------------
// 「直接改钱 / 直接改经验」一键入口：已定位 → 立即写入；未定位 → 自动先定位，
// 定位成功后由工作线程自动写入。全程免锁定。（声明见 ui.h）
// ---------------------------------------------------------------------------

void directModifyMoney(AppState& app) {
    if (const std::string reason = structuralWriteBlockReason(); !reason.empty()) {
        app.locateStatus = reason;
        logLine(reason);
        return;
    }
    if (app.busy.load()) {
        app.locateStatus = "A task is running. Wait for it to finish.";
        return;
    }
    int64_t target = 0;
    std::string targetError;
    if (!parseMoneyTarget(app.moneyTarget, &target, &targetError)) {
        app.locateStatus = "Please fill in the valid amount in the \"Amount to be changed\" first:" + targetError;
        return;
    }
    const std::string gate = writeGateReason(app);
    if (!gate.empty()) {
        app.locateStatus = "Write denied:\nWhen using" + gate;
        logLine(app.locateStatus);
        return;
    }
    if (app.locatedMoneyAddress) {
        writeLocatedMoney(app, false);
        return;
    }
    double current = 0.0;
    if (selectedGame() == GameId::Ets2 && !parseNumber(app.moneyCurrent, false, &current)) {
        app.locateStatus = "for the first time, you need to be guided and positioned: fill in the current amount displayed in the game into \"Game Current Value\"."
                           "Click \"Change Money Directly\" (you don't need to fill it out again after that).";
        return;
    }
    app.autoWriteMoneyAfterLocate = true;
    app.locateStatus = "has not been positioned yet: Automatically locate first (about 10~25 seconds), and write the target amount directly after success.";
    startLocateEconomy(app);
}

void directModifyXp(AppState& app) {
    if (const std::string reason = structuralWriteBlockReason(); !reason.empty()) {
        app.locateStatus = reason;
        logLine(reason);
        return;
    }
    if (app.busy.load()) {
        app.locateStatus = "A task is running. Wait for it to finish.";
        return;
    }
    int32_t target = 0;
    std::string targetError;
    if (!parseXpTarget(app.xpTarget, &target, &targetError)) {
        app.locateStatus = "Please fill in the valid experience in the \"Experience to be changed to\" first:" + targetError;
        return;
    }
    const std::string gate = writeGateReason(app);
    if (!gate.empty()) {
        app.locateStatus = "Write denied:\nWhen using" + gate;
        logLine(app.locateStatus);
        return;
    }
    if (app.locatedXpAddress) {
        writeLocatedXp(app, false);
        return;
    }
    double current = 0.0;
    if (selectedGame() == GameId::Ets2 && !parseNumber(app.moneyCurrent, false, &current)) {
        app.locateStatus = "The experience address is obtained together with the money positioning: fill in the current amount displayed in the game into \"Game Current Value\"."
                           "Click \"Change experience directly\".";
        return;
    }
    app.autoWriteXpAfterLocate = true;
    app.locateStatus = "has not been positioned yet: automatically locate it first (about 10~25 seconds), and write the target experience directly after success.";
    startLocateEconomy(app);
}

// 写入 / 锁定定位到的地址。
// 直接写入走「结构校验 + 范围检查 + 回读比对」的验证路径（writeBankMoneyVerified），
// 写的是游戏自己的权威存储：UI 显示、收支结算、自动存档都读这个字段，
// 因此一次性写入即可持久，**不需要后台锁定**。
void writeLocatedMoney(AppState& app, bool lock) {
    if (!app.locatedMoneyAddress) {
        app.locateStatus = "There is no positioning result yet: please fill in the current amount and click \"Change Money Directly\".";
        return;
    }
    int64_t money = 0;
    std::string targetError;
    if (!parseMoneyTarget(app.moneyTarget, &money, &targetError)) {
        // 目标数值越界/非法：不得清除定位结果，也不能写入
        app.locateStatus = "is not written:" + targetError + "(The positioning result is retained and can be modified and tried again).";
        logLine(app.locateStatus);
        return;
    }
    // 真正执行写入前复核写入闸门（扫描/等待期间可能进入联机）
    if (lock) {
        ProbeResult probe;
        if (!probeBankObject(app.mem, app.locatedMoneyAddress, &probe) ||
            probe.object != app.locatedMoneyBank) {
            clearLocatedEconomy(app);
            app.locateStatus = "The positioning address has expired (the archive may have been changed or reloaded), the positioning result has been cleared, please relocate.";
            return;
        }
        if (!app.freezer) {
            app.locateStatus = "Locker is not ready yet (please attach game first).";
            return;
        }
        const std::string gate = writeGateReason(app);
        if (!gate.empty()) {
            app.locateStatus = "Write denied:\nWhen using" + gate + "(positioning results retained)";
            logLine(app.locateStatus);
            return;
        }
        app.freezer->addInt(app.locatedMoneyAddress, VType::Int64, money);
        app.locateStatus = fmt("Locked money = %s (optional insurance: prevents in-game income and expenditure from changing the amount;"
                               "Generally, you can use \"Change Money Directly\" without locking)",
                               formatInt(money).c_str());
    } else {
        std::string error;
        if (!writeBankMoneyGuarded(app, app.locatedMoneyAddress, app.locatedMoneyBank, money,
                                   &error)) {
            if (error.find("online protection") != std::string::npos) {
                // 闸门拦截：不是地址失效，定位结果必须保留
                app.locateStatus = "Write denied:\nWhen using" + error + "(positioning results retained)";
            } else if (error.find("structure verification failed") != std::string::npos) {
                clearLocatedEconomy(app);
                app.locateStatus = "Direct writing failed:" + error + "(Location result has been cleared, please relocate)";
            } else {
                app.locateStatus = "Direct writing failed:" + error + "(The positioning result is retained and can be retried)";
            }
            logLine(app.locateStatus);
            return;
        }
        app.locatedMoneyValue = money;
        app.locateStatus = fmt("has been written directly to money = %s (readback consistent). No locking required: this is what the game reads"
                               "address, automatic archiving will write it into the archive.",
                               formatInt(money).c_str());
    }
    logLine(app.locateStatus);
}

void writeLocatedXp(AppState& app, bool lock) {
    if (!app.locatedXpAddress) {
        app.locateStatus = "The experience address has not been located yet (fill in the current amount in \"Game Current Value\" and click \"Change Money Directly\"."
                           "will also locate experience when locating money).";
        return;
    }
    int32_t xp = 0;
    std::string targetError;
    if (!parseXpTarget(app.xpTarget, &xp, &targetError)) {
        // 越界/非整数绝不先转换：定位结果保留
        app.locateStatus = "is not written:" + targetError + "(The positioning result is retained and can be modified and tried again).";
        logLine(app.locateStatus);
        return;
    }
    const std::string gate = writeGateReason(app);
    if (lock) {
        ProbeResult probe;
        if (!probeEconomyObject(app.mem, app.locatedXpAddress, &probe) ||
            probe.object != app.locatedEconomy) {
            clearLocatedEconomy(app);
            app.locateStatus = "The experience address has expired and the positioning results have been cleared. Please relocate.";
            return;
        }
        if (!app.freezer) {
            app.locateStatus = "Locker is not ready yet (please attach game first).";
            return;
        }
        if (!gate.empty()) {
            app.locateStatus = "Write denied:\nWhen using" + gate + "(positioning results retained)";
            logLine(app.locateStatus);
            return;
        }
        app.freezer->addInt(app.locatedXpAddress, VType::Int32, xp);
        app.locateStatus = fmt("Locked experience = %s (optional insurance; generally use \"directly change experience\" without locking)",
                               formatInt(xp).c_str());
    } else {
        std::string error;
        if (!writeXpGuarded(app, app.locatedXpAddress, app.locatedEconomy, xp, &error)) {
            if (error.find("online protection") != std::string::npos) {
                app.locateStatus = "Write denied:\nWhen using" + error + "(positioning results retained)";
            } else if (error.find("structure verification failed") != std::string::npos) {
                clearLocatedEconomy(app);
                app.locateStatus = "Direct writing failed:" + error + "(Location result has been cleared, please relocate)";
            } else {
                app.locateStatus = "Direct writing failed:" + error + "(The positioning result is retained and can be retried)";
            }
            logLine(app.locateStatus);
            return;
        }
        app.locatedXpValue = xp;
        app.locateStatus = fmt("has been written directly to experience = %s (readback consistent). No locking required: upgrades and skill points are settled immediately,"
                               "Autosave will write it into the archive.",
                               formatInt(xp).c_str());
    }
    logLine(app.locateStatus);
}

void renderMoneyTab(AppState& app) {
    if (!supportsVerifiedStructures(selectedGame())) {
        ::ImGui::TextWrapped("ATS automatic money/experience positioning has not been adapted yet. Please use the generic scan below, or modify the supported clear text fields in Archive Management.");
        renderValuePanel(app, PanelKind::Money, "Cash (manual scan)", "Enter the current amount and narrow it after change.",
                         &app.moneyType, app.moneyCurrent, app.moneyTarget, &app.moneyLock);
        renderValuePanel(app, PanelKind::Xp, "experience (manual scan)", "Enter the current experience and narrow it after change.",
                         &app.xpType, app.xpCurrent, app.xpTarget, &app.xpLock);
        return;
    }
    const bool inputsLocked = app.busy.load();
    if (!app.locatedMoneyAddress && selectedGame() == GameId::Ets2) {
        ui::beginCard("##economy_seed", "first positioning");
        ui::text(ui::muted, "Fill in the current amount displayed in the game, and then apply the target funds or experience. First fix takes approximately 10–25 seconds.");
        ::ImGui::BeginDisabled(inputsLocked);
        ::ImGui::SetNextItemWidth(std::min(340.0f, ::ImGui::GetContentRegionAvail().x));
        ::ImGui::InputTextWithHint("##current", "Game current amount", app.moneyCurrent, 32,
            ImGuiInputTextFlags_CharsDecimal);
        ::ImGui::EndDisabled();
        ui::endCard();
        ::ImGui::Spacing();
    }
    const int columns = ::ImGui::GetContentRegionAvail().x >= 760 ? 2 : 1;
    if (::ImGui::BeginTable("##economy_cards", columns, ImGuiTableFlags_SizingStretchSame)) {
        ::ImGui::TableNextColumn();
        ui::beginCard("##money_card", "account funds");
        ui::heading(app.locatedMoneyAddress ? formatInt(app.locatedMoneyValue).c_str() : "—", 30);
        ::ImGui::TextDisabled(app.locatedMoneyAddress ? "Positioned amount" : "waiting for positioning");
        ::ImGui::Spacing();
        ::ImGui::TextUnformatted("target amount");
        ::ImGui::BeginDisabled(inputsLocked);
        ::ImGui::SetNextItemWidth(-1);
        ::ImGui::InputTextWithHint("##target_money", "Enter target amount", app.moneyTarget, 32,
            ImGuiInputTextFlags_CharsDecimal);
        if (ui::primaryButton("Application target amount")) directModifyMoney(app);
        ::ImGui::Spacing();
        if (::ImGui::Button("+1 million")) {
            double cur = 0;
            parseNumber(app.moneyTarget, false, &cur);
            if (cur <= 0) parseNumber(app.moneyCurrent, false, &cur);
            char buf[32];
            ::snprintf(buf, sizeof(buf), "%.0f", cur + 1000000.0);
            ::strncpy_s(app.moneyTarget, 32, buf, _TRUNCATE);
        }
        ui::nextButton("10 million");
        if (::ImGui::Button("10 million")) ::strncpy_s(app.moneyTarget, 32, "10000000", _TRUNCATE);
        ui::nextButton("50 million");
        if (::ImGui::Button("50 million")) ::strncpy_s(app.moneyTarget, 32, "50000000", _TRUNCATE);
        ui::nextButton("100 million");
        if (::ImGui::Button("100 million")) ::strncpy_s(app.moneyTarget, 32, "100000000", _TRUNCATE);
        ui::nextButton("999 million");
        if (::ImGui::Button("999 million")) ::strncpy_s(app.moneyTarget, 32, "999999999", _TRUNCATE);
        ::ImGui::EndDisabled();
        ui::endCard();
        ::ImGui::TableNextColumn();
        ui::beginCard("##xp_card", "driving experience");
        ui::heading(app.locatedXpAddress ? formatInt(app.locatedXpValue).c_str() : "—", 30);
        ::ImGui::TextDisabled("XP / Positioned together with funds");
        ::ImGui::Spacing();
        ::ImGui::TextUnformatted("Target Experience");
        ::ImGui::BeginDisabled(app.busy.load());
        ::ImGui::SetNextItemWidth(-1);
        ::ImGui::InputTextWithHint("##target_xp", "Enter target experience", app.xpTarget, 32,
            ImGuiInputTextFlags_CharsDecimal);
        if (ui::primaryButton("application target experience")) directModifyXp(app);
        ::ImGui::Spacing();
        if (::ImGui::Button("5,000 / about level 10")) ::strncpy_s(app.xpTarget, 32, "5000", _TRUNCATE);
        ui::nextButton("50,000 / about level 35");
        if (::ImGui::Button("50,000 / about level 35")) ::strncpy_s(app.xpTarget, 32, "50000", _TRUNCATE);
        ui::nextButton("150,000 / full level skill");
        if (::ImGui::Button("150,000 / full level skill")) ::strncpy_s(app.xpTarget, 32, "150000", _TRUNCATE);
        ui::nextButton("1,000,000");
        if (::ImGui::Button("1,000,000")) ::strncpy_s(app.xpTarget, 32, "1000000", _TRUNCATE);
        ::ImGui::EndDisabled();
        ui::endCard();
        ::ImGui::EndTable();
    }
    ::ImGui::Spacing();
    ui::beginCard("##locate_economy", "positioning and operating status");
    const bool hasMoney = app.locatedMoneyAddress != 0;
    const bool hasXp = app.locatedXpAddress != 0;
    if (hasMoney) {
        ui::textf(ImVec4(0.40f, 0.85f, 0.55f, 1.0f),
                             "Money 0x%llX = %s", (unsigned long long)app.locatedMoneyAddress,
                             formatInt(app.locatedMoneyValue).c_str());
        if (!app.locatedMoneyDetail.empty()) {
            ui::textf(ui::muted, "%s",
                                 app.locatedMoneyDetail.c_str());
        }
    }
    if (hasXp) {
        ui::textf(ImVec4(0.40f, 0.85f, 0.55f, 1.0f),
                             "experience 0x%llX = %s", (unsigned long long)app.locatedXpAddress,
                             formatInt(app.locatedXpValue).c_str());
        if (!app.locatedXpDetail.empty()) {
            ui::textf(ui::muted, "%s",
                                 app.locatedXpDetail.c_str());
        }
    }
    ui::textf(ui::accent, "%s", app.locateStatus.c_str());

    // 可选保险：锁定（一般不需要）。直接写权威地址后游戏不会改回去；
    // 只有想「钉死」数值不受游戏内收支影响时才用。
    if (ImGui::CollapsingHeader("Optional: locking insurance (generally not required)")) {
        ::ImGui::BeginDisabled(app.busy.load() || !hasMoney);
        if (::ImGui::Button(" Lock money ", ImVec2(110, 0))) writeLocatedMoney(app, true);
        ::ImGui::EndDisabled();
        ::ImGui::SameLine();
        ::ImGui::BeginDisabled(app.busy.load() || !hasXp);
        if (::ImGui::Button(" lock experience ", ImVec2(110, 0))) writeLocatedXp(app, true);
        ::ImGui::EndDisabled();
        ::ImGui::SameLine();
        if (::ImGui::Button(" Unlock ", ImVec2(100, 0))) {
            if (app.freezer) {
                if (app.locatedMoneyAddress) app.freezer->remove(app.locatedMoneyAddress);
                if (app.locatedXpAddress) app.freezer->remove(app.locatedXpAddress);
            }
            app.locateStatus = "The money/experience lock has been unlocked (values written directly are not affected).";
        }
        ui::textf(ui::muted,
                             "Lock = Write the value back every 0.25 seconds to offset the real changes in the income and expenditure in the game."
                             "is only optional and is not required for direct modification itself.");
        if (app.freezer && app.freezer->blocked() > 0) {
            ui::textf(ImVec4(0.95f, 0.40f, 0.40f, 1.0f),
                                 "Locked writes have been intercepted by the online gate %llu times: %s (unlocked entries remain,"
                                 "automatically resumes writing after exiting online)",
                                 (unsigned long long)app.freezer->blocked(),
                                 app.freezer->blockedReason().c_str());
        }
    }
    ::ImGui::EndChild();

    ::ImGui::Spacing();

    // ---------------- 兼容保留：重新定位 / 清除 ----------------
    if (ImGui::CollapsingHeader("Maintenance: Relocation/Clear (for use after changing archives or loading files)")) {
        ::ImGui::BeginDisabled(app.busy.load());
        if (::ImGui::Button(" Relocation only (not writing) ", ImVec2(170, 26))) startLocateEconomy(app);
        ::ImGui::SameLine();
        if (::ImGui::Button(" Clear positioning results ", ImVec2(140, 26))) clearLocatedEconomy(app);
        ::ImGui::EndDisabled();
        ui::textf(ui::muted,
                             "The positioning address is bound to the current process: after changing the archive, reading the file or restarting the game, it will automatically be written before directly writing."
                             "rechecks the structure and will prompt for relocation when it fails.");
    }

    ::ImGui::Spacing();

    // ---------------- 高级：手动扫描（兼容性排查用） ----------------
    if (ImGui::CollapsingHeader("Advanced: Manual scanning and narrowing (for compatibility check, generally not needed)")) {
        ::ImGui::TextWrapped("Cheat Engine-style full memory scan: only used for troubleshooting when direct modification and positioning fails."
                             "Note that the scan hit may contain a mirror copy, and the wrong copy will be changed back by the game (this is"
                             "The reason why the old version needs to be \"locked\").");
        ::ImGui::Spacing();
        renderValuePanel(app, PanelKind::Money, "Cash (money)",
                         "Fill in the cash in the game → Scan for the first time; fill in the new value after the amount changes → Narrow after the change.",
                         &app.moneyType, app.moneyCurrent, app.moneyTarget, &app.moneyLock);
        ::ImGui::Spacing();
        renderValuePanel(app, PanelKind::Xp, "experience value",
                         "32-bit integer; changes will also upgrade and give skill points.",
                         &app.xpType, app.xpCurrent, app.xpTarget, &app.xpLock);
    }
}

void renderVehicleTabImpl(AppState& app) {
    const bool attached = app.mem.isOpen() && app.mem.alive();
    if (attached && ::GetTickCount64() - app.telemetryCheckedAt >= 1000) {
        app.telemetryCheckedAt = ::GetTickCount64();
        app.telemetryAvailable = readTelemetry(&app.telemetry, &app.telemetryError);
    }
    const bool telemetry = attached && app.telemetryAvailable && app.telemetry.sdkActive;
    syncAutoVehicleLocks(app);
    if (app.engineTuner) {
        const auto status = app.engineTuner->status();
        if (app.engineTuner->active()) app.engineStatus = status;
        else if (app.enginePowerOption != 0) {
            app.enginePowerOption = 0;
            app.engineRaiseLimit = false;
            app.engineStateText = app.engineTuner->exportState();
            app.engineStatus = status;
        }
    }
    if (app.engineDraftSourceOption != app.enginePowerOption ||
        app.engineDraftSourceRaiseLimit != app.engineRaiseLimit ||
        app.engineDraftPid != app.pid || app.engineDraftGame != selectedGame()) {
        app.engineDraftOption = app.engineDraftSourceOption = app.enginePowerOption;
        app.engineDraftRaiseLimit = app.engineDraftSourceRaiseLimit = app.engineRaiseLimit;
        app.engineDraftPid = app.pid;
        app.engineDraftGame = selectedGame();
    }

    ui::beginCard("##vehicle_metrics");
    if (::ImGui::BeginTable("##metrics", 3, ImGuiTableFlags_SizingStretchSame)) {
        ::ImGui::TableNextColumn();
        ::ImGui::TextDisabled("Remaining fuel / battery");
        ui::heading(telemetry ? fmt("%.1f%%", 100.0f * std::clamp(app.telemetry.fuel / app.telemetry.fuelCapacity, 0.0f, 1.0f)).c_str() : "—", 29);
        ::ImGui::TextDisabled(telemetry ? fmt("game energy value %.0f / %.0f", app.telemetry.fuel, app.telemetry.fuelCapacity).c_str() : "Waiting for game telemetry");
        if (telemetry && app.telemetry.fuelCapacity > 0)
            ::ImGui::ProgressBar(std::clamp(app.telemetry.fuel / app.telemetry.fuelCapacity, 0.0f, 1.0f), ImVec2(-1, 4), "");
        ::ImGui::TableNextColumn();
        ::ImGui::TextDisabled("Maximum component wear");
        const float wear = *std::max_element(app.telemetry.wear.begin(), app.telemetry.wear.end());
        ui::heading(telemetry ? fmt("%.1f%%", wear * 100).c_str() : "—", 29);
        ::ImGui::TextDisabled(telemetry ? "engine/gearbox/body/wheel" : "No valid vehicle data");
        ::ImGui::TableNextColumn();
        ::ImGui::TextDisabled("Current power multiplier");
        ui::heading(fmt("%.2f ×", kEngineScales[std::clamp(app.enginePowerOption, 0, 3)]).c_str(), 29);
        ::ImGui::TextDisabled(app.enginePowerOption == 0 ? "Stock baseline" : "Maintained in background");
        ::ImGui::EndTable();
    }
    ui::endCard();
    ::ImGui::Spacing();
    if (!supportsVerifiedStructures(selectedGame())) {
        ui::text(ui::accent, structuralWriteBlockReason().c_str());
        if (::ImGui::CollapsingHeader("Manual fuel level and damage scan")) {
            renderValuePanel(app, PanelKind::Fuel, "fuel level (manual scan)", "continues to narrow according to game changes after scanning.",
                &app.fuelType, app.fuelCurrent, app.fuelTarget, &app.fuelLock);
            renderValuePanel(app, PanelKind::Damage, "damage (manual scan)", "Confirm the type and address before writing.",
                &app.damageType, app.damageCurrent, app.damageTarget, &app.damageLock);
        }
        return;
    }
    const int columns = ::ImGui::GetContentRegionAvail().x >= 820 ? 2 : 1;
    if (::ImGui::BeginTable("##vehicle_controls", columns, ImGuiTableFlags_SizingStretchSame)) {
        ::ImGui::TableNextColumn();
        ui::beginCard("##engine_power", "Engine tuning");
        ::ImGui::TextDisabled("Target multiplier / click Apply to activate");
        ui::heading(fmt("%.2f ×", kEngineScales[std::clamp(app.engineDraftOption, 0, 3)]).c_str(), 36);
        ::ImGui::SetNextItemWidth(-1);
        ::ImGui::SliderInt("##power_draft", &app.engineDraftOption, 0, 3, "", ImGuiSliderFlags_NoInput);
        static const char* presets[] = {"Stock", "1.25×", "1.50×", "2.00×"};
        for (int i = 0; i < 4; ++i) {
            if (i) ui::nextButton(presets[i]);
            if (ui::choice(presets[i], app.engineDraftOption == i)) app.engineDraftOption = i;
        }
        ::ImGui::Spacing();
        ::ImGui::Checkbox("RPM limit +10%", &app.engineDraftRaiseLimit);
        ::ImGui::Spacing();
        if (ui::primaryButton("Apply tuning")) {
            applyEnginePower(app, app.engineDraftOption, app.engineDraftRaiseLimit);
            app.engineDraftSourceOption = -1;
        }
        ui::nextButton("Restore stock");
        if (::ImGui::Button("Restore stock")) {
            applyEnginePower(app, 0, false);
            app.engineDraftSourceOption = -1;
        }
        ::ImGui::Spacing();
        ui::text(ui::muted, app.engineStatus.c_str());
        if (app.engineTuner) {
        // 如实区分恢复记录状态：修改仍存在 / 尚未核对 / 对象已明确失效 / 已确认恢复
        const char* recordState = "Recovery confirmed (no record to be recovered)";
        ImVec4 recordColor(0.45f, 0.85f, 0.60f, 1.0f);
        if (app.engineTuner->hasUnverifiedImportedRecord()) {
            recordState = "The recovery record of the last run has not been checked (retained; the amplification value will not be regarded as the original factory value)";
            recordColor = ImVec4(0.95f, 0.75f, 0.30f, 1.0f);
        } else if (app.engineTuner->active()) {
            recordState = "modification still exists (maintained in the background)";
            recordColor = ImVec4(0.95f, 0.55f, 0.35f, 1.0f);
        } else if (app.engineTuner->hasInvalidatedRecord()) {
            recordState = "The object has clearly expired (the value does not match the record of this program, the record has been discarded and has not been written)";
            recordColor = ImVec4(0.95f, 0.45f, 0.45f, 1.0f);
        }
        ui::text(recordColor, recordState);
    }

        ui::endCard();
        ::ImGui::TableNextColumn();
        ui::beginCard("##auto_vehicle", "Driving assists");
        bool fuel = app.autoFuelLock;
        if (::ImGui::Checkbox("Unlimited fuel", &fuel)) toggleAutoVehicleLock(app, true, fuel);
        ui::text(ui::muted, app.fuelStatus.c_str());
        ::ImGui::Spacing();
        bool electric = app.autoElectricLock;
        if (::ImGui::Checkbox("Unlimited battery", &electric)) toggleAutoVehicleLock(app, true, electric, true);
        ui::text(ui::muted, app.electricStatus.c_str());
        ::ImGui::TextWrapped("Used by electric vehicles; mutually exclusive with unlimited fuel capacity. Ctrl+F6 to switch.");
        ::ImGui::Spacing();
        ::ImGui::Separator();
        ::ImGui::Spacing();
        bool damage = app.autoDamageLock;
        if (::ImGui::Checkbox("Prevent damage", &damage)) toggleAutoVehicleLock(app, false, damage);
        ui::text(ui::muted, app.damageStatus.c_str());
        ::ImGui::Spacing();
        if (::ImGui::Button("Enable all")) {
            if (!app.autoFuelLock && !app.autoElectricLock) toggleAutoVehicleLock(app, true, true);
            if (!app.autoDamageLock) toggleAutoVehicleLock(app, false, true);
        }
        ui::nextButton("Disable all");
        if (::ImGui::Button("Disable all")) {
            if (app.autoFuelLock) toggleAutoVehicleLock(app, true, false);
            if (app.autoElectricLock) toggleAutoVehicleLock(app, true, false, true);
            if (app.autoDamageLock) toggleAutoVehicleLock(app, false, false);
        }
        ::ImGui::Spacing();
        ::ImGui::Separator();
        ui::text(ui::muted, multiplayerSummary().c_str());
        if (!app.mpDetail.empty()) ui::text(app.mpBlocks ? ui::accent : ui::muted, app.mpDetail.c_str());
        if (!telemetry && attached) {
            if (::ImGui::CollapsingHeader("telemetry status")) ui::text(ui::muted, app.telemetryError.c_str());
        }
        ui::endCard();
        ::ImGui::EndTable();
    }
    ::ImGui::Spacing();
    if (::ImGui::CollapsingHeader("Advanced: Manual Scan (for compatibility check)")) {
        ::ImGui::TextWrapped("Full memory scan will significantly increase the game memory pressure, so it is recommended to save the game first.");
        renderValuePanel(app, PanelKind::Fuel, "Manual fuel level scan", "is for compatibility check only.",
            &app.fuelType, app.fuelCurrent, app.fuelTarget, &app.fuelLock);
        ::ImGui::Spacing();
        renderValuePanel(app, PanelKind::Damage, "Manual damage scan", "is for compatibility check only.",
            &app.damageType, app.damageCurrent, app.damageTarget, &app.damageLock);
    }
}

void renderScannerTabImpl(AppState& app) {
    ui::beginCard("##scanner_workspace", "numerical scan and results");
    ::ImGui::TextWrapped("%s", "type");
    ui::next(220);
    typeCombo("##stype", &app.scanType);
    if (vtypeInfo(app.scanType).isFloat) {
        ui::next(220);
        ::ImGui::TextWrapped("%s", " tolerance");
        ui::next(220);
        ::ImGui::SetNextItemWidth(80);
        ::ImGui::InputText("##stolerance", app.scanTolerance, sizeof(app.scanTolerance),
                           ImGuiInputTextFlags_CharsDecimal);
    }
    ui::next(220);
    ::ImGui::TextWrapped("%s", " value");
    ui::next(220);
    ::ImGui::SetNextItemWidth(160);
    ::ImGui::InputText("##svalue", app.scanValue, 32, ImGuiInputTextFlags_CharsDecimal);
    ui::next(220);
    ::ImGui::BeginDisabled(app.busy.load());
    if (ui::primaryButton("first scan", ImVec2(110, 0))) startFirstScan(app, PanelKind::Scanner);
    ::ImGui::EndDisabled();
    ui::next(220);
    ::ImGui::TextWrapped("%s", "Alignment");
    ui::next(220);
    {
        int alignIndex = 2;
        for (int i = 0; i < 4; ++i) {
            if ((1 << i) == app.scanAlign) alignIndex = i;
        }
        ::ImGui::SetNextItemWidth(60);
        if (::ImGui::Combo("##align", &alignIndex, "1\0" "2\0" "4\0" "8\0")) {
            app.scanAlign = 1 << alignIndex;
        }
    }
    ui::next(220);
    ::ImGui::Checkbox("contains a large memory area (much slower)", &app.scanIncludeBig);

    ::ImGui::Spacing();
    ::ImGui::TextWrapped("%s", "Scan again:");
    ui::next(220);
    ::ImGui::BeginDisabled(app.busy.load());
    if (::ImGui::Button("is equal to the new value", ImVec2(100, 0))) {
        startNextScan(app, PanelKind::Scanner, ScanMode::Exact);
    }
    ui::next(220);
    if (::ImGui::Button("value changed", ImVec2(90, 0))) {
        startNextScan(app, PanelKind::Scanner, ScanMode::Changed);
    }
    ui::next(220);
    if (::ImGui::Button("value has not changed", ImVec2(90, 0))) {
        startNextScan(app, PanelKind::Scanner, ScanMode::Unchanged);
    }
    ui::next(220);
    if (::ImGui::Button("gets bigger", ImVec2(80, 0))) {
        startNextScan(app, PanelKind::Scanner, ScanMode::Increased);
    }
    ui::next(220);
    if (::ImGui::Button("becomes smaller", ImVec2(80, 0))) {
        startNextScan(app, PanelKind::Scanner, ScanMode::Decreased);
    }
    ::ImGui::EndDisabled();

    // --- 扫描器：写入行与结果表格 ---
    ::ImGui::Spacing();
    ::ImGui::TextWrapped("%s", "write value");
    ui::next(220);
    ::ImGui::SetNextItemWidth(160);
    ::ImGui::InputText("##snvalue", app.scanNewValue, 32, ImGuiInputTextFlags_CharsDecimal);
    ui::next(220);
    ::ImGui::BeginDisabled(app.busy.load());
    if (::ImGui::Button("Write selected row", ImVec2(120, 0))) {
        ScanSession* session = app.scanSession.get();
        double value = 0;
        if (session && !session->empty() && !app.scanSelection.empty() &&
            parseNumber(app.scanNewValue, vtypeInfo(session->type()).isFloat, &value)) {
            const bool isFloat = vtypeInfo(session->type()).isFloat;
            int written = 0;
            for (int idx : app.scanSelection) {
                if (idx < 0 || idx >= (int)session->addresses().size()) continue;
                uint64_t addr = session->addresses()[(size_t)idx];
                bool ok = isFloat ? app.mem.writeDouble(addr, session->type(), value)
                                  : app.mem.writeInt(addr, session->type(), (int64_t)value);
                if (ok) ++written;
            }
            app.scanStatus = fmt("has been written to row %d in selected row %d = %s",
                                 (int)app.scanSelection.size(), written, app.scanNewValue);
            logLine(app.scanStatus);
        } else {
            app.scanStatus = "Please scan and select the row first, and fill in the write value";
        }
    }
    ui::next(220);
    if (::ImGui::Button("Lock selected row", ImVec2(120, 0))) {
        ScanSession* session = app.scanSession.get();
        double value = 0;
        if (session && !session->empty() && !app.scanSelection.empty() && app.freezer &&
            parseNumber(app.scanNewValue, vtypeInfo(session->type()).isFloat, &value)) {
            int n = 0;
            for (int idx : app.scanSelection) {
                if (idx < 0 || idx >= (int)session->addresses().size()) continue;
                app.freezer->add(session->addresses()[(size_t)idx], session->type(), value);
                ++n;
            }
            app.scanStatus = fmt("Row %d locked = %s", n, app.scanNewValue);
            logLine(app.scanStatus);
        } else {
            app.scanStatus = "Please scan and select the row first, and fill in the value to be locked.";
        }
    }
    ui::next(220);
    if (::ImGui::Button("Select all rows/clear", ImVec2(150, 0))) {
        ScanSession* session = app.scanSession.get();
        if (session) {
            if (app.scanSelection.empty()) {
                int n = (int)(session->count() < 500 ? session->count() : 500);
                for (int i = 0; i < n; ++i) app.scanSelection.insert(i);
            } else {
                app.scanSelection.clear();
            }
        }
    }
    ::ImGui::EndDisabled();
    ui::next(220);
    if (::ImGui::Button("All unlocked", ImVec2(100, 0))) {
        if (app.freezer) app.freezer->clear();
        app.moneyLock = false;
        app.xpLock = false;
        app.fuelLock = false;
        app.damageLock = false;
        app.scanStatus = "All locks have been unlocked";
        logLine(app.scanStatus);
    }
    ui::next(220);
    ui::textf(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", app.scanStatus.c_str());

    ::ImGui::Spacing();
    ScanSession* session = app.scanSession.get();
    if (!app.busy.load() && session && !session->empty()) {
        auto rows = session->snapshot(500);
        int rowIndex = 0;
        if (::ImGui::BeginTable("##results", 3,
                                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                    ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
                                ImVec2(0, 260))) {
            ::ImGui::TableSetupColumn("memory address", ImGuiTableColumnFlags_WidthFixed, 190);
            ::ImGui::TableSetupColumn("current value", ImGuiTableColumnFlags_WidthFixed, 170);
            ::ImGui::TableSetupColumn("last value", ImGuiTableColumnFlags_WidthFixed, 170);
            ::ImGui::TableHeadersRow();
            for (const auto& row : rows) {
                const uint64_t addr = std::get<0>(row);
                const Value& cur = std::get<1>(row);
                const Value& prev = std::get<2>(row);
                ::ImGui::TableNextRow();
                ::ImGui::TableSetColumnIndex(0);
                bool selected = app.scanSelection.count(rowIndex) > 0;
                ::ImGui::PushID(rowIndex);
                if (::ImGui::Selectable(hexAddr(addr).c_str(), selected,
                                        ImGuiSelectableFlags_SpanAllColumns)) {
                    if (selected) app.scanSelection.erase(rowIndex);
                    else app.scanSelection.insert(rowIndex);
                }
                ::ImGui::PopID();
                ::ImGui::TableSetColumnIndex(1);
                ::ImGui::TextUnformatted(valueToString(cur).c_str());
                ::ImGui::TableSetColumnIndex(2);
                ::ImGui::TextUnformatted(valueToString(prev).c_str());
                ++rowIndex;
            }
            ::ImGui::EndTable();
        }
        ::ImGui::Text("Total %llu candidate addresses (the table shows the first 500 addresses)",
                      (unsigned long long)session->count());
    }
    ui::endCard();
}

// ==========================================================================
// 地图传送（控制台 goto）
// ==========================================================================
namespace {

// 只保留控制台能安全输入、goto 能接受的字符（城市内部名与坐标）。
std::string sanitizeTeleportTarget(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
            c == ';' || c == '-' || c == ' ') {
            out.push_back(c);
        }
    }
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

}  // namespace

void loadMapCities(AppState& app) {
    if (app.slots.empty()) app.slots = listSlots();
    if (app.slots.empty()) {
        app.teleportNote = "Archive not found, city list cannot be read.";
        return;
    }
    const int index = (app.selectedSlot >= 0 && app.selectedSlot < (int)app.slots.size())
                          ? app.selectedSlot
                          : 0;
    const SaveSlot& slot = app.slots[(size_t)index];
    std::vector<std::string> cities;
    std::string error;
    if (!listMapCities(slot.gameSii, &cities, &error)) {
        app.teleportNote = "Failed to read city list:" + error;
        logLine(app.teleportNote);
        return;
    }
    app.mapCities = std::move(cities);
    app.mapCitySelected = app.mapCities.empty() ? -1 : 0;
    app.teleportNote = fmt("Read cities from %s: %d internal names (from garage units in the save).",
                           slot.label.c_str(), (int)app.mapCities.size());
    logLine(app.teleportNote);
}

void teleportTo(AppState& app, const std::string& target) {
    const std::string clean = sanitizeTeleportTarget(target);
    if (clean.empty()) {
        app.teleportNote = "Please fill in the city name or coordinates first (coordinate format X;Y;Z, with ;A;E).";
        return;
    }
    app.teleportNote = "Transmission command sent: goto " + clean;
    logLine(app.teleportNote);
    sendConsole(app, "goto " + clean);
}

void saveTeleportSpot(AppState& app) {
    const std::string name = sanitizeTeleportTarget(app.spotName);
    std::string target = sanitizeTeleportTarget(app.coordInput);
    if (target.empty()) target = sanitizeTeleportTarget(app.cityInput);
    if (target.empty() && app.mapCitySelected >= 0 &&
        app.mapCitySelected < (int)app.mapCities.size()) {
        target = app.mapCities[(size_t)app.mapCitySelected];
    }
    if (name.empty() || target.empty()) {
        app.teleportNote = "Collection requires a name and a target (city name or coordinates).";
        return;
    }
    app.teleportSpots.emplace_back(name, target);
    app.spotName[0] = 0;
    saveSettings(app);
    app.teleportNote = fmt("has collected \"%s\" → %s", name.c_str(), target.c_str());
    logLine(app.teleportNote);
}

void removeTeleportSpot(AppState& app, int index) {
    if (index < 0 || index >= (int)app.teleportSpots.size()) return;
    app.teleportSpots.erase(app.teleportSpots.begin() + index);
    saveSettings(app);
}

namespace {

void renderTeleportSection(AppState& app) {
    if (!::ImGui::CollapsingHeader("map transfer (console goto)", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ui::textf(ImVec4(1.0f, 0.85f, 0.45f, 1.0f),
                         "Teleport only moves the camera; after adjusting the height, press Ctrl+F9 to drop the truck there.");
    ::ImGui::Spacing();

    if (::ImGui::Button("Read city list (decrypted archive)", ImVec2(200, 0))) loadMapCities(app);
    ui::next(220);
    ::ImGui::SetNextItemWidth(140);
    ::ImGui::InputTextWithHint("##cityfilter", "Filter city name", app.cityFilter, sizeof(app.cityFilter));
    ui::next(220);
    ui::textf(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "total %d cities",
                         (int)app.mapCities.size());

    if (!app.mapCities.empty()) {
        std::vector<int> filtered;
        const std::string filter = sanitizeTeleportTarget(app.cityFilter);
        for (size_t i = 0; i < app.mapCities.size(); ++i) {
            if (filter.empty() || app.mapCities[i].find(filter) != std::string::npos) {
                filtered.push_back((int)i);
            }
        }
        const std::string preview = (app.mapCitySelected >= 0 &&
                                     app.mapCitySelected < (int)app.mapCities.size())
                                        ? app.mapCities[(size_t)app.mapCitySelected]
                                        : std::string("(not selected)");
        ::ImGui::SetNextItemWidth(220);
        if (::ImGui::BeginCombo("##city", preview.c_str())) {
            for (int index : filtered) {
                const bool selected = (index == app.mapCitySelected);
                if (::ImGui::Selectable(app.mapCities[(size_t)index].c_str(), selected)) {
                    app.mapCitySelected = index;
                }
                if (selected) ::ImGui::SetItemDefaultFocus();
            }
            ::ImGui::EndCombo();
        }
        ui::next(220);
        if (::ImGui::Button("Teleport to this city", ImVec2(130, 0))) {
            if (app.mapCitySelected >= 0 && app.mapCitySelected < (int)app.mapCities.size()) {
                teleportTo(app, app.mapCities[(size_t)app.mapCitySelected]);
            }
        }
    }

    ::ImGui::Spacing();
    ::ImGui::TextWrapped("%s", "city name");
    ui::next(220);
    ::ImGui::SetNextItemWidth(160);
    ::ImGui::InputTextWithHint("##cityinput", "Example: berlin/calais", app.cityInput,
                               sizeof(app.cityInput));
    ui::next(220);
    if (::ImGui::Button("Teleport (city)", ImVec2(110, 0))) teleportTo(app, app.cityInput);

    ui::next(220);
    ::ImGui::TextWrapped("%s", "coordinates");
    ui::next(220);
    ::ImGui::SetNextItemWidth(180);
    ::ImGui::InputTextWithHint("##coords", "Example: 366.0;46.7;617.2", app.coordInput,
                               sizeof(app.coordInput));
    ui::next(220);
    if (::ImGui::Button("transmission (coordinates)", ImVec2(110, 0))) teleportTo(app, app.coordInput);

    ::ImGui::Spacing();
    if (::ImGui::Button("navigation starting point", ImVec2(90, 0))) teleportTo(app, "nav_start");
    ui::next(220);
    if (::ImGui::Button("Navigation end point", ImVec2(90, 0))) teleportTo(app, "nav_end");
    ui::next(220);
    if (::ImGui::Button("next navigation point", ImVec2(110, 0))) teleportTo(app, "nav_next");
    ui::next(220);
    if (::ImGui::Button("Previous navigation point", ImVec2(110, 0))) teleportTo(app, "nav_prev");
    ui::next(220);
    if (::ImGui::Button("Return to the previous position", ImVec2(110, 0))) teleportTo(app, "back");
    ui::next(220);
    if (::ImGui::Button("Teleport the vehicle to the camera (Ctrl+F9)", ImVec2(220, 0))) {
        DWORD pid = app.pid;
        if (!pid) {
            std::wstring exe;
            findProcess(selectedProcessName(), &pid, &exe);
        }
        if (pid) {
            uintptr_t hwnd = findGameWindow(pid);
            if (hwnd && activateWindow(hwnd)) {
                BYTE ctrlScan = (BYTE)::MapVirtualKeyW(VK_CONTROL, 0);
                BYTE f9Scan = (BYTE)::MapVirtualKeyW(VK_F9, 0);
                ::keybd_event(VK_CONTROL, ctrlScan, 0, 0);
                ::Sleep(80);
                ::keybd_event(VK_F9, f9Scan, 0, 0);
                ::Sleep(120);
                ::keybd_event(VK_F9, f9Scan, KEYEVENTF_KEYUP, 0);
                ::Sleep(50);
                ::keybd_event(VK_CONTROL, ctrlScan, KEYEVENTF_KEYUP, 0);
                ::Sleep(100);
                logLine("Sent [Ctrl+F9] Teleport truck to camera location");
            }
        }
    }

    ::ImGui::Spacing();
    ::ImGui::TextWrapped("%s", "collection point");
    ui::next(220);
    ::ImGui::SetNextItemWidth(120);
    ::ImGui::InputTextWithHint("##spotname", "name", app.spotName, sizeof(app.spotName));
    ui::next(220);
    if (::ImGui::Button("Collect current target", ImVec2(120, 0))) saveTeleportSpot(app);
    for (int i = 0; i < (int)app.teleportSpots.size(); ++i) {
        ::ImGui::PushID(i);
        if (::ImGui::Button("transmission", ImVec2(60, 0))) teleportTo(app, app.teleportSpots[(size_t)i].second);
        ui::next(220);
        if (::ImGui::Button("Delete", ImVec2(60, 0))) {
            removeTeleportSpot(app, i);
            ::ImGui::PopID();
            break;
        }
        ui::next(220);
        ::ImGui::TextUnformatted(fmt("%s → %s", app.teleportSpots[(size_t)i].first.c_str(),
                                     app.teleportSpots[(size_t)i].second.c_str()).c_str());
        ::ImGui::PopID();
    }
    ::ImGui::Spacing();
    ::ImGui::TextWrapped("%s", app.teleportNote.c_str());
}

}  // namespace

// ==========================================================================
// 联网 / 联运模式
// ==========================================================================
void refreshConvoy(AppState& app) {
    MultiplayerStatus status = checkMultiplayer(app.pid, true);
    app.mpBlocks = status.blocksWrite();
    app.mpDetail = status.detail;
    if (isConvoyCheckEnabled()) {
        app.convoy = readConvoySession();
        app.convoyNote = app.convoy.note;
        if (app.convoy.active && !app.convoy.players.empty()) {
            std::string names;
            for (const auto& player : app.convoy.players) {
                if (!names.empty()) names += "、";
                names += player.name;
                if (player.you) names += "(you)";
            }
            app.convoyNote += "：" + names;
        }
    } else {
        app.convoy = ConvoySession{};
        app.convoyNote = "Intermodal detection is closed. Click \"Read Player List\" to manually read the sessions in the log.";
    }
    logLine("online status:" + app.mpDetail + (isConvoyCheckEnabled() ? ("；" + app.convoy.note) : ""));
}

void setMultiplayerPolicy(AppState& app, int policy) {
    app.mpPolicy = (policy == 1) ? 1 : 0;
    saveSettings(app);
    logLine(app.mpPolicy == 1
                ? "has been switched to \"intermodal mode\": release the console/teleport/free camera when online, writing functions are still disabled"
                : "has been switched to \"strict mode\": all functions are disabled while online\nThis function has been disabled in the");
}

std::string multiplayerGateForSafeFeature(const AppState& app) {
    DWORD pid = app.pid;
    if (!pid) {
        std::wstring exe;
        findProcess(selectedProcessName(), &pid, &exe);
    }
    MultiplayerStatus status = checkMultiplayer(pid);
    if (status.kind == MultiplayerKind::TruckersMp) {
        return "TruckersMP environment (the other party has anti-cheating and is easily banned)";
    }
    if (isConvoyCheckEnabled() && status.kind == MultiplayerKind::Convoy && app.mpPolicy != 1) {
        return "Official Convoy session detected: disabled by default. If necessary, please explicitly switch to intermodal mode in the \"Intermodal Mode\" panel.";
    }
    return std::string();
}

namespace {

void renderConvoySection(AppState& app) {
    if (!::ImGui::CollapsingHeader("intermodal mode (Convoy / online)", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    bool checkConvoy = app.checkConvoy;
    if (::ImGui::Checkbox("Enable official Convoy session detection", &checkConvoy)) {
        app.checkConvoy = checkConvoy;
        setConvoyCheckEnabled(checkConvoy);
        refreshConvoy(app);
        saveSettings(app);
    }
    if (!app.checkConvoy) {
        ui::next(220);
        ui::textf(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "(detection is turned off)");
    }
    ::ImGui::Spacing();

    if (::ImGui::Button("Refresh online status", ImVec2(130, 0))) refreshConvoy(app);
    ui::next(220);
    if (::ImGui::Button("Read player list", ImVec2(130, 0))) {
        app.convoy = readConvoySession();
        app.convoyNote = app.convoy.note;
    }
    ui::next(220);
    ui::textf(app.mpBlocks ? ImVec4(1.0f, 0.6f, 0.6f, 1.0f)
                                      : ImVec4(0.6f, 0.9f, 0.6f, 1.0f),
                         "%s", app.mpDetail.empty() ? "has not checked the connection status yet" : app.mpDetail.c_str());

    if (app.convoy.active) {
        ui::textf(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "%s (started with %s)",
                             app.convoy.note.c_str(),
                             app.convoy.startedAt.empty() ? "unknown" : app.convoy.startedAt.c_str());
        if (app.convoy.players.empty()) {
            ::ImGui::TextWrapped("%s", "(The player has not been resolved yet, maybe just entered the session)");
        } else {
            for (const auto& player : app.convoy.players) {
                ::ImGui::BulletText("%s  (client %d)%s", player.name.c_str(), player.clientId,
                                    player.you ? " ← you" : "");
            }
        }
    } else {
        ui::textf(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", app.convoyNote.c_str());
    }

    if (app.checkConvoy) {
        ::ImGui::Spacing();
        int policy = app.mpPolicy;
        if (::ImGui::RadioButton("strict mode: restrict online functions (except TruckersMP automatic fuel/power)", &policy, 0)) {
            setMultiplayerPolicy(app, 0);
        }
        if (::ImGui::RadioButton("intermodal mode: release the function of not writing memory when online", &policy, 1)) {
            setMultiplayerPolicy(app, 1);
        }
    }
    ::ImGui::Spacing();
    const std::string gate = multiplayerGateForSafeFeature(app);
    ui::textf(gate.empty() ? ImVec4(0.6f, 0.9f, 0.6f, 1.0f)
                                      : ImVec4(1.0f, 0.75f, 0.5f, 1.0f),
                         "Security function currently: %s", gate.empty() ? "available" : gate.c_str());
    ui::textf(ImVec4(1.0f, 0.6f, 0.6f, 1.0f),
                         "Other writing functions: %s",
                         app.mpBlocks ? "is disabled while online" : (app.checkConvoy ? "available (standalone)" : "is available (intermodal detection is turned off)"));
    ::ImGui::TextWrapped("TruckersMP allows automatic unlimited fuel/electricity for vehicle pages; damage prevention, power, economy and manual writing still use the original protection.");
}

void renderFlyModeSection(AppState& app);

void renderConsoleTabImpl(AppState& app) {
    ui::beginCard("##travel_teleport", "Quick transmission");
    renderTeleportSection(app);
    ui::endCard();
    ::ImGui::Spacing();
    ui::beginCard("##travel_camera", "free camera");
    renderFlyModeSection(app);
    ui::endCard();
    ::ImGui::Spacing();
    ui::beginCard("##travel_console", "game console");
    ::ImGui::TextWrapped("%s", "console button");
    ui::next(220);
    const auto& options = consoleKeyOptions();
    int currentOption = 0;
    for (size_t i = 0; i < options.size(); ++i) {
        if (options[i].vk == app.consoleKey) currentOption = (int)i;
    }
    std::string preview = options[(size_t)currentOption].label;
    ::ImGui::SetNextItemWidth(170);
    if (::ImGui::BeginCombo("##ckey", preview.c_str())) {
        for (size_t i = 0; i < options.size(); ++i) {
            bool selected = ((int)i == currentOption);
            if (::ImGui::Selectable(options[i].label, selected)) {
                app.consoleKey = options[i].vk;
                saveSettings(app);
            }
            if (selected) ::ImGui::SetItemDefaultFocus();
        }
        ::ImGui::EndCombo();
    }
    ui::next(220);
    if (::ImGui::Button("automatically recognized from controls.sii")) detectConsoleKey(app);
    ui::next(220);
    if (::ImGui::Button("detection/opening console (config.cfg)")) checkConsoleConfig(app);
    if (!app.consoleNote.empty()) {
        ui::textf(ImVec4(0.85f, 0.85f, 0.55f, 1.0f), "%s", app.consoleNote.c_str());
    }
    ::ImGui::Spacing();
    ::ImGui::TextWrapped("%s", "command");
    ui::next(220);
    ::ImGui::SetNextItemWidth(-140);
    ::ImGui::InputText("##cmd", app.consoleCmd, sizeof(app.consoleCmd));
    ui::next(220);
    if (::ImGui::Button("Send to game", ImVec2(120, 0))) sendConsole(app, app.consoleCmd);

    ::ImGui::Spacing();
    ::ImGui::TextWrapped("%s", "Common commands");
    struct Preset {
        const char* label;
        const char* command;
    };
    static const Preset kPresets[] = {
        {"time 08:00", "g_set_time 8 0"},   {"time 20:00", "g_set_time 20 0"},
        {"Close the police", "g_police 0"},         {"Turn on the police", "g_police 1"},
        {"Turn off fatigue", "g_fatigue 0"},        {"Turn on fatigue", "g_fatigue 1"},
        {"income x5", "g_income_factor 5.0"}, {"income x1", "g_income_factor 1.0"},
        {"clear traffic", "g_traffic 0"},        {"restore traffic", "g_traffic 1"},
        {"display FPS", "g_fps 1"},            {"Turn off FPS", "g_fps 0"},
    };
    const int perRow = std::max(1, (int)((::ImGui::GetContentRegionAvail().x + 12) / 122));
    for (int i = 0; i < (int)(sizeof(kPresets) / sizeof(kPresets[0])); ++i) {
        if (i % perRow) ::ImGui::SameLine();
        if (::ImGui::Button(kPresets[i].label, ImVec2(110, 0))) {
            sendConsole(app, kPresets[i].command);
        }
    }
    ::ImGui::Spacing();
    ui::textf(ImVec4(0.85f, 0.55f, 0.55f, 1.0f),
                         "Sending will switch the game window to the foreground. Please stop the car first while driving.");
    ui::textf(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                         "requires config.cfg: uset g_console \"1\" and uset g_developer \"1\".");

    ::ImGui::Spacing();
    ::ImGui::Separator();
    ::ImGui::Spacing();
    ui::endCard();
    ::ImGui::Spacing();
    ui::beginCard("##travel_multiplayer", "online status and protection");
    renderConvoySection(app);
    ui::endCard();
}

void renderFlyModeSection(AppState& app) {
    if (::ImGui::CollapsingHeader("flight mode (free camera)", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (::ImGui::Button("check config.cfg", ImVec2(140, 0))) refreshFlyMode(app);
        ui::next(220);
        if (::ImGui::Button("identifies camera buttons from controls.sii", ImVec2(220, 0))) detectCameraKeys(app);
        ui::next(220);
        if (::ImGui::Button("Write flight mode settings", ImVec2(160, 0))) enableFlyMode(app);
        ::ImGui::Spacing();
        ::ImGui::TextWrapped("%s", app.flyNote.c_str());
        if (app.cameraLoaded) {
            ui::textf(ImVec4(0.7f, 0.85f, 1.0f, 1.0f),
                                 "Camera buttons: switch %s · Forward %s · Back %s · Left %s · Right %s · Up %s · "
                                 "down %s",
                                 keyLabel(app.camera.toggle).c_str(),
                                 keyLabel(app.camera.forward).c_str(),
                                 keyLabel(app.camera.back).c_str(),
                                 keyLabel(app.camera.left).c_str(),
                                 keyLabel(app.camera.right).c_str(),
                                 keyLabel(app.camera.up).c_str(),
                                 keyLabel(app.camera.down).c_str());
        }
        ::ImGui::Spacing();
        ::ImGui::TextWrapped("%s", "flight speed g_flyspeed");
        ui::next(220);
        ::ImGui::SetNextItemWidth(90);
        ::ImGui::InputText("##flyspeed", app.flySpeed, sizeof(app.flySpeed),
                           ImGuiInputTextFlags_CharsDecimal);
        ui::next(220);
        if (::ImGui::Button("Apply immediately", ImVec2(90, 0))) setFlySpeed(app, app.flySpeed);
        static const char* kSpeeds[] = {"50", "100", "300", "1000", "3000"};
        for (const char* speed : kSpeeds) {
            ui::next(220);
            if (::ImGui::Button(speed, ImVec2(60, 0))) setFlySpeed(app, speed);
        }
        ::ImGui::Spacing();
        if (::ImGui::Button("Switch free camera (enter/exit perspective)", ImVec2(220, 0))) toggleFreeCamera(app);
        ui::next(220);
        if (::ImGui::Button("Teleport the vehicle to the current free camera position (Ctrl+F9)", ImVec2(260, 0))) {
            DWORD pid = app.pid;
            if (!pid) {
                std::wstring exe;
                findProcess(selectedProcessName(), &pid, &exe);
            }
            if (pid) {
                uintptr_t hwnd = findGameWindow(pid);
                if (hwnd && activateWindow(hwnd)) {
                    BYTE ctrlScan = (BYTE)::MapVirtualKeyW(VK_CONTROL, 0);
                    BYTE f9Scan = (BYTE)::MapVirtualKeyW(VK_F9, 0);
                    ::keybd_event(VK_CONTROL, ctrlScan, 0, 0);
                    ::Sleep(80);
                    ::keybd_event(VK_F9, f9Scan, 0, 0);
                    ::Sleep(120);
                    ::keybd_event(VK_F9, f9Scan, KEYEVENTF_KEYUP, 0);
                    ::Sleep(50);
                    ::keybd_event(VK_CONTROL, ctrlScan, KEYEVENTF_KEYUP, 0);
                    ::Sleep(100);
                    logLine("Sent [Ctrl+F9] Teleport truck to camera location");
                }
            }
        }
        ui::next(220);
        if (::ImGui::Button("Switch back to the first-person cab (press 1)", ImVec2(180, 0))) {
            DWORD pid = app.pid;
            if (!pid) {
                std::wstring exe;
                findProcess(selectedProcessName(), &pid, &exe);
            }
            if (pid) {
                sendKeyTap('1', pid);
            }
        }
    }
}

void renderSaveTabImpl(AppState& app) {
    if(app.slots.empty()) {app.saveProfiles.clear();app.selectedSaveProfile=-1;clearSaveSlotSelection(app);}
    if(!app.slots.empty() && app.saveProfiles.empty()) {
        app.saveProfiles=groupSaveProfiles(app.slots);app.selectedSaveProfile=0;
    }
    if(app.selectedSaveProfile<0 || app.selectedSaveProfile>=(int)app.saveProfiles.size())
        app.selectedSaveProfile=app.saveProfiles.empty() ? -1 : 0;
    if(app.selectedSlot>=0 && saveProfileForSlot(app.saveProfiles,app.selectedSlot)!=app.selectedSaveProfile)
        clearSaveSlotSelection(app);
    app.visibleSaveSlotCount=0;
    ::ImGui::BeginDisabled(app.busy);
    if (::ImGui::Button("Refresh archive list")) refreshSaves(app);
    ui::next(300);
    ::ImGui::TextDisabled("%d files · %d files",(int)app.saveProfiles.size(), (int)app.slots.size());
    ::ImGui::Spacing();
    const int columns = ::ImGui::GetContentRegionAvail().x >= 880 ? 2 : 1;
    if (::ImGui::BeginTable("##save_workspace", columns, ImGuiTableFlags_SizingStretchProp)) {
        if (columns == 2) {
            ::ImGui::TableSetupColumn("library", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ::ImGui::TableSetupColumn("detail", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        }
        ::ImGui::TableNextColumn();
        ui::beginCard("##save_library", "My files and archives");
        ::ImGui::BeginChild("##save_profiles",ImVec2(0,144));
        if(app.saveProfiles.empty()) ui::text(ui::muted,"The file containing the archive has not been found.");
        for(int i=0;i<(int)app.saveProfiles.size();++i) {
            const auto& profile=app.saveProfiles[i];
            ::ImGui::PushID(i);
            const std::string label=fmt("%s [%s] · %d archives",profile.name.c_str(),
                                       profile.cloud ? "Steam Cloud" : "local",(int)profile.slots.size());
            if(::ImGui::Selectable(label.c_str(),app.selectedSaveProfile==i,ImGuiSelectableFlags_None,ImVec2(0,32)))
                selectSaveProfile(app,i);
            if(app.saveProfileUiProbe.capture && i<3) {
                const auto min=::ImGui::GetItemRectMin(),max=::ImGui::GetItemRectMax();
                app.saveProfileUiProbe.rowX[i]=(min.x+max.x)*0.5f;
                app.saveProfileUiProbe.rowY[i]=(min.y+max.y)*0.5f;
            }
            if(::ImGui::IsItemHovered()) ::ImGui::SetTooltip("%s\n%s",profile.name.c_str(),W2U(profile.profileDir).c_str());
            ::ImGui::PopID();
        }
        ::ImGui::EndChild();
        ::ImGui::Separator();
        const SaveProfileGroup* selectedProfile=app.selectedSaveProfile>=0 ? &app.saveProfiles[app.selectedSaveProfile] : nullptr;
        if(selectedProfile) ui::textf(ui::accent,"%s · %d archives",selectedProfile->name.c_str(),(int)selectedProfile->slots.size());
        ::ImGui::BeginChild("##slots", ImVec2(0, 266));
        if (app.slots.empty()) ui::text(ui::muted, "The archive has not been found. You can refresh the list, or confirm the currently selected game.");
        const std::vector<int> noSlots;
        for (int i : selectedProfile ? selectedProfile->slots : noSlots) {
            if(i<0 || i>=(int)app.slots.size()) continue;
            ++app.visibleSaveSlotCount;
            const auto& slot = app.slots[(size_t)i];
            ::ImGui::PushID(i);
            const ImVec2 pos = ::ImGui::GetCursorScreenPos();
            const float width = ::ImGui::GetContentRegionAvail().x;
            if (::ImGui::Selectable("##slot", app.selectedSlot == i, ImGuiSelectableFlags_None, ImVec2(0, 62)))
                selectSlot(app, i);
            const bool hovered = ::ImGui::IsItemHovered();
            auto* draw = ::ImGui::GetWindowDrawList();
            draw->PushClipRect(pos, ImVec2(pos.x + width, pos.y + 62), true);
            const std::string name = slot.displayName.empty() ? W2U(slot.slotName) : slot.displayName;
            draw->AddText(ImVec2(pos.x + 10, pos.y + 6), ::ImGui::GetColorU32(app.selectedSlot == i ? ui::accent : ui::foreground), name.c_str());
            const std::string detail = "slot "+W2U(slot.slotName) + " / " + localTimeString(slot.mtime);
            ::ImGui::PushFont(nullptr, 14);
            draw->AddText(ImVec2(pos.x + 10, pos.y + 33), ::ImGui::GetColorU32(ui::muted), detail.c_str());
            ::ImGui::PopFont();
            draw->PopClipRect();
            if (hovered) ::ImGui::SetTooltip("%s\n%s\n%s / %s", name.c_str(), detail.c_str(), slot.formatText.c_str(), formatSize(slot.size).c_str());
            ::ImGui::PopID();
        }
        ::ImGui::EndChild();
        ui::endCard();
        ::ImGui::TableNextColumn();
        ui::beginCard("##save_selection", "archive details and operations");
        if (app.selectedSlot >= 0 && app.selectedSlot < (int)app.slots.size()) {
            const auto& slot = app.slots[(size_t)app.selectedSlot];
            ui::text(ui::accent, slot.displayName.empty() ? W2U(slot.slotName).c_str() : slot.displayName.c_str());
            ui::textf(ui::muted, "File: %s / Slot: %s", W2U(slot.profileName).c_str(), W2U(slot.slotName).c_str());
            ui::textf(ui::muted, "%s  ·  %s  ·  %s", slot.cloud ? "Steam Cloud" : "local", slot.formatText.c_str(), formatSize(slot.size).c_str());
            ::ImGui::Spacing();
        } else {
            ui::text(ui::muted, "Select the file first, then select the archive within it to view details, backup or export.");
            ::ImGui::Spacing();
        }
    if (ui::primaryButton("Backup selected archive")) backupSelectedSlot(app);
    ui::next(210);
    if (::ImGui::Button("decryption export selected", ImVec2(130, 0))) exportSelectedSlot(app);
    if (::ImGui::IsItemHovered()) ::ImGui::SetTooltip("retains the decrypted format: BSII/3nK is still binary.");
    ui::next(210);
    if (::ImGui::Button("exports readable SII", ImVec2(150, 0))) exportReadableSelectedSlot(app);
    if (::ImGui::IsItemHovered()) ::ImGui::SetTooltip("automatically decrypts and converts BSII v3 into readable text; if it is SII text, it is left intact.");
    if (::ImGui::Button("Open the backup folder", ImVec2(130, 0))) {
        std::wstring root = backupRoot();
        ::CreateDirectoryW(root.c_str(), nullptr);
        ::ShellExecuteW(nullptr, L"open", root.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ui::next(210);
    if (::ImGui::Button("Open the export folder", ImVec2(130, 0))) {
        const std::wstring exports = documentsDir() + L"\\exports";
        if (!isDirSegment(exports)) ::CreateDirectoryW(exports.c_str(), nullptr);
        if (!isDirSegment(exports)) {
            app.saveNote = "Failed to open the export folder: The target is not an available directory";
            logLine(app.saveNote);
        } else if ((INT_PTR)::ShellExecuteW(nullptr, L"open", exports.c_str(), nullptr, nullptr,
                                            SW_SHOWNORMAL) <= 32) {
            app.saveNote = "Failed to open the export folder: The system cannot open the directory";
            logLine(app.saveNote);
        }
    }

    ui::next(210);
    if (::ImGui::Button("Copy as new archive")) {
        if (app.selectedSlot < 0 || app.selectedSlot >= (int)app.slots.size()) app.saveNote = "Please select the source archive first";
        else {
            std::string error;
            if (copyBlockedByRunningGame(selectedGame(), &error)) app.saveNote = error;
            else {
                app.copyRequest = {selectedGame(), app.slots[(size_t)app.selectedSlot], {}};
                const auto& name = app.copyRequest.source.displayName;
                ::strncpy_s(app.copyName, (name.empty() ? std::string("archived copy") : name + " copy").c_str(), _TRUNCATE);
                app.showCopyDialog = true;
            }
        }
    }
    if (app.showCopyDialog) { ::ImGui::OpenPopup("copied as new archive ##copy"); app.showCopyDialog = false; }
    if (::ImGui::BeginPopupModal("copied as new archive ##copy", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (app.copyRequest.source.slotDir.empty()) ::ImGui::CloseCurrentPopup();
        ::ImGui::Text("Game: %s", gameDescriptor(app.copyRequest.game).shortName);
        ::ImGui::Text("File: %s / Source slot: %s", W2U(app.copyRequest.source.profileName).c_str(), W2U(app.copyRequest.source.slotName).c_str());
        ::ImGui::TextUnformatted("Creates an idle number in the same file; retains all save files and game progress. Name can be up to 64 characters.");
        ::ImGui::SetNextItemWidth(420);
        ::ImGui::InputText("New archive name", app.copyName, sizeof(app.copyName));
        std::string escaped, error;
        bool valid = siiEscapeName(app.copyName, &escaped, &error);
        if (!valid) ::ImGui::TextWrapped("%s", error.c_str());
        ::ImGui::BeginDisabled(!valid);
        if (::ImGui::Button("Start copying")) { startSaveCopy(app); ::ImGui::CloseCurrentPopup(); }
        ::ImGui::EndDisabled();
        ui::next(210);
        if (::ImGui::Button("Cancel")) ::ImGui::CloseCurrentPopup();
        ::ImGui::EndPopup();
    }

    ::ImGui::Spacing();
    if (::ImGui::Button("Copy truck across archives")) {
        if(app.truckTransferUiProbe.capture) app.truckTransferUiProbe.clicked=true;
        openTruckTransfer(app);
    }
    if(app.truckTransferUiProbe.capture) {
        const auto lo=::ImGui::GetItemRectMin(),hi=::ImGui::GetItemRectMax();
        app.truckTransferUiProbe.buttonX=(lo.x+hi.x)/2;
        app.truckTransferUiProbe.buttonY=(lo.y+hi.y)/2;
    }
    if(app.busy && ::ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ::ImGui::SetTooltip("The current task is being processed, please complete or cancel before copying the truck.");
    ui::next(210);
    ui::text(ui::muted, "Directly select the existing archive; use it after exiting the corresponding game");
    ::ImGui::Spacing();
    ::ImGui::BeginDisabled(selectedGame()!=GameId::Ets2);
    if(::ImGui::Button("Home accessory code")) openHomeAccessories(app);
    ::ImGui::EndDisabled();
    ui::next(210);
    ui::text(ui::muted,"Paste /def/... path, preview and save a copy (ETS2)");
    ::ImGui::Spacing();
    ::ImGui::BeginDisabled(selectedGame()!=GameId::Ets2);
    if(::ImGui::Button("permanent fuel tank/battery expansion"))openHomeAccessories(app,2);
    ::ImGui::EndDisabled();
    ui::next(210);ui::text(ui::muted,"Select truck and capacity, generate independent chassis Mod and archive copy");
    ::ImGui::EndDisabled();
    ::ImGui::Spacing();
    if(::ImGui::Button("replacement trailer")) openTrailerSwap(app);
    ui::next(210);
    ui::text(ui::muted,"The currently connected trailer is replaced by another one in the archive, and the saved copy will take effect.");
    ::ImGui::Spacing();
    ::ImGui::BeginDisabled(selectedGame()!=GameId::Ets2 || app.busy);
    if(::ImGui::Button("Fleet economic management")) openFleet(app);
    ::ImGui::EndDisabled();
    ui::next(210);
    ui::text(ui::muted,"Add drivers and trucks in batches, assign them to the purchased garage and save a copy");
    renderFleetDialog(app);
    renderTruckTransferDialog(app);
    renderHomeAccessoriesDialog(app);
    renderTrailerSwapDialog(app);
    ::ImGui::BeginDisabled(app.busy);
    ::ImGui::Spacing();
    if (::ImGui::Button("split into independent files")) openProfileSplit(app);
    ui::next(210);
    ui::text(ui::muted, "Multiple archives of the same file → independent local files");
    renderProfileSplitDialog(app);

        ui::endCard();
        ::ImGui::EndTable();
    }
    ui::text(ui::muted, app.saveNote.c_str());
    ::ImGui::Spacing();
    ui::beginCard("##save_edit", "Plain text archive value modification");
    ::ImGui::SameLine();
    ui::textf(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "(automatic backup before writing)");
    ::ImGui::TextUnformatted("Cash");
    ::ImGui::SameLine();
    ::ImGui::SetNextItemWidth(150);
    ::ImGui::InputText("##smoney", app.saveMoney, sizeof(app.saveMoney),
                       ImGuiInputTextFlags_CharsDecimal);
    ::ImGui::SameLine();
    ::ImGui::TextUnformatted("Experience");
    ::ImGui::SameLine();
    ::ImGui::SetNextItemWidth(150);
    ::ImGui::InputText("##sxp", app.saveXp, sizeof(app.saveXp), ImGuiInputTextFlags_CharsDecimal);
    ::ImGui::SameLine();
    if (::ImGui::Button("Write plain text archive", ImVec2(130, 0))) applyTextPatch(app);

    ::ImGui::Spacing();
    ui::endCard();
    ::ImGui::Spacing();
    ui::beginCard("##backup_card", "backup list");
    ::ImGui::SameLine();
    if (::ImGui::Button("Restore selected backup", ImVec2(120, 0))) restoreSelectedBackup(app);
    if (::ImGui::BeginTable("##backups", 4,
                            ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
                            ImVec2(0, 150))) {
        ::ImGui::TableSetupColumn("backup time", ImGuiTableColumnFlags_WidthFixed, 160);
        ::ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthFixed, 120);
        ::ImGui::TableSetupColumn("archive location", ImGuiTableColumnFlags_WidthFixed, 130);
        ::ImGui::TableSetupColumn("Remarks", ImGuiTableColumnFlags_WidthFixed, 110);
        ::ImGui::TableHeadersRow();
        for (int i = 0; i < (int)app.backups.size(); ++i) {
            const BackupInfo& info = app.backups[(size_t)i];
            ::ImGui::TableNextRow();
            ::ImGui::TableSetColumnIndex(0);
            ::ImGui::PushID(10000 + i);
            bool selected = (i == app.selectedBackup);
            if (::ImGui::Selectable(W2U(info.time).c_str(), selected,
                                    ImGuiSelectableFlags_SpanAllColumns)) {
                app.selectedBackup = i;
            }
            ::ImGui::PopID();
            ::ImGui::TableSetColumnIndex(1);
            ::ImGui::TextUnformatted(W2U(info.profile).c_str());
            ::ImGui::TableSetColumnIndex(2);
            ::ImGui::TextUnformatted(W2U(info.slot).c_str());
            ::ImGui::TableSetColumnIndex(3);
            ::ImGui::TextUnformatted(W2U(info.label).c_str());
        }
        ::ImGui::EndTable();
    }
    ui::endCard();
    ::ImGui::EndDisabled();
    if (app.busy && ::ImGui::Button("Cancel the current task")) app.cancel = true;
}

void renderHelpTabImpl(AppState& app) {
    static const char* kHelp =
        "[Prerequisite] Start the game (stand-alone) first, and then click \"Additional Games\" at the top; changing memory requires administrator rights. \n"
        "\n"
        "[Change Money/Experience] ATS: Change the money directly after filling in the target amount; ETS2 needs to fill in the current amount as the positioning seed for the first time. \n"
        "After that: directly fill in the target value and click again. What is written is the authoritative storage of the game and does not require locking. \n"
        "[Vehicle] Check unlimited fuel capacity for fuel vehicles and unlimited power for electric vehicles. The two are mutually exclusive; the remaining energy percentage is displayed on the vehicle page. After setting the power target, click \"Apply Adjustment.\" \n"
        "Click \"Restore to Original Factory\" to restore power; change vehicles and read gears to automatically follow the current vehicle. \n"
        "[Scanner] Any value can be scanned; confirmation will be required before writing to multiple candidate addresses. \n"
        "[Archive] Backup/Restore/Decrypt Export/Export Readable SII/Copy to New Archive/Copy Trucks Across Archives/Split Independent Archives. \n"
        "Exports to the exports of the current game document directory without overwriting old files; writing, copying and splitting require exiting the corresponding game first. \n"
        "\n"
        "[Encrypted Archive] Decrypted and readable SII conversion is saved as an export file; truck copy generates a new archive slot, retaining the original archive. \n"
        "The amount and experience can be modified in the memory and then automatically saved by the game. \n"
        "\n"
        "[Security] Only used on a single machine; TruckersMP write protection is always enabled, and Convoy write protection takes effect according to detection settings. \n"
        "This tool does not modify the game exe nor inject code. It only reads and writes memory and archive files.";
    ui::beginCard("##settings_preferences", "workspace settings");
    ::ImGui::Spacing();
    ::ImGui::Separator();
    ::ImGui::Spacing();
    ui::textf(ui::accent, "Global game hotkey (no need to switch the screen):");
    bool hk = app.hotkeysEnabled;
    if (::ImGui::Checkbox("Enable in-game global hotkeys", &hk)) {
        app.hotkeysEnabled = hk;
        saveSettings(app);
    }
    ::ImGui::BulletText("Ctrl+F1 transfer navigation end point · Ctrl+F2 unlimited fuel · Ctrl+F3 vehicle intact");
    ::ImGui::BulletText("Ctrl+F4 Engine power · Ctrl+F5 Time 09:00 · Ctrl+F6 Unlimited power · Ctrl+Backspace Undo transfer");
    ::ImGui::Spacing();
    ::ImGui::Separator();
    ::ImGui::Spacing();
    ::ImGui::TextWrapped("%s", "Skip more than\nMemory area of");
    ui::next(220);
    ::ImGui::SetNextItemWidth(90);
    ::ImGui::DragFloat("##maxgb", &app.maxRegionGb, 0.1f, 0.0f, 64.0f, "%.1f GB");
    ui::next(220);
    ::ImGui::TextWrapped("%s", "(0 = do not skip)");
    ui::next(220);
    ::ImGui::TextWrapped("%s", " Number of threads");
    ui::next(220);
    {
        int idx = 0;
        static const int kChoices[] = {1, 2, 4, 6, 8};
        for (int i = 0; i < 5; ++i) {
            if (kChoices[i] == app.workers) idx = i;
        }
        ::ImGui::SetNextItemWidth(70);
        if (::ImGui::Combo("##workers", &idx, "1\0" "2\0" "4\0" "6\0" "8\0")) {
            app.workers = kChoices[idx];
        }
    }
    ui::next(220);
    if (::ImGui::Button("Save settings")) {
        saveSettings(app);
        logLine("settings saved to settings.ini");
    }
    ::ImGui::Spacing();
    ui::textf(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", app.procStatus.c_str());
    if (!app.memInfo.empty()) {
        ui::textf(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", app.memInfo.c_str());
    }
    ui::endCard();
    ::ImGui::Spacing();
    ui::beginCard("##settings_manual", "Instructions for Use");
    ::ImGui::TextWrapped("%s", kHelp);
    ui::endCard();
}

void renderConfirmDialogs(AppState& app) {
    if (app.requestMultiConfirm) {
        ::ImGui::OpenPopup("Confirm batch modification");
        app.requestMultiConfirm = false;
    }
    if (::ImGui::BeginPopupModal("Confirm batch modification", nullptr,
                                 ImGuiWindowFlags_AlwaysAutoResize)) {
        PanelKind kind = (app.pendingMultiKind >= (int)PanelKind::Money &&
                          app.pendingMultiKind <= (int)PanelKind::Damage)
                             ? (PanelKind)app.pendingMultiKind
                             : PanelKind::Money;
        ScanSession* session = sessionFor(app, kind);
        const size_t count = session ? session->count() : 0;
        ui::textf(ImVec4(0.95f, 0.50f, 0.35f, 1.0f),
                             "currently still has %llu candidate addresses.",
                             (unsigned long long)count);
        ::ImGui::TextWrapped("Batch modification may modify irrelevant game data together. It is recommended to cancel and continue narrowing until only 1 address remains.");
        ::ImGui::Spacing();
        const char* confirmLabel = app.pendingMultiAction == 2 ? "still locks all candidates"
                                                                : "still writes all candidates";
        ::ImGui::BeginDisabled(!session || count < 2);
        if (::ImGui::Button(confirmLabel, ImVec2(190, 0))) {
            if (app.pendingMultiAction == 2) {
                if (bool* state = lockFor(app, kind)) *state = true;
                togglePanelLock(app, kind, true, true);
            } else {
                writePanelValues(app, kind, true);
            }
            app.pendingMultiAction = 0;
            app.pendingMultiKind = -1;
            ::ImGui::CloseCurrentPopup();
        }
        ::ImGui::EndDisabled();
        ::ImGui::SameLine();
        if (::ImGui::Button("Cancel, continue to narrow", ImVec2(170, 0))) {
            if (app.pendingMultiAction == 2) {
                if (bool* state = lockFor(app, kind)) *state = false;
            }
            app.pendingMultiAction = 0;
            app.pendingMultiKind = -1;
            ::ImGui::CloseCurrentPopup();
        }
        ::ImGui::EndPopup();
    }

    if (app.requestRestoreConfirm) {
        ::ImGui::OpenPopup("Confirm to restore archive");
        app.requestRestoreConfirm = false;
    }
    if (::ImGui::BeginPopupModal("Confirm to restore archive", nullptr,
                                 ImGuiWindowFlags_AlwaysAutoResize)) {
        const BackupInfo* info =
            (app.selectedBackup >= 0 && app.selectedBackup < (int)app.backups.size())
                ? &app.backups[(size_t)app.selectedBackup]
                : nullptr;
        if (info) {
            ::ImGui::Text("File: %s", W2U(info->profile).c_str());
            ::ImGui::Text("archive location: %s", W2U(info->slot).c_str());
            ::ImGui::Text("Backup time: %s", W2U(info->time).c_str());
        }
        DWORD gamePid = 0;
        const bool gameRunning = findProcess(selectedProcessName(), &gamePid, nullptr);
        if (gameRunning) {
            ui::textf(ImVec4(0.95f, 0.40f, 0.35f, 1.0f),
                                 "%s is still running (PID %u), please exit the game first.\nThe", selectedGameDescriptor().shortName, gamePid);
        } else {
            ::ImGui::TextWrapped("program will first back up the current archive and then overwrite and restore it. Please make sure you have paused Steam Cloud Sync or learn how conflicts are handled.");
        }
        ::ImGui::Spacing();
        ::ImGui::BeginDisabled(!info || gameRunning);
        if (::ImGui::Button("Back up the current status and restore", ImVec2(210, 0))) {
            performRestoreSelectedBackup(app);
            ::ImGui::CloseCurrentPopup();
        }
        ::ImGui::EndDisabled();
        ::ImGui::SameLine();
        if (::ImGui::Button("Cancel", ImVec2(100, 0))) ::ImGui::CloseCurrentPopup();
        ::ImGui::EndPopup();
    }
}

}  // namespace

void renderApp(AppState& app) {
    applyPending(app);
    consumePendingEconomy(app);  // 经济定位线程的结构化结果由本线程统一落地
    consumePendingCopy(app);
    consumePendingTransferInspection(app);
    consumePendingFleet(app);
    consumePendingHomeInspection(app);
    consumePendingTrailerSwapInspection(app);
    consumePendingSplit(app);
    consumePendingExport(app);
    // 刷新写入闸门策略快照（pid / 联运策略）：工作线程只读这份原子快照
    updateWriteGatePolicy(app);
    // 发动机后台（换车/新基准/恢复成败/待恢复记录变化）→ 立即持久化。
    // 不从维护线程碰 ImGui / UI 状态；持久化失败保留修订号差值以便节流重试。
    if (app.engineTuner) {
        const uint64_t revision = app.engineTuner->stateRevision();
        if (revision != app.lastEngineStateRevision &&
            ::GetTickCount64() - app.lastEngineSaveTick >= 1000) {
            app.lastEngineSaveTick = ::GetTickCount64();
            app.engineStateText = app.engineTuner->exportState();
            if (saveSettings(app)) app.lastEngineStateRevision = revision;
        }
    }
    ImGuiIO& io = ::ImGui::GetIO();
    static bool showFullLog = false;
    const bool attached = app.mem.isOpen() && app.mem.alive();
    ::ImGui::SetNextWindowPos(ImVec2(0, 0));
    ::ImGui::SetNextWindowSize(io.DisplaySize);
    ::ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ::ImGui::Begin("##main", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    ::ImGui::PopStyleVar();

    ::ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0);
    ::ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 16));
    ::ImGui::BeginChild("##cockpit_header", ImVec2(0, 126),
        ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    ::ImGui::BeginGroup();
    ::ImGui::PushStyleColor(ImGuiCol_Text, ui::accent);
    ui::heading("TRUCK TOOLS", 23);
    ::ImGui::PopStyleColor();
    ::ImGui::TextDisabled("ETS2 2 / ATS Modifier");
    ::ImGui::EndGroup();
    ::ImGui::SameLine(260);
    ::ImGui::BeginGroup();
    ::ImGui::TextDisabled("Target game");
    for (const GameId id : {GameId::Ets2, GameId::Ats}) {
        if (id == GameId::Ats) ::ImGui::SameLine();
        const auto& game = gameDescriptor(id);
        if (ui::choice(game.shortName, selectedGame() == id, ImVec2(80, 34)) && selectedGame() != id)
            selectGame(app, id);
    }
    ::ImGui::EndGroup();
    ::ImGui::SameLine(std::max(470.0f, ::ImGui::GetWindowWidth() - 345.0f));
    ::ImGui::BeginGroup();
    ui::textf(attached ? ui::success : ui::muted,
        attached ? "is connected to the game" : "Not connected to game");
    if (attached) {
        if (::ImGui::Button("disconnected", ImVec2(110, 34))) detachGame(app);
    } else if (ui::primaryButton("Attach game", ImVec2(110, 34))) attachGame(app);
    ::ImGui::SameLine();
    if (::ImGui::Button("Refresh detection", ImVec2(100, 34))) {
        DWORD pid = 0;
        std::wstring exe;
        if (findProcess(selectedProcessName(), &pid, &exe)) {
            if (!attached) {
                app.procStatus = fmt("found %s (PID %u) - click \"Additional Games\" to start", W2U(exe).c_str(), pid);
                logLine(app.procStatus);
            }
        } else {
            app.procStatus = fmt("%s not found, please start the game first", W2U(selectedProcessName()).c_str());
            logLine(app.procStatus);
        }
    }
    ::ImGui::EndGroup();
    ::ImGui::Spacing();
    const char* guard = !attached ? "online status to be detected" :
        (!app.mpHits.empty() ? "TruckersMP is online" :
         (app.mpBlocks ? "Online protection: writing restricted" :
          (isConvoyCheckEnabled() ? "No connection detected" : "Convoy not detected")));
    ui::textf(app.mpBlocks ? ui::accent : ui::muted, "%s  /  %s", guard,
        isAdmin() ? "Administrator" : "Normal permissions");
    if (::ImGui::IsItemHovered())
        ::ImGui::SetTooltip("%s\n%s", app.procStatus.c_str(), app.memInfo.c_str());
    if (!supportsVerifiedStructures(selectedGame())) {
        ui::next(330);
        ui::textf(ui::accent, "The current version is not verified, structured writing is disabled");
    }
    ::ImGui::EndChild();
    ::ImGui::PopStyleVar(2);

    const float footerHeight = showFullLog ? 192.0f : 64.0f;
    const float bodyHeight = std::max(180.0f, ::ImGui::GetContentRegionAvail().y - footerHeight - 10.0f);
    ::ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ::ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0);
    ::ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::background);
    ::ImGui::BeginChild("##workspace", ImVec2(0, bodyHeight), ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar);
    ::ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 20));
    ::ImGui::BeginChild("##navigation", ImVec2(182, 0),
        ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    ::ImGui::PopStyleVar();
    ::ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 7));
    ::ImGui::TextDisabled("WORKSPACE");
    ::ImGui::Dummy(ImVec2(0, 12));
    const int order[] = {1, 0, 2, 4, 3, 5};
    const char* labels[] = {"Money and XP", "Vehicle tuning", "Teleport and camera", "Memory scanner", "Saves and backups", "Settings and help"};
    const char* subtitles[] = {"Capital and growth are clear and controllable.", "Your truck, your settings.",
        "Go where you want to go and change your perspective.", "Find the target from the numerical changes.",
        "saves every departure and leaves a way out for changes.", "Adapt the tool to your habits."};
    const char* sections[] = {"ECONOMY", "VEHICLE CONTROL", "WORLD & CAMERA", "MEMORY SCANNER", "SAVE LIBRARY", "PREFERENCES"};
    if (app.activeTab < 0 || app.activeTab > 5) app.activeTab = 1;
    for (const int page : order) {
        ::ImGui::PushID(page);
        ::ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ::ImGui::PushStyleColor(ImGuiCol_Text, ui::muted);
        if (ui::choice(labels[page], app.activeTab == page, ImVec2(-1, 44))) app.activeTab = page;
        ::ImGui::PopStyleColor(2);
        ::ImGui::PopID();
    }
    if (::ImGui::GetContentRegionAvail().y > 60) {
        ::ImGui::SetCursorPosY(::ImGui::GetWindowHeight() - 67);
        ::ImGui::TextDisabled(selectedGame() == GameId::Ets2 ? "EUROPEAN ROADS" : "AMERICAN ROADS");
        ::ImGui::TextDisabled("Ready for the next journey.");
    }
    ::ImGui::PopStyleVar();
    ::ImGui::EndChild();
    ::ImGui::SameLine();
    ::ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 20));
    // Each game/workspace owns its scroll position and widget focus.
    ::ImGui::PushID(gameKey(selectedGame()));
    ::ImGui::PushID(app.activeTab);
    ::ImGui::BeginChild("##page_content", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    ::ImGui::PopStyleVar();
    ::ImGui::PopStyleColor();
    ::ImGui::PopStyleVar(2);
    ui::textf(ui::accent, "%s", sections[app.activeTab]);
    ui::heading(labels[app.activeTab], 28);
    ui::text(ui::muted, subtitles[app.activeTab]);
    ::ImGui::Dummy(ImVec2(0, 10));
    switch (app.activeTab) {
        case 0: renderMoneyTab(app); break;
        case 1: renderVehicleTabImpl(app); break;
        case 2: renderConsoleTabImpl(app); break;
        case 3: renderScannerTabImpl(app); break;
        case 4: renderSaveTabImpl(app); break;
        case 5: renderHelpTabImpl(app); break;
    }
    ::ImGui::EndChild();
    ::ImGui::PopID();
    ::ImGui::PopID();
    ::ImGui::EndChild();

    ::ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0);
    ::ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 12));
    ::ImGui::BeginChild("##session_footer", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ::ImGui::PopStyleVar(2);
    if (::ImGui::BeginTable("##footer_columns", 2, ImGuiTableFlags_SizingStretchProp)) {
        ::ImGui::TableSetupColumn("status", ImGuiTableColumnFlags_WidthStretch);
        ::ImGui::TableSetupColumn("actions", ImGuiTableColumnFlags_WidthFixed, 225);
        ::ImGui::TableNextColumn();
        if (app.busy.load()) {
            const uint64_t done = app.progressDone.load(), total = app.progressTotal.load();
            const float fraction = total ? std::clamp(float(double(done) / double(total)), 0.0f, 1.0f) : 0;
            ::ImGui::ProgressBar(fraction, ImVec2(150, 18));
            ui::next(180);
            ::ImGui::TextUnformatted(app.progressNote.c_str());
        } else {
            const auto lines = logSnapshot();
            const std::string latest = lines.empty() ? app.procStatus : lines.back();
            // A clipped single-line summary keeps the footer stable; hover exposes the whole entry.
            ::ImGui::TextUnformatted(latest.c_str());
            if (::ImGui::IsItemHovered()) ::ImGui::SetTooltip("%s", latest.c_str());
        }
        ::ImGui::TableNextColumn();
        if (app.busy.load()) {
            if (::ImGui::SmallButton("Cancel task")) { app.cancel.store(true); logLine("has requested to cancel the current task..."); }
            ::ImGui::SameLine();
        }
        if (::ImGui::SmallButton(showFullLog ? "Close log" : "Session log")) showFullLog = !showFullLog;
        ::ImGui::SameLine();
        if (::ImGui::SmallButton("Clear")) logClear();
        ::ImGui::EndTable();
    }
    if (!logFileError().empty()) ui::text(ImVec4(1, .4f, .3f, 1), logFileError().c_str());
    if (showFullLog) {
        ui::text(ui::muted, W2U(logFilePath()).c_str());
        ::ImGui::BeginChild("##log_expanded", ImVec2(0, 0), ImGuiChildFlags_None,
            ImGuiWindowFlags_HorizontalScrollbar);
        const auto lines = logSnapshot();
        const bool follow = ::ImGui::GetScrollY() >= ::ImGui::GetScrollMaxY();
        for (const auto& line : lines) ::ImGui::TextUnformatted(line.c_str());
        if (follow) ::ImGui::SetScrollHereY(1);
        ::ImGui::EndChild();
    }
    ::ImGui::EndChild();
    renderConfirmDialogs(app);
    ::ImGui::End();
}

}  // namespace ets2
