#include "package.h"
#include "files_posix.h"
#include <miniz.h>
#include <algorithm>
#include <fcntl.h>
#include <map>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

// POSIX twin of updater/package.cpp (Steam Deck): the same checks on the archive and the staged tree, file descriptors instead of handles,
// symlinks refused instead of reparse points, and the known executables get 0755 whatever the archive stored.
namespace uc::update {
namespace {
Error archive_error(std::string message) { return {ErrorCode::invalid_archive,std::move(message)}; }
struct Reader {
    mz_zip_archive zip{};
    ~Reader() { if(zip.m_pState) mz_zip_reader_end(&zip); }
};
size_t zip_read(void* opaque,mz_uint64 offset,void* buffer,size_t size) {
    const int fd=*static_cast<int*>(opaque);
    size_t total=0;
    while(total<size) {
        const auto got=::pread(fd,static_cast<char*>(buffer)+total,size-total,static_cast<off_t>(offset+total));
        if(got<0&&errno==EINTR) continue;
        if(got<=0) break;
        total+=static_cast<size_t>(got);
    }
    return total;
}
struct Sink { int fd; uint64_t limit, written{}; std::stop_token stop; bool io_error=false; int error=0; /* errno of the failed write: miniz returns through its own code before we look */ };
size_t zip_write(void* opaque,mz_uint64 offset,const void* bytes,size_t size) {
    auto& sink=*static_cast<Sink*>(opaque);
    if(sink.stop.stop_requested() || offset!=sink.written || sink.written>sink.limit || size>sink.limit-sink.written) return 0;
    size_t total=0;
    while(total<size) {
        const auto wrote=::write(sink.fd,static_cast<const char*>(bytes)+total,size-total);
        if(wrote<0&&errno==EINTR) continue;
        if(wrote<=0) { sink.io_error=true; sink.error=wrote<0?errno:ENOSPC; break; }   // a write of 0 bytes means the device is full
        total+=static_cast<size_t>(wrote);
    }
    sink.written+=total; return total;
}
Result<std::string> entry_name(mz_zip_archive& zip,unsigned index) {
    auto size=mz_zip_reader_get_filename(&zip,index,nullptr,0);
    if(!size || size>32761) return archive_error("Invalid archive filename length");
    std::string name(size,'\0');
    if(mz_zip_reader_get_filename(&zip,index,name.data(),size)!=size || name.back()!='\0' || name.find('\0')!=name.size()-1)
        return archive_error("Invalid archive filename");
    name.pop_back(); return name;
}
bool same_manifest(const Manifest& a,const Manifest& b) {
    if(a.version!=b.version || a.helper_files!=b.helper_files || a.files.size()!=b.files.size()) return false;
    for(size_t i=0;i<a.files.size();++i)
        if(a.files[i].path!=b.files[i].path || a.files[i].size!=b.files[i].size || a.files[i].sha256!=b.files[i].sha256) return false;
    return true;
}
bool executable_path(std::string_view path) {
    return path=="uc" || path=="uc_launcher" || path=="uc-launcher.sh" || path=="uc.sh" || path=="tools/wit/wit";
}
}
Result<std::string> sha256_file(const fs::path& file) {
    auto opened=open_read(file);
    if(failed(opened)) return std::get<Error>(opened);
    return sha256_fd(std::get<Fd>(opened).get());
}
Status validate_target_path(const fs::path& install,std::string_view relative) {
    auto checked=path_key(relative);
    if(failed(checked)) return std::get<Error>(checked);
    // Missing parents are allowed during preflight; every existing component below the install folder must be ordinary.
    fs::path current=fs::absolute(install).lexically_normal();
    const auto parts=utf8_path(relative);
    auto it=parts.begin();
    for(size_t index=0,count=std::distance(parts.begin(),parts.end());it!=parts.end();++it,++index) {
        current/=*it;
        struct stat info{};
        if(::lstat(current.c_str(),&info)!=0) {
            if(errno==ENOENT||errno==ENOTDIR) return std::monostate{};
            return file_error("Inspect destination");
        }
        if(S_ISLNK(info.st_mode)) return Error{ErrorCode::unsafe_path,"Destination contains a symlink"};
        const bool last=index+1==count;
        if(!last&&!S_ISDIR(info.st_mode)) return Error{ErrorCode::unsafe_path,"Destination parent is not a directory"};
        if(last&&!S_ISREG(info.st_mode)) return Error{ErrorCode::unsafe_path,"Destination is a directory or special file"};
    }
    return std::monostate{};
}
Status verify_package(const Package& package) {
    auto text=read_text(package.root/"release.json",max_manifest_size);
    if(failed(text)) return std::get<Error>(text);
    auto parsed=parse_manifest(std::get<std::string>(text),host_platform);
    if(failed(parsed)) return std::get<Error>(parsed);
    if(!same_manifest(package.manifest,std::get<Manifest>(parsed))) return Error{ErrorCode::integrity,"Staged manifest changed"};
    std::set<std::wstring> expected{L"RELEASE.JSON"};
    for(const auto& file:package.manifest.files) {
        auto safe=validate_managed_path(file.path);
        if(failed(safe)) return std::get<Error>(safe);
        expected.insert(std::get<std::wstring>(path_key(file.path)));
        auto opened=open_read(package.root/utf8_path(file.path));
        if(failed(opened)) return std::get<Error>(opened);
        auto fd=std::get<Fd>(std::move(opened));
        struct stat info{};
        if(::fstat(fd.get(),&info)!=0) return file_error("Inspect staged size");
        auto hash=sha256_fd(fd.get());
        if(failed(hash)) return std::get<Error>(hash);
        if(info.st_size<0 || static_cast<uint64_t>(info.st_size)!=file.size || std::get<std::string>(hash)!=file.sha256)
            return Error{ErrorCode::integrity,"Staged file size or SHA-256 mismatch"};
    }
    std::error_code ec;
    fs::recursive_directory_iterator it(package.root,ec),end;
    if(ec) return Error{ErrorCode::io,"Cannot enumerate package: "+ec.message()};
    for(;it!=end;it.increment(ec)) {
        if(ec) return Error{ErrorCode::io,"Cannot enumerate package: "+ec.message()};
        const auto status=it->symlink_status(ec);
        if(ec) return Error{ErrorCode::io,"Cannot inspect package entry: "+ec.message()};
        if(fs::is_symlink(status)) return Error{ErrorCode::unsafe_path,"Symlink in package"};
        if(fs::is_directory(status)) continue;
        auto relative=it->path().lexically_relative(package.root).generic_string();
        auto key=path_key(relative);
        if(failed(key) || !expected.erase(std::get<std::wstring>(key))) return Error{ErrorCode::integrity,"Unexpected package file"};
    }
    if(!expected.empty()) return Error{ErrorCode::integrity,"Incomplete package"};
    return std::monostate{};
}
Result<Package> prepare_package(const fs::path& zip_path,const Release& release,const fs::path& staging,std::stop_token stop) {
    if(stop.stop_requested()) return Error{ErrorCode::cancelled,"Update cancelled"};
    if(release.size>max_zip_size) return archive_error("Archive exceeds limit");
    auto opened=open_read(zip_path);
    if(failed(opened)) return std::get<Error>(opened);
    auto input=std::get<Fd>(std::move(opened));
    struct stat info{};
    if(::fstat(input.get(),&info)!=0) return file_error("Read archive size");
    auto digest=sha256_fd(input.get());
    if(failed(digest)) return std::get<Error>(digest);
    if(info.st_size<0 || static_cast<uint64_t>(info.st_size)!=release.size || std::get<std::string>(digest)!=release.sha256)
        return Error{ErrorCode::integrity,"Archive size or SHA-256 mismatch"};
    Reader reader;
    int fd_value=input.get();
    reader.zip.m_pRead=zip_read; reader.zip.m_pIO_opaque=&fd_value;
    if(!mz_zip_reader_init(&reader.zip,release.size,0)) return archive_error("Cannot read ZIP");
    auto& zip=reader.zip;
    const auto count=mz_zip_reader_get_num_files(&zip);
    if(!count || count>max_entries+1) return archive_error("Too many archive entries");
    struct Entry { unsigned index; uint64_t size; std::string path; };
    std::map<std::wstring,Entry> entries;
    uint64_t expanded=0;
    for(unsigned i=0;i<count;++i) {
        mz_zip_archive_file_stat stat{};
        if(!mz_zip_reader_file_stat(&zip,i,&stat)) return archive_error("Cannot read ZIP entry");
        const auto mode=(stat.m_external_attr>>16)&0170000;
        if(stat.m_is_directory || stat.m_is_encrypted || !stat.m_is_supported || (mode && mode!=0100000))
            return archive_error("Only ordinary unencrypted files are allowed");
        if(stat.m_uncomp_size>max_zip_size || expanded>max_package_size-stat.m_uncomp_size)
            return archive_error("Expanded archive exceeds limit");
        expanded+=stat.m_uncomp_size;
        auto name=entry_name(zip,i);
        if(failed(name)) return std::get<Error>(name);
        auto path=std::get<std::string>(std::move(name));
        auto key=path_key(path);
        if(failed(key)) return std::get<Error>(key);
        auto normalized=std::get<std::wstring>(key);
        if(normalized!=L"RELEASE.JSON") {
            auto safe=validate_managed_path(path);
            if(failed(safe)) return std::get<Error>(safe);
        } else if(path!="release.json") return archive_error("Noncanonical manifest name");
        if(!entries.emplace(normalized,Entry{i,stat.m_uncomp_size,std::move(path)}).second) return archive_error("Duplicate archive path");
    }
    auto manifest_entry=entries.find(L"RELEASE.JSON");
    if(manifest_entry==entries.end() || manifest_entry->second.size>max_manifest_size)
        return archive_error("Missing or oversized release.json");
    std::string text(static_cast<size_t>(manifest_entry->second.size),'\0');
    if(!mz_zip_reader_extract_to_mem(&zip,manifest_entry->second.index,text.data(),text.size(),0))
        return archive_error("Cannot extract release.json");
    auto parsed=parse_manifest(text,host_platform);
    if(failed(parsed)) return std::get<Error>(parsed);
    auto manifest=std::get<Manifest>(std::move(parsed));
    if(manifest.version!=release.version || entries.size()!=manifest.files.size()+1)
        return archive_error("Archive does not match release manifest");
    for(const auto& file:manifest.files) {
        auto entry=entries.find(std::get<std::wstring>(path_key(file.path)));
        if(entry==entries.end() || entry->second.path!=file.path || entry->second.size!=file.size)
            return archive_error("Archive entry does not match manifest");
    }
    if(::mkdir(staging.c_str(),0700)!=0) return file_error("Create new staging directory");
    auto result=[&]() -> Result<Package> {
        for(const auto& [key,entry]:entries) {
            if(stop.stop_requested()) return Error{ErrorCode::cancelled,"Update cancelled"};
            const auto target=staging/utf8_path(entry.path);
            std::error_code ec;
            fs::create_directories(target.parent_path(),ec);   // names were validated; staging holds only what we created
            if(ec) return Error{ErrorCode::io,"Create staging directory: "+ec.message()};
            Fd output(::open(target.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));
            if(!output.valid()) return file_error("Create staged file");
            Sink sink{output.get(),entry.size,0,stop};
            if(!mz_zip_reader_extract_to_callback(&zip,entry.index,zip_write,&sink,0) || sink.written!=entry.size) {
                if(stop.stop_requested()) return Error{ErrorCode::cancelled,"Update cancelled"};
                if(sink.io_error) return file_error("Write staged file",sink.error);
                return archive_error("ZIP extraction or CRC verification failed");
            }
            if(auto mode=set_file_mode(output.get(),executable_path(entry.path)?0755:0644);failed(mode)) return std::get<Error>(mode);   // an SD card (exFAT) cannot hold modes
            if(::fsync(output.get())!=0) return file_error("Flush staged file");
        }
        Package package{staging,std::move(manifest)};
        auto verified=verify_package(package);
        if(failed(verified)) return std::get<Error>(verified);
        return package;
    }();
    if(failed(result)) {
        auto cleanup=remove_owned_tree(staging);
        if(failed(cleanup)) std::get<Error>(result).detail += "; staging retained: "+std::get<Error>(cleanup).detail;
    }
    return result;
}
}
