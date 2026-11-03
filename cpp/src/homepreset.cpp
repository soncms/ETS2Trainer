#include "homepreset.h"
#include "common.h"
#include "savecopy.h"
#include <bcrypt.h>
#include <filesystem>
#include <stdexcept>
#include <unordered_set>

namespace ets2 {
namespace {
namespace fs=std::filesystem;
struct PresetAssetSpec { int id; const wchar_t* path; size_t size; const char* sha; };
#include "homepreset_manifest.h"
struct Asset { fs::path path; std::vector<uint8_t> bytes; };
std::string digest(const std::vector<uint8_t>& bytes) {
    BCRYPT_ALG_HANDLE alg=nullptr;
    if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)
        throw std::runtime_error("Unable to verify built-in model resources");
    uint8_t hash[32];
    const auto status=BCryptHash(alg,nullptr,0,const_cast<PUCHAR>(bytes.data()),(ULONG)bytes.size(),hash,32);
    BCryptCloseAlgorithmProvider(alg,0);
    if(status<0) throw std::runtime_error("Built-in model resource verification failed");
    const char* hex="0123456789abcdef";std::string result;
    for(auto b:hash){result+=hex[b>>4];result+=hex[b&15];}return result;
}
std::vector<Asset> embeddedAssets() {
    std::vector<Asset> result;
    for(const auto& spec:kPresetAssets) {
        const auto resource=FindResourceW(nullptr,MAKEINTRESOURCEW(spec.id),RT_RCDATA);
        if(!resource || SizeofResource(nullptr,resource)!=spec.size)
            throw std::runtime_error("This version does not contain complete three-screen TV model resources");
        const auto loaded=LoadResource(nullptr,resource);
        const auto* bytes=static_cast<const uint8_t*>(LockResource(loaded));
        if(!bytes) throw std::runtime_error("Failed to read built-in model resources");
        Asset asset{fs::path(spec.path),{bytes,bytes+spec.size}};
        asset.path.make_preferred();
        if(digest(asset.bytes)!=spec.sha) throw std::runtime_error("The content of built-in model resources does not match the list");
        if(asset.path.is_absolute() || asset.path.empty()) throw std::runtime_error("Invalid resource path");
        for(const auto& part:asset.path) if(part==L".." || part==L".") throw std::runtime_error("Invalid resource path");
        result.push_back(std::move(asset));
    }
    return result;
}
void requireDirectory(const fs::path& path) {
    const auto attributes=GetFileAttributesW(path.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES || !(attributes&FILE_ATTRIBUTE_DIRECTORY) ||
       (attributes&FILE_ATTRIBUTE_REPARSE_POINT)) throw std::runtime_error("The resource directory is unavailable or contains a reparse point");
}
void requireParents(const fs::path& path) {
    if(!path.is_absolute() || path.lexically_normal()!=path) throw std::runtime_error("resource directory must be a canonical absolute path");
    fs::path current=path.root_path();requireDirectory(current);
    for(const auto& part:path.relative_path()){current/=part;requireDirectory(current);}
}
HANDLE readLock(const fs::path& path,bool directory=false) {
    HANDLE h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                        FILE_FLAG_OPEN_REPARSE_POINT|(directory ? FILE_FLAG_BACKUP_SEMANTICS : 0),nullptr);
    BY_HANDLE_FILE_INFORMATION info{};
    if(h==INVALID_HANDLE_VALUE || !GetFileInformationByHandle(h,&info) ||
       (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) ||
       bool(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=directory) {
        if(h!=INVALID_HANDLE_VALUE) CloseHandle(h);
        throw std::runtime_error("Unable to lock model resources stably, please wait for synchronization to complete and try again.");
    }
    return h;
}
bool sameFile(HANDLE h,const std::vector<uint8_t>& expected) {
    LARGE_INTEGER size{};
    if(!GetFileSizeEx(h,&size) || size.QuadPart!=(LONGLONG)expected.size()) return false;
    LARGE_INTEGER zero{};if(!SetFilePointerEx(h,zero,nullptr,FILE_BEGIN)) return false;
    std::vector<uint8_t> actual(expected.size());DWORD read=0;
    return ReadFile(h,actual.data(),(DWORD)actual.size(),&read,nullptr) && read==actual.size() && actual==expected;
}
void writeNew(const fs::path& path,const std::vector<uint8_t>& bytes) {
    HANDLE h=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE) throw std::runtime_error("Failed to create model resource file");
    DWORD written=0;
    const bool ok=WriteFile(h,bytes.data(),(DWORD)bytes.size(),&written,nullptr) &&
                  written==bytes.size() && FlushFileBuffers(h);
    CloseHandle(h);
    if(!ok) throw std::runtime_error("Failed to write model resources");
}
void removeOwnedTree(const fs::path& path,const fs::path& parent) {
    if(!fs::exists(path)) return;
    if(path.parent_path()!=parent) throw std::runtime_error("The resource rollback directory is out of bounds");
    requireParents(path);
    for(const auto& entry:fs::recursive_directory_iterator(path)) {
        const auto a=GetFileAttributesW(entry.path().c_str());
        if(a==INVALID_FILE_ATTRIBUTES || (a&FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Resource rollback detects reparse point, retains existing files");
    }
    fs::remove_all(path);
}
}
struct ScaniaTvPresetAssets::Impl {
    fs::path root,target,stage;
    std::vector<fs::path> createdDirs;
    std::vector<Asset> assets;
    std::vector<HANDLE> locks;
    bool newTarget=false,stageOwned=false,committed=false;
    void unlock(){for(auto h:locks)CloseHandle(h);locks.clear();}
    void lockAssets() {
        requireParents(target);
        locks.push_back(readLock(target,true));
        for(const auto& asset:assets) {
            const auto file=root/asset.path;
            requireParents(file.parent_path());
            HANDLE h=readLock(file);locks.push_back(h);
            if(!sameFile(h,asset.bytes)) throw std::runtime_error("Existing MoRan assets differ from this preset and importing will not overwrite them");
        }
    }
};
bool scaniaTvPresetAvailable() {
    for(const auto& spec:kPresetAssets) {
        const auto r=FindResourceW(nullptr,MAKEINTRESOURCEW(spec.id),RT_RCDATA);
        if(!r || SizeofResource(nullptr,r)!=spec.size) return false;
    }
    return true;
}
ScaniaTvPresetAssets::ScaniaTvPresetAssets():impl_(new Impl){}
ScaniaTvPresetAssets::~ScaniaTvPresetAssets() {
    std::string error;
    if(!rollback(&error) && !error.empty()) logLine("Home resource rollback:"+error);
}
bool ScaniaTvPresetAssets::createdNew() const {return impl_->newTarget;}
void ScaniaTvPresetAssets::commit(){impl_->committed=true;impl_->unlock();}
bool ScaniaTvPresetAssets::install(const std::wstring& gameHome,std::string* error,std::function<void()> guard) {
    if(error) error->clear();
    auto& state=*impl_;
    try {
        if(!state.root.empty()) throw std::runtime_error("Resource installation transaction cannot be reused");
        if(guard) guard();
        state.root=fs::path(gameHome);requireParents(state.root);
        state.target=state.root/L"MoRan"/L"scania"/L"TV";
        state.assets=embeddedAssets();
        if(fs::exists(state.target)) {state.lockAssets();return true;}
        auto current=state.root;
        for(const wchar_t* name:{L"MoRan",L"scania"}) {
            if(guard) guard();
            current/=name;
            if(fs::exists(current)) requireDirectory(current);
            else {
                if(!fs::create_directory(current)) throw std::runtime_error("model directory creation conflict");
                state.createdDirs.push_back(current);
            }
        }
        state.stage=state.root/(L"~ets2home_tv_"+std::to_wstring(GetCurrentProcessId())+L"_"+std::to_wstring(GetTickCount64()));
        if(!fs::create_directory(state.stage)) throw std::runtime_error("Model temporary directory creation conflict");
        state.stageOwned=true;
        for(const auto& asset:state.assets) {
            if(guard) guard();
            // The installed TV subtree starts after MoRan/scania/TV.
            fs::path relative;
            int depth=0;for(const auto& part:asset.path) if(++depth>3) relative/=part;
            const auto file=state.stage/relative;
            fs::create_directories(file.parent_path());requireParents(file.parent_path());
            writeNew(file,asset.bytes);
        }
        if(guard) guard();
        publishNewDirectory(state.stage.wstring(),state.target.wstring(),[&] {
            if(guard) guard();
            requireParents(state.target.parent_path());
        });
        state.stage.clear();state.stageOwned=false;state.newTarget=true;
        state.lockAssets();return true;
    } catch(const std::exception& e) {
        if(error) *error=e.what();
        std::string rollbackError;
        if(!rollback(&rollbackError) && error) *error+="；"+rollbackError;
        return false;
    }
}
bool ScaniaTvPresetAssets::rollback(std::string* error) {
    if(error) error->clear();
    auto& state=*impl_;
    state.unlock();
    if(state.committed) return true;
    try {
        if(state.stageOwned) {removeOwnedTree(state.stage,state.root);state.stage.clear();state.stageOwned=false;}
        if(state.newTarget) {
            // Do not remove files introduced/changed by another actor after installation.
            std::unordered_set<std::wstring> expected;
            for(const auto& asset:state.assets) {
                const auto file=state.root/asset.path;expected.insert(file.wstring());
                for(auto parent=file.parent_path();parent!=state.target;parent=parent.parent_path())
                    expected.insert(parent.wstring());
            }
            requireParents(state.target);
            for(const auto& entry:fs::recursive_directory_iterator(state.target))
                if(!expected.count(entry.path().wstring()))
                    throw std::runtime_error("There are other files in the model directory and the resources have been reserved");
            for(const auto& asset:state.assets) {
                const auto file=state.root/asset.path;requireParents(file.parent_path());
                HANDLE h=readLock(file);const bool equal=sameFile(h,asset.bytes);CloseHandle(h);
                if(!equal) throw std::runtime_error("model resources have changed and existing files have been retained.");
            }
            removeOwnedTree(state.target,state.target.parent_path());state.newTarget=false;
        }
        for(auto it=state.createdDirs.rbegin();it!=state.createdDirs.rend();++it) {
            requireDirectory(*it);if(fs::is_empty(*it)) fs::remove(*it);
        }
        state.createdDirs.clear();return true;
    } catch(const std::exception& e) {if(error)*error=e.what();return false;}
}
}
