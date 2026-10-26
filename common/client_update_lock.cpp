#include "client_update_lock.h"
#include <windows.h>
#include <array>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>
namespace uc {
namespace {
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h=INVALID_HANDLE_VALUE):value(h) {}
    ~Handle() {if(value && value!=INVALID_HANDLE_VALUE) CloseHandle(value);}
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
    Handle(Handle&& h) noexcept:value(std::exchange(h.value,INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& h) noexcept {std::swap(value,h.value);return *this;}
    bool valid() const {return value && value!=INVALID_HANDLE_VALUE;}
};
std::error_code error(DWORD code=GetLastError()) {return {static_cast<int>(code),std::system_category()};}
bool valid_id(const std::string& id) {
    if(id.size()!=36) return false;
    for(size_t i=0;i<id.size();++i) {
        if(i==8 || i==13 || i==18 || i==23) {if(id[i]!='-') return false;}
        else if(!((id[i]>='0' && id[i]<='9') || (id[i]>='a' && id[i]<='f') || (id[i]>='A' && id[i]<='F'))) return false;
    }
    return true;
}
struct WireTicket {std::array<char,8> magic;DWORD volume,index_high,index_low;std::array<char,37> operation;};
constexpr std::array<char,8> magic={'U','C','R','E','S','U','M','1'};
}
struct ClientLockState {
    std::vector<Handle> directories;
    Handle gate,session;
    BY_HANDLE_FILE_INFORMATION identity{};
    bool exclusive=false;
};
namespace {
std::variant<std::unique_ptr<ClientLockState>,std::error_code> open_state(const std::filesystem::path& root) {
    auto state=std::make_unique<ClientLockState>();
    auto absolute=std::filesystem::absolute(root).lexically_normal();
    auto current=absolute.root_path();
    auto hold=[&](const std::filesystem::path& path)->std::error_code {
        Handle h(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(!h.valid()) return error();
        FILE_ATTRIBUTE_TAG_INFO tag{};
        if(!GetFileInformationByHandleEx(h.value,FileAttributeTagInfo,&tag,sizeof(tag))) return error();
        if(!(tag.FileAttributes&FILE_ATTRIBUTE_DIRECTORY) || (tag.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))
            return error(ERROR_REPARSE_TAG_INVALID);
        state->directories.push_back(std::move(h));return {};
    };
    if(auto ec=hold(current)) return ec;
    for(const auto& part:absolute.relative_path()) {current/=part;if(auto ec=hold(current)) return ec;}
    if(!GetFileInformationByHandle(state->directories.back().value,&state->identity)) return error();
    current/=L".uc-update";
    if(!CreateDirectoryW(current.c_str(),nullptr) && GetLastError()!=ERROR_ALREADY_EXISTS) return error();
    if(auto ec=hold(current)) return ec;
    return state;
}
std::variant<Handle,std::error_code> lock_file(const std::filesystem::path& path,DWORD sharing) {
    Handle h(CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,sharing,nullptr,OPEN_ALWAYS,
        FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(!h.valid()) return error();
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(h.value,&info)) return error();
    if((info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) || info.nNumberOfLinks!=1)
        return error(ERROR_INVALID_DATA);
    return h;
}
std::error_code take_lock(ClientLockState& state,bool exclusive) {
    OVERLAPPED overlapped{};
    if(!LockFileEx(state.session.value,LOCKFILE_FAIL_IMMEDIATELY|(exclusive?LOCKFILE_EXCLUSIVE_LOCK:0),0,1,0,&overlapped)) return error();
    state.exclusive=exclusive;return {};
}
bool ticket_matches(const ResumeTicket& ticket,const ClientLockState& state) {
    if(!valid_id(ticket.operation_id) || !ticket.channel) return false;
    HANDLE pipe=reinterpret_cast<HANDLE>(ticket.channel);
    DWORD flags=0,available=0;
    if(!GetHandleInformation(pipe,&flags) || !(flags&HANDLE_FLAG_INHERIT) || GetFileType(pipe)!=FILE_TYPE_PIPE ||
        !PeekNamedPipe(pipe,nullptr,0,nullptr,&available,nullptr) || available!=sizeof(WireTicket)) return false;
    WireTicket wire{};DWORD read=0;
    if(!ReadFile(pipe,&wire,sizeof(wire),&read,nullptr) || read!=sizeof(wire)) return false;
    return wire.magic==magic && wire.volume==state.identity.dwVolumeSerialNumber &&
        wire.index_high==state.identity.nFileIndexHigh && wire.index_low==state.identity.nFileIndexLow &&
        wire.operation.back()==0 && std::string_view(wire.operation.data(),36)==ticket.operation_id;
}
std::error_code reject_pending(const std::filesystem::path& root) {
    for(const auto& entry:std::filesystem::directory_iterator(root/L".uc-update")) {
        auto attributes=GetFileAttributesW(entry.path().c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES) return error();
        if(attributes&FILE_ATTRIBUTE_REPARSE_POINT) return error(ERROR_REPARSE_TAG_INVALID);
        if(!(attributes&FILE_ATTRIBUTE_DIRECTORY) || !std::filesystem::exists(entry.path()/L"journal.json")) continue;
        Handle log(CreateFileW((entry.path()/L"journal.log").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,
            OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(!log.valid()) return error(ERROR_RECOVERY_FAILURE);
        FILE_ATTRIBUTE_TAG_INFO tag{};
        if(!GetFileInformationByHandleEx(log.value,FileAttributeTagInfo,&tag,sizeof(tag)) ||
            (tag.FileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))) return error(ERROR_RECOVERY_FAILURE);
        LARGE_INTEGER size{};
        if(!GetFileSizeEx(log.value,&size)) return error();
        LARGE_INTEGER offset{};offset.QuadPart=std::max<LONGLONG>(0,size.QuadPart-128);
        if(!SetFilePointerEx(log.value,offset,nullptr,FILE_BEGIN)) return error();
        std::array<char,128> bytes{};DWORD got=0;
        if(!ReadFile(log.value,bytes.data(),static_cast<DWORD>(bytes.size()),&got,nullptr)) return error();
        // These terminal records are the transaction journal's durable commit/rollback boundary.
        auto tail=std::string_view(bytes.data(),got);
        if(!tail.ends_with("{\"event\":\"committed\"}\n") && !tail.ends_with("{\"event\":\"rolled_back\"}\n"))
            return error(ERROR_RECOVERY_FAILURE);
    }
    return {};
}
}
ClientSessionGuard::ClientSessionGuard(std::unique_ptr<ClientLockState> s):state_(std::move(s)) {}
ClientSessionGuard::~ClientSessionGuard()=default;
ClientSessionGuard::ClientSessionGuard(ClientSessionGuard&&) noexcept=default;
ClientSessionGuard& ClientSessionGuard::operator=(ClientSessionGuard&&) noexcept=default;
ClientInstallGuard::ClientInstallGuard(std::unique_ptr<ClientLockState> s):state_(std::move(s)) {}
ClientInstallGuard::~ClientInstallGuard()=default;
ClientInstallGuard::ClientInstallGuard(ClientInstallGuard&&) noexcept=default;
ClientInstallGuard& ClientInstallGuard::operator=(ClientInstallGuard&&) noexcept=default;
std::variant<ClientSessionGuard,std::error_code> enter_client_session(const std::filesystem::path& root,const std::optional<ResumeTicket>& ticket) {
    auto opened=open_state(root);if(auto* ec=std::get_if<std::error_code>(&opened)) return *ec;
    auto state=std::get<std::unique_ptr<ClientLockState>>(std::move(opened));
    if(ticket) {if(!ticket_matches(*ticket,*state)) return error(ERROR_INVALID_TOKEN);}
    else {
        auto gate=lock_file(root/L".uc-update"/L"gate.lock",0);
        if(auto* ec=std::get_if<std::error_code>(&gate)) return *ec;
        state->gate=std::get<Handle>(std::move(gate));
    }
    auto session=lock_file(root/L".uc-update"/L"session.lock",FILE_SHARE_READ|FILE_SHARE_WRITE);
    if(auto* ec=std::get_if<std::error_code>(&session)) return *ec;
    state->session=std::get<Handle>(std::move(session));
    if(auto ec=take_lock(*state,false)) return ec;
    if(!ticket) {if(auto ec=reject_pending(root)) return ec;}
    state->gate=Handle{};
    return ClientSessionGuard(std::move(state));
}
std::variant<ClientInstallGuard,std::error_code> reserve_client_install(const std::filesystem::path& root) {
    auto opened=open_state(root);if(auto* ec=std::get_if<std::error_code>(&opened)) return *ec;
    auto state=std::get<std::unique_ptr<ClientLockState>>(std::move(opened));
    auto gate=lock_file(root/L".uc-update"/L"gate.lock",0);
    if(auto* ec=std::get_if<std::error_code>(&gate)) return *ec;
    state->gate=std::get<Handle>(std::move(gate));
    auto session=lock_file(root/L".uc-update"/L"session.lock",FILE_SHARE_READ|FILE_SHARE_WRITE);
    if(auto* ec=std::get_if<std::error_code>(&session)) return *ec;
    state->session=std::get<Handle>(std::move(session));
    return ClientInstallGuard(std::move(state));
}
std::error_code ClientInstallGuard::wait_until_unused(std::chrono::milliseconds timeout) {
    if(!state_) return error(ERROR_INVALID_HANDLE);
    if(state_->exclusive) return {};
    auto end=std::chrono::steady_clock::now()+timeout;
    for(;;) {
        auto ec=take_lock(*state_,true);if(!ec || ec.value()!=ERROR_LOCK_VIOLATION) return ec;
        if(std::chrono::steady_clock::now()>=end) return error(ERROR_TIMEOUT);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
std::error_code ClientInstallGuard::allow_child_start() {
    if(!state_ || !state_->exclusive) return error(ERROR_INVALID_STATE);
    OVERLAPPED overlapped{};
    if(!UnlockFileEx(state_->session.value,0,1,0,&overlapped)) return error();
    state_->exclusive=false;return {};
}
std::error_code ClientInstallGuard::reacquire_for_rollback(std::chrono::milliseconds t) {return wait_until_unused(t);}
void ClientInstallGuard::release_after_commit() {state_.reset();}
std::variant<std::uintptr_t,std::error_code> ClientInstallGuard::resume_channel(const std::string& id) {
    if(!state_ || state_->exclusive || !valid_id(id)) return error(ERROR_INVALID_PARAMETER);
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};HANDLE input{},output{};
    if(!CreatePipe(&input,&output,&security,sizeof(WireTicket))) return error();
    Handle reader(input),writer(output);
    WireTicket wire{magic,state_->identity.dwVolumeSerialNumber,state_->identity.nFileIndexHigh,state_->identity.nFileIndexLow,{}};
    std::copy(id.begin(),id.end(),wire.operation.begin());DWORD written=0;
    if(!WriteFile(writer.value,&wire,sizeof(wire),&written,nullptr) || written!=sizeof(wire)) return error();
    return reinterpret_cast<std::uintptr_t>(std::exchange(reader.value,INVALID_HANDLE_VALUE));
}
}
