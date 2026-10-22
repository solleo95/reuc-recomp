#include "input_api.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>

namespace uc::mods {
namespace {
constexpr uint32_t kButtons = 0x1C10, kFireButton = 0x400;   // A 0x800, B 0x400 (the trigger), minus 0x10, plus 0x1000

class InputApi : public GameApiTicker, public std::enable_shared_from_this<InputApi> {
public:
    ~InputApi() override;

    void register_members(ApiRegistry& api);

    void tick(const GameEventSink&) override {}
    void drop_owner(const std::string& owner) override {
        std::lock_guard lock(mutex_);
        force_.erase(owner); block_.erase(owner); aim_.erase(owner);
        aim_order_.erase(std::remove(aim_order_.begin(), aim_order_.end(), owner), aim_order_.end());
    }

    void filter(InputView& sample) {
        std::lock_guard lock(mutex_);
        uint32_t force = 0, block = 0;
        for (const auto& [owner, mask] : force_) force |= mask;
        for (const auto& [owner, mask] : block_) block |= mask;
        const uint32_t held = (sample.held | force) & ~block;
        sample.pressed = (sample.pressed | (held & ~sample.held)) & held;
        sample.released = (sample.released | (sample.held & ~held)) & ~held;
        sample.held = held;
        if (!aim_order_.empty()) { const auto& aim = aim_.at(aim_order_.back()); sample.x = aim[0]; sample.y = aim[1]; }
        seen_ = sample;
    }

private:
    static uint32_t mask_of(const Value& v, const char* what) {
        if (!v.is_number_integer() || v.get<long long>() < 0 || (v.get<long long>() & ~static_cast<long long>(kButtons)) != 0)
            throw ApiError(std::string(what) + " must be a combination of the buttons A 0x800, B 0x400, minus 0x10 and plus 0x1000");
        return uint32_t(v.get<long long>());
    }
    uint32_t combined(const std::map<std::string, uint32_t>& masks) const {
        uint32_t all = 0;
        for (const auto& [owner, mask] : masks) all |= mask;
        return all;
    }

    std::mutex mutex_;
    InputView seen_;                                       // the last sample after the filter
    std::map<std::string, uint32_t> force_, block_;        // per mod
    std::map<std::string, std::array<float, 2>> aim_;
    std::vector<std::string> aim_order_;                   // owners of an aim override, the latest last
};

std::atomic<InputApi*> g_active{nullptr};

InputApi::~InputApi() {
    InputApi* self = this;
    g_active.compare_exchange_strong(self, nullptr);
}

void InputApi::register_members(ApiRegistry& api) {
    const auto self = shared_from_this();
    const auto add = [&](const char* name, std::function<Value()> get, std::function<void(const Value&)> set) {
        Member member;
        member.path = std::string("input.") + name;
        member.get = [get](const Value&) { return get(); };
        if (set) member.set = [set](const Value&, const Value& value) { set(value); };
        api.add(std::move(member));
    };
    add("held", [self] { std::lock_guard lock(self->mutex_); return Value(self->seen_.held); }, nullptr);
    add("fire_held", [self] { std::lock_guard lock(self->mutex_); return Value((self->seen_.held & kFireButton) != 0); }, nullptr);
    add("aim", [self] { std::lock_guard lock(self->mutex_); return Value::array({double(self->seen_.x), double(self->seen_.y)}); }, nullptr);
    add("force_held",
        [self] { std::lock_guard lock(self->mutex_); return Value(self->combined(self->force_)); },
        [self](const Value& v) {
            const uint32_t mask = mask_of(v, "force_held");
            std::lock_guard lock(self->mutex_);
            if (mask) self->force_[OwnerScope::current()] = mask; else self->force_.erase(OwnerScope::current());
        });
    add("block_held",
        [self] { std::lock_guard lock(self->mutex_); return Value(self->combined(self->block_)); },
        [self](const Value& v) {
            const uint32_t mask = mask_of(v, "block_held");
            std::lock_guard lock(self->mutex_);
            if (mask) self->block_[OwnerScope::current()] = mask; else self->block_.erase(OwnerScope::current());
        });
    add("aim_override",
        [self]() -> Value {
            std::lock_guard lock(self->mutex_);
            if (self->aim_order_.empty()) return Value(nullptr);
            const auto& aim = self->aim_.at(self->aim_order_.back());
            return Value::array({double(aim[0]), double(aim[1])});
        },
        [self](const Value& v) {
            const std::string& owner = OwnerScope::current();
            std::lock_guard lock(self->mutex_);
            if (v.is_null()) {
                self->aim_.erase(owner);
                self->aim_order_.erase(std::remove(self->aim_order_.begin(), self->aim_order_.end(), owner), self->aim_order_.end());
                return;
            }
            if (!v.is_array() || v.size() != 2 || !v[0].is_number() || !v[1].is_number()) throw ApiError("aim_override must be nil or a pair {x, y} with both between -1 and 1");
            const double x = v[0].get<double>(), y = v[1].get<double>();
            if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 1 || std::abs(y) > 1) throw ApiError("aim_override must be nil or a pair {x, y} with both between -1 and 1");
            self->aim_[owner] = {float(x), float(y)};
            self->aim_order_.erase(std::remove(self->aim_order_.begin(), self->aim_order_.end(), owner), self->aim_order_.end());
            self->aim_order_.push_back(owner);
        });
}
}  // namespace

std::shared_ptr<GameApiTicker> register_input_api(ApiRegistry& api) {
    auto implementation = std::make_shared<InputApi>();
    implementation->register_members(api);
    g_active.store(implementation.get());
    return implementation;
}

void filter_input(InputView& sample) {
    if (InputApi* active = g_active.load()) active->filter(sample);
}

}  // namespace uc::mods
