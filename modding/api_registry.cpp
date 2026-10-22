#include "api_registry.h"

namespace uc::mods {
namespace {
const std::string*& current_owner() {
    thread_local const std::string* owner = nullptr;
    return owner;
}
}  // namespace

OwnerScope::OwnerScope(const std::string& id) : previous_(current_owner()) { current_owner() = &id; }
OwnerScope::~OwnerScope() { current_owner() = previous_; }
const std::string& OwnerScope::current() {
    static const std::string none;
    return current_owner() ? *current_owner() : none;
}

void ApiRegistry::add(Member member) {
    const auto path = member.path;
    if (!members_.emplace(path, std::move(member)).second) throw std::logic_error("API member registered twice: " + path);
}

const Member* ApiRegistry::find(std::string_view path) const {
    const auto at = members_.find(path);
    return at == members_.end() ? nullptr : &at->second;
}

std::vector<std::string> ApiRegistry::paths() const {
    std::vector<std::string> result;
    for (const auto& [path, member] : members_) result.push_back(path);
    return result;
}

ApiRegistry ApiRegistry::for_mod(const std::string& owner, bool gameplay) const {
    ApiRegistry copy;
    for (const auto& [path, original] : members_) {
        Member member = original;
        const std::string denied = "'" + path + "' changes the game: add \"touches_gameplay\": true to the mod's mod.json";
        if (member.set) {
            member.set = [owner, gameplay, denied, inner = original.set](const Value& args, const Value& value) {
                if (!gameplay) throw ApiError(denied);
                OwnerScope scope(owner);
                inner(args, value);
            };
        }
        if (member.call) {
            member.call = [owner, gameplay, denied, writes = original.writes, inner = original.call](const Value& args) {
                if (writes && !gameplay) throw ApiError(denied);
                OwnerScope scope(owner);
                return inner(args);
            };
        }
        copy.members_.emplace(path, std::move(member));
    }
    return copy;
}

}  // namespace uc::mods
