#include "input_config.h"
#include <algorithm>
#include <atomic>
#include <fstream>
#include <set>
#include <stdexcept>
#include <system_error>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "file_rename.h"
#else
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>
#endif
namespace uc {
namespace {
using Json=nlohmann::ordered_json;
constexpr size_t kMaxBytes=1024*1024;
[[noreturn]] void invalid(std::string message) {throw std::invalid_argument("config: "+message);}
void fields(const Json& value,std::initializer_list<std::string_view> allowed) {
    if(!value.is_object())invalid("expected object");
    for(auto i=value.begin();i!=value.end();++i)
        if(std::find(allowed.begin(),allowed.end(),i.key())==allowed.end())invalid("unknown field "+i.key());
}
std::string kind(const Json& value) {
    if(!value.contains("kind") || !value["kind"].is_string())invalid("binding kind must be a string");
    return value["kind"].get<std::string>();
}
PhysicalInput physical(const Json& value) {
    if(!value.contains("code") || !value["code"].is_number_unsigned())invalid("binding code must be an unsigned integer");
    const auto code=value["code"].get<uint64_t>();
    if(code>UINT16_MAX)invalid("binding code out of range");
    const auto name=kind(value);
    PhysicalKind type;
    if(name=="key")type=PhysicalKind::key;
    else if(name=="mouse")type=PhysicalKind::mouse;
    else if(name=="wheel")type=PhysicalKind::wheel;
    else if(name=="pad_button")type=PhysicalKind::pad_button;
    else if(name=="pad_axis")type=PhysicalKind::pad_axis;
    else if(name=="wii_button")type=PhysicalKind::wii_button;
    else invalid("unknown physical kind "+name);
    return {type,uint16_t(code)};
}
Json physical_json(PhysicalInput input) {
    const char* name="wheel";
    switch(input.kind) {
    case PhysicalKind::key: name="key"; break;
    case PhysicalKind::mouse: name="mouse"; break;
    case PhysicalKind::pad_button: name="pad_button"; break;
    case PhysicalKind::pad_axis: name="pad_axis"; break;
    case PhysicalKind::wii_button: name="wii_button"; break;
    case PhysicalKind::wheel: break;
    }
    return {{"kind",name},{"code",input.code}};
}
std::optional<std::string> read_file(const std::filesystem::path& path) {
    std::error_code error;
    if(!std::filesystem::exists(path,error)) {
        if(error)throw std::runtime_error("config: cannot inspect file: "+error.message());
        return {};
    }
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file)throw std::runtime_error("config: cannot open file");
    const auto size=file.tellg();
    if(size<0 || uint64_t(size)>kMaxBytes)invalid("file exceeds 1 MiB");
    std::string bytes(size_t(size),'\0'); file.seekg(0);
    if(!file.read(bytes.data(),std::streamsize(bytes.size())))throw std::runtime_error("config: cannot read file");
    return bytes;
}
#ifdef _WIN32
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    ~Handle() {if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
    explicit Handle(HANDLE h):value(h) {}
};
[[noreturn]] void io_error(const char* message) {
    throw std::runtime_error(std::string("config: ")+message+": "+std::system_category().message(int(GetLastError())));
}
#else
[[noreturn]] void io_error(const char* message) {
    throw std::runtime_error(std::string("config: ")+message+": "+std::generic_category().message(errno));
}
#endif
}
Bindings bindings_from_json(const nlohmann::ordered_json& bindings,Bindings result) {
    try {
        if(!bindings.is_object())invalid("bindings must be an object");
        for(auto i=bindings.begin();i!=bindings.end();++i) {
            auto name=std::find(action_names.begin(),action_names.end(),i.key());
            if(name==action_names.end())invalid("unknown action "+i.key());
            if(!i->is_array() || i->size()>64)invalid("action bindings must be an array of at most 64 entries");
            auto& output=result[size_t(name-action_names.begin())]; output.clear();
            for(const auto& value:*i) {
                if(!value.is_object())invalid("binding must be an object");
                Binding binding;
                const auto type=kind(value);
                if(type=="interact_fire" || type=="interact_motion" || type=="qte_motion") {
                    fields(value,{"kind"});
                    binding.kind=type=="interact_fire" ? BindingKind::interact_fire :
                        type=="interact_motion" ? BindingKind::interact_motion : BindingKind::qte_motion;
                } else {
                    fields(value,{"kind","code","modifiers"}); binding.input=physical(value);
                    if(value.contains("modifiers")) {
                        const auto& modifiers=value["modifiers"];
                        if(!modifiers.is_array() || modifiers.size()>4)invalid("at most 4 modifiers required");
                        for(const auto& modifier:modifiers) {fields(modifier,{"kind","code"});binding.modifiers.push_back(physical(modifier));}
                    }
                }
                output.push_back(std::move(binding));
            }
        }
    } catch(const Json::exception& error) {invalid(error.what());}
    return result;
}
nlohmann::ordered_json bindings_to_json(const Bindings& bindings) {
    auto result=Json::object();
    for(size_t i=0;i<bindings.size();++i) {
        auto entries=Json::array();
        for(const auto& binding:bindings[i]) {
            Json value;
            if(binding.kind==BindingKind::physical) {
                value=physical_json(binding.input);
                if(!binding.modifiers.empty()) {
                    value["modifiers"]=Json::array();
                    for(auto modifier:binding.modifiers)value["modifiers"].push_back(physical_json(modifier));
                }
            } else value={{"kind",binding.kind==BindingKind::interact_fire ? "interact_fire" :
                binding.kind==BindingKind::interact_motion ? "interact_motion" : "qte_motion"}};
            entries.push_back(std::move(value));
        }
        result[std::string(action_names[i])]=std::move(entries);
    }
    return result;
}
Json physical_input_json(PhysicalInput input) { return physical_json(input); }
PhysicalInput physical_input_from_json(const Json& value) { fields(value,{"kind","code"}); return physical(value); }
ConfigDocument parse_config(std::string_view bytes) {
    if(bytes.size()>kMaxBytes)invalid("file exceeds 1 MiB");
    ConfigDocument result;
    std::vector<std::set<std::string>> keys;
    try {
        result.properties=Json::parse(bytes,[&](int depth,Json::parse_event_t event,Json& value) {
            if(depth>32)invalid("nesting exceeds 32");
            if(event==Json::parse_event_t::object_start)keys.emplace_back();
            else if(event==Json::parse_event_t::object_end)keys.pop_back();
            else if(event==Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)invalid("duplicate property "+value.get<std::string>());
            return true;
        });
        if(!result.properties.is_object())invalid("root must be an object");
        if(!result.properties.contains("input"))return result;
        const auto& input=result.properties["input"];
        fields(input,{"version","bindings"});
        if(!input.contains("version") || !input["version"].is_number_unsigned() || input["version"]!=1)invalid("unsupported input version");
        if(!input.contains("bindings"))return result;
        result.input=bindings_from_json(input["bindings"],result.input);
    } catch(const Json::exception& error) {invalid(error.what());}
    if(auto error=validate_bindings(result.input))invalid(*error);
    return result;
}
ConfigDocument load_config(const std::filesystem::path& path) {
    auto bytes=read_file(path);
    if(!bytes)return {};
    auto result=parse_config(*bytes); result.original_bytes=std::move(bytes); return result;
}
std::string serialize_config(const ConfigDocument& document) {
    if(auto error=validate_bindings(document.input))invalid(*error);
    if(!document.properties.is_object())invalid("root must be an object");
    auto properties=document.properties;
    properties["input"]={{"version",1},{"bindings",bindings_to_json(document.input)}};
    auto bytes=properties.dump(2)+"\n";
    if(bytes.size()>kMaxBytes)invalid("serialized file exceeds 1 MiB");
    return bytes;
}
#ifndef _WIN32
void save_config(const std::filesystem::path& path,ConfigDocument& document) {
    const auto bytes=serialize_config(document);
    auto lock_path=path; lock_path+=".write-lock";
    struct Lock {   // the lock file exists while a writer runs; only the writer that created it removes it
        std::filesystem::path path; int fd;
        ~Lock() {if(fd>=0) {::close(fd);std::error_code ignored;std::filesystem::remove(path,ignored);}}
    } lock{lock_path,::open(lock_path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600)};
    if(lock.fd<0)io_error("writer lock unavailable; another writer may be active");
    if(read_file(path)!=document.original_bytes)throw std::runtime_error("config: file changed since loading; reload before saving");
    static std::atomic<uint64_t> serial=0;
    const auto ticks=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto temporary=path; temporary+=".tmp-"+std::to_string(getpid())+"-"+std::to_string(ticks)+"-"+std::to_string(serial.fetch_add(1));
    int file=::open(temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0644);
    if(file<0)io_error("cannot create temporary file");
    try {
        // The new bytes are written and flushed beside the file and renamed over it: a crash leaves the old or the new config, never half of one.
        for(size_t done=0;done<bytes.size();) {
            const ssize_t count=::write(file,bytes.data()+done,bytes.size()-done);
            if(count<0) {if(errno==EINTR)continue;io_error("cannot write temporary file");}
            done+=size_t(count);
        }
        if(fsync(file))io_error("cannot flush temporary file");
        const int closed=::close(file);
        file=-1;   // closed (even when close reports an error): never close this number again
        if(closed)io_error("cannot close temporary file");
        if(::rename(temporary.c_str(),path.c_str()))io_error("cannot replace file");
    } catch(...) {
        if(file>=0)::close(file);
        std::error_code ignored;
        std::filesystem::remove(temporary,ignored);   // only this writer's unique temporary file
        throw;
    }
    document.original_bytes=bytes;
}
#else
void save_config(const std::filesystem::path& path,ConfigDocument& document) {
    const auto bytes=serialize_config(document);
    auto lock_path=path; lock_path+=L".write-lock";
    Handle lock(CreateFileW(lock_path.c_str(),GENERIC_WRITE|DELETE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_HIDDEN|FILE_FLAG_DELETE_ON_CLOSE,nullptr));
    if(lock.value==INVALID_HANDLE_VALUE)io_error("writer lock unavailable; another writer may be active");
    if(read_file(path)!=document.original_bytes)throw std::runtime_error("config: file changed since loading; reload before saving");
    static std::atomic<uint64_t> serial=0;
    auto temporary=path; temporary+=L".tmp-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(serial.fetch_add(1));
    bool created=false;
    try {
        {
            Handle file(CreateFileW(temporary.c_str(),GENERIC_WRITE|DELETE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY,nullptr));
            if(file.value==INVALID_HANDLE_VALUE)io_error("cannot create temporary file");
            created=true;
            DWORD written=0;
            if(!WriteFile(file.value,bytes.data(),DWORD(bytes.size()),&written,nullptr) || written!=bytes.size())io_error("cannot write temporary file");
            if(!FlushFileBuffers(file.value))io_error("cannot flush temporary file");
            if(auto error=rename_sibling_file(file.value,path.filename().native(),true))
                throw std::runtime_error("config: cannot replace file: "+error.message());
        }
    } catch(...) {
        // Only this writer's unique temporary file is eligible for cleanup.
        if(created && std::filesystem::exists(temporary) && !DeleteFileW(temporary.c_str()))
            throw std::runtime_error("config: save failed and temporary file cleanup failed");
        throw;
    }
    document.original_bytes=bytes;
}
#endif
}
