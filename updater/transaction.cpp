#include "transaction.h"
#include "files.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
#include <set>

namespace uc::update {
namespace detail { Status checkpoint(std::string_view name); }
namespace {
using Json=nlohmann::json;
constexpr size_t max_journal_size=64u<<20;
std::string path_text(const fs::path& path) {auto text=path.generic_u8string();return {text.begin(),text.end()};}
Error recovery_error(std::string text) {return {ErrorCode::recovery_required,std::move(text)};}
bool equal_path(const fs::path& a,const fs::path& b) {
    auto x=fs::absolute(a).lexically_normal().wstring(), y=fs::absolute(b).lexically_normal().wstring();
    return CompareStringOrdinal(x.c_str(),-1,y.c_str(),-1,TRUE)==CSTR_EQUAL;
}
Status operation_path(const fs::path& install,const fs::path& operation) {
    const auto id=path_text(operation.filename());
    if(id.size()!=36 || !equal_path(operation.parent_path(),install/".uc-update"))
        return recovery_error("Operation is outside this installation");
    for(size_t i=0;i<id.size();++i) {
        if(i==8 || i==13 || i==18 || i==23) {if(id[i]!='-') return recovery_error("Invalid operation ID");}
        else if(!((id[i]>='0' && id[i]<='9') || (id[i]>='a' && id[i]<='f') || (id[i]>='A' && id[i]<='F')))
            return recovery_error("Invalid operation ID");
    }
    auto lock=lock_parent(operation,"journal.json");
    if(failed(lock)) return std::get<Error>(lock);
    return std::monostate{};
}
Result<std::optional<FileEntry>> snapshot(const fs::path& root,const std::string& relative) {
    auto safe=validate_target_path(root,relative);
    if(failed(safe)) return std::get<Error>(safe);
    auto path=root/utf8_path(relative);
    auto attrs=GetFileAttributesW(path.c_str());
    if(attrs==INVALID_FILE_ATTRIBUTES) {
        if(GetLastError()==ERROR_FILE_NOT_FOUND || GetLastError()==ERROR_PATH_NOT_FOUND) return std::optional<FileEntry>{};
        return file_error("Inspect installed file");
    }
    auto opened=open_read(path);
    if(failed(opened)) return std::get<Error>(opened);
    auto& handle=std::get<Handle>(opened);
    LARGE_INTEGER size{};
    if(!GetFileSizeEx(handle.get(),&size)) return file_error("Read installed size");
    if(size.QuadPart<0 || static_cast<uint64_t>(size.QuadPart)>max_zip_size)
        return Error{ErrorCode::integrity,"Installed file exceeds backup limit"};
    auto hash=sha256_handle(handle.get());
    if(failed(hash)) return std::get<Error>(hash);
    return std::optional<FileEntry>{FileEntry{relative,static_cast<uint64_t>(size.QuadPart),std::get<std::string>(hash)}};
}
Status write_all(HANDLE file,std::string_view text) {
    size_t offset=0;
    while(offset<text.size()) {
        DWORD wrote=0,count=static_cast<DWORD>(std::min<size_t>(text.size()-offset,65536));
        if(!WriteFile(file,text.data()+offset,count,&wrote,nullptr) || !wrote) return file_error("Write journal");
        offset+=wrote;
    }
    if(!FlushFileBuffers(file)) return file_error("Flush journal");
    return std::monostate{};
}
Status append_event(const fs::path& operation,Json event) {
    auto lock=lock_parent(operation,"journal.log");
    if(failed(lock)) return std::get<Error>(lock);
    Handle file(CreateFileW((operation/"journal.log").c_str(),FILE_APPEND_DATA,FILE_SHARE_READ,nullptr,
        OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_WRITE_THROUGH,nullptr));
    if(!file.valid()) return file_error("Open journal events");
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if(!GetFileInformationByHandleEx(file.get(),FileAttributeTagInfo,&attributes,sizeof(attributes)) ||
        (attributes.FileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)))
        return recovery_error("Unsafe journal events file");
    return write_all(file.get(),event.dump()+"\n");
}
Status write_header(const fs::path& operation,const Json& header) {
    auto lock=lock_parent(operation,"journal.json");
    if(failed(lock)) return std::get<Error>(lock);
    Handle file(CreateFileW((operation/"journal.json.new").c_str(),GENERIC_WRITE|DELETE,0,nullptr,
        CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_WRITE_THROUGH,nullptr));
    if(!file.valid()) return file_error("Create journal header");
    auto written=write_all(file.get(),header.dump());
    if(failed(written)) return written;
    return rename_sibling(file.get(),L"journal.json");
}
Result<Handle> copy_verified(const fs::path& source,const fs::path& destination,const FileEntry& expected) {
    auto input=open_read(source);
    if(failed(input)) return std::get<Error>(input);
    auto name=path_text(destination.filename());
    auto parent=lock_parent(destination.parent_path(),name);
    if(failed(parent)) return std::get<Error>(parent);
    Handle output(CreateFileW(destination.c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,0,nullptr,CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_WRITE_THROUGH,nullptr));
    if(!output.valid()) return file_error("Create transaction copy");
    std::array<char,65536> bytes{};uint64_t copied=0;
    for(;;) {
        DWORD got=0;
        if(!ReadFile(std::get<Handle>(input).get(),bytes.data(),static_cast<DWORD>(bytes.size()),&got,nullptr))
            return file_error("Read transaction copy");
        if(!got) break;
        if(copied>expected.size || got>expected.size-copied)
            return Error{ErrorCode::integrity,"Transaction source grew"};
        DWORD offset=0;
        while(offset<got) {
            DWORD wrote=0;
            if(!WriteFile(output.get(),bytes.data()+offset,got-offset,&wrote,nullptr) || !wrote)
                return file_error("Write transaction copy");
            offset+=wrote;
        }
        copied+=got;
        auto point=detail::checkpoint("copy_chunk");if(failed(point)) return std::get<Error>(point);
    }
    if(!FlushFileBuffers(output.get())) return file_error("Flush transaction copy");
    auto hash=sha256_handle(output.get());
    if(failed(hash)) return std::get<Error>(hash);
    if(copied!=expected.size || std::get<std::string>(hash)!=expected.sha256)
        return Error{ErrorCode::integrity,"Transaction source changed"};
    return output;
}
std::string temporary_name(const fs::path& operation,size_t index) {
    return ".uc-new-"+path_text(operation.filename())+"-"+std::to_string(index);
}
Status remove_file(const fs::path& root,std::string_view relative) {
    auto parent=lock_parent(root,relative);
    if(failed(parent)) {
        auto attr=GetFileAttributesW((root/utf8_path(relative)).c_str());
        if(attr==INVALID_FILE_ATTRIBUTES && (GetLastError()==ERROR_PATH_NOT_FOUND || GetLastError()==ERROR_FILE_NOT_FOUND))
            return std::monostate{};
        return std::get<Error>(parent);
    }
    auto target=std::get<ParentLock>(parent).target;
    if(!DeleteFileW(target.c_str()) && GetLastError()!=ERROR_FILE_NOT_FOUND)
        return file_error("Delete transaction file");
    return std::monostate{};
}
Json operation_json(const InstallOperation& op) {
    Json j={{"path",op.path},{"existed",op.existed},{"old_size",op.old_size},{"old_sha256",op.old_sha256},{"replacement",nullptr}};
    if(op.replacement) j["replacement"]={{"path",op.replacement->path},{"size",op.replacement->size},{"sha256",op.replacement->sha256}};
    return j;
}
struct Journal {
    InstallPlan plan;
    std::set<size_t> intents,done,restored;
    std::string phase="prepared";
    size_t valid_log_bytes=0;
    bool prepared_logged=false, torn_tail=false;
};
void cleanup_backup(const fs::path& operation) {
    if(!fs::exists(operation/"backup")) return;
    auto result=remove_owned_tree(operation/"backup");
    if(failed(result)) std::fprintf(stderr,"[update] backup cleanup deferred: %s\n",std::get<Error>(result).detail.c_str());
}
Result<Journal> load_journal(const fs::path& install,const fs::path& operation) {
    auto safe=operation_path(install,operation);
    if(failed(safe)) return std::get<Error>(safe);
    auto text=read_text(operation/"journal.json",max_journal_size);
    if(failed(text)) return std::get<Error>(text);
    try {
        auto header=Json::parse(std::get<std::string>(text));
        if(header.at("format")!=1 || !equal_path(utf8_path(header.at("install").get<std::string>()),install) ||
            header.at("id")!=path_text(operation.filename()) || !header.at("operations").is_array() ||
            header.at("operations").size()>max_entries*2+1)
            return recovery_error("Invalid journal identity or operation count");
        auto old=parse_manifest(header.at("old_manifest").dump()), next=parse_manifest(header.at("new_manifest").dump());
        if(failed(old) || failed(next)) return recovery_error("Invalid journal manifests");
        Journal journal;
        journal.plan={install,operation,{operation/"stage",std::get<Manifest>(next)},std::get<Manifest>(old),{}};
        std::map<std::wstring,FileEntry> old_files,new_files;
        for(const auto& file:journal.plan.old_manifest.files) old_files.emplace(std::get<std::wstring>(path_key(file.path)),file);
        for(const auto& file:journal.plan.package.manifest.files) new_files.emplace(std::get<std::wstring>(path_key(file.path)),file);
        std::set<std::wstring> paths;
        for(const auto& j:header.at("operations")) {
            InstallOperation op;op.path=j.at("path").get<std::string>();
            auto safe_path=op.path=="release.json"?Status{std::monostate{}}:validate_managed_path(op.path);
            if(failed(safe_path)) return recovery_error("Unsafe journal path");
            auto key=std::get<std::wstring>(path_key(op.path));
            if(!paths.insert(key).second) return recovery_error("Duplicate journal operation");
            op.existed=j.at("existed").get<bool>();op.old_size=j.at("old_size").get<uint64_t>();op.old_sha256=j.at("old_sha256").get<std::string>();
            auto valid_hash=[](const std::string& hash) {return hash.size()==64 && std::all_of(hash.begin(),hash.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});};
            if(op.old_size>max_zip_size || (op.existed && (!valid_hash(op.old_sha256) ||
                (op.path!="release.json" && !old_files.contains(key)))))
                return recovery_error("Invalid original file ownership");
            if(!j.at("replacement").is_null()) {
                const auto& r=j.at("replacement");
                op.replacement=FileEntry{r.at("path").get<std::string>(),r.at("size").get<uint64_t>(),r.at("sha256").get<std::string>()};
                if(op.replacement->path!=op.path || op.replacement->size>max_zip_size || !valid_hash(op.replacement->sha256))
                    return recovery_error("Invalid replacement file");
                if(op.path!="release.json") {
                    auto found=new_files.find(key);
                    if(found==new_files.end() || found->second.size!=op.replacement->size || found->second.sha256!=op.replacement->sha256)
                        return recovery_error("Replacement differs from new manifest");
                }
            } else if(op.path=="release.json" || !old_files.contains(key) || new_files.contains(key))
                return recovery_error("Invalid removed file ownership");
            journal.plan.operations.push_back(std::move(op));
        }
        if(journal.plan.operations.empty() || journal.plan.operations.back().path!="release.json")
            return recovery_error("Missing final metadata operation");
        if(!fs::exists(operation/"journal.log")) return journal; // No intent can precede the durable log.
        auto events=read_text(operation/"journal.log",max_journal_size);
        if(failed(events)) return std::get<Error>(events);
        auto header_hash=sha256_file(operation/"journal.json");
        if(failed(header_hash)) return std::get<Error>(header_hash);
        size_t start=0;bool prepared=false;
        const auto& lines=std::get<std::string>(events);
        while(start<lines.size()) {
            auto end=lines.find('\n',start);
            if(end==lines.npos) break; // A torn final append was never followed by a file mutation.
            auto event=Json::parse(lines.substr(start,end-start));start=end+1;
            auto kind=event.at("event").get<std::string>();
            if(!prepared) {
                if(kind!="prepared" || event.at("header_sha256")!=std::get<std::string>(header_hash))
                    return recovery_error("Journal header checksum mismatch");
                prepared=true;continue;
            }
            if(journal.phase=="committed" || journal.phase=="rolled_back")
                return recovery_error("Events after final transaction state");
            if(kind=="intent" || kind=="done" || kind=="restored") {
                auto index=event.at("index").get<size_t>();
                if(index>=journal.plan.operations.size()) return recovery_error("Invalid journal index");
                if(kind=="intent") {
                    if(journal.phase!="prepared" && journal.phase!="applying") return recovery_error("Late install intent");
                    if(!journal.intents.insert(index).second) return recovery_error("Duplicate install intent");
                    journal.phase="applying";
                } else if(kind=="done") {
                    if(!journal.intents.contains(index) || !journal.done.insert(index).second) return recovery_error("Invalid completed operation");
                } else {
                    if(journal.phase!="rolling_back" || !journal.intents.contains(index) ||
                        !journal.restored.insert(index).second) return recovery_error("Invalid rollback event");
                }
            } else if(kind=="awaiting_startup") {
                if(journal.done.size()!=journal.plan.operations.size()) return recovery_error("Incomplete installation");
                journal.phase=kind;
            } else if(kind=="committed") {
                if(journal.phase!="awaiting_startup") return recovery_error("Premature commit");
                journal.phase=kind;
            } else if(kind=="rolling_back") journal.phase=kind;
            else if(kind=="rolled_back") {
                if(journal.restored.size()!=journal.intents.size()) return recovery_error("Incomplete rollback");
                journal.phase=kind;
            } else return recovery_error("Unknown journal event");
        }
        journal.valid_log_bytes=start;
        journal.prepared_logged=prepared;
        journal.torn_tail=start<lines.size();
        return journal;
    } catch(const Json::exception& e) {
        return recovery_error(std::string("Malformed journal: ")+e.what());
    }
}
}
Result<InstallPlan> plan_install(const fs::path& install,const Package& package) {
    auto root=fs::absolute(install).lexically_normal(),operation=package.root.parent_path();
    auto safe=operation_path(root,operation);
    if(failed(safe)) return std::get<Error>(safe);
    if(!equal_path(package.root,operation/"stage")) return recovery_error("Invalid package staging location");
    auto verified=verify_package(package);
    if(failed(verified)) return std::get<Error>(verified);
    auto text=read_text(root/"release.json",max_manifest_size);
    if(failed(text)) return std::get<Error>(text);
    auto parsed=parse_manifest(std::get<std::string>(text));
    if(failed(parsed)) return std::get<Error>(parsed);
    auto previous=std::get<Manifest>(parsed);
    if(package.manifest.version<=previous.version) return Error{ErrorCode::invalid_version,"Release is not newer"};
    InstallPlan plan{root,operation,package,previous,{}};
    std::map<std::wstring,std::string> paths;
    std::map<std::wstring,FileEntry> old_files,new_files;
    for(const auto& file:previous.files) {auto key=std::get<std::wstring>(path_key(file.path));old_files[key]=file;paths[key]=file.path;}
    for(const auto& file:package.manifest.files) {auto key=std::get<std::wstring>(path_key(file.path));new_files[key]=file;paths[key]=file.path;}
    for(const auto& [key,path]:paths) {
        auto actual=snapshot(root,path);
        if(failed(actual)) return std::get<Error>(actual);
        auto old=std::get<std::optional<FileEntry>>(actual);
        auto next=new_files.find(key);
        // An unowned file already byte-identical to the new release (copied by hand, left by a dev deploy) is adopted: nothing to lose.
        const bool identical=old && next!=new_files.end() && old->sha256==next->second.sha256 && old->size==next->second.size;
        if(old && !old_files.contains(key) && !identical) return Error{ErrorCode::unsafe_path,"New release collides with an unowned file: "+path};
        if(next==new_files.end() && !old) continue;
        if(next!=new_files.end() && old && old->sha256==next->second.sha256 && old->size==next->second.size) continue;
        plan.operations.push_back({path,old.has_value(),old?old->size:0,old?old->sha256:"",
            next==new_files.end()?std::optional<FileEntry>{}:std::optional<FileEntry>{next->second}});
    }
    auto old_meta=snapshot(root,"release.json"),new_meta=snapshot(package.root,"release.json");
    if(failed(old_meta)) return std::get<Error>(old_meta);
    if(failed(new_meta)) return std::get<Error>(new_meta);
    auto old=std::get<std::optional<FileEntry>>(old_meta),next=std::get<std::optional<FileEntry>>(new_meta);
    if(!old || !next) return Error{ErrorCode::invalid_manifest,"Missing release metadata"};
    plan.operations.push_back({"release.json",true,old->size,old->sha256,next});
    return plan;
}
Status apply_install(const InstallPlan& plan) {
    auto safe=operation_path(plan.install_root,plan.operation_root);
    if(failed(safe)) return safe;
    auto verified=verify_package(plan.package);
    if(failed(verified)) return verified;
    auto fresh=plan_install(plan.install_root,plan.package);
    if(failed(fresh)) return std::get<Error>(fresh);
    if(std::get<InstallPlan>(fresh).operations.size()!=plan.operations.size())
        return Error{ErrorCode::integrity,"Installation changed after planning"};
    uint64_t required=1u<<20;
    for(size_t i=0;i<plan.operations.size();++i) {
        const auto& op=plan.operations[i];
        if(operation_json(op)!=operation_json(std::get<InstallPlan>(fresh).operations[i]))
            return Error{ErrorCode::integrity,"Installed file changed"};
        const auto temporary=(plan.install_root/utf8_path(op.path)).parent_path()/temporary_name(plan.operation_root,i);
        if(GetFileAttributesW(temporary.c_str())!=INVALID_FILE_ATTRIBUTES)
            return Error{ErrorCode::busy,"Transaction temporary file already exists"};
        if(GetLastError()!=ERROR_FILE_NOT_FOUND && GetLastError()!=ERROR_PATH_NOT_FOUND)
            return file_error("Inspect transaction temporary file");
        required+=op.old_size+(op.replacement?op.replacement->size:0);
        if(op.existed) {
            auto parent=lock_parent(plan.install_root,op.path);
            if(failed(parent)) return std::get<Error>(parent);
            Handle check(CreateFileW(std::get<ParentLock>(parent).target.c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,
                FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
            if(!check.valid()) return file_error("Installed file is locked or not writable");
        }
    }
    ULARGE_INTEGER free{};
    if(!GetDiskFreeSpaceExW(plan.install_root.c_str(),&free,nullptr,nullptr)) return file_error("Inspect free space");
    if(free.QuadPart<required) return Error{ErrorCode::no_space,"Insufficient space for backup and replacement"};
    if(fs::exists(plan.operation_root/"journal.json") || fs::exists(plan.operation_root/"journal.log"))
        return recovery_error("Operation already has a journal");
    auto backup_parent=lock_parent(plan.operation_root,"backup");
    if(failed(backup_parent)) return std::get<Error>(backup_parent);
    if(!CreateDirectoryW((plan.operation_root/"backup").c_str(),nullptr)) return file_error("Create backup directory");
    for(size_t i=0;i<plan.operations.size();++i) {
        const auto& op=plan.operations[i];
        if(!op.existed) continue;
        auto copied=copy_verified(plan.install_root/utf8_path(op.path),plan.operation_root/"backup"/std::to_string(i),
            FileEntry{op.path,op.old_size,op.old_sha256});
        if(failed(copied)) return std::get<Error>(copied);
    }
    auto old_text=read_text(plan.install_root/"release.json",max_manifest_size);
    auto new_text=read_text(plan.package.root/"release.json",max_manifest_size);
    if(failed(old_text)) return std::get<Error>(old_text);
    if(failed(new_text)) return std::get<Error>(new_text);
    Json header={{"format",1},{"install",path_text(plan.install_root)},{"id",path_text(plan.operation_root.filename())},
        {"old_manifest",Json::parse(std::get<std::string>(old_text))},{"new_manifest",Json::parse(std::get<std::string>(new_text))},
        {"operations",Json::array()}};
    for(const auto& op:plan.operations) header["operations"].push_back(operation_json(op));
    auto status=write_header(plan.operation_root,header);
    if(failed(status)) return status;
    status=detail::checkpoint("header");if(failed(status)) return status;
    auto hash=sha256_file(plan.operation_root/"journal.json");
    if(failed(hash)) return std::get<Error>(hash);
    status=append_event(plan.operation_root,{{"event","prepared"},{"header_sha256",std::get<std::string>(hash)}});
    if(failed(status)) return status;
    status=detail::checkpoint("prepared");if(failed(status)) return status;
    for(size_t i=0;i<plan.operations.size();++i) {
        const auto& op=plan.operations[i];
        status=append_event(plan.operation_root,{{"event","intent"},{"index",i}});if(failed(status)) return status;
        status=detail::checkpoint("intent");if(failed(status)) return status;
        auto parent=lock_parent(plan.install_root,op.path,true);
        if(failed(parent)) return std::get<Error>(parent);
        auto target=std::get<ParentLock>(parent).target;
        status=detail::checkpoint("before_replace");if(failed(status)) return status;
        if(op.replacement) {
            auto temporary=target.parent_path()/temporary_name(plan.operation_root,i);
            auto copied=copy_verified(plan.package.root/utf8_path(op.path),temporary,*op.replacement);
            if(failed(copied)) return std::get<Error>(copied);
            status=detail::checkpoint("copied");if(failed(status)) return status;
            status=rename_sibling(std::get<Handle>(copied).get(),target.filename().wstring(),op.existed);
            if(failed(status)) return status;
        } else {
            status=remove_file(plan.install_root,op.path);
            if(failed(status)) return status;
        }
        status=detail::checkpoint("replaced");if(failed(status)) return status;
        status=append_event(plan.operation_root,{{"event","done"},{"index",i}});if(failed(status)) return status;
        status=detail::checkpoint("done");if(failed(status)) return status;
    }
    status=append_event(plan.operation_root,{{"event","awaiting_startup"}});
    if(failed(status)) return status;
    return detail::checkpoint("awaiting_startup");
}
Status finish_install(const fs::path& operation) {
    auto state=load_journal(operation.parent_path().parent_path(),operation);
    if(failed(state)) return std::get<Error>(state);
    if(std::get<Journal>(state).phase=="committed") return std::monostate{};
    if(std::get<Journal>(state).phase!="awaiting_startup") return recovery_error("Installation is not ready to commit");
    auto status=append_event(operation,{{"event","committed"}});
    if(!failed(status)) cleanup_backup(operation);
    return status;
}
Result<bool> pending_install(const fs::path& install,const fs::path& operation) {
    auto safe=operation_path(install,operation);if(failed(safe)) return std::get<Error>(safe);
    if(!fs::exists(operation/"journal.json")) return false;
    auto parsed=load_journal(install,operation);if(failed(parsed)) return std::get<Error>(parsed);
    const auto& phase=std::get<Journal>(parsed).phase;
    return phase!="committed" && phase!="rolled_back";
}
Status recover_install(const fs::path& install,const fs::path& operation) {
    auto safe=operation_path(install,operation);
    if(failed(safe)) return safe;
    if(!fs::exists(operation/"journal.json")) return std::monostate{}; // No installation mutation precedes the header.
    auto parsed=load_journal(install,operation);
    if(failed(parsed)) return std::get<Error>(parsed);
    auto journal=std::get<Journal>(std::move(parsed));
    if(journal.phase=="committed" || journal.phase=="rolled_back") {cleanup_backup(operation);return std::monostate{};}
    auto status=Status{std::monostate{}};
    if(journal.torn_tail) {
        auto parent=lock_parent(operation,"journal.log");
        if(failed(parent)) return std::get<Error>(parent);
        Handle log(CreateFileW((operation/"journal.log").c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(!log.valid()) return file_error("Repair interrupted journal append");
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if(!GetFileInformationByHandleEx(log.get(),FileAttributeTagInfo,&attributes,sizeof(attributes)) ||
            (attributes.FileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)))
            return recovery_error("Unsafe interrupted journal");
        LARGE_INTEGER end{};end.QuadPart=static_cast<LONGLONG>(journal.valid_log_bytes);
        if(!SetFilePointerEx(log.get(),end,nullptr,FILE_BEGIN) || !SetEndOfFile(log.get()) || !FlushFileBuffers(log.get()))
            return file_error("Truncate interrupted journal append");
    }
    if(!journal.prepared_logged) {
        auto hash=sha256_file(operation/"journal.json");
        if(failed(hash)) return std::get<Error>(hash);
        status=append_event(operation,{{"event","prepared"},{"header_sha256",std::get<std::string>(hash)}});
        if(failed(status)) return status;
    }
    if(journal.phase!="rolling_back") {
        status=append_event(operation,{{"event","rolling_back"}});
        if(failed(status)) return status;
    }
    status=detail::checkpoint("rollback_started");if(failed(status)) return status;
    for(size_t reverse=journal.plan.operations.size();reverse>0;--reverse) {
        auto i=reverse-1;const auto& op=journal.plan.operations[i];
        if(!journal.intents.contains(i) || journal.restored.contains(i)) continue;
        status=detail::checkpoint("before_restore");if(failed(status)) return status;
        auto actual=snapshot(install,op.path);
        if(failed(actual)) return std::get<Error>(actual);
        auto current=std::get<std::optional<FileEntry>>(actual);
        bool original=current && op.existed && current->sha256==op.old_sha256 && current->size==op.old_size;
        bool replacement=current && op.replacement && current->sha256==op.replacement->sha256 && current->size==op.replacement->size;
        if(current && !original && !replacement) return recovery_error("File changed outside the update; backup retained: "+op.path);
        fs::path temporary;
        for(unsigned attempt=0;attempt<1024;++attempt) {
            auto name=temporary_name(operation,i)+(attempt?"-"+std::to_string(attempt):"");
            auto relative=path_text(utf8_path(op.path).parent_path()/name);
            auto contents=snapshot(install,relative);if(failed(contents)) return std::get<Error>(contents);
            const auto& file=std::get<std::optional<FileEntry>>(contents);
            if(file) {
                bool ours=(op.existed && file->size==op.old_size && file->sha256==op.old_sha256) ||
                    (op.replacement && file->size==op.replacement->size && file->sha256==op.replacement->sha256);
                if(!ours) continue; // Preserve ambiguous partial copies and files created after the crash.
                status=remove_file(install,relative);if(failed(status)) return status;
            }
            temporary=install/utf8_path(relative);break;
        }
        if(temporary.empty()) return recovery_error("Too many occupied recovery temporary paths; backup retained");
        if(op.existed && !original) {
            auto parent=lock_parent(install,op.path,true);
            if(failed(parent)) return std::get<Error>(parent);
            auto target=std::get<ParentLock>(parent).target;
            auto restored=copy_verified(operation/"backup"/std::to_string(i),temporary,
                FileEntry{op.path,op.old_size,op.old_sha256});
            if(failed(restored)) return std::get<Error>(restored);
            status=rename_sibling(std::get<Handle>(restored).get(),target.filename().wstring(),current.has_value());
            if(failed(status)) return status;
        } else if(!op.existed && current) {
            status=remove_file(install,op.path);if(failed(status)) return status;
        }
        status=detail::checkpoint("after_restore");if(failed(status)) return status;
        status=append_event(operation,{{"event","restored"},{"index",i}});
        if(failed(status)) return status;
        status=detail::checkpoint("restore_logged");if(failed(status)) return status;
    }
    status=append_event(operation,{{"event","rolled_back"}});
    if(!failed(status)) cleanup_backup(operation);
    return status;
}
}
