#include "files.h"
#include "../common/file_rename.h"
#include <bcrypt.h>
#include <winternl.h>
#include <array>
#include <cstdio>
namespace uc::update {
Error file_error(std::string action, DWORD code) {
    auto type = ErrorCode::io;
    if(code==ERROR_ACCESS_DENIED) type=ErrorCode::permission;
    if(code==ERROR_SHARING_VIOLATION || code==ERROR_LOCK_VIOLATION) type=ErrorCode::busy;
    if(code==ERROR_DISK_FULL || code==ERROR_HANDLE_DISK_FULL) type=ErrorCode::no_space;
    return {type,std::move(action)+" (Windows "+std::to_string(code)+")"};
}
fs::path utf8_path(std::string_view text) {
    std::u8string value; value.reserve(text.size());
    for(unsigned char c:text) value.push_back(static_cast<char8_t>(c));
    return fs::path(value);
}
namespace {
Result<Handle> directory(const fs::path& path) {
    // Metadata-only access does not enforce sharing restrictions against rename.
    Handle handle(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,
        OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(!handle.valid()) return file_error("Open directory");
    FILE_ATTRIBUTE_TAG_INFO info{};
    if(!GetFileInformationByHandleEx(handle.get(),FileAttributeTagInfo,&info,sizeof(info)))
        return file_error("Inspect directory");
    if(!(info.FileAttributes&FILE_ATTRIBUTE_DIRECTORY) || (info.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))
        return Error{ErrorCode::unsafe_path,"Directory is a reparse point or not a directory"};
    return handle;
}
}
Result<ParentLock> lock_parent(const fs::path& root, std::string_view relative, bool create) {
    auto checked=path_key(relative);
    if(failed(checked)) return std::get<Error>(checked);
    std::error_code ec;
    auto absolute=fs::absolute(root,ec).lexically_normal();
    if(ec || absolute.root_name().wstring().size()!=2)
        return Error{ErrorCode::unsafe_path,"Expected an absolute local drive path"};
    ParentLock result;
    auto current=absolute.root_path();
    auto first=directory(current);
    if(failed(first)) return std::get<Error>(first);
    result.directories.push_back(std::get<Handle>(std::move(first)));
    for(const auto& part:absolute.relative_path()) {
        current/=part;
        auto opened=directory(current);
        if(failed(opened)) return std::get<Error>(opened);
        result.directories.push_back(std::get<Handle>(std::move(opened)));
    }
    auto path=utf8_path(relative);
    for(const auto& part:path.parent_path()) {
        current/=part;
        if(create && !CreateDirectoryW(current.c_str(),nullptr) && GetLastError()!=ERROR_ALREADY_EXISTS)
            return file_error("Create package directory");
        auto opened=directory(current);
        if(failed(opened)) return std::get<Error>(opened);
        result.directories.push_back(std::get<Handle>(std::move(opened)));
    }
    result.target=current/path.filename();
    return result;
}
Result<Handle> open_read(const fs::path& path) {
    auto name=path.filename().u8string();
    auto locked=lock_parent(path.parent_path(),std::string(name.begin(),name.end()));
    if(failed(locked)) return std::get<Error>(locked);
    Handle handle(CreateFileW(std::get<ParentLock>(locked).target.c_str(),GENERIC_READ,FILE_SHARE_READ,
        nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
    if(!handle.valid()) return file_error("Open file");
    FILE_ATTRIBUTE_TAG_INFO info{};
    if(!GetFileInformationByHandleEx(handle.get(),FileAttributeTagInfo,&info,sizeof(info))) return file_error("Inspect file");
    if(info.FileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))
        return Error{ErrorCode::unsafe_path,"File is a reparse point or directory"};
    return handle;
}
Result<std::string> read_text(const fs::path& path,uint64_t limit) {
    auto opened=open_read(path);
    if(failed(opened)) return std::get<Error>(opened);
    auto& handle=std::get<Handle>(opened);
    LARGE_INTEGER length{};
    if(!GetFileSizeEx(handle.get(),&length)) return file_error("Read file size");
    if(length.QuadPart<0 || static_cast<uint64_t>(length.QuadPart)>limit)
        return Error{ErrorCode::integrity,"File exceeds size limit"};
    std::string bytes(static_cast<size_t>(length.QuadPart),'\0');
    size_t offset=0;
    while(offset<bytes.size()) {
        DWORD got=0, wanted=static_cast<DWORD>(std::min<size_t>(bytes.size()-offset,65536));
        if(!ReadFile(handle.get(),bytes.data()+offset,wanted,&got,nullptr)) return file_error("Read file");
        if(!got) return Error{ErrorCode::integrity,"Unexpected end of file"};
        offset+=got;
    }
    return bytes;
}
Result<std::string> sha256_handle(HANDLE handle) {
    BCRYPT_ALG_HANDLE algorithm=nullptr; BCRYPT_HASH_HANDLE hash=nullptr;
    struct Cleanup {
        BCRYPT_ALG_HANDLE& algorithm; BCRYPT_HASH_HANDLE& hash;
        ~Cleanup() { if(hash) BCryptDestroyHash(hash); if(algorithm) BCryptCloseAlgorithmProvider(algorithm,0); }
    } cleanup{algorithm,hash};
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0 ||
        BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)<0)
        return Error{ErrorCode::integrity,"Cannot initialize SHA-256"};
    LARGE_INTEGER zero{};
    if(!SetFilePointerEx(handle,zero,nullptr,FILE_BEGIN)) return file_error("Seek file");
    std::array<unsigned char,65536> buffer{};
    for(;;) {
        DWORD got=0;
        if(!ReadFile(handle,buffer.data(),static_cast<DWORD>(buffer.size()),&got,nullptr)) return file_error("Hash file");
        if(!got) break;
        if(BCryptHashData(hash,buffer.data(),got,0)<0) return Error{ErrorCode::integrity,"Cannot hash file"};
    }
    std::array<unsigned char,32> digest{};
    if(BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0)<0)
        return Error{ErrorCode::integrity,"Cannot finish SHA-256"};
    std::string text; text.reserve(64);
    for(auto byte:digest) { text.push_back("0123456789abcdef"[byte>>4]); text.push_back("0123456789abcdef"[byte&15]); }
    return text;
}
Status rename_sibling(HANDLE file,std::wstring_view filename,bool replace) {
    if(filename.empty() || filename.find_first_of(L"/\\:")!=filename.npos)
        return Error{ErrorCode::unsafe_path,"Rename requires a sibling filename"};
    if(auto error=uc::rename_sibling_file(file,filename,replace))return file_error("Rename sibling",DWORD(error.value()));
    return std::monostate{};
}
Status remove_owned_tree(const fs::path& root) {
    auto filename=root.filename().u8string();
    auto parent=lock_parent(root.parent_path(),std::string(filename.begin(),filename.end()));
    if(failed(parent)) return std::get<Error>(parent);
    {
        auto hold=directory(root);
        if(failed(hold)) return std::get<Error>(hold);
        std::error_code ec;
        fs::directory_iterator entries(root,ec);
        if(ec) return Error{ErrorCode::io,"Cannot enumerate staging cleanup: "+ec.message()};
        for(const auto& entry:entries) {
            auto attr=GetFileAttributesW(entry.path().c_str());
            if(attr==INVALID_FILE_ATTRIBUTES) return file_error("Inspect staging cleanup");
            if(attr&FILE_ATTRIBUTE_DIRECTORY) {
                if(attr&FILE_ATTRIBUTE_REPARSE_POINT) {
                    if(!RemoveDirectoryW(entry.path().c_str())) return file_error("Remove staging link");
                } else {
                    auto status=remove_owned_tree(entry.path());
                    if(failed(status)) return status;
                }
            } else if(!DeleteFileW(entry.path().c_str())) return file_error("Remove staging file");
        }
    }
    if(!RemoveDirectoryW(root.c_str())) return file_error("Remove staging directory");
    return std::monostate{};
}
}
