#include "savefs.h"
#include "save_rollback.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

// POSIX counterpart of savefs_win.cpp: the same guest-visible results (the negative codes below), the same refusal of links and aliased files.
// shortcut: Windows pins every directory of a path open so nothing can rename it during an operation; POSIX cannot, so a second process that
// swaps the save directory's parts mid-operation is not stopped (the save folder belongs to the user running the game); upgrade to openat() chains if that matters.
namespace fs=std::filesystem;
namespace {
int error(int value) {
    static const bool trace=std::getenv("UC_SAVE_TRACE")!=nullptr;   // diagnostics: the system's reason behind an ISFS error (runtime/hle/save.cpp traces the calls)
    if (trace) std::fprintf(stderr,"[save] host error %d: %s\n",value,std::strerror(value));
    switch (value) {
    case ENOENT: case ENOTDIR: return -106;
    case EEXIST: return -105;
    case EACCES: case EPERM: case EBUSY: case ELOOP: case EISDIR: case EROFS: return -102;
    case ENOSPC: case EDQUOT: return -108;
    case ENOTEMPTY: return -115;
    case EINVAL: case EBADF: case ENAMETOOLONG: return -101;
    default: return -103;
    }
}
// A link anywhere between the save root and the path (the root included) is refused; what lies above the root is the user's choice of folder.
int check_ancestors(const fs::path& root,fs::path path) {
    for (;;) {
        struct stat info;
        if (lstat(path.c_str(),&info)==0 && S_ISLNK(info.st_mode)) return -102;
        if (path==root) break;
        const auto parent=path.parent_path();
        if (parent==path) break;
        path=parent;
    }
    return 0;
}
bool reserved(std::string part) {   // the guest's names stay valid on every platform the saves travel to
    for (char& ch:part) if (ch>='a' && ch<='z') ch=char(ch-'a'+'A');
    part=part.substr(0,part.find('.'));
    return part=="CON" || part=="PRN" || part=="AUX" || part=="NUL" ||
        (part.size()==4 && (part.starts_with("COM") || part.starts_with("LPT")) && part[3]>='0' && part[3]<='9');
}
bool internal(std::string name) {
    for (auto& ch:name) if (ch>='A' && ch<='Z') ch=char(ch+'a'-'A');
    return name==".backups" || name==".deleted";
}
int file_info(const struct stat& info) { return !S_ISREG(info.st_mode) || info.st_nlink!=1 ? -102 : 0; }
int file_info(int file) {
    struct stat info;
    if (fstat(file,&info)) return error(errno);
    return file_info(info);
}
int write_all(int file,const uint8_t* data,size_t size) {
    while (size) {
        const ssize_t written=::write(file,data,size);
        if (written<0) { if (errno==EINTR) continue; return error(errno); }
        data+=written; size-=size_t(written);
    }
    return 0;
}
int sync_directory(const fs::path& directory) {
    const int handle=::open(directory.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if (handle<0) return error(errno);
    const int result=fsync(handle) ? error(errno) : 0;
    ::close(handle);
    return result;
}
int move(const fs::path& source,const fs::path& target,bool replace) {
    struct stat info;
    if (lstat(source.c_str(),&info)) return error(errno);
    if (S_ISLNK(info.st_mode)) return -102;
    if (!S_ISDIR(info.st_mode) && info.st_nlink!=1) return -102;
    const auto parent=target.parent_path();
    if (lstat(parent.c_str(),&info)) return error(errno);
    if (!S_ISDIR(info.st_mode)) return -102;
#if defined(__APPLE__)   // macOS: renamex_np's RENAME_EXCL is the no-replace rename
    const int result=replace ? ::rename(source.c_str(),target.c_str()) : renamex_np(source.c_str(),target.c_str(),RENAME_EXCL);
#else
    int result=replace ? ::rename(source.c_str(),target.c_str()) : renameat2(AT_FDCWD,source.c_str(),AT_FDCWD,target.c_str(),RENAME_NOREPLACE);
    if (result && !replace && (errno==EINVAL || errno==ENOSYS || errno==EOPNOTSUPP)) {
        // A file system without the no-replace flag (Android's /sdcard, a FUSE file system): only this process writes the saves, so "nothing there"
        // checked right before a plain rename gives the same result. Without it removing and backing up fail, and the game waits on its save forever.
        struct stat existing;
        if (lstat(target.c_str(),&existing)==0) errno=EEXIST;
        else if (errno==ENOENT) result=::rename(source.c_str(),target.c_str());
    }
#endif
    if (result) return error(errno);
    return sync_directory(parent);   // the rename itself must survive a power cut
}
}
namespace uc {
SaveFs::SaveFs(fs::path root):root_(fs::absolute(root).lexically_normal()) {}
SaveFs::~SaveFs() {
    for (auto& [fd,file]:files_) {
        if ((file.mode&2) && fsync(file.handle)) std::fprintf(stderr,"[save] flush fd %d failed: %s\n",fd,std::strerror(errno));
        if (::close(file.handle)) std::fprintf(stderr,"[save] close fd %d failed: %s\n",fd,std::strerror(errno));
    }
}
int SaveFs::pin(const fs::path& directory,Pins& pins) {
    if (std::any_of(pins.begin(),pins.end(),[&](const Pin& pinned) { return pinned.path==directory; })) return 0;
    const int handle=::open(directory.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);   // a real directory, not a link
    if (handle<0) {
        struct stat info;   // O_DIRECTORY|O_NOFOLLOW on a link or a file says ENOTDIR: something is there but it is not a real directory
        return errno==ENOTDIR && lstat(directory.c_str(),&info)==0 ? -102 : error(errno);
    }
    std::shared_ptr<void> owner(new int(handle),[](void* value) { ::close(*static_cast<int*>(value)); delete static_cast<int*>(value); });
    pins.push_back({directory,std::move(owner)});
    return 0;
}
int SaveFs::initialize() {
    if(rollback_) return 0;
    Pins pins;
    if (mkdir(root_.c_str(),0777) && errno!=EEXIST) return error(errno);
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
        if (host==root_ && internal(part)) return -102;
        host/=part;
        if (end==std::string::npos) break;
        at=end+1;
    }
    if (auto result=pin(host==root_ ? host : host.parent_path(),pins)) return result;
    return check_ancestors(root_,host);
}
fs::path SaveFs::revision(const char* directory,Pins& pins) {
    const auto parent=root_/directory;
    int result=pin(root_,pins);
    if (!result && mkdir(parent.c_str(),0777) && errno!=EEXIST) result=error(errno);
    if (!result) result=pin(parent,pins);
    if (result) throw fs::filesystem_error("unsafe save backup directory",parent,std::error_code(EACCES,std::generic_category()));
    const auto ticks=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    return parent/(std::to_string(getpid())+"-"+std::to_string(ticks)+"-"+std::to_string(++revision_));
}
int SaveFs::backup(int source) {
    struct stat info;
    if (fstat(source,&info)) return error(errno);
    if (!info.st_size) return 0;
    int result=0;
    try {
        Pins pins;
        const int target=::open(revision(".backups",pins).c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        if (target<0) return error(errno);
        std::array<uint8_t,65536> buffer{};
        for (off_t at=0;!result;) {
            const ssize_t count=pread(source,buffer.data(),buffer.size(),at);   // pread: the caller's file position stays where it was
            if (count<0) { if (errno!=EINTR) result=error(errno); continue; }
            if (!count) break;
            result=write_all(target,buffer.data(),size_t(count));
            at+=count;
        }
        if (!result && fsync(target)) result=error(errno);
        if (::close(target) && !result) result=error(errno);
    }
    catch (const fs::filesystem_error& failure) { return error(failure.code().value()); }
    return result;
}
int SaveFs::backup(const fs::path& path) {
    const int source=::open(path.c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if (source<0) return error(errno);
    int result=file_info(source);
    if (!result) result=backup(source);
    ::close(source);
    return result;
}
int SaveFs::create_dir(const std::string& path) {
    if(rollback_) return writable().create(path,true);
    Pins pins; fs::path host; if (auto result=resolve(path,host,pins)) return result;
    return mkdir(host.c_str(),0777)==0 ? 0 : error(errno);
}
int SaveFs::create_file(const std::string& path) {
    if(rollback_) return writable().create(path,false);
    Pins pins; fs::path host; if (auto result=resolve(path,host,pins)) return result;
    const int file=::open(host.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0666);
    if (file<0) return error(errno);
    return ::close(file)==0 ? 0 : error(errno);
}
int SaveFs::open(const std::string& path,uint32_t mode) {
    if(rollback_) return writable().open(path,mode);
    if (mode<1 || mode>3 || next_fd_==INT_MAX) return -101;
    Pins pins; fs::path host; if (auto result=resolve(path,host,pins)) return result;
    const int file=::open(host.c_str(),(mode&2 ? O_RDWR : O_RDONLY)|O_NOFOLLOW|O_CLOEXEC);
    if (file<0) return error(errno);
    int result=file_info(file);
    if (!result && (mode&2)) result=backup(file);
    if (result) { ::close(file); return result; }
    const auto fd=next_fd_++;
    files_.emplace(fd,File{file,mode,std::move(pins)}); return fd;
}
int SaveFs::close(int fd) {
    if(rollback_) return writable().close(fd);
    auto found=files_.find(fd); if (found==files_.end()) return -101;
    if ((found->second.mode&2) && fsync(found->second.handle)) return error(errno);
    if (::close(found->second.handle)) return error(errno);
    files_.erase(found); return 0;
}
int SaveFs::read(int fd,std::span<uint8_t> bytes) {
    if(rollback_) return writable().read(fd,bytes);
    auto found=files_.find(fd); if (found==files_.end() || bytes.size()>INT_MAX) return -101;
    if (!(found->second.mode&1)) return -102;
    for (;;) {
        const ssize_t count=::read(found->second.handle,bytes.data(),bytes.size());
        if (count>=0) return int(count);
        if (errno!=EINTR) return error(errno);
    }
}
int SaveFs::write(int fd,std::span<const uint8_t> bytes) {
    if(rollback_) return writable().write(fd,bytes);
    auto found=files_.find(fd); if (found==files_.end() || bytes.size()>INT_MAX) return -101;
    auto& file=found->second;
    if (!(file.mode&2)) return -102;
    const int result=write_all(file.handle,bytes.data(),bytes.size());
    return result ? result : int(bytes.size());
}
int SaveFs::seek(int fd,int32_t offset,int32_t whence) {
    if(rollback_) return writable().seek(fd,offset,whence);
    auto found=files_.find(fd); if (found==files_.end() || whence<0 || whence>2) return -101;
    int64_t base=0;
    if (whence==1) { const off_t here=lseek(found->second.handle,0,SEEK_CUR); if (here<0) return error(errno); base=here; }
    if (whence==2) { struct stat info; if (fstat(found->second.handle,&info)) return error(errno); base=info.st_size; }
    const int64_t position=base+offset;
    if (position<0 || position>INT_MAX) return -101;
    if (lseek(found->second.handle,off_t(position),SEEK_SET)<0) return error(errno);
    return int(position);
}
SaveFs::Stats SaveFs::stats(int fd) {
    if(rollback_) return rollback_->stats(fd);
    auto found=files_.find(fd); if (found==files_.end()) return {-101};
    struct stat info;
    const off_t position=lseek(found->second.handle,0,SEEK_CUR);
    if (fstat(found->second.handle,&info) || position<0) return {error(errno)};
    if (uint64_t(info.st_size)>UINT32_MAX || uint64_t(position)>UINT32_MAX) return {-101};
    return {0,uint32_t(info.st_size),uint32_t(position)};
}
int SaveFs::attributes(const std::string& path,bool& directory) {
    if(rollback_) return rollback_->attributes(path,directory);
    Pins pins; fs::path host; if (auto result=resolve(path,host,pins)) return result;
    struct stat info;
    if (lstat(host.c_str(),&info)) return error(errno);
    if (S_ISLNK(info.st_mode)) return -102;
    directory=S_ISDIR(info.st_mode); return 0;
}
SaveFs::Directory SaveFs::read_dir(const std::string& path) {
    if(rollback_) return rollback_->read_dir(path);
    Pins pins; fs::path host; if (auto result=resolve(path,host,pins)) return {result};
    bool directory; if (auto result=attributes(path,directory)) return {result};
    if (!directory) return {-101};
    if (auto result=pin(host,pins)) return {result};
    Directory result{0}; std::error_code ec;
    for (fs::directory_iterator it(host,ec),end;!ec && it!=end;it.increment(ec)) {
        if (host==root_ && internal(it->path().filename().string())) continue;
        if (check_ancestors(root_,it->path())) return {-102};
        result.names.push_back(it->path().filename().string());
    }
    if (ec) return {error(ec.value())};
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
        if (host==root_ && internal(it->path().filename().string()) && it->path().parent_path()==root_) { it.disable_recursion_pending(); continue; }
        if (check_ancestors(root_,it->path())) return {-102};
        ++entries;
        if (it->is_directory(ec)) { if (auto failure=pin(it->path(),pins)) return {failure}; }
        else {
            struct stat info;
            if (lstat(it->path().c_str(),&info)) return {error(errno)};
            if (auto failure=file_info(info)) return {failure};
            blocks+=(uint64_t(info.st_size)+16383)/16384;
        }
    }
    if (ec) return {error(ec.value())};
    if (blocks>UINT32_MAX || entries>UINT32_MAX) return {-101};
    return {0,uint32_t(blocks),uint32_t(entries)};
}
int SaveFs::rename(const std::string& from,const std::string& to) {
    if(rollback_) return writable().rename(from,to);
    Pins pins; fs::path source,target; if (auto result=resolve(to,target,pins)) return result; if (auto result=resolve(from,source,pins)) return result;
    if (source==root_ || target==root_) return -102;
    struct stat info;
    if (source==target) return lstat(source.c_str(),&info) ? error(errno) : 0;
    if (lstat(target.c_str(),&info)==0) if (auto result=backup(target)) return result;
    return move(source,target,true);
}
int SaveFs::remove(const std::string& path) {
    if(rollback_) return writable().remove(path);
    Pins pins; fs::path host; if (auto result=resolve(path,host,pins)) return result;
    if (host==root_) return -102;
    struct stat info;
    if (lstat(host.c_str(),&info)) return error(errno);
    if (S_ISDIR(info.st_mode)) { auto contents=read_dir(path); if (contents.result) return contents.result; if (!contents.names.empty()) return -115; }
    try { const auto target=revision(".deleted",pins); return move(host,target,false); }
    catch (const fs::filesystem_error& failure) { return error(failure.code().value()); }
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
        struct stat info;
        if(lstat(target.c_str(),&info)==0) require(disk.backup(target),"backup");
        // The new content is written and flushed beside the target and renamed over it: a crash leaves the old or the new file, never half of one.
        const auto temporary=disk.revision(".backups",pins);
        const int handle=::open(temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        if(handle<0) require(error(errno),"create staged file");
        int result=write_all(handle,node.bytes->data(),node.bytes->size());
        if(!result && fsync(handle)) result=error(errno);
        if(::close(handle) && !result) result=error(errno);
        require(result,"write staged file");
        require(move(temporary,target,true),"publish");
    }
    committed_=std::make_shared<Snapshot>(saved);
}
}
