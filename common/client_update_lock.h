#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <variant>

namespace uc {
struct ResumeTicket { std::uintptr_t channel{}; std::string operation_id; };
struct ClientLockState;
class ClientSessionGuard {
    std::unique_ptr<ClientLockState> state_;
public:
    explicit ClientSessionGuard(std::unique_ptr<ClientLockState>);
    ~ClientSessionGuard();
    ClientSessionGuard(ClientSessionGuard&&) noexcept;
    ClientSessionGuard& operator=(ClientSessionGuard&&) noexcept;
};
class ClientInstallGuard {
    std::unique_ptr<ClientLockState> state_;
public:
    explicit ClientInstallGuard(std::unique_ptr<ClientLockState>);
    ~ClientInstallGuard();
    ClientInstallGuard(ClientInstallGuard&&) noexcept;
    ClientInstallGuard& operator=(ClientInstallGuard&&) noexcept;
    std::error_code wait_until_unused(std::chrono::milliseconds timeout);
    std::error_code allow_child_start();
    std::error_code reacquire_for_rollback(std::chrono::milliseconds timeout);
    void release_after_commit();
    // Caller owns the returned inheritable pipe handle. Reading consumes the ticket once.
    std::variant<std::uintptr_t,std::error_code> resume_channel(const std::string& operation_id);
};
std::variant<ClientSessionGuard,std::error_code> enter_client_session(
    const std::filesystem::path& root,const std::optional<ResumeTicket>& ticket=std::nullopt);
std::variant<ClientInstallGuard,std::error_code> reserve_client_install(const std::filesystem::path& root);
}
