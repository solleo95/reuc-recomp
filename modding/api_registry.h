#pragma once
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace uc::mods {

using Value = nlohmann::json;

// A problem the script caused (bad address, wrong argument type, missing player). Runtimes turn it into a Lua error / JS exception.
struct ApiError : std::runtime_error { using std::runtime_error::runtime_error; };

// One API member under a registry domain, named "<domain>.<member>" as in modding/api/uc_api.json. Arguments arrive as one JSON object
// keyed by the schema's argument names; members of a scoped domain also get their scope key ("slot", "name" or "id").
struct Member {
    std::string path;
    std::vector<std::string> arg_names;   // functions: names in call order, so runtimes can map positional arguments
    std::function<Value(const Value& args)> get;                      // property read
    std::function<void(const Value& args, const Value& value)> set;   // property write
    std::function<Value(const Value& args)> call;                     // function
    std::string scope;   // "slot", "name" or "id" for members of a scoped domain (uc.player(1)): the key the scope value is passed under in args
    bool writes = false; // a function that changes the game (mem.write_*): like every setter it needs "touches_gameplay" in the mod's manifest
};

// The mod whose script is calling the API on this thread (set by the per-mod registry around every setter and function). State the API keeps
// for a mod (switches, weapon values) is filed under it, so it can be taken back when the mod is switched off or reloaded.
class OwnerScope {
public:
    explicit OwnerScope(const std::string& id);
    ~OwnerScope();
    OwnerScope(const OwnerScope&) = delete;
    OwnerScope& operator=(const OwnerScope&) = delete;
    static const std::string& current();   // empty outside a mod call
private:
    const std::string* previous_;
};

class ApiRegistry {
public:
    void add(Member member);   // throws std::logic_error for a path that is already registered
    const Member* find(std::string_view path) const;
    std::vector<std::string> paths() const;
    // The registry one mod's runtimes see: every setter and function runs under OwnerScope(owner); without `gameplay` the setters and the
    // writing functions throw an ApiError that names the missing manifest flag. Getters and reading functions are unchanged.
    ApiRegistry for_mod(const std::string& owner, bool gameplay) const;
private:
    std::map<std::string, Member, std::less<>> members_;
};

}  // namespace uc::mods
