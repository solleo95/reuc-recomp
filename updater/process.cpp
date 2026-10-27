#include "update_process.h"
#include "windows_args.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <thread>

namespace uc::update {
namespace {
Error lock_error(const std::error_code& error) {
    return file_error("Client update coordination",static_cast<DWORD>(error.value()));
}
std::string text(const fs::path& path) {auto s=path.generic_u8string();return {s.begin(),s.end()};}
Result<bool> cleanup_ready(const fs::path& operation) {
    auto attributes=GetFileAttributesW((operation/"cleanup-ready").c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES) {
        auto code=GetLastError();
        if(code==ERROR_FILE_NOT_FOUND || code==ERROR_PATH_NOT_FOUND) return false;
        return file_error("Inspect update cleanup marker",code);
    }
    auto marker=read_text(operation/"cleanup-ready",37);if(failed(marker)) return std::get<Error>(marker);
    if(std::get<std::string>(marker)!=text(operation.filename())+"\n")
        return Error{ErrorCode::recovery_required,"Invalid update cleanup marker"};
    return true;
}
Status remove_cleanup_file(const fs::path& path) {
    auto attributes=GetFileAttributesW(path.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES) {
        auto code=GetLastError();
        if(code==ERROR_FILE_NOT_FOUND) return std::monostate{};
        return file_error("Inspect update cleanup file",code);
    }
    {auto ordinary=open_read(path);if(failed(ordinary)) return std::get<Error>(ordinary);}
    if(!DeleteFileW(path.c_str())) return file_error("Remove update cleanup file");
    return std::monostate{};
}
Status mark_cleanup_ready(const fs::path& operation) {
    auto pending=pending_install(operation.parent_path().parent_path(),operation);
    if(failed(pending)) return std::get<Error>(pending);
    if(std::get<bool>(pending)) return Error{ErrorCode::recovery_required,"Pending update cannot be cleaned"};
    auto safe=lock_parent(operation,"cleanup-ready");if(failed(safe)) return std::get<Error>(safe);
    auto existing=cleanup_ready(operation);if(failed(existing)) return std::get<Error>(existing);
    if(std::get<bool>(existing)) return std::monostate{};
    auto cleared=remove_cleanup_file(operation/"cleanup-ready.new");if(failed(cleared)) return cleared;
    Handle marker(CreateFileW((operation/"cleanup-ready.new").c_str(),GENERIC_WRITE|DELETE,0,nullptr,CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_WRITE_THROUGH,nullptr));
    if(!marker.valid()) return file_error("Create update cleanup marker");
    auto value=text(operation.filename())+"\n";DWORD written=0;
    if(!WriteFile(marker.get(),value.data(),static_cast<DWORD>(value.size()),&written,nullptr) || written!=value.size())
        return file_error("Write update cleanup marker");
    if(!FlushFileBuffers(marker.get())) return file_error("Flush update cleanup marker");
    return rename_sibling(marker.get(),L"cleanup-ready");
}
void defer_cleanup(const fs::path& operation) {
    auto marked=mark_cleanup_ready(operation);
    if(failed(marked)) std::fprintf(stderr,"[update] Cleanup marker deferred at %s: %s\n",
        text(operation).c_str(),std::get<Error>(marked).detail.c_str());
}
Status cleanup_operation(const fs::path& operation) {
    auto marked=mark_cleanup_ready(operation);if(failed(marked)) return marked;
    {
        auto safe=lock_parent(operation,"cleanup-ready");if(failed(safe)) return std::get<Error>(safe);
        for(const auto& entry:fs::directory_iterator(operation)) {
            auto name=entry.path().filename().wstring();
            if(CompareStringOrdinal(name.c_str(),-1,L"cleanup-ready",-1,TRUE)==CSTR_EQUAL ||
                CompareStringOrdinal(name.c_str(),-1,L"journal.json",-1,TRUE)==CSTR_EQUAL ||
                CompareStringOrdinal(name.c_str(),-1,L"journal.log",-1,TRUE)==CSTR_EQUAL) continue;
            auto attributes=GetFileAttributesW(entry.path().c_str());
            if(attributes==INVALID_FILE_ATTRIBUTES) return file_error("Inspect update cleanup entry");
            if(attributes&FILE_ATTRIBUTE_DIRECTORY) {
                auto removed=remove_owned_tree(entry.path());if(failed(removed)) return removed;
            } else {
                auto removed=remove_cleanup_file(entry.path());if(failed(removed)) return removed;
            }
        }
        // Retain the complete terminal journal until payload cleanup succeeds.
        for(auto name:{"journal.json","journal.log","cleanup-ready"}) {
            auto removed=remove_cleanup_file(operation/name);if(failed(removed)) return removed;
        }
    }
    if(!RemoveDirectoryW(operation.c_str())) {
        auto error=file_error("Remove update cleanup directory");defer_cleanup(operation);return error;
    }
    return std::monostate{};
}
Result<Manifest> installed_manifest(const fs::path& root) {
    auto json=read_text(root/"release.json",max_manifest_size);
    if(failed(json)) return std::get<Error>(json);
    return parse_manifest(std::get<std::string>(json));
}
Result<Handle> inherited_event() {
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
    Handle event(CreateEventW(&sa,TRUE,FALSE,nullptr));
    if(!event.valid()) return file_error("Create update event");return event;
}
Result<Handle> inherited_self() {
    HANDLE duplicate{};
    if(!DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),GetCurrentProcess(),&duplicate,
        SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,TRUE,0)) return file_error("Duplicate process handle");
    return Handle(duplicate);
}
Status wait_event(HANDLE event,HANDLE process,std::chrono::milliseconds timeout) {
    std::array<HANDLE,2> handles{event,process};
    auto result=WaitForMultipleObjects(2,handles.data(),FALSE,static_cast<DWORD>(timeout.count()));
    if(result==WAIT_OBJECT_0) return std::monostate{};
    if(result==WAIT_FAILED) return file_error("Wait for update handshake");
    return Error{ErrorCode::startup,result==WAIT_TIMEOUT?"Update handshake timed out":"Update process exited before confirmation"};
}
Status stop_owned(HANDLE process) {
    if(WaitForSingleObject(process,0)==WAIT_OBJECT_0) return std::monostate{};
    if(!TerminateProcess(process,2)) return file_error("Stop failed update child");
    if(WaitForSingleObject(process,10000)!=WAIT_OBJECT_0) return Error{ErrorCode::recovery_required,"Update child did not stop; backup retained"};
    return std::monostate{};
}
Status verify_recovery(const fs::path& recovery,const Manifest& manifest) {
    for(const auto& path:manifest.helper_files) {
        auto entry=std::find_if(manifest.files.begin(),manifest.files.end(),[&](const FileEntry& f){return f.path==path;});
        if(entry==manifest.files.end()) return Error{ErrorCode::invalid_manifest,"Missing helper file metadata"};
        auto input=open_read(recovery/utf8_path(path));if(failed(input)) return std::get<Error>(input);
        LARGE_INTEGER size{};
        if(!GetFileSizeEx(std::get<Handle>(input).get(),&size)) return file_error("Read helper file size");
        auto hash=sha256_handle(std::get<Handle>(input).get());if(failed(hash)) return std::get<Error>(hash);
        if(size.QuadPart<0 || static_cast<uint64_t>(size.QuadPart)!=entry->size || std::get<std::string>(hash)!=entry->sha256)
            return Error{ErrorCode::integrity,"Installed helper file changed: "+path};
    }
    return std::monostate{};
}
// The helper comes from the verified package, so edits to the installed copy (antivirus, hand-copied builds) cannot block an update.
Status copy_recovery(const fs::path& operation) {
    const auto stage=operation/"stage";
    auto parsed=installed_manifest(stage);if(failed(parsed)) return std::get<Error>(parsed);
    auto manifest=std::get<Manifest>(std::move(parsed));
    auto check=verify_recovery(stage,manifest);if(failed(check)) return check;
    auto recovery=operation/"recovery";
    auto parent=lock_parent(operation,"recovery");if(failed(parent)) return std::get<Error>(parent);
    if(!CreateDirectoryW(recovery.c_str(),nullptr)) return file_error("Create recovery directory");
    auto paths=manifest.helper_files;paths.push_back("release.json");
    for(const auto& path:paths) {
        auto source_parent=lock_parent(stage,path);if(failed(source_parent)) return std::get<Error>(source_parent);
        auto destination_parent=lock_parent(recovery,path);if(failed(destination_parent)) return std::get<Error>(destination_parent);
        if(!CopyFileW((stage/utf8_path(path)).c_str(),(recovery/utf8_path(path)).c_str(),TRUE)) return file_error("Copy recovery file");
    }
    return verify_recovery(recovery,manifest);
}
Result<HelperProcess> start_helper(const fs::path& operation,bool recover,std::chrono::milliseconds timeout=std::chrono::seconds(30)) {
    auto parent=inherited_self();if(failed(parent)) {if(!recover) defer_cleanup(operation);return std::get<Error>(parent);}
    auto ready=inherited_event();if(failed(ready)) {if(!recover) defer_cleanup(operation);return std::get<Error>(ready);}
    auto p=std::get<Handle>(parent).get(),r=std::get<Handle>(ready).get();
    std::array<std::wstring,4> args{recover?L"--recover":L"--apply",operation.wstring(),
        std::to_wstring(reinterpret_cast<uintptr_t>(p)),std::to_wstring(reinterpret_cast<uintptr_t>(r))};
    std::array<HANDLE,2> handles{p,r};
    auto child=create_child(operation/"recovery"/"uc_updater.exe",operation/"recovery",args,handles);
    if(failed(child)) {if(!recover) defer_cleanup(operation);return std::get<Error>(child);}
    auto process=std::get<Handle>(std::move(child));
    auto status=wait_event(r,process.get(),timeout);
    if(failed(status)) {
        auto stopped=stop_owned(process.get());if(failed(stopped)) return std::get<Error>(stopped);
        if(!recover) defer_cleanup(operation);
        return std::get<Error>(status);
    }
    return HelperProcess{std::move(process)};
}
Status restart_launcher(const fs::path& install) {
    auto result=create_child(install/"uc_launcher.exe",install,{});
    if(failed(result)) return std::get<Error>(result);
    return std::monostate{};
}
}
Result<Handle> create_child(const fs::path& executable,const fs::path& directory,
    std::span<const std::wstring> arguments,std::span<const HANDLE> inherited) {
    auto command=uc::quote_windows_arg(executable.wstring());
    for(const auto& arg:arguments) command+=L" "+uc::quote_windows_arg(arg);
    if(command.size()>=32767) return Error{ErrorCode::startup,"Update command is too long"};
    STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);SIZE_T bytes=0;
    std::vector<unsigned char> storage;
    if(!inherited.empty()) {
        InitializeProcThreadAttributeList(nullptr,1,0,&bytes);storage.resize(bytes);
        startup.lpAttributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if(!InitializeProcThreadAttributeList(startup.lpAttributeList,1,0,&bytes)) return file_error("Initialize child handles");
        if(!UpdateProcThreadAttribute(startup.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            const_cast<HANDLE*>(inherited.data()),inherited.size_bytes(),nullptr,nullptr)) {
            auto error=file_error("Restrict child handles");DeleteProcThreadAttributeList(startup.lpAttributeList);return error;
        }
    }
    PROCESS_INFORMATION child{};
    DWORD previous_mode=0;
    if(!SetThreadErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX,&previous_mode)) {
        auto error=file_error("Set child startup error mode");
        if(startup.lpAttributeList) DeleteProcThreadAttributeList(startup.lpAttributeList);
        return error;
    }
    bool success=CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,!inherited.empty(),
        CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,nullptr,directory.c_str(),&startup.StartupInfo,&child)!=FALSE;
    auto code=GetLastError();
    SetThreadErrorMode(previous_mode,nullptr);
    if(startup.lpAttributeList) DeleteProcThreadAttributeList(startup.lpAttributeList);
    if(!success) return file_error("Start update process",code);
    CloseHandle(child.hThread);return Handle(child.hProcess);
}
Result<std::optional<ResumeStartup>> parse_resume_startup(int argc,wchar_t** argv) {
    if(argc<2 || std::wstring_view(argv[1])!=L"--update-resume") return std::optional<ResumeStartup>{};
    if(argc!=7) return Error{ErrorCode::startup,"Invalid update resume arguments"};
    ResumeStartup startup;
    auto id=std::wstring_view(argv[2]);
    if(id.size()!=36 || std::any_of(id.begin(),id.end(),[](wchar_t c){return c>127;}))
        return Error{ErrorCode::startup,"Invalid update operation ID"};
    startup.ticket.operation_id.assign(id.begin(),id.end());
    std::array<Handle*,4> handles{&startup.channel,&startup.acknowledged,&startup.committed,&startup.helper};
    for(size_t i=0;i<handles.size();++i) {
        std::wstring_view value=argv[i+3];uintptr_t number=0;
        if(value.empty()) return Error{ErrorCode::startup,"Invalid inherited handle"};
        for(auto c:value) {
            if(c<L'0' || c>L'9' || number>(std::numeric_limits<uintptr_t>::max()-(c-L'0'))/10)
                return Error{ErrorCode::startup,"Invalid inherited handle"};
            number=number*10+(c-L'0');
        }
        auto h=reinterpret_cast<HANDLE>(number);DWORD flags=0;
        if(!number || !GetHandleInformation(h,&flags) || !(flags&HANDLE_FLAG_INHERIT))
            return Error{ErrorCode::startup,"Update handle was not inherited"};
        for(size_t previous=0;previous<i;++previous) if(handles[previous]->get()==h)
            return Error{ErrorCode::startup,"Duplicate update handle"};
        *handles[i]=Handle(h);
    }
    if(!GetProcessId(startup.helper.get()) || GetProcessId(startup.helper.get())==GetCurrentProcessId())
        return Error{ErrorCode::startup,"Invalid update helper process"};
    startup.ticket.channel=reinterpret_cast<uintptr_t>(startup.channel.get());
    return std::optional<ResumeStartup>{std::move(startup)};
}
Status ResumeStartup::confirm(std::chrono::milliseconds timeout) {
    if(!SetEvent(acknowledged.get())) return file_error("Confirm launcher startup");
    return wait_event(committed.get(),helper.get(),timeout);
}
Status start_and_confirm(const fs::path& install,const fs::path& operation,uc::ClientInstallGuard& guard,std::chrono::milliseconds timeout) {
    if(auto ec=guard.allow_child_start()) return lock_error(ec);
    auto issued=guard.resume_channel(text(operation.filename()));
    if(auto* ec=std::get_if<std::error_code>(&issued)) return lock_error(*ec);
    Handle channel(reinterpret_cast<HANDLE>(std::get<uintptr_t>(issued)));
    auto ack=inherited_event();if(failed(ack)) return std::get<Error>(ack);
    auto commit=inherited_event();if(failed(commit)) return std::get<Error>(commit);
    auto self=inherited_self();if(failed(self)) return std::get<Error>(self);
    std::array<HANDLE,4> handles{channel.get(),std::get<Handle>(ack).get(),std::get<Handle>(commit).get(),std::get<Handle>(self).get()};
    std::vector<std::wstring> args{L"--update-resume",operation.filename().wstring()};
    for(auto handle:handles) args.push_back(std::to_wstring(reinterpret_cast<uintptr_t>(handle)));
    auto started=create_child(install/"uc_launcher.exe",install,args,handles);
    if(failed(started)) return std::get<Error>(started);
    auto child=std::get<Handle>(std::move(started));
    auto status=wait_event(handles[1],child.get(),timeout);
    if(!failed(status) && WaitForSingleObject(child.get(),0)!=WAIT_TIMEOUT)
        status=Error{ErrorCode::startup,"Launcher exited after startup confirmation"};
    if(!failed(status)) status=finish_install(operation);
    if(!failed(status)) {
        guard.release_after_commit();
        if(!SetEvent(handles[2])) status=file_error("Release confirmed launcher");
    }
    if(failed(status)) {
        auto stopped=stop_owned(child.get());if(failed(stopped)) return stopped;
    }
    return status;
}
Result<HelperProcess> start_update_helper(const fs::path& install,const Package& package,std::chrono::milliseconds timeout) {
    auto pending=pending_install(install,package.root.parent_path());if(failed(pending)) return std::get<Error>(pending);
    if(std::get<bool>(pending)) return Error{ErrorCode::recovery_required,"An unfinished update requires recovery"};
    auto plan=plan_install(install,package);if(failed(plan)) {defer_cleanup(package.root.parent_path());return std::get<Error>(plan);}
    auto copied=copy_recovery(package.root.parent_path());
    if(failed(copied)) {defer_cleanup(package.root.parent_path());return std::get<Error>(copied);}
    return start_helper(package.root.parent_path(),false,timeout);
}
Status run_update_helper(const fs::path& operation,HANDLE parent,HANDLE ready,bool recover,std::chrono::milliseconds timeout) {
    const auto install=operation.parent_path().parent_path();
    auto reserved=uc::reserve_client_install(install);
    if(auto* ec=std::get_if<std::error_code>(&reserved)) return lock_error(*ec);
    auto guard=std::get<uc::ClientInstallGuard>(std::move(reserved));
    std::optional<InstallPlan> plan;
    auto pending=pending_install(install,operation);if(failed(pending)) return std::get<Error>(pending);
    auto unchanged_failure=[&](Error error)->Status {if(!recover) defer_cleanup(operation);return error;};
    if(!recover) {
        if(std::get<bool>(pending)) return Error{ErrorCode::recovery_required,"An unfinished update requires recovery"};
        auto manifest=installed_manifest(operation/"stage");if(failed(manifest)) return unchanged_failure(std::get<Error>(manifest));
        auto planned=plan_install(install,Package{operation/"stage",std::get<Manifest>(std::move(manifest))});
        if(failed(planned)) return unchanged_failure(std::get<Error>(planned));
        plan=std::get<InstallPlan>(std::move(planned));
        uint64_t required=1u<<20;
        for(const auto& op:plan->operations) required+=op.old_size+(op.replacement?op.replacement->size:0);
        ULARGE_INTEGER free{};
        if(!GetDiskFreeSpaceExW(install.c_str(),&free,nullptr,nullptr)) return unchanged_failure(file_error("Inspect installation free space"));
        if(free.QuadPart<required) return unchanged_failure(Error{ErrorCode::no_space,"Insufficient space for backup and replacement"});
        auto safe=lock_parent(operation,"write-probe");if(failed(safe)) return unchanged_failure(std::get<Error>(safe));
        Handle probe(CreateFileW((operation/"write-probe").c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,nullptr));
        if(!probe.valid()) return unchanged_failure(file_error("Installation directory is not writable"));
    }
    if(parent) {
        if(!GetProcessId(parent) || GetProcessId(parent)==GetCurrentProcessId())
            return unchanged_failure(Error{ErrorCode::startup,"Invalid launcher process handle"});
        if(!ready || !SetEvent(ready)) return unchanged_failure(file_error("Notify launcher of readiness"));
        if(WaitForSingleObject(parent,static_cast<DWORD>(timeout.count()))!=WAIT_OBJECT_0)
            return unchanged_failure(Error{ErrorCode::busy,"Launcher did not close; installation unchanged"});
    }
    if(auto ec=guard.wait_until_unused(timeout)) {
        if(!recover) defer_cleanup(operation);
        guard.release_after_commit();
        auto restarted=restart_launcher(install);if(failed(restarted)) return restarted;
        return Error{ErrorCode::busy,"Another launcher or game is still running; installation unchanged"};
    }
    Status status=std::monostate{};
    try {
        if(recover) status=recover_install(install,operation);
        else {
            status=apply_install(*plan);
            if(!failed(status)) status=start_and_confirm(install,operation,guard,timeout);
            if(!failed(status)) return status;
        }
    } catch(const std::exception& e) {status=Error{ErrorCode::io,std::string("Update failed: ")+e.what()};}
    if(!recover) {
        auto unfinished=pending_install(install,operation);
        if(failed(unfinished)) return std::get<Error>(unfinished);
        if(!std::get<bool>(unfinished)) {
            defer_cleanup(operation);
            // Commit is irreversible; restart the committed client if its notification failed.
            guard.release_after_commit();
            auto restarted=restart_launcher(install);return failed(restarted)?restarted:status;
        }
        if(auto ec=guard.reacquire_for_rollback(timeout)) return lock_error(ec);
        auto restored=recover_install(install,operation);
        if(failed(restored)) return Error{ErrorCode::recovery_required,std::get<Error>(restored).detail+"; recovery: "+text(operation/"recovery"/"uc_updater.exe")};
    } else if(failed(status)) return status;
    guard.release_after_commit();
    auto restarted=restart_launcher(install);if(failed(restarted)) return restarted;
    return status;
}
Result<bool> resume_pending_update(const fs::path& install) {
    if(!fs::exists(install/".uc-update")) return false;
    auto reserved=uc::reserve_client_install(install);
    if(auto* ec=std::get_if<std::error_code>(&reserved)) return lock_error(*ec);
    auto guard=std::get<uc::ClientInstallGuard>(std::move(reserved));
    for(const auto& entry:fs::directory_iterator(install/".uc-update")) {
        if(!entry.is_directory()) continue;
        auto marker=cleanup_ready(entry.path());if(failed(marker)) return std::get<Error>(marker);
        if(!std::get<bool>(marker) && !fs::exists(entry.path()/"journal.json")) continue;
        auto pending=pending_install(install,entry.path());if(failed(pending)) return std::get<Error>(pending);
        if(std::get<bool>(marker) && std::get<bool>(pending))
            return Error{ErrorCode::recovery_required,"Cleanup marker conflicts with pending update"};
        if(!std::get<bool>(pending)) {
            auto cleaned=cleanup_operation(entry.path());
            if(failed(cleaned)) std::fprintf(stderr,"[update] Cleanup deferred: %s\n",std::get<Error>(cleaned).detail.c_str());
            continue;
        }
        auto manifest=installed_manifest(entry.path()/"recovery");if(failed(manifest)) return std::get<Error>(manifest);
        auto checked=verify_recovery(entry.path()/"recovery",std::get<Manifest>(manifest));if(failed(checked)) return std::get<Error>(checked);
        guard.release_after_commit();
        auto started=start_helper(entry.path(),true);if(failed(started)) return std::get<Error>(started);
        return true;
    }
    return false;
}
}
