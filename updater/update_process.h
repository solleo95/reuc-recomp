#pragma once
#include "files.h"
#include "transaction.h"
#include "client_update_lock.h"
#include <span>
namespace uc::update {
struct HelperProcess {Handle process;};
struct ResumeStartup {
    uc::ResumeTicket ticket;
    Handle channel,acknowledged,committed,helper;
    Status confirm(std::chrono::milliseconds timeout=std::chrono::seconds(30));
};
Result<std::optional<ResumeStartup>> parse_resume_startup(int argc,wchar_t** argv);
Result<Handle> create_child(const fs::path& executable,const fs::path& directory,
    std::span<const std::wstring> arguments,std::span<const HANDLE> inherited={});
Result<HelperProcess> start_update_helper(const fs::path& install,const Package& package,
    std::chrono::milliseconds timeout=std::chrono::seconds(30));
Status run_update_helper(const fs::path& operation,HANDLE parent,HANDLE ready,bool recover,
    std::chrono::milliseconds timeout=std::chrono::seconds(30));
Result<bool> resume_pending_update(const fs::path& install);
// Fixed launcher path and inherited IPC; exposed for fixture-process tests.
Status start_and_confirm(const fs::path& install,const fs::path& operation,
    uc::ClientInstallGuard& guard,std::chrono::milliseconds timeout=std::chrono::seconds(30));
}
