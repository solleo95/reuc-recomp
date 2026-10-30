// The guest's 4 GiB address space: every RAM block is visible at all the addresses the console reaches it by, and the hardware registers are not mapped.
#include "check.h"
#include "guest_mem.h"
#include <cstdint>
#include <cstdio>
#include <string_view>
#ifdef __linux__
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

// An address space too small for the 4 GiB range: the game must exit with code 4 and say why, not hang or crash.
static int init_under_address_limit() {
    int fds[2];
    if (pipe(fds)) return -1;
    const pid_t child = fork();
    if (child == 0) {
        dup2(fds[1], 2);
        const rlimit limit{1ull << 30, 1ull << 30};   // 1 GiB of address space
        setrlimit(RLIMIT_AS, &limit);
        uc::guest_mem_init();
        _exit(0);
    }
    close(fds[1]);
    char message[256]{};
    const ssize_t got = read(fds[0], message, sizeof message - 1);
    int status = 0;
    waitpid(child, &status, 0);
    std::printf("limited init: exit %d, message: %s", WIFEXITED(status) ? WEXITSTATUS(status) : -1, got > 0 ? message : "(none)\n");
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 4);
    CHECK(got > 0 && std::string_view(message).find("cannot map guest memory") != std::string_view::npos);
    return 0;
}

// Reading the hardware registers' range (0xCC000000) must fault: the translated code tests for registers before it touches memory.
static void registers_are_unmapped(const uint8_t* base) {
    const pid_t child = fork();
    if (child == 0) {
        signal(SIGSEGV, SIG_DFL);
        volatile uint8_t sink = *(volatile const uint8_t*)(base + 0xCC000000u);
        (void)sink;
        _exit(0);
    }
    int status = 0;
    waitpid(child, &status, 0);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
}
#endif

int main() {
#ifdef __linux__
    init_under_address_limit();
#endif
    uc::guest_mem_init();
    uc::guest_mem_init();   // idempotent
    uint8_t* raw_base = uc::guest_mem_base();
    CHECK(raw_base != nullptr);
    volatile uint8_t* base = raw_base;   // volatile: the offsets are constants, the optimizer would otherwise treat different offsets as different bytes
    struct Alias { uint64_t a, b, c; };
    // MEM1 24 MB: physical, cached, uncached; MEM2 64 MB likewise
    const Alias blocks[] = {{0x00000010, 0x80000010, 0xC0000010}, {0x10000010, 0x90000010, 0xD0000010}};
    uint8_t value = 0x40;
    for (const Alias& alias : blocks) {
        base[alias.a] = value;
        CHECK(base[alias.b] == value && base[alias.c] == value);
        base[alias.c] = uint8_t(value + 1);
        CHECK(base[alias.a] == value + 1 && base[alias.b] == value + 1);
        value += 8;
    }
    // the last byte of each block is mapped, the first byte after it is not part of the same view
    base[0x017FFFFF] = 0x5A; CHECK(base[0x817FFFFF] == 0x5A);
    base[0x13FFFFFF] = 0x5B; CHECK(base[0x93FFFFFF] == 0x5B);
    // locked cache: physical alias 0x20000000 and 0xE0000000
    base[0x20000010] = 0x77; CHECK(base[0xE0000010] == 0x77);
    CHECK(base[0x00000010] == 0x41 && base[0x10000010] == 0x49);   // MEM1 and MEM2 are different blocks
#ifdef __linux__
    registers_are_unmapped(raw_base);
#endif
    return 0;
}
