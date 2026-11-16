// uc_cli: the Android build's game started from adb shell, without the app (headless smoke runs and checks of the runtime on the device).
// It runs the game of libmain.so next to it: LD_LIBRARY_PATH=. ./uc_cli --headless --game <folder>/game; set UC_INSTALL_DIR to use that folder's data.
int uc_program_main(int argc, char** argv);   // game/uc_main.cpp, in libmain.so

int main(int argc, char** argv) { return uc_program_main(argc, argv); }
