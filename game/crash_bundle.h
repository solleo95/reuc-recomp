#pragma once
#include <filesystem>
#include <string>
namespace uc {
// Machine state of a crash for offline inspection (`python -m uctest inspect`): guest MEM1/MEM2 as raw files (mem1.bin 0x80000000, mem2.bin 0x90000000),
// the given threads.txt / gx.txt texts, and functions.txt with the entry of every translated function and its symbol. Throws on I/O errors.
void write_crash_bundle(const std::filesystem::path& dir, const std::string& threads, const std::string& gx);
}
