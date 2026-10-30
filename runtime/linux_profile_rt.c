/* A minimal instrumentation profile runtime for the cross build to Linux (scripts/linux/pgo.py, UC_PGO_GENERATE).
 *
 * The sysroot's libclang_rt.profile is LLVM 11's and does not match the compiler's (23) data layout, and the compiler's Windows install has no Linux runtime. The layout of the
 * raw profile file is the compiler's own: the record struct and the header come from clang's InstrProfData.inc, which this file includes, so a newer compiler brings its layout
 * with it. Counters, records and names are the sections the instrumented code leaves in the executable (__llvm_prf_*); at exit they are written as they are, the way compiler-rt's
 * InstrProfilingFile.c does it for a program without value profiling, binary ids, vtables or GPU counters (all of which are off in this build).
 * Output: $LLVM_PROFILE_FILE (%p becomes the process id), default "default.profraw". Not instrumented itself: compiled as plain C. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef intptr_t IntPtrT;
#include "profile/InstrProfData.inc"   /* without any of the section macros: only the shared definitions (the magic, the raw version) */
enum {
#define VALUE_PROF_KIND(Enumerator, Value, Descr) Enumerator = Value,
#include "profile/InstrProfData.inc"
};
typedef struct __attribute__((aligned(8))) __llvm_profile_data {
#define INSTR_PROF_DATA(Type, LLVMType, Name, Initializer) Type Name;
#include "profile/InstrProfData.inc"
} __llvm_profile_data;
typedef struct {
#define INSTR_PROF_RAW_HEADER(Type, Name, Initializer) Type Name;
#include "profile/InstrProfData.inc"
} __llvm_profile_header;

#define SECTION(name) extern char __start_##name[] __attribute__((weak, visibility("hidden"))); extern char __stop_##name[] __attribute__((weak, visibility("hidden")));
SECTION(__llvm_prf_data)
SECTION(__llvm_prf_cnts)
SECTION(__llvm_prf_bits)
SECTION(__llvm_prf_names)

/* referenced by every instrumented object: pulls this runtime in */
int __llvm_profile_runtime = 0;
extern const uint64_t __llvm_profile_raw_version __attribute__((weak));   /* the compiler emits it with the variant bits of the instrumentation */
/* indirect-call value profiling is switched off at compile time (-mllvm -enable-value-profiling=false); the hook exists for objects built without that */
void __llvm_profile_instrument_target(uint64_t value, void* data, uint32_t index) { (void)value; (void)data; (void)index; }

static uint64_t padding(uint64_t size) { return (8 - size % 8) % 8; }
static const char zeros[8];

static void write_profile(void) {
    static int written = 0;
    if (written) return;
    written = 1;
    const char* pattern = getenv("LLVM_PROFILE_FILE");
    char path[4096];
    const char* source = pattern && *pattern ? pattern : "default.profraw";
    size_t out = 0;
    for (const char* c = source; *c && out + 24 < sizeof path; ++c) {
        if (c[0] == '%' && c[1] == 'p') { out += (size_t)snprintf(path + out, sizeof path - out, "%d", (int)getpid()); ++c; }
        else path[out++] = *c;
    }
    path[out] = 0;
    const char* DataBegin = __start___llvm_prf_data; const char* DataEnd = __stop___llvm_prf_data;
    const char* CountersBegin = __start___llvm_prf_cnts; const char* CountersEnd = __stop___llvm_prf_cnts;
    const char* BitmapBegin = __start___llvm_prf_bits; const char* BitmapEnd = __stop___llvm_prf_bits;
    const char* NamesBegin = __start___llvm_prf_names; const char* NamesEnd = __stop___llvm_prf_names;
    if (!DataBegin || DataEnd <= DataBegin) return;   /* nothing was instrumented */
    const char* UniformCountersBegin = 0;
    const uint64_t NumData = (uint64_t)(DataEnd - DataBegin) / sizeof(__llvm_profile_data);
    const uint64_t DataSize = NumData * sizeof(__llvm_profile_data);
    const uint64_t NumCounters = (uint64_t)(CountersEnd - CountersBegin) / sizeof(uint64_t);
    const uint64_t NumBitmapBytes = BitmapBegin ? (uint64_t)(BitmapEnd - BitmapBegin) : 0;
    const uint64_t NamesSize = (uint64_t)(NamesEnd - NamesBegin);
    const uint64_t PaddingBytesBeforeCounters = padding(DataSize);
    const uint64_t PaddingBytesAfterCounters = padding(NumCounters * 8);
    const uint64_t PaddingBytesAfterBitmapBytes = padding(NumBitmapBytes);
    const uint64_t NumUniformCounters = 0, PaddingBytesAfterUniformCounters = 0, NumVTables = 0, VNamesSize = 0;
    __llvm_profile_header header;
    memset(&header, 0, sizeof header);
#define INSTR_PROF_RAW_HEADER(Type, Name, Initializer) header.Name = (Type)(Initializer);
#define __llvm_profile_get_magic() (INSTR_PROF_RAW_MAGIC_64)
#define __llvm_profile_get_version() (&__llvm_profile_raw_version ? __llvm_profile_raw_version : (uint64_t)INSTR_PROF_RAW_VERSION)
#define __llvm_write_binary_ids(x) 0
#include "profile/InstrProfData.inc"
    FILE* file = fopen(path, "wb");
    if (!file) { fprintf(stderr, "[pgo] cannot write %s: %s\n", path, strerror(errno)); return; }
    fwrite(&header, sizeof header, 1, file);
    fwrite(DataBegin, 1, DataSize, file); fwrite(zeros, 1, PaddingBytesBeforeCounters, file);
    fwrite(CountersBegin, 8, NumCounters, file); fwrite(zeros, 1, PaddingBytesAfterCounters, file);
    if (NumBitmapBytes) fwrite(BitmapBegin, 1, NumBitmapBytes, file);
    fwrite(zeros, 1, PaddingBytesAfterBitmapBytes, file);
    fwrite(NamesBegin, 1, NamesSize, file); fwrite(zeros, 1, padding(NamesSize), file);
    fclose(file);
    fprintf(stderr, "[pgo] profile written: %s (%llu functions, %llu counters)\n", path, (unsigned long long)NumData, (unsigned long long)NumCounters);
}
__attribute__((destructor)) static void at_exit(void) { write_profile(); }
/* the game ends with exit() or _exit-like paths in some runs: the harness's quit uses the normal one; this is for a program that wants a dump before it stops */
void __llvm_profile_write_file(void) { write_profile(); }
