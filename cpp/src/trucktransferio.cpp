#include "trucktransferio.h"
#include "bsii.h"
#include "homepreset.h"
#include "fueltank.h"
#include "fueltankassets.h"
#include <filesystem>
#include <cstring>
#include <memory>
#include <stdexcept>
namespace ets2 {
namespace {
namespace fs=std::filesystem;
struct ReadLock {
    HANDLE h=INVALID_HANDLE_VALUE;
    explicit ReadLock(const std::wstring& path) {
        h=::CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                       FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        BY_HANDLE_FILE_INFORMATION info{};
        if(h==INVALID_HANDLE_VALUE || !::GetFileInformationByHandle(h,&info) ||
           (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))) {
            if(h!=INVALID_HANDLE_VALUE) ::CloseHandle(h);
            h=INVALID_HANDLE_VALUE;
            throw std::runtime_error("The archive cannot be read stably, please exit the game and wait for synchronization to complete; reparsing files does not support importing");
        }
    }
    ~ReadLock(){if(h!=INVALID_HANDLE_VALUE)::CloseHandle(h);}
    ReadLock(const ReadLock&)=delete;
};
bool loadText(const std::wstring& path,std::string* out,std::string* error,size_t limit,
              const std::function<bool()>& canceled = {}) {
    out->clear();
    if(canceled && canceled()) { if(error)*error="Reading archive has been canceled";return false; }
    std::vector<uint8_t> bytes; DecryptInfo info;
    if(!decryptSave(path,bytes,&info,error,limit)) return false;
    if(info.innerFormat=="bsii") {
        std::string detail;
        if(decodeBsiiText(bytes,out,&detail,canceled,limit)) return true;
        if(error)*error="Binary archive conversion failed:"+detail;
        return false;
    }
    if(info.innerFormat!="text" || bytes.size()<8 || ::memcmp(bytes.data(),"SiiNunit",8)) {
        if(error)*error="Archive format not recognized; encrypted archives, BSII v3 and SiiNunit text archives are supported.";
        return false;
    }
    out->assign(bytes.begin(),bytes.end()); return true;
}
bool sameSlot(const SaveSlot& a,const SaveSlot& b) {
    return ::_wcsicmp(a.slotDir.c_str(),b.slotDir.c_str())==0 &&
           ::_wcsicmp(a.profileDir.c_str(),b.profileDir.c_str())==0;
}
}
bool inspectTruckTransferSlot(const SaveSlot& slot,TransferInventory* inventory,std::string* error,
                              std::function<bool()> canceled) {
    if(inventory)*inventory={};
    try {
        ReadLock game(slot.gameSii);
        std::string text;
        return loadText(slot.gameSii,&text,error,128ull*1024*1024,canceled) &&
               inspectTruckTransferText(text,inventory,error,canceled);
    } catch(const std::exception& e) {if(error)*error=e.what();return false;}
}
bool inspectFleetSlot(const SaveSlot& slot,FleetInventory* inventory,std::string* error,
                     std::function<bool()> canceled) {
    if(inventory) *inventory={};
    try {
        if(selectedGame()!=GameId::Ets2) throw std::runtime_error("fleet economic management currently only supports ETS2");
        ReadLock game(slot.gameSii); std::string text;
        return loadText(slot.gameSii,&text,error,128ull*1024*1024,canceled) &&
               inspectFleetText(text,inventory,error,canceled);
    } catch(const std::exception& e) {if(error)*error=e.what();return false;}
}
SaveCopyResult fleetToNewSave(const FleetRequest& request,std::function<bool()> canceled) {
    SaveCopyResult failure;failure.game=request.game;
    try {
        auto gate=[&] {
            if(canceled && canceled()) throw std::runtime_error("fleet expansion has been cancelled");
            if(request.game!=GameId::Ets2 || selectedGame()!=request.game)
                throw std::runtime_error("Fleet economic management currently only supports ETS2, please re-select the archive");
            std::string error;
            if(copyBlockedByRunningGame(request.game,&error)) throw std::runtime_error(error);
        };
        gate();
        SaveSlot fresh;bool found=false;
        for(const auto& slot:listSlots()) if(sameSlot(slot,request.source)) {fresh=slot;found=true;break;}
        if(!found) throw std::runtime_error("The archive does not belong to the current game, please refresh and select again.");
        ReadLock game(fresh.gameSii),info((fs::path(fresh.slotDir)/L"info.sii").wstring());
        std::string text,result,error,escaped;
        if(!siiEscapeName(request.displayName,&escaped,&error) ||
           !loadText(fresh.gameSii,&text,&error,128ull*1024*1024,canceled) ||
           !expandFleetText(text,request.expansion,&result,&error,canceled))
            throw std::runtime_error(error);
        gate();
        return copySlotToNewSave({request.game,fresh,request.displayName},std::move(canceled),&result);
    } catch(const std::exception& e) {failure.error=e.what();return failure;}
}
bool inspectHomeAccessoriesSlot(const SaveSlot& slot,HomeAccessoryInventory* inventory,
                               std::string* error,std::function<bool()> canceled) {
    if(inventory) *inventory={};
    if(selectedGame()!=GameId::Ets2) {
        if(error) *error="Home accessory code currently only supports ETS2";
        return false;
    }
    try {
        ReadLock game(slot.gameSii);
        std::string text;
        return loadText(slot.gameSii,&text,error,128ull*1024*1024,canceled) &&
               inspectHomeAccessoriesText(text,inventory,error,canceled);
    } catch(const std::exception& e) {if(error)*error=e.what();return false;}
}

SaveCopyResult homeAccessoryToNewSave(const HomeAccessoryRequest& request,
                                     std::function<bool()> canceled) {
    SaveCopyResult failure; failure.game=request.game;
    try {
        auto gate=[&] {
            if(request.game!=GameId::Ets2 || selectedGame()!=request.game)
                throw std::runtime_error("Home accessory code currently only supports ETS2, please re-select the archive");
            if(canceled && canceled()) throw std::runtime_error("Home accessories modification has been canceled");
            std::string error;
            if(copyBlockedByRunningGame(request.game,&error)) throw std::runtime_error(error);
        };
        gate();
        std::string error,escaped;
        if(request.scaniaTvPreset && request.tankCapacity) throw std::runtime_error("Accessory preset and fuel tank expansion cannot be executed at the same time");
        if((!request.scaniaTvPreset && !request.tankCapacity && !validateHomeAccessoryPath(request.newPath,&error)) ||
           !siiEscapeName(request.displayName,&escaped,&error)) throw std::runtime_error(error);
        SaveSlot fresh; bool found=false;
        for(const auto& slot:listSlots()) {
            if(sameSlot(slot,request.source)){fresh=slot;found=true;break;}
        }
        if(!found) throw std::runtime_error("The source archive does not belong to the current game, please refresh and select again.");
        ReadLock game(fresh.gameSii);
        ReadLock info((fs::path(fresh.slotDir)/L"info.sii").wstring());
        std::string text,replacement;
        if(request.scaniaTvPreset) {
            std::string metadata;
            if(!loadText((fs::path(fresh.slotDir)/L"info.sii").wstring(),&metadata,&error,4ull*1024*1024,canceled) ||
               !validateScaniaTvPresetInfo(metadata,&error) ||
               !loadText(fresh.gameSii,&text,&error,128ull*1024*1024,canceled) ||
               !installScaniaTvPresetText(text,request.truckId,&replacement,&error,canceled))
                throw std::runtime_error(error);
            gate();
            ScaniaTvPresetAssets assets;
            if(!assets.install(documentsDir(),&error,gate)) throw std::runtime_error(error);
            auto result=copySlotToNewSave({request.game,fresh,request.displayName},canceled,&replacement);
            if(result.ok) assets.commit();
            else {
                std::string rollbackError;
                if(!assets.rollback(&rollbackError)) result.error+="；"+rollbackError;
            }
            return result;
        }
        HomeAccessoryInventory inventory;
        if(!loadText(fresh.gameSii,&text,&error,128ull*1024*1024,canceled) ||
           !inspectHomeAccessoriesText(text,&inventory,&error,canceled))
            throw std::runtime_error(error);
        const HomeAccessoryEntry* chosen=nullptr;
        for(const auto& entry:inventory.entries)
            if(entry.truckId==request.truckId && entry.unitId==request.unitId) {chosen=&entry;break;}
        if(!chosen || chosen->shared || chosen->dataPath!=request.originalPath)
            throw std::runtime_error("The selected accessory has been changed or shared, please re-read and preview");
        if(request.tankCapacity) {
            if(chosen->type!="vehicle_accessory" || chosen->dataPath.find("/chassis/")==std::string::npos)
                throw std::runtime_error("Please select supported original chassis accessories");
            std::vector<std::string> engines;
            for(const auto& accessory:inventory.entries)if(accessory.truckId==request.truckId &&
                accessory.dataPath.find("/engine/")!=std::string::npos)engines.push_back(accessory.dataPath);
            FuelTankMod mod;
            if(!buildFuelTankMod(chosen->dataPath,request.tankCapacity,&mod,&error) ||
               !validateFuelTankPowertrain(mod,engines,&error) ||
               !replaceHomeAccessoryPath(text,request.truckId,request.unitId,mod.chassisPath,&replacement,&error,canceled))
                throw std::runtime_error(error);
            gate();FuelTankModAssets assets;
            if(!assets.install(documentsDir(),mod.fileName,mod.archive,&error,gate))throw std::runtime_error(error);
            auto result=copySlotToNewSave({request.game,fresh,request.displayName},canceled,&replacement);
            if(result.ok) {
                assets.commit();
                result.note=std::string(mod.electric?"permanent battery ":"permanent fuel tank ")+std::to_string(mod.originalCapacity)+" → "+std::to_string(mod.capacity)+
                    (mod.electric?" kWh":" Liter")+". Please enable \""+mod.displayName+"\", then load this copy; keep it enabled while using the copy."
                    "Mod file:"+mod.fileName+". Original chassis path:"+mod.originalPath+". This plan is for single use.";
            } else {
                std::string rollbackError;if(!assets.rollback(&rollbackError))result.error+="；"+rollbackError;
            }
            return result;
        }
        if(request.newPath==chosen->dataPath) throw std::runtime_error("The new path is the same as the current path and does not need to be modified.");
        if(!replaceHomeAccessoryPath(text,request.truckId,request.unitId,request.newPath,
                                     &replacement,&error,canceled)) throw std::runtime_error(error);
        gate();
        return copySlotToNewSave({request.game,fresh,request.displayName},std::move(canceled),&replacement);
    } catch(const std::exception& e) {failure.error=e.what();return failure;}
}
bool inspectTrailerSwapSlot(const SaveSlot& slot,TrailerSwapInventory* inventory,
                            std::string* error,std::function<bool()> canceled) {
    if(inventory) *inventory={};
    try {
        ReadLock game(slot.gameSii);
        std::string text;
        return loadText(slot.gameSii,&text,error,128ull*1024*1024,canceled) &&
               inspectTrailerSwapText(text,inventory,error,canceled);
    } catch(const std::exception& e) {if(error)*error=e.what();return false;}
}

SaveCopyResult trailerSwapToNewSave(const TrailerSwapRequest& request,
                                    std::function<bool()> canceled) {
    SaveCopyResult failure;failure.game=request.game;
    try {
        auto gate=[&] {
            if(canceled && canceled()) throw std::runtime_error("Trailer replacement has been cancelled.");
            if(selectedGame()!=request.game)
                throw std::runtime_error("Game selection has changed, please refresh and re-select the archive");
            std::string error;
            if(copyBlockedByRunningGame(request.game,&error)) throw std::runtime_error(error);
        };
        gate();
        std::string error,escaped;
        if(!siiEscapeName(request.displayName,&escaped,&error)) throw std::runtime_error(error);
        SaveSlot fresh; bool found=false;
        for(const auto& slot:listSlots()) {
            if(sameSlot(slot,request.source)){fresh=slot;found=true;break;}
        }
        if(!found) throw std::runtime_error("The source archive does not belong to the current game, please refresh and select again.");
        ReadLock game(fresh.gameSii);
        ReadLock info((fs::path(fresh.slotDir)/L"info.sii").wstring());
        std::string text,replacement;
        if(!loadText(fresh.gameSii,&text,&error,128ull*1024*1024,canceled) ||
           !swapTrailerText(text,request.targetTrailerId,&replacement,&error,canceled))
            throw std::runtime_error(error);
        gate();
        return copySlotToNewSave({request.game,fresh,request.displayName},std::move(canceled),
                                 &replacement);
    } catch(const std::exception& e) {failure.error=e.what();return failure;}
}

bool inspectTruckTransferTarget(const SaveSlot& source,const SaveSlot& target,
                                TransferInventory* inventory,std::string* error,
                                std::function<bool()> canceled) {
    if(inventory)*inventory={};
    try {
        ReadLock sourceInfo((fs::path(source.slotDir)/L"info.sii").wstring());
        ReadLock targetInfo((fs::path(target.slotDir)/L"info.sii").wstring());
        std::string srcInfo,dstInfo;
        if(!loadText((fs::path(source.slotDir)/L"info.sii").wstring(),&srcInfo,error,4*1024*1024,canceled) ||
           !loadText((fs::path(target.slotDir)/L"info.sii").wstring(),&dstInfo,error,4*1024*1024,canceled) ||
           !validateTruckTransferMetadata(srcInfo,dstInfo,error)) return false;
        return inspectTruckTransferSlot(target,inventory,error,canceled);
    } catch(const std::exception& e) {if(error)*error=e.what();return false;}
}
SaveCopyResult transferTruckToNewSave(const TruckTransferRequest& request,std::function<bool()> canceled) {
    SaveCopyResult failure;failure.game=request.game;
    try {
        auto gate=[&] {
            if(canceled && canceled()) throw std::runtime_error("Vehicle import has been canceled");
            if(selectedGame()!=request.game) throw std::runtime_error("The game selection has changed, please select the source and target again.");
            std::string error;
            if(copyBlockedByRunningGame(request.game,&error)) throw std::runtime_error(error);
        };
        gate();
        SaveSlot source,target; bool hasSource=false,hasTarget=false;
        for(const auto& slot:listSlots()) {
            if(sameSlot(slot,request.source)){source=slot;hasSource=true;}
            if(sameSlot(slot,request.target)){target=slot;hasTarget=true;}
        }
        if(!hasSource || !hasTarget) throw std::runtime_error("The source or target archive does not belong to the current game, please refresh and select again.");
        if(fs::equivalent(source.gameSii,target.gameSii))
            throw std::runtime_error("Please select another target archive. The source archive and the target cannot be the same.");
        // Keep both game and metadata snapshots immutable until the new slot is published.
        ReadLock sourceGame(source.gameSii),targetGame(target.gameSii);
        ReadLock sourceInfo((fs::path(source.slotDir)/L"info.sii").wstring());
        ReadLock targetInfo((fs::path(target.slotDir)/L"info.sii").wstring());
        std::string src,dst,srcInfo,dstInfo,error,result;
        if(!loadText(source.gameSii,&src,&error,128ull*1024*1024,canceled) ||
           !loadText(target.gameSii,&dst,&error,128ull*1024*1024,canceled) ||
           !loadText((fs::path(source.slotDir)/L"info.sii").wstring(),&srcInfo,&error,4*1024*1024,canceled) ||
           !loadText((fs::path(target.slotDir)/L"info.sii").wstring(),&dstInfo,&error,4*1024*1024,canceled))
            throw std::runtime_error(error);
        if(!validateTruckTransferMetadata(srcInfo,dstInfo,&error))
            throw std::runtime_error(error);
        gate();
        if(!transferTruckText(src,dst,request.truckId,request.garageId,&result,&error,canceled))
            throw std::runtime_error("Vehicle data verification failed:"+error);
        gate();
        SaveCopyRequest copy{request.game,target,request.displayName};
        return copySlotToNewSave(copy,std::move(canceled),&result);
    } catch(const std::exception& e) {failure.error=e.what();return failure;}
}
}
