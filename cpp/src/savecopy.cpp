#include "savecopy.h"
#include "bsii.h"
#include "gameio.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <memory>
#include <set>
#include <stdexcept>
#include <ctime>
#include <tlhelp32.h>

namespace ets2 {
namespace {
namespace fs = std::filesystem;
constexpr uint64_t kFileLimit = 128ull * 1024 * 1024;
constexpr uint64_t kTotalLimit = 512ull * 1024 * 1024;
constexpr size_t kInfoLimit = 4 * 1024 * 1024;
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h = INVALID_HANDLE_VALUE) : value(h) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE) ::CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return value != INVALID_HANDLE_VALUE; }
};
bool samePath(const fs::path& a, const fs::path& b) { return ::_wcsicmp(a.c_str(), b.c_str()) == 0; }
bool reparse(const fs::path& p) {
    const DWORD a = ::GetFileAttributesW(p.c_str());
    return a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_REPARSE_POINT);
}
bool dots(const fs::path& p) { for (const auto& part : p) if (part == L".." || part == L".") return true; return false; }
uint64_t fileSize(HANDLE h) {
    LARGE_INTEGER n{};
    if (!::GetFileSizeEx(h,&n) || n.QuadPart < 0) throw std::runtime_error("Unable to read file length");
    return static_cast<uint64_t>(n.QuadPart);
}
void rewind(HANDLE h) {
    LARGE_INTEGER zero{};
    if (!::SetFilePointerEx(h,zero,nullptr,FILE_BEGIN)) throw std::runtime_error("Unable to reset file reading position");
}
std::unique_ptr<Handle> lockDirectory(const fs::path& p) {
    auto h = std::make_unique<Handle>(::CreateFileW(p.c_str(),FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr));
    if (!*h) throw std::runtime_error("Unable to lock the archive directory, please end other file operations and try again.");
    return h; // no FILE_SHARE_DELETE: prevents the directory being renamed during the transaction
}
struct Token { std::string text; size_t begin, end; int depth; bool quoted; };
std::vector<Token> tokenize(const std::string& s) {
    std::vector<Token> out;
    int depth = 0;
    for (size_t i=0;i<s.size();) {
        const unsigned char c=static_cast<unsigned char>(s[i]);
        if (c==' ' || c=='\t' || c=='\r' || c=='\n') { ++i; continue; }
        if (c=='#' || (c=='/' && i+1<s.size() && s[i+1]=='/')) {
            i=s.find('\n',i); if(i==std::string::npos) break; continue;
        }
        const size_t begin=i; bool quoted=false;
        if (c=='"') {
            quoted=true; ++i; bool closed=false;
            while (i<s.size()) {
                if (s[i]=='\\') { if(i+1>=s.size()) break; i+=2; }
                else if (s[i]=='"') { ++i; closed=true; break; }
                else if (s[i]=='\r' || s[i]=='\n' || s[i]=='\0') break;
                else ++i;
            }
            if(!closed) throw std::runtime_error("info.sii quotation marks or incomplete escaping");
        } else if (std::strchr("{}:[]",c)) ++i;
        else {
            while(i<s.size() && !std::strchr(" \t\r\n{}:[]\"",s[i])) {
                if(s[i]=='\0') throw std::runtime_error("info.sii contains invalid null byte");
                ++i;
            }
        }
        out.push_back({s.substr(begin,i-begin),begin,i,depth,quoted});
        if(!quoted && c=='{') ++depth;
        if(!quoted && c=='}' && --depth<0) throw std::runtime_error("info.sii brackets do not match");
    }
    if(depth) throw std::runtime_error("info.sii brackets are incomplete");
    return out;
}
struct InfoFields { Token name; Token time; };
InfoFields infoFields(const std::string& text) {
    if(text.size()>kInfoLimit) throw std::runtime_error("info.sii exceeds the upper limit of meta-information length");
    const auto t=tokenize(text);
    if(t.size()<10 || t[0].text!="SiiNunit" || t[1].text!="{" ||
       t[2].text!="save_container" || t[2].depth!=1 || t[3].text!=":" ||
       t[4].quoted || t[4].text.empty() || t[5].text!="{" ||
       t[t.size()-2].text!="}" || t[t.size()-2].depth!=2 ||
       t.back().text!="}" || t.back().depth!=1)
        throw std::runtime_error("info.sii is not the complete and unique save_container text");
    InfoFields fields{}; int names=0,times=0;
    for(size_t i=6;i<t.size()-2;++i) {
        if(!t[i].quoted && (t[i].text=="{" || t[i].text=="}" || t[i].depth!=2))
            throw std::runtime_error("info.sii contains additional or nested units");
        if(i+2>=t.size()-2 || t[i].quoted || t[i].depth!=2 || t[i+1].text!=":") continue;
        if(t[i].text=="name") { fields.name=t[i+2]; ++names; }
        if(t[i].text=="file_time") { fields.time=t[i+2]; ++times; }
    }
    if(names!=1 || times!=1) throw std::runtime_error("info.sii must appear once each");
    if(fields.time.quoted || fields.time.text.empty() ||
       fields.time.text.find_first_not_of("0123456789")!=std::string::npos)
        throw std::runtime_error("file_time of info.sii is not a valid timestamp");
    if(!fields.name.quoted && (fields.name.text.empty() ||
       fields.name.text.find_first_of("{}:[]")!=std::string::npos))
        throw std::runtime_error("The archive name for info.sii is invalid");
    return fields;
}
std::string nameFromInfo(const std::string& text) {
    const auto name=infoFields(text).name;
    return name.quoted ? siiUnescapeName(name.text.substr(1,name.text.size()-2)) : name.text;
}
std::string loadInfo(const fs::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if(!::GetFileAttributesExW(p.c_str(),GetFileExInfoStandard,&attr) ||
       attr.nFileSizeHigh || attr.nFileSizeLow>kInfoLimit) throw std::runtime_error("info.sii failed to read or the file is too large");
    std::vector<uint8_t> bytes; DecryptInfo info; std::string error;
    if(!decryptSave(p.wstring(),bytes,&info,&error,kInfoLimit)) throw std::runtime_error("Failed to read meta information:"+error);
    if(info.innerFormat=="bsii") {
        std::string text;
        if(!decodeBsiiText(bytes,&text,&error,{},kInfoLimit))
            throw std::runtime_error("Meta information conversion failed:"+error);
        return text;
    }
    if(detectFormat(bytes)!="text" || bytes.size()>kInfoLimit) throw std::runtime_error("info.sii must be readable SiiN text");
    return {bytes.begin(),bytes.end()};
}
void checkOptions(const SaveCopyOptions& options, bool full = true) {
    if(options.canceled && options.canceled()) throw std::runtime_error("The copy has been canceled and the original archive has not been modified.");
    std::string error;
    if(full && options.guard && !options.guard(&error)) throw std::runtime_error(error.empty()?"Copy conditions change":error);
}
bool existsEntry(const fs::path& p) {
    if(::GetFileAttributesW(p.c_str())!=INVALID_FILE_ATTRIBUTES) return true;
    const DWORD error=::GetLastError();
    if(error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND) return false;
    throw std::runtime_error("Unable to check whether the target slot exists");
}
fs::path physicalProfile(const SaveSlot& source, fs::path* slot) {
    const fs::path profileInput(source.profileDir), slotInput(source.slotDir);
    if(profileInput.empty() || slotInput.empty() || dots(profileInput) || dots(slotInput))
        throw std::runtime_error("The archive path is empty or contains a relative directory jump");
    const fs::path profile=fs::canonical(profileInput);
    const fs::path save=profile/L"save";
    if(reparse(profileInput) || reparse(save) || reparse(slotInput))
        throw std::runtime_error("The archive directory is a reparse point or does not exist, and copying has been refused.");
    *slot=fs::canonical(slotInput);
    if(!samePath(slot->parent_path(),save) || !fs::is_directory(*slot))
        throw std::runtime_error("The source slot is not a direct subdirectory of the save directory of the file.");
    return profile;
}
void copyAndVerify(HANDLE source,const fs::path& dest,uint64_t length,const SaveCopyOptions& options) {
    Handle output(::CreateFileW(dest.c_str(),GENERIC_WRITE | GENERIC_READ,0,nullptr,
        CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(!output) throw std::runtime_error("Failed to create copy file");
    std::array<uint8_t,65536> original{},copied{};
    uint64_t done=0; rewind(source);
    while(done<length) {
        checkOptions(options, false);
        DWORD got=0,written=0;
        DWORD count=static_cast<DWORD>((std::min)(uint64_t(original.size()),length-done));
        if(!::ReadFile(source,original.data(),count,&got,nullptr) || got!=count ||
           !::WriteFile(output.value,original.data(),got,&written,nullptr) || written!=got)
            throw std::runtime_error("File copying failed, the source file may have changed");
        done+=got;
    }
    if(!::FlushFileBuffers(output.value) || fileSize(output.value)!=length || fileSize(source)!=length)
        throw std::runtime_error("Copy length or write refresh verification failed");
    rewind(source); rewind(output.value); done=0;
    while(done<length) {
        checkOptions(options, false);
        DWORD a=0,b=0; DWORD count=static_cast<DWORD>((std::min)(uint64_t(original.size()),length-done));
        if(!::ReadFile(source,original.data(),count,&a,nullptr) || a!=count ||
           !::ReadFile(output.value,copied.data(),count,&b,nullptr) || b!=count ||
           ::memcmp(original.data(),copied.data(),count)) throw std::runtime_error("The copy content fails to be verified byte by byte.");
        done+=count;
    }
}
bool cleanupStage(const fs::path& profile,const fs::path& stage,const std::vector<fs::path>& files) {
    try {
        if(stage.empty()) return true;
        if(reparse(stage) || !samePath(fs::canonical(stage).parent_path(),profile)) return false;
        bool ok=true;
        for(const auto& file:files) {
            const auto p=stage/file;
            if(!existsEntry(p)) continue;
            if(!samePath(p.parent_path(),stage) || reparse(p) || !::DeleteFileW(p.c_str())) ok=false;
        }
        if(!::RemoveDirectoryW(stage.c_str())) ok=false;
        return ok;
    } catch(...) { return false; }
}
} // namespace

bool siiEscapeName(const std::string& utf8,std::string* out,std::string* error) {
    if(out) out->clear();
    auto fail=[&](const char* msg){if(error)*error=msg;return false;};
    if(utf8.empty() || utf8.size()>256 || trim(utf8).empty()) return fail("The copy name cannot be empty or too long");
    const int count=::MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,utf8.data(),static_cast<int>(utf8.size()),nullptr,0);
    if(!count) return fail("Replica name is not a valid UTF-8");
    std::wstring wide(count,L'\0');
    ::MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,utf8.data(),static_cast<int>(utf8.size()),wide.data(),count);
    size_t points=0;
    for(wchar_t c:wide) {
        if(c<0x20 || (c>=0x7F && c<=0x9F)) return fail("The copy name cannot contain control characters");
        if(c<0xDC00 || c>0xDFFF) ++points;
    }
    if(points>64) return fail("Copy name can be up to 64 characters");
    std::string escaped; const char* hex="0123456789ABCDEF";
    for(unsigned char c:utf8) {
        if(c=='"' || c=='\\') { escaped+='\\'; escaped+=char(c); }
        else if(c>=0x80) { escaped+="\\x"; escaped+=hex[c>>4]; escaped+=hex[c&15]; }
        else escaped+=char(c);
    }
    if(out) *out=std::move(escaped);
    if(error) error->clear();
    return true;
}
std::string siiUnescapeName(const std::string& text) {
    std::string out;
    auto hex=[](char c)->int {if(c>='0'&&c<='9')return c-'0';if(c>='A'&&c<='F')return c-'A'+10;if(c>='a'&&c<='f')return c-'a'+10;return -1;};
    for(size_t i=0;i<text.size();++i) {
        if(text[i]!='\\') {out+=text[i];continue;}
        if(++i>=text.size()) return {};
        if(text[i]=='x') {
            if(i+2>=text.size() || hex(text[i+1])<0 || hex(text[i+2])<0) return {};
            out+=char(hex(text[i+1])*16+hex(text[i+2])); i+=2;
        } else if(text[i]=='\\' || text[i]=='"') out+=text[i];
        else if(text[i]=='n') out+='\n';
        else if(text[i]=='r') out+='\r';
        else if(text[i]=='t') out+='\t';
        else return {};
    }
    return out;
}
bool patchSaveInfo(std::string& text,const std::string& name,uint64_t time,std::string* error) {
    std::string escaped;
    if(!siiEscapeName(name,&escaped,error)) return false;
    try {
        const auto fields=infoFields(text);
        struct Edit {size_t begin,end; std::string replacement;};
        std::array<Edit,2> edits{{{fields.name.begin,fields.name.end,"\""+escaped+"\""},
                                 {fields.time.begin,fields.time.end,std::to_string(time)}}};
        std::sort(edits.begin(),edits.end(),[](const Edit& a,const Edit& b){return a.begin>b.begin;});
        auto copy=text;
        for(const auto& edit:edits) copy.replace(edit.begin,edit.end-edit.begin,edit.replacement);
        if(nameFromInfo(copy)!=name) throw std::runtime_error("Replica name readback is inconsistent");
        text.swap(copy);
        if(error) error->clear();
        return true;
    } catch(const std::exception& e) {if(error)*error=e.what();return false;}
}
std::string readSaveDisplayName(const SaveSlot& slot) {
    try {return nameFromInfo(loadInfo(fs::path(slot.slotDir)/L"info.sii"));} catch(...) {return {};}
}
bool copyBlockedByRunningGame(GameId game,std::string* error) {
    Handle snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));
    if(!snapshot) {if(error)*error="Unable to confirm whether the game has exited and the copy has been cancelled.";return true;}
    PROCESSENTRY32W entry{}; entry.dwSize=sizeof(entry);
    if(!::Process32FirstW(snapshot.value,&entry)) {if(error)*error="Unable to enumerate processes, replication canceled";return true;}
    do {
        if(::_wcsicmp(entry.szExeFile,gameDescriptor(game).processName)==0) {
            if(error)*error=std::string("Please exit first")+gameDescriptor(game).shortName+"Copy the archive again";
            return true;
        }
    } while(::Process32NextW(snapshot.value,&entry));
    if(::GetLastError()!=ERROR_NO_MORE_FILES) {if(error)*error="The process check is not completed and the copy has been cancelled.";return true;}
    return false;
}
void publishNewDirectory(const std::wstring& stage,const std::wstring& target,const std::function<void()>& validate) {
    for (int attempt=0;attempt<2;++attempt) {
        if(validate) validate();
        if(existsEntry(fs::path(target))) throw std::runtime_error("The target directory already exists and overwriting has been refused.");
        if(::MoveFileExW(stage.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH)) return;
        const DWORD error=::GetLastError();
        if(attempt==0 && (error==ERROR_ACCESS_DENIED || error==ERROR_SHARING_VIOLATION || error==ERROR_LOCK_VIOLATION) &&
           !existsEntry(fs::path(target))) { ::Sleep(200); continue; }
        throw std::runtime_error("Replica publishing failed (Windows error "+std::to_string(error)+"), please check the file occupation and try again");
    }
}
SaveCopyResult copySaveSlotInProfile(const SaveSlot& source,const std::string& name,const SaveCopyOptions& options) {
    SaveCopyResult result; result.displayName=name;
    fs::path profile,slot,stage; std::vector<fs::path> created;
    try {
        std::string escaped;
        if(!siiEscapeName(name,&escaped,&result.error)) return result;
        if(options.replacementGameText && (options.replacementGameText->size()>kFileLimit ||
           options.replacementGameText->compare(0,8,"SiiNunit")!=0))
            throw std::runtime_error("Import result is not SII text of a supported size");
        checkOptions(options);
        profile=physicalProfile(source,&slot);
        auto profileLock=lockDirectory(profile), saveLock=lockDirectory(profile/L"save"), slotLock=lockDirectory(slot);
        std::vector<fs::path> entries;
        for(const auto& e:fs::directory_iterator(slot)) {
            if(entries.size()>=256 || reparse(e.path()) || !e.is_regular_file())
                throw std::runtime_error("The source archive contains subdirectories, reparse points or too many files, and copying has been refused.");
            entries.push_back(e.path().filename());
        }
        std::sort(entries.begin(),entries.end());
        if(std::find(entries.begin(),entries.end(),fs::path(L"game.sii"))==entries.end() ||
           std::find(entries.begin(),entries.end(),fs::path(L"info.sii"))==entries.end())
            throw std::runtime_error("source archive is missing game.sii or info.sii");
        std::vector<std::unique_ptr<Handle>> held; std::vector<uint64_t> sizes;
        uint64_t total=0;
        for(const auto& file:entries) {
            auto h=std::make_unique<Handle>(::CreateFileW((slot/file).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,
                OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
            BY_HANDLE_FILE_INFORMATION info{};
            if(!*h || !::GetFileInformationByHandle(h->value,&info) ||
               info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
                throw std::runtime_error("The source file cannot be locked for reading or the type has changed");
            const auto size=fileSize(h->value);
            if(size>kFileLimit || (total+=size)>kTotalLimit || (file==L"info.sii" && size>kInfoLimit))
                throw std::runtime_error("The archive file exceeds the maximum size limit for this copy.");
            if(file==L"game.sii") {
                std::vector<uint8_t> head(8); DWORD got=0;
                if(!::ReadFile(h->value,head.data(),8,&got,nullptr) || got<4)
                    throw std::runtime_error("source game.sii format cannot be recognized");
                head.resize(got);
                if(detectFormat(head)=="unknown")
                    throw std::runtime_error("source game.sii format cannot be recognized");
            }
            sizes.push_back(size); held.push_back(std::move(h));
        }
        // Source handles are held together until after publication; no writes/deletes can enter mid-copy.
        auto metadata=loadInfo(slot/L"info.sii"); std::string error;
        if(!patchSaveInfo(metadata,name,static_cast<uint64_t>(::time(nullptr)),&error))
            throw std::runtime_error(error);
        unsigned next=1;
        for(;next<=1000000;++next) if(!existsEntry(profile/L"save"/std::to_wstring(next))) break;
        if(next>1000000) throw std::runtime_error("No new archive number available");
        const auto target=profile/L"save"/std::to_wstring(next);
        static std::atomic<unsigned> serial{0};
        for(unsigned attempt=0;attempt<16;++attempt) {
            auto candidate=profile/(L".trainer_copy_"+std::to_wstring(::GetCurrentProcessId())+L"_"+
                std::to_wstring(::GetTickCount64())+L"_"+std::to_wstring(++serial));
            if(::CreateDirectoryW(candidate.c_str(),nullptr)) {stage=candidate;break;}
            if(::GetLastError()!=ERROR_ALREADY_EXISTS) break;
        }
        if(stage.empty()) throw std::runtime_error("Failed to create independent temporary directory");
        for(size_t i=0;i<entries.size();++i) {
            checkOptions(options);
            created.push_back(entries[i]);
            copyAndVerify(held[i]->value,stage/entries[i],sizes[i],options);
            if(options.afterFile && !options.afterFile(i)) throw std::runtime_error("Replication aborted during verification phase");
        }
        // info changes are intentional; all OTHER files have already passed byte comparison.
        {
            Handle output(::CreateFileW((stage/L"info.sii").c_str(),GENERIC_WRITE,0,nullptr,
                TRUNCATE_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
            DWORD written=0;
            if(!output || !::WriteFile(output.value,metadata.data(),static_cast<DWORD>(metadata.size()),&written,nullptr) ||
               written!=metadata.size() || !::FlushFileBuffers(output.value))
                throw std::runtime_error("Failed to write copy meta information");
        }
        if(loadInfo(stage/L"info.sii")!=metadata) throw std::runtime_error("Failed to read back copy meta information");
        if(options.replacementGameText) {
            const auto& replacement=*options.replacementGameText;
            {
                Handle output(::CreateFileW((stage/L"game.sii").c_str(),GENERIC_WRITE,0,nullptr,
                    TRUNCATE_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
                DWORD written=0;
                if(!output || !::WriteFile(output.value,replacement.data(),static_cast<DWORD>(replacement.size()),&written,nullptr) ||
                   written!=replacement.size() || !::FlushFileBuffers(output.value))
                    throw std::runtime_error("Failed to write temporary progress of vehicle import");
            }
            std::vector<uint8_t> verified;
            if(!readFileBytes((stage/L"game.sii").wstring(),verified) || verified.size()!=replacement.size() ||
               ::memcmp(verified.data(),replacement.data(),replacement.size()))
                throw std::runtime_error("The temporary progress readback verification of vehicle import failed.");
        }
        if(options.beforePublish) options.beforePublish(target.wstring());
        publishNewDirectory(stage.wstring(),target.wstring(),[&] {
        checkOptions(options);
        // Hooks may have added or replaced source entries; re-verify the bounded membership before publishing.
        {
            std::vector<fs::path> published; bool entriesStable=true;
            for(const auto& e:fs::directory_iterator(slot)) {
                if(published.size()>=256 || reparse(e.path()) || !e.is_regular_file()) {entriesStable=false;break;}
                published.push_back(e.path().filename());
            }
            std::sort(published.begin(),published.end());
            if(!entriesStable || published!=entries)
                throw std::runtime_error("The source archive changed during replication and the replication was canceled; please try again after synchronization is complete");
        }
        if(!samePath(fs::canonical(stage).parent_path(),profile) || reparse(stage) ||
           !samePath(fs::canonical(profile/L"save"),profile/L"save") || existsEntry(target))
            throw std::runtime_error("The directory status has changed before publishing or the target number already exists.");
        });
        stage.clear();
        result.ok=true; result.slotDir=target.wstring(); result.slotName=std::to_wstring(next);
        return result;
    } catch(const std::exception& e) {
        result.error=e.what();
        result.slotDir.clear(); result.slotName.clear();
        if(!cleanupStage(profile,stage,created)) result.error+="; Temporary storage cleanup is not completed, location:"+W2U(stage.wstring());
        return result;
    }
}
SaveCopyResult copySlotToNewSave(const SaveCopyRequest& request,std::function<bool()> canceled,
                               const std::string* replacementGameText) {
    SaveCopyResult failure; failure.game=request.game;
    if(selectedGame()!=request.game) {failure.error="The game has been switched, please reselect the game's archive";return failure;}
    if(copyBlockedByRunningGame(request.game,&failure.error)) return failure;
    SaveSlot fresh; bool member=false;
    for(const auto& slot:listSlots()) {
        if(::_wcsicmp(slot.slotDir.c_str(),request.source.slotDir.c_str())==0 &&
           ::_wcsicmp(slot.profileDir.c_str(),request.source.profileDir.c_str())==0) {fresh=slot;member=true;break;}
    }
    if(!member) {failure.error="The source archive does not belong to the archive list of the current game";return failure;}
    SaveCopyOptions options; options.canceled=std::move(canceled);
    options.replacementGameText=replacementGameText;
    options.guard=[game=request.game](std::string* error) {
        if(selectedGame()!=game) {if(error)*error="Game selection changed during copying";return false;}
        return !copyBlockedByRunningGame(game,error);
    };
    auto result=copySaveSlotInProfile(fresh,request.displayName,options);
    result.game=request.game;
    return result;
}
}
