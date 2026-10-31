// Linux/Deck only (tests/linux/CMakeLists.txt): one launcher per install folder.
#include "check.h"
#include "deck/instance_lock.h"
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
namespace fs = std::filesystem;
using namespace uc::deck;
int main() {
    const auto root = fs::temp_directory_path() / ("uc-lock-posix-" + std::to_string(::getpid()));
    fs::remove_all(root); fs::create_directories(root);
    {
        auto first = InstanceLock::acquire(root);
        CHECK(first.state() == InstanceLock::State::held);
        CHECK(InstanceLock::acquire(root).state() == InstanceLock::State::busy);        // a second launcher of the same install must not run its start-up recovery or an update
        const pid_t child = ::fork();                                                   // another process sees it too
        CHECK(child >= 0);
        if (child == 0) ::_exit(InstanceLock::acquire(root).state() == InstanceLock::State::busy ? 0 : 1);
        int status = 0; CHECK(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        CHECK(first.close_on_exec());                                                   // the game and the restarted launcher do not inherit it: the new process takes it itself
    }
    CHECK(InstanceLock::acquire(root).state() == InstanceLock::State::held);            // released with the object (or the process)
    CHECK(InstanceLock::acquire(root / "no-such-folder").state() == InstanceLock::State::unavailable);   // cannot be created: the launcher goes on without exclusion
    fs::remove_all(root);
    std::puts("instance lock posix: passed");
}
