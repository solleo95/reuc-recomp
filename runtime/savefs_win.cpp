#include "savefs.h"
#include "save_rollback.h"
#include <stdexcept>
#include <algorithm>
#include <limits>
#include <array>
#include <cstdio>
#include <winternl.h>

namespace fs=std::filesystem;
namespace {
int error(DWORD value) {
    switch (value) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: return -106;
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return -105;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: return -102;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: return -108;
    case ERROR_DIR_NOT_EMPTY: return -115;
    case ERROR_INVALID_NAME: case ERROR_INVALID_PARAMETER: case ERROR_INVALID_HANDLE: case ERROR_NEGATIVE_SEEK: return -101;
    default: return -103;
    }
}
int check_ancestors(fs::path path) {
    for (;!path.empty();) {
        const auto attrs=GetFileAttributesW(path.c_str());
        if (attrs!=INVALID_FILE_ATTRIBUTES && (attrs&FILE_ATTRIBUTE_REPARSE_POINT)) return -102;
        const auto parent=path.parent_path();
        if (parent==path) break;
        path=parent;
    }
    return 0;
}
bool reserved(std::string part) {
    for (char& ch:part) if (ch>='a' && ch<='z') ch=char(ch-'a'+'A');
    part=part.substr(0,part.find('.'));
    return part=="CON" || part=="PRN" || part=="AUX" || part=="NUL" ||
        (part.size()==4 && (part.starts_with("COM") || part.starts_with("LPT")) && part[3]>='0' && part[3]<='9');
}
bool internal(std::wstring name) {
    for (auto& ch:name) if (ch>=L'A' && ch<=L'Z') ch+=L'a'-L'A';
    return name==L".backups" || name==L".deleted";
}
int file_info(HANDLE file,BY_HANDLE_FILE_INFORMATION& info) {
    if (!GetFileInformationByHandle(file,&info)) return error(GetLastError());
    return (info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)) || info.nNumberOfLinks!=1 ? -102 : 0;
}
template<class Pins> int move(const fs::path& source,const fs::path& target,bool replace,Pins& pins) {
    HANDLE file=CreateFileW(source.c_str(),DELETE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if (file==INVALID_HANDLE_VALUE) return error(GetLastError());
    std::shared_ptr<void> owner(file,[](void* value) { CloseHandle(value); });
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file,&info)) return error(GetLastError());
    if (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) return -102;
    if (!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) && info.nNumberOfLinks!=1) return -102;
    // Keep every directory's identity pinned while permitting the kernel's namespace writes.
    std::vector<std::shared_ptr<void>> identities;
    for (const auto& pinned:pins) {
        HANDLE handle=CreateFileW(pinned.path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if (handle==INVALID_HANDLE_VALUE) return error(GetLastError());
        identities.emplace_back(handle,[](void* value) { CloseHandle(value); });
    }
    pins.clear();
    HANDLE parent=CreateFileW(target.parent_path().c_str(),GENERIC_READ|FILE_ADD_FILE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if (parent==INVALID_HANDLE_VALUE) return error(GetLastError());
    std::shared_ptr<void> parent_owner(parent,[](void* value) { CloseHandle(value); });
    if (!GetFileInformationByHandle(parent,&info)) return error(GetLastError());
    if ((info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) || !(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)) return -102;
    const auto name=target.filename().native();
    std::vector<std::byte> storage(sizeof(FILE_RENAME_INFO)+name.size()*sizeof(wchar_t));
    auto* rename=reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    rename->ReplaceIfExists=replace; rename->RootDirectory=parent;
    rename->FileNameLength=DWORD(name.size()*sizeof(wchar_t));
    std::memcpy(rename->FileName,name.data(),rename->FileNameLength);
    using SetInfo=NTSTATUS (NTAPI*)(HANDLE,PIO_STATUS_BLOCK,PVOID,ULONG,FILE_INFORMATION_CLASS);
    using DosError=ULONG (NTAPI*)(NTSTATUS);
    static const auto set_info=reinterpret_cast<SetInfo>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtSetInformationFile"));
    static const auto dos_error=reinterpret_cast<DosError>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"RtlNtStatusToDosError"));
    if (!set_info || !dos_error) return -103;
    IO_STATUS_BLOCK status{};
    const auto result=set_info(file,&status,rename,ULONG(storage.size()),static_cast<FILE_INFORMATION_CLASS>(10)); // FileRenameInformation
    return result>=0 ? 0 : error(dos_error(result));
}
}
namespace uc {
SaveFs::SaveFs(fs::path root):root_(fs::absolute(root).lexically_normal()) {}
SaveFs::~SaveFs() {
    for (auto& [fd,file]:files_) {
        if ((file.mode&2) && !FlushFileBuffers(file.handle)) std::fprintf(stderr,"[save] flush fd %d failed: %lu\n",fd,GetLastError());
        if (!CloseHandle(file.handle)) std::fprintf(stderr,"[save] close fd %d failed: %lu\n",fd,GetLastError());
    }
}
int SaveFs::pin(const fs::path& directory,Pins& pins) {
    fs::path current;
    for (const auto& part:directory) {
        current/=part;
        if (!current.has_root_directory()) continue;
        if (std::any_of(pins.begin(),pins.end(),[&](const Pin& pinned) { return pinned.path==current; })) continue;
        HANDLE handle=CreateFileW(current.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if (handle==INVALID_HANDLE_VALUE) return error(GetLastError());
        std::shared_ptr<void> owner(handle,[](void* value) { CloseHandle(value); });
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle,&info)) return error(GetLastError());
        if ((info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) || !(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)) return -102;
        // Deny write/delete sharing until the path operation (or open file) finishes.
        pins.push_back({current,std::move(owner)});
    }
    return 0;
}
int SaveFs::initialize() {
    if(rollback_) return 0;
    Pins pins;
    if (auto result=pin(root_.parent_path(),pins)) return result;
    if (!CreateDirectoryW(root_.c_str(),nullptr) && GetLastError()!=ERROR_ALREADY_EXISTS) return error(GetLastError());
    return pin(root_,pins);
}
int SaveFs::resolve(const std::string& path,fs::path& host,Pins& pins) const {
    if (path.empty() || path[0]!='/' || path.size()>63 || path.starts_with("//")) return -101;
    host=root_;
    for (size_t at=1;at<path.size();) {
        const auto end=path.find('/',at);
        auto part=path.substr(at,end==std::string::npos ? end : end-at);
        if (part.empty() || part=="." || part==".." || part.back()=='.' || part.back()==' ' || reserved(part)) return -101;
        for (unsigned char ch:part) if (ch<32 || ch>=127 || std::string_view("\\:*?\"<>|").find(char(ch))!=std::string_view::npos) return -101;
        if (host==root_ && internal(fs::path(part).wstring())) return -102;
        host/=part;
        if (end==std::string::npos) break;
        at=end+1;
    }
    if (auto result=pin(host==root_ ? host : host.parent_path(),pins)) return result;
    return check_ancestors(host);
}
fs::path SaveFs::revision(const wchar_t* directory,Pins& pins) {
    const auto parent=root_/directory;
    int result=pin(root_,pins);
    if (!result && !CreateDirectoryW(parent.c_str(),nullptr) && GetLastError()!=ERROR_ALREADY_EXISTS) result=error(GetLastError());
    if (!result) result=pin(parent,pins);
    if (result) throw fs::filesystem_error("unsafe save backup directory",parent,std::error_code(ERROR_ACCESS_DENIED,std::system_category()));
    return parent/(std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(++revision_));
}
int SaveFs::backup(HANDLE source) {
    LARGE_INTEGER zero{},position{},size{};
    if (!GetFileSizeEx(source,&size) || !SetFilePointerEx(source,zero,&position,FILE_CURRENT)) return error(GetLastError());
    if (!size.QuadPart) return 0;
    int result=0;
    try {
        Pins pins;
        HANDLE target=CreateFileW(revision(L".backups",pins).c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if (target==INVALID_HANDLE_VALUE) return error(GetLastError());
        std::shared_ptr<void> owner(target,[](void* value) { CloseHandle(value); });
        if (!SetFilePointerEx(source,zero,nullptr,FILE_BEGIN)) result=error(GetLastError());
        std::array<uint8_t,65536> buffer{};
        while (!result) {
            DWORD count=0,written=0;
            if (!ReadFile(source,buffer.data(),DWORD(buffer.size()),&count,nullptr)) { result=error(GetLastError()); break; }
            if (!count) break;
            if (!WriteFile(target,buffer.data(),count,&written,nullptr)) result=error(GetLastError());
            else if (written!=count) result=-103;
        }
        if (!result && !FlushFileBuffers(target)) result=error(GetLastError());
    }
    catch (const fs::filesystem_error& failure) { return error(DWORD(failure.code().value())); }
    if (!SetFilePointerEx(source,position,nullptr,FILE_BEGIN)) return error(GetLastError());
    return result;
}
int SaveFs::backup(const fs::path& path) {
    HANDLE source=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if (source==INVALID_HANDLE_VALUE) return error(GetLastError());
    std::shared_ptr<void> owner(source,[](void* value) { CloseHandle(value); });
    BY_HANDLE_FILE_INFORMATION info{};
    if (auto result=file_info(source,info)) return result;
    return backup(source);
}
int SaveFs::create_dir(const std::string& path) {
    if(rollback_) return writable().create(path,true);
    Pins pins; fs::path host; if (auto result=resolve(path,host,pins)) return result;
    return CreateDirectoryW(host.c_str(),nullptr) ? 0 : error(GetLastError());
}
int SaveFs::create_file(const std::string& path) {
    if(rollback_) return writable().create(path,false);
    Pins pins; fs::path host; if (auto result=resolve(path,host,pins)) return result;
    HANDLE file=CreateFileW(host.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if (file==INVALID_HANDLE_VALUE) return error(GetLastError());
    return CloseHandle(file) ? 0 : error(GetLastError());
}
int SaveFs::open(const std::string& path,uint32_t mode) {
    if(rollback_) return writable().open(path,mode);
    if (mode<1 || mode>3 || next_fd_==INT_MAX) return -101;
    Pins pins; fs::path host; if (auto result=resolve(path,host,pins)) return result;
    HANDLE file=CreateFileW(host.c_str(),GENERIC_READ|(mode&2 ? GENERIC_WRITE : 0),FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if (file==INVALID_HANDLE_VALUE) return error(GetLastError());
    BY_HANDLE_FILE_INFORMATION info{};
    int result=file_info(file,info);
    if (!result && (mode&2)) result=backup(file);
    if (result) { CloseHandle(file); return result; }
    const auto fd=next_fd_++;
    files_.emplace(fd,File{file,mode,std::move(pins)}); return fd;
}
int SaveFs::close(int fd) {
    if(rollback_) return writable().close(fd);
    auto found=files_.find(fd); if (found==files_.end()) return -101;
    if ((found->second.mode&2) && !FlushFileBuffers(found->second.handle)) return error(GetLastError());
    if (!CloseHandle(found->second.handle)) return error(GetLastError());
    files_.erase(found); return 0;
}
int SaveFs::read(int fd,std::span<uint8_t> bytes) {
    if(rollback_) return writable().read(fd,bytes);
    auto found=files_.find(fd); if (found==files_.end() || bytes.size()>INT_MAX) return -101;
    if (!(found->second.mode&1)) return -102;
    DWORD read=0;
    return ReadFile(found->second.handle,bytes.data(),DWORD(bytes.size()),&read,nullptr) ? int(read) : error(GetLastError());
}
int SaveFs::write(int fd,std::span<const uint8_t> bytes) {
    if(rollback_) return writable().write(fd,bytes);
    auto found=files_.find(fd); if (found==files_.end() || bytes.size()>INT_MAX) return -101;
    auto& file=found->second;
    if (!(file.mode&2)) return -102;
    DWORD written=0;
    return WriteFile(file.handle,bytes.data(),DWORD(bytes.size()),&written,nullptr) ? int(written) : error(GetLastError());
}
int SaveFs::seek(int fd,int32_t offset,int32_t whence) {
    if(rollback_) return writable().seek(fd,offset,whence);
    auto found=files_.find(fd); if (found==files_.end() || whence<0 || whence>2) return -101;
    LARGE_INTEGER zero{},base{},position{};
    if (whence==1 && !SetFilePointerEx(found->second.handle,zero,&base,FILE_CURRENT)) return error(GetLastError());
    if (whence==2 && !GetFileSizeEx(found->second.handle,&base)) return error(GetLastError());
    position.QuadPart=base.QuadPart+offset;
    if (position.QuadPart<0 || position.QuadPart>INT_MAX) return -101;
    if (!SetFilePointerEx(found->second.handle,position,nullptr,FILE_BEGIN)) return error(GetLastError());
    return int(position.QuadPart);
}
SaveFs::Stats SaveFs::stats(int fd) {
    if(rollback_) return rollback_->stats(fd);
    auto found=files_.find(fd); if (found==files_.end()) return {-101};
    LARGE_INTEGER size{},zero{},position{};
    if (!GetFileSizeEx(found->second.handle,&size) || !SetFilePointerEx(found->second.handle,zero,&position,FILE_CURRENT)) return {error(GetLastError())};
    if (size.QuadPart>UINT32_MAX || position.QuadPart>UINT32_MAX) return {-101};
    return {0,uint32_t(size.QuadPart),uint32_t(position.QuadPart)};
}
int SaveFs::attributes(const std::string& path,bool& directory) {
    if(rollback_) return rollback_->attributes(path,directory);
    Pins pins; fs::path host; if (auto result=resolve(path,host,pins)) return result;
    auto attributes=GetFileAttributesW(host.c_str());
    if (attributes==INVALID_FILE_ATTRIBUTES) return error(GetLastError());
    if (attributes&FILE_ATTRIBUTE_REPARSE_POINT) return -102;
    directory=(attributes&FILE_ATTRIBUTE_DIRECTORY)!=0; return 0;
}
SaveFs::Directory SaveFs::read_dir(const std::string& path) {
    if(rollback_) return rollback_->read_dir(path);
    Pins pins; fs::path host; if (auto result=resolve(path,host,pins)) return {result};
    bool directory; if (auto result=attributes(path,directory)) return {result};
    if (!directory) return {-101};
    if (auto result=pin(host,pins)) return {result};
    Directory result{0}; std::error_code ec;
    for (fs::directory_iterator it(host,ec),end;!ec && it!=end;it.increment(ec)) {
        if (host==root_ && internal(it->path().filename().wstring())) continue;
        if (check_ancestors(it->path())) return {-102};
        result.names.push_back(it->path().filename().string());
    }
    if (ec) return {error(DWORD(ec.value()))};
    std::sort(result.names.begin(),result.names.end()); return result;
}
SaveFs::Usage SaveFs::usage(const std::string& path) {
    if(rollback_) return rollback_->usage(path);
    Pins pins; fs::path host; if (auto result=resolve(path,host,pins)) return {result};
    bool directory; if (auto result=attributes(path,directory)) return {result};
    Usage result{0}; std::error_code ec;
    if (!directory) {
        const int fd=open(path,1); if (fd<0) return {fd};
        const auto info=stats(fd); const int closed=close(fd);
        if (info.result || closed) return {info.result ? info.result : closed};
        return {0,uint32_t((uint64_t(info.length)+16383)/16384),1};
    }
    if (auto result=pin(host,pins)) return {result};
    uint64_t blocks=0,entries=0;
    for (fs::recursive_directory_iterator it(host,ec),end;!ec && it!=end;it.increment(ec)) {
        if (host==root_ && internal(it->path().filename().wstring()) && it->path().parent_path()==root_) { it.disable_recursion_pending(); continue; }
        if (check_ancestors(it->path())) return {-102};
        ++entries;
        if (it->is_directory(ec)) { if (auto failure=pin(it->path(),pins)) return {failure}; }
        else {
            HANDLE file=CreateFileW(it->path().c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
            if (file==INVALID_HANDLE_VALUE) return {error(GetLastError())};
            std::shared_ptr<void> owner(file,[](void* value) { CloseHandle(value); });
            BY_HANDLE_FILE_INFORMATION info{};
            if (auto failure=file_info(file,info)) return {failure};
            const uint64_t size=(uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow;
            blocks+=(size+16383)/16384;
        }
    }
    if (ec) return {error(DWORD(ec.value()))};
    if (blocks>UINT32_MAX || entries>UINT32_MAX) return {-101};
    return {0,uint32_t(blocks),uint32_t(entries)};
}
int SaveFs::rename(const std::string& from,const std::string& to) {
    if(rollback_) return writable().rename(from,to);
    Pins pins; fs::path source,target; if (auto result=resolve(to,target,pins)) return result; if (auto result=resolve(from,source,pins)) return result;
    if (source==root_ || target==root_) return -102;
    if (source==target) return GetFileAttributesW(source.c_str())==INVALID_FILE_ATTRIBUTES ? error(GetLastError()) : 0;
    if (GetFileAttributesW(target.c_str())!=INVALID_FILE_ATTRIBUTES) if (auto result=backup(target)) return result;
    return move(source,target,true,pins);
}
int SaveFs::remove(const std::string& path) {
    if(rollback_) return writable().remove(path);
    Pins pins; fs::path host; if (auto result=resolve(path,host,pins)) return result;
    if (host==root_) return -102;
    auto attr=GetFileAttributesW(host.c_str()); if (attr==INVALID_FILE_ATTRIBUTES) return error(GetLastError());
    if (attr&FILE_ATTRIBUTE_DIRECTORY) { auto contents=read_dir(path); if (contents.result) return contents.result; if (!contents.names.empty()) return -115; }
    try { const auto target=revision(L".deleted",pins); return move(host,target,false,pins); }
    catch (const fs::filesystem_error& failure) { return error(DWORD(failure.code().value())); }
}
void SaveFs::commit(const Snapshot& saved) {
    if(!rollback_ || saved.owner!=this || saved.revision<committed_->revision)
        throw std::logic_error("invalid confirmed save checkpoint");
    if(saved.revision==committed_->revision) return;
    SaveFs disk(root_);
    const auto require=[](int result,const char* operation) {
        if(result) throw std::runtime_error(std::string("confirmed save ")+operation+" failed: "+std::to_string(result));
    };
    // Children retire first; backups preserve each replaced or deleted version.
    for(auto it=committed_->nodes.rbegin();it!=committed_->nodes.rend();++it) {
        if(it->first=="/") continue;
        const auto next=saved.nodes.find(it->first);
        if(next==saved.nodes.end() || bool(next->second.bytes)!=bool(it->second.bytes)) {
            const int result=disk.remove(it->first);
            if(result!=-106) require(result,"delete");
        }
    }
    for(const auto& [path,node]:saved.nodes) {
        if(path=="/") continue;
        const auto old=committed_->nodes.find(path);
        if(!node.bytes) {
            const int result=disk.create_dir(path);
            if(result!=-105) require(result,"mkdir");
            continue;
        }
        if(old!=committed_->nodes.end() && old->second.bytes &&
           (old->second.bytes==node.bytes || *old->second.bytes==*node.bytes)) continue;
        Pins pins; fs::path target;
        require(disk.resolve(path,target,pins),"resolve");
        if(GetFileAttributesW(target.c_str())!=INVALID_FILE_ATTRIBUTES) require(disk.backup(target),"backup");
        const auto temporary=disk.revision(L".backups",pins);
        HANDLE handle=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(handle==INVALID_HANDLE_VALUE) require(error(GetLastError()),"create staged file");
        int result=0; DWORD written=0;
        if(!WriteFile(handle,node.bytes->data(),DWORD(node.bytes->size()),&written,nullptr)) result=error(GetLastError());
        else if(written!=node.bytes->size()) result=-103;
        if(!result && !FlushFileBuffers(handle)) result=error(GetLastError());
        if(!CloseHandle(handle) && !result) result=error(GetLastError());
        require(result,"write staged file");
        require(move(temporary,target,true,pins),"publish");
    }
    committed_=std::make_shared<Snapshot>(saved);
}
}
