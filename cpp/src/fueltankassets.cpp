#include "fueltankassets.h"
#include "common.h"
#include <filesystem>
#include <stdexcept>
#include <algorithm>
namespace ets2 {
namespace {
namespace fs=std::filesystem;
void needFuelAsset(bool ok,const char* error) {if(!ok)throw std::runtime_error(error);}
bool sameFuelAsset(HANDLE handle,const std::vector<std::uint8_t>& bytes) {
    LARGE_INTEGER size{},start{};
    if(!GetFileSizeEx(handle,&size)||size.QuadPart!=(LONGLONG)bytes.size()||
       !SetFilePointerEx(handle,start,nullptr,FILE_BEGIN))return false;
    std::vector<std::uint8_t> actual(bytes.size());DWORD count=0;
    return ReadFile(handle,actual.data(),(DWORD)actual.size(),&count,nullptr) && count==bytes.size() && actual==bytes;
}
}
struct FuelTankModAssets::Impl {
    std::vector<HANDLE> parents;
    HANDLE file=INVALID_HANDLE_VALUE;
    bool created=false,committed=false;
    ~Impl() {
        if(file!=INVALID_HANDLE_VALUE) {
            if(created&&!committed) {FILE_DISPOSITION_INFO info{TRUE};SetFileInformationByHandle(file,FileDispositionInfo,&info,sizeof(info));}
            CloseHandle(file);
        }
        for(auto it=parents.rbegin();it!=parents.rend();++it)CloseHandle(*it);
    }
    void holdDirectory(const fs::path& p) {
        const auto h=CreateFileW(p.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        BY_HANDLE_FILE_INFORMATION info{};
        if(h==INVALID_HANDLE_VALUE||!GetFileInformationByHandle(h,&info)||
           !(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) {
            if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);
            throw std::runtime_error("Mod directory is unavailable, changing or contains reparse points");
        }
        parents.push_back(h);
    }
};
FuelTankModAssets::FuelTankModAssets():impl_(std::make_unique<Impl>()){}
FuelTankModAssets::~FuelTankModAssets()=default;
bool FuelTankModAssets::install(const std::wstring& gameHome,const std::string& fileName,
    const std::vector<std::uint8_t>& bytes,std::string* error,std::function<void()> guard) {
    if(error)error->clear();
    try {
        needFuelAsset(impl_->file==INVALID_HANDLE_VALUE,"Mod installer cannot be reused");
        needFuelAsset(fileName.rfind("ets2trainer_fuel_",0)==0 && fileName.size()>25 && fileName.size()<100 &&
            fileName.substr(fileName.size()-4)==".scs" && fileName.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_.")==std::string::npos,
            "Fuel Tank Mod file name is invalid");
        needFuelAsset(!bytes.empty() && bytes.size()<=4u*1024u*1024u,"Fuel tank Mod volume is invalid");
        if(guard)guard();
        const fs::path home(gameHome);
        needFuelAsset(home.is_absolute()&&home.lexically_normal()==home,"The game document directory must be a canonical absolute path");
        fs::path parent=home.root_path();impl_->holdDirectory(parent);
        for(const auto& part:home.relative_path()){parent/=part;impl_->holdDirectory(parent);}
        const auto mod=home/L"mod";
        if(!CreateDirectoryW(mod.c_str(),nullptr))needFuelAsset(GetLastError()==ERROR_ALREADY_EXISTS,"Unable to create Mod directory");
        impl_->holdDirectory(mod);
        if(guard)guard();
        const auto target=mod/fs::path(fileName);
        impl_->file=CreateFileW(target.c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,FILE_SHARE_READ,nullptr,CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(impl_->file==INVALID_HANDLE_VALUE) {
            needFuelAsset(GetLastError()==ERROR_FILE_EXISTS||GetLastError()==ERROR_ALREADY_EXISTS,"Unable to create fuel tank Mod file");
            impl_->file=CreateFileW(target.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
            BY_HANDLE_FILE_INFORMATION info{};
            needFuelAsset(impl_->file!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(impl_->file,&info)&&
                !(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)),"cannot stably read the existing fuel tank Mod");
            needFuelAsset(sameFuelAsset(impl_->file,bytes),"There is already a Mod with the same name but the content is different and the original file has been retained.");
        } else {
            impl_->created=true;DWORD written=0;
            needFuelAsset(WriteFile(impl_->file,bytes.data(),(DWORD)bytes.size(),&written,nullptr)&&written==bytes.size()&&
                FlushFileBuffers(impl_->file)&&sameFuelAsset(impl_->file,bytes),"Fuel tank Mod write or readback verification failed");
        }
        if(guard)guard();
        return true;
    } catch(const std::exception& e) {
        if(error)*error=e.what();std::string rollbackError;
        if(!rollback(&rollbackError)&&error)*error+="；"+rollbackError;
        return false;
    }
}
bool FuelTankModAssets::rollback(std::string* error) {
    if(error)error->clear();
    if(impl_->file==INVALID_HANDLE_VALUE||!impl_->created||impl_->committed)return true;
    FILE_DISPOSITION_INFO info{TRUE};
    if(!SetFileInformationByHandle(impl_->file,FileDispositionInfo,&info,sizeof(info))) {
        if(error)*error="Failed to roll back new fuel tank mod, please check the documentation directory mod folder";
        return false;
    }
    CloseHandle(impl_->file);impl_->file=INVALID_HANDLE_VALUE;impl_->created=false;return true;
}
void FuelTankModAssets::commit(){impl_->committed=true;}
}
