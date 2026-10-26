#include "package.h"
#include "files.h"
#include <miniz.h>
#include <algorithm>
#include <map>
#include <set>

namespace uc::update {
namespace {
Error archive_error(std::string message) { return {ErrorCode::invalid_archive,std::move(message)}; }
struct Reader {
    mz_zip_archive zip{};
    ~Reader() { if(zip.m_pState) mz_zip_reader_end(&zip); }
};
size_t zip_read(void* opaque,mz_uint64 offset,void* buffer,size_t size) {
    auto handle=static_cast<HANDLE>(opaque);
    LARGE_INTEGER pos{}; pos.QuadPart=static_cast<LONGLONG>(offset);
    if(!SetFilePointerEx(handle,pos,nullptr,FILE_BEGIN)) return 0;
    size_t total=0;
    while(total<size) {
        DWORD got=0, count=static_cast<DWORD>(std::min<size_t>(size-total,65536));
        if(!ReadFile(handle,static_cast<char*>(buffer)+total,count,&got,nullptr) || !got) break;
        total+=got;
    }
    return total;
}
struct Sink { HANDLE file; uint64_t limit, written{}; std::stop_token stop; bool io_error=false; };
size_t zip_write(void* opaque,mz_uint64 offset,const void* bytes,size_t size) {
    auto& sink=*static_cast<Sink*>(opaque);
    if(sink.stop.stop_requested() || offset!=sink.written || sink.written>sink.limit ||
        size>sink.limit-sink.written) return 0;
    size_t total=0;
    while(total<size) {
        DWORD wrote=0, count=static_cast<DWORD>(std::min<size_t>(size-total,65536));
        if(!WriteFile(sink.file,static_cast<const char*>(bytes)+total,count,&wrote,nullptr) || !wrote) {
            sink.io_error=true; break;
        }
        total+=wrote;
    }
    sink.written+=total; return total;
}
Result<std::string> entry_name(mz_zip_archive& zip,unsigned index) {
    auto size=mz_zip_reader_get_filename(&zip,index,nullptr,0);
    if(!size || size>32761) return archive_error("Invalid archive filename length");
    std::string name(size,'\0');
    if(mz_zip_reader_get_filename(&zip,index,name.data(),size)!=size ||
        name.back()!='\0' || name.find('\0')!=name.size()-1)
        return archive_error("Invalid archive filename");
    name.pop_back(); return name;
}
bool same_manifest(const Manifest& a,const Manifest& b) {
    if(a.version!=b.version || a.helper_files!=b.helper_files || a.files.size()!=b.files.size()) return false;
    for(size_t i=0;i<a.files.size();++i)
        if(a.files[i].path!=b.files[i].path || a.files[i].size!=b.files[i].size ||
            a.files[i].sha256!=b.files[i].sha256) return false;
    return true;
}
}
Result<std::string> sha256_file(const fs::path& file) {
    auto handle=open_read(file);
    if(failed(handle)) return std::get<Error>(handle);
    return sha256_handle(std::get<Handle>(handle).get());
}
Status validate_target_path(const fs::path& install,std::string_view relative) {
    auto checked=path_key(relative);
    if(failed(checked)) return std::get<Error>(checked);
    // Missing parents are allowed during preflight; every existing prefix must be ordinary.
    auto full=fs::absolute(install).lexically_normal()/utf8_path(relative);
    auto parent=full.parent_path();
    while(GetFileAttributesW(parent.c_str())==INVALID_FILE_ATTRIBUTES) {
        if(GetLastError()!=ERROR_FILE_NOT_FOUND && GetLastError()!=ERROR_PATH_NOT_FOUND)
            return file_error("Inspect destination");
        if(parent==parent.root_path()) return Error{ErrorCode::unsafe_path,"Missing destination root"};
        parent=parent.parent_path();
    }
    auto hold=lock_parent(parent,".uc-probe");
    if(failed(hold)) return std::get<Error>(hold);
    auto attr=GetFileAttributesW(full.c_str());
    if(attr==INVALID_FILE_ATTRIBUTES) {
        if(GetLastError()==ERROR_FILE_NOT_FOUND || GetLastError()==ERROR_PATH_NOT_FOUND) return std::monostate{};
        return file_error("Inspect destination file");
    }
    if(attr&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))
        return Error{ErrorCode::unsafe_path,"Destination is a directory or reparse point"};
    return std::monostate{};
}
Status verify_package(const Package& package) {
    auto root_lock=lock_parent(package.root,"release.json");
    if(failed(root_lock)) return std::get<Error>(root_lock);
    auto text=read_text(package.root/"release.json",max_manifest_size);
    if(failed(text)) return std::get<Error>(text);
    auto parsed=parse_manifest(std::get<std::string>(text));
    if(failed(parsed)) return std::get<Error>(parsed);
    if(!same_manifest(package.manifest,std::get<Manifest>(parsed)))
        return Error{ErrorCode::integrity,"Staged manifest changed"};
    std::set<std::wstring> expected{L"RELEASE.JSON"};
    for(const auto& file:package.manifest.files) {
        auto safe=validate_managed_path(file.path);
        if(failed(safe)) return std::get<Error>(safe);
        expected.insert(std::get<std::wstring>(path_key(file.path)));
        auto opened=open_read(package.root/utf8_path(file.path));
        if(failed(opened)) return std::get<Error>(opened);
        auto handle=std::get<Handle>(std::move(opened));
        LARGE_INTEGER size{};
        if(!GetFileSizeEx(handle.get(),&size)) return file_error("Inspect staged size");
        auto hash=sha256_handle(handle.get());
        if(failed(hash)) return std::get<Error>(hash);
        if(size.QuadPart<0 || static_cast<uint64_t>(size.QuadPart)!=file.size || std::get<std::string>(hash)!=file.sha256)
            return Error{ErrorCode::integrity,"Staged file size or SHA-256 mismatch"};
    }
    std::error_code ec;
    fs::recursive_directory_iterator it(package.root,ec),end;
    if(ec) return file_error("Enumerate package");
    for(;it!=end;it.increment(ec)) {
        if(ec) return Error{ErrorCode::io,"Cannot enumerate package: "+ec.message()};
        auto attr=GetFileAttributesW(it->path().c_str());
        if(attr==INVALID_FILE_ATTRIBUTES) return file_error("Inspect package entry");
        if(attr&FILE_ATTRIBUTE_REPARSE_POINT) return Error{ErrorCode::unsafe_path,"Reparse point in package"};
        if(attr&FILE_ATTRIBUTE_DIRECTORY) continue;
        auto relative=it->path().lexically_relative(package.root).generic_u8string();
        auto key=path_key(std::string(relative.begin(),relative.end()));
        if(failed(key) || !expected.erase(std::get<std::wstring>(key)))
            return Error{ErrorCode::integrity,"Unexpected package file"};
    }
    if(ec || !expected.empty()) return Error{ErrorCode::integrity,"Incomplete package"};
    return std::monostate{};
}
Result<Package> prepare_package(const fs::path& zip_path,const Release& release,
    const fs::path& staging,std::stop_token stop) {
    if(stop.stop_requested()) return Error{ErrorCode::cancelled,"Update cancelled"};
    if(release.size>max_zip_size) return archive_error("Archive exceeds limit");
    auto opened=open_read(zip_path);
    if(failed(opened)) return std::get<Error>(opened);
    auto input=std::get<Handle>(std::move(opened));
    LARGE_INTEGER size{};
    if(!GetFileSizeEx(input.get(),&size)) return file_error("Read archive size");
    auto digest=sha256_handle(input.get());
    if(failed(digest)) return std::get<Error>(digest);
    if(size.QuadPart<0 || static_cast<uint64_t>(size.QuadPart)!=release.size || std::get<std::string>(digest)!=release.sha256)
        return Error{ErrorCode::integrity,"Archive size or SHA-256 mismatch"};
    Reader reader;
    reader.zip.m_pRead=zip_read; reader.zip.m_pIO_opaque=input.get();
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
        if(stat.m_is_directory || stat.m_is_encrypted || !stat.m_is_supported ||
            (stat.m_external_attr&FILE_ATTRIBUTE_REPARSE_POINT) || (mode && mode!=0100000))
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
        if(!entries.emplace(normalized,Entry{i,stat.m_uncomp_size,std::move(path)}).second)
            return archive_error("Duplicate archive path");
    }
    auto manifest_entry=entries.find(L"RELEASE.JSON");
    if(manifest_entry==entries.end() || manifest_entry->second.size>max_manifest_size)
        return archive_error("Missing or oversized release.json");
    std::string text(static_cast<size_t>(manifest_entry->second.size),'\0');
    if(!mz_zip_reader_extract_to_mem(&zip,manifest_entry->second.index,text.data(),text.size(),0))
        return archive_error("Cannot extract release.json");
    auto parsed=parse_manifest(text);
    if(failed(parsed)) return std::get<Error>(parsed);
    auto manifest=std::get<Manifest>(std::move(parsed));
    if(manifest.version!=release.version || entries.size()!=manifest.files.size()+1)
        return archive_error("Archive does not match release manifest");
    for(const auto& file:manifest.files) {
        auto entry=entries.find(std::get<std::wstring>(path_key(file.path)));
        if(entry==entries.end() || entry->second.path!=file.path || entry->second.size!=file.size)
            return archive_error("Archive entry does not match manifest");
    }
    const auto stage_name=staging.filename().u8string();
    auto stage_parent=lock_parent(staging.parent_path(),std::string(stage_name.begin(),stage_name.end()));
    if(failed(stage_parent)) return std::get<Error>(stage_parent);
    if(!CreateDirectoryW(staging.c_str(),nullptr)) return file_error("Create new staging directory");
    auto result=[&]() -> Result<Package> {
        auto hold=lock_parent(staging,"release.json");
        if(failed(hold)) return std::get<Error>(hold);
        for(const auto& [key,entry]:entries) {
            if(stop.stop_requested()) return Error{ErrorCode::cancelled,"Update cancelled"};
            auto parent=lock_parent(staging,entry.path,true);
            if(failed(parent)) return std::get<Error>(parent);
            auto target=std::get<ParentLock>(parent).target;
            Handle output(CreateFileW(target.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
            if(!output.valid()) return file_error("Create staged file");
            Sink sink{output.get(),entry.size,0,stop};
            if(!mz_zip_reader_extract_to_callback(&zip,entry.index,zip_write,&sink,0) || sink.written!=entry.size) {
                if(stop.stop_requested()) return Error{ErrorCode::cancelled,"Update cancelled"};
                if(sink.io_error) return file_error("Write staged file");
                return archive_error("ZIP extraction or CRC verification failed");
            }
            if(!FlushFileBuffers(output.get())) return file_error("Flush staged file");
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
