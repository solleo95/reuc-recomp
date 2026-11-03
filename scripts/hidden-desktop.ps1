# Runs a program on a desktop nobody looks at: Invoke-OnHiddenDesktop <exe> <arguments...>  (dot-source this file).
# The tests open real windows (the updater, the launcher, the menus); on the user's desktop they pop up over whatever is being done there, and a game being played loses the
# focus. A window created on another desktop of the same window station is never shown and never takes the input. The program's output goes where ours goes, its exit code is
# returned. Anything that stops the desktop from being made falls back to running the program as usual (with a warning).
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class UcHiddenDesktop {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct STARTUPINFO {
        public int cb; public string lpReserved, lpDesktop, lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern IntPtr CreateDesktopW(string name, string device, IntPtr devmode, int flags, uint access, IntPtr attributes);
    [DllImport("user32.dll", SetLastError = true)] static extern bool CloseDesktop(IntPtr desktop);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool CreateProcessW(string app, string command, IntPtr pa, IntPtr ta, bool inherit, uint flags, IntPtr env, string dir, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
    [DllImport("kernel32.dll", SetLastError = true)] static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool GetExitCodeProcess(IntPtr handle, out uint code);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr GetStdHandle(int which);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool SetHandleInformation(IntPtr handle, uint mask, uint flags);
    // Returns the exit code, or -1 with `error` set when the desktop or the process could not be made.
    public static int Run(string commandLine, string directory, string desktopName, out string error) {
        error = null;
        IntPtr desktop = CreateDesktopW(desktopName, null, IntPtr.Zero, 0, 0x10000000 /* GENERIC_ALL */, IntPtr.Zero);
        if (desktop == IntPtr.Zero) { error = "CreateDesktop failed: " + Marshal.GetLastWin32Error(); return -1; }
        try {
            var si = new STARTUPINFO();
            si.cb = Marshal.SizeOf(typeof(STARTUPINFO));
            si.lpDesktop = "winsta0\\" + desktopName;
            si.dwFlags = 0x100;   // STARTF_USESTDHANDLES: the program writes where we write
            si.hStdInput = GetStdHandle(-10); si.hStdOutput = GetStdHandle(-11); si.hStdError = GetStdHandle(-12);
            foreach (var h in new[] { si.hStdInput, si.hStdOutput, si.hStdError }) if (h != IntPtr.Zero && h != new IntPtr(-1)) SetHandleInformation(h, 1, 1);   // HANDLE_FLAG_INHERIT
            PROCESS_INFORMATION pi;
            if (!CreateProcessW(null, commandLine, IntPtr.Zero, IntPtr.Zero, true, 0, IntPtr.Zero, directory, ref si, out pi)) { error = "CreateProcess failed: " + Marshal.GetLastWin32Error(); return -1; }
            CloseHandle(pi.hThread);
            WaitForSingleObject(pi.hProcess, 0xFFFFFFFF);
            uint code; GetExitCodeProcess(pi.hProcess, out code);
            CloseHandle(pi.hProcess);
            return unchecked((int)code);
        } finally { CloseDesktop(desktop); }
    }
}
'@

function ConvertTo-CommandLineArgument([string]$text) {
    if ($text -notmatch '[\s"]' -and $text -ne '') { return $text }
    '"' + ($text -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"'
}

function Invoke-OnHiddenDesktop([string]$Program, [string[]]$Arguments) {
    $resolved = (Get-Command $Program -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
    $line = (@($resolved) + @($Arguments) | ForEach-Object { ConvertTo-CommandLineArgument "$_" }) -join ' '
    if ($env:UC_TESTS_VISIBLE) { & $resolved @Arguments; return $LASTEXITCODE }   # UC_TESTS_VISIBLE=1: on the real desktop, to watch a test
    $error_text = $null
    $code = [UcHiddenDesktop]::Run($line, (Get-Location).Path, 'uc-tests-' + $PID, [ref]$error_text)
    if ($null -ne $error_text) {
        Write-Warning "$error_text; running $Program on the visible desktop"
        & $resolved @Arguments
        return $LASTEXITCODE
    }
    return $code
}
