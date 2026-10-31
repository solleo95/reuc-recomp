#include "files_posix.h"
#include "sha256.h"
#include <array>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace uc::update {
Fd::~Fd() { if(fd_>=0) ::close(fd_); }
Fd& Fd::operator=(Fd&& other) noexcept {
    if(this!=&other) { if(fd_>=0) ::close(fd_); fd_=std::exchange(other.fd_,-1); }
    return *this;
}
Error file_error(std::string action,int code) {
    const auto kind=code==ENOSPC?ErrorCode::no_space:(code==EACCES||code==EPERM||code==EROFS)?ErrorCode::permission:ErrorCode::io;
    return {kind,std::move(action)+": "+std::strerror(code)};
}
fs::path utf8_path(std::string_view text) { return fs::path(std::string(text)); }
Result<Fd> open_read(const fs::path& path) {
    Fd fd(::open(path.c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC));
    if(!fd.valid()) return file_error("Open "+path.string());
    struct stat info{};
    if(::fstat(fd.get(),&info)!=0) return file_error("Inspect "+path.string());
    if(!S_ISREG(info.st_mode)) return Error{ErrorCode::unsafe_path,"Not an ordinary file: "+path.string()};
    return fd;
}
Result<std::string> read_text(const fs::path& path,uint64_t limit) {
    auto opened=open_read(path);
    if(failed(opened)) return std::get<Error>(opened);
    auto fd=std::get<Fd>(std::move(opened));
    std::string text;
    std::array<char,65536> buffer{};
    for(;;) {
        const auto got=::read(fd.get(),buffer.data(),buffer.size());
        if(got<0) { if(errno==EINTR) continue; return file_error("Read "+path.string()); }
        if(!got) break;
        if(static_cast<uint64_t>(got)>limit-std::min<uint64_t>(limit,text.size())) return Error{ErrorCode::io,"File exceeds limit: "+path.string()};
        text.append(buffer.data(),static_cast<size_t>(got));
    }
    return text;
}
Result<std::string> sha256_fd(int fd) {
    if(::lseek(fd,0,SEEK_SET)<0) return file_error("Rewind file");
    Sha256 hash;
    std::array<unsigned char,65536> buffer{};
    for(;;) {
        const auto got=::read(fd,buffer.data(),buffer.size());
        if(got<0) { if(errno==EINTR) continue; return file_error("Read file for SHA-256"); }
        if(!got) break;
        hash.update({buffer.data(),static_cast<size_t>(got)});
    }
    return hash.finish_hex();
}
namespace {
Status remove_entry(int parent,const char* name,int depth) {
    if(depth>128) return Error{ErrorCode::io,"Directory tree too deep"};
    struct stat info{};
    if(::fstatat(parent,name,&info,AT_SYMLINK_NOFOLLOW)!=0) return errno==ENOENT?Status{std::monostate{}}:Status{file_error("Inspect "+std::string(name))};
    if(!S_ISDIR(info.st_mode)) return ::unlinkat(parent,name,0)==0||errno==ENOENT?Status{std::monostate{}}:Status{file_error("Delete "+std::string(name))};
    Fd directory(::openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
    if(!directory.valid()) return file_error("Open directory "+std::string(name));
    DIR* stream=::fdopendir(::dup(directory.get()));
    if(!stream) return file_error("List directory "+std::string(name));
    Status result=std::monostate{};
    while(auto* entry=::readdir(stream)) {
        const std::string_view child=entry->d_name;
        if(child=="."||child=="..") continue;
        result=remove_entry(directory.get(),entry->d_name,depth+1);
        if(failed(result)) break;
    }
    ::closedir(stream);
    if(failed(result)) return result;
    if(::unlinkat(parent,name,AT_REMOVEDIR)!=0&&errno!=ENOENT) return file_error("Delete directory "+std::string(name));
    return std::monostate{};
}
}
Status remove_owned_tree(const fs::path& root) {
    auto absolute=fs::absolute(root);
    return remove_entry(AT_FDCWD,absolute.c_str(),0);
}
Status fsync_file(const fs::path& path) {
    Fd fd(::open(path.c_str(),O_RDONLY|O_CLOEXEC));
    if(!fd.valid()) return file_error("Open "+path.string()+" for fsync");
    if(::fsync(fd.get())!=0) return file_error("Flush "+path.string());
    return std::monostate{};
}
Status set_file_mode(int fd,mode_t wanted) {
    if(::fchmod(fd,wanted)==0) return std::monostate{};
    const int code=errno;
    if(code==EPERM || code==ENOTSUP || code==EOPNOTSUPP || code==ENOSYS) {
        struct stat info{};
        if(::fstat(fd,&info)==0 && (!(wanted&S_IXUSR) || (info.st_mode&S_IXUSR))) return std::monostate{};
    }
    return file_error("Set file mode",code);
}
Status replace_file(const fs::path& from,const fs::path& to) {
    if(::rename(from.c_str(),to.c_str())!=0) return file_error("Rename "+from.string());
    auto parent=to.parent_path();
    if(parent.empty()) parent=".";
    return fsync_file(parent);
}
}
