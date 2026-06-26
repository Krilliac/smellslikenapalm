// src/Utils/StackTrace.h
//
// Portable call-stack capture + symbolization for diagnostics and the crash
// handler. Header-only so any translation unit (and the tests) can use it
// without an extra link object.
//
// Backends:
//   * Windows : CaptureStackBackTrace + DbgHelp (SymFromAddr / SymGetLineFromAddr64).
//               SymInitialize is run exactly once per process (std::call_once),
//               with a search path that includes the executable's directory so
//               the PDB is found regardless of the working directory.
//   * POSIX   : backtrace() + dladdr()/backtrace_symbols() + abi::__cxa_demangle.
//   * other   : no-op (returns an empty trace).
//
// dbghelp is linked via CMake (target_link_libraries) so this works on BOTH
// MSVC and MinGW — we deliberately do NOT use #pragma comment(lib,...) (MSVC-only).
//
// Frame format (one per line in ToString):
//   #i 0xADDR  Function+0xOff (file:line) [module]
//
// Adapted/simplified from a sibling engine's StackTrace.h.

#pragma once

#include <cstdint>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
// NOTE: dbghelp is linked via CMake (works on MSVC + MinGW); no #pragma comment.
#elif defined(__unix__) || defined(__APPLE__)
#include <cxxabi.h>
#include <cstdlib>
#include <dlfcn.h>     // dladdr()
#include <execinfo.h>  // backtrace()
#endif

namespace rs2v {

// A single resolved frame in a captured stack trace.
struct StackFrame {
    uintptr_t   address      = 0;  // Instruction pointer.
    std::string moduleName;        // e.g. "rs2v_server.exe"
    std::string functionName;      // Demangled function name (if resolved).
    std::string fileName;          // Source file (if line info available).
    int         lineNumber   = 0;  // Source line (if available).
    int         displacement = 0;  // Byte offset from function start.

    // Render as: 0xADDR  Function+0xOff (file:line) [module]
    std::string ToString() const {
        std::ostringstream oss;
        oss << "0x" << std::hex << address << std::dec;

        if (!functionName.empty())
            oss << "  " << functionName;
        else
            oss << "  <unknown>";

        if (displacement > 0)
            oss << "+0x" << std::hex << displacement << std::dec;

        if (!fileName.empty())
            oss << " (" << fileName << ":" << lineNumber << ")";

        if (!moduleName.empty())
            oss << " [" << moduleName << "]";

        return oss.str();
    }
};

class StackTrace {
public:
    static constexpr int MAX_FRAMES = 64;

    // Capture the current call stack.
    //   skipFrames : caller frames to skip beyond Capture() itself
    //                (default 1 = skip Capture()'s immediate caller's bookkeeping).
    //   maxFrames  : cap on number of frames.
    static StackTrace Capture(int skipFrames = 1, int maxFrames = MAX_FRAMES) {
        StackTrace trace;
        // +1 to drop CaptureRaw/Capture's own frame.
        trace.m_capturedFrames =
            CaptureRaw(trace.m_rawAddresses, skipFrames + 1, maxFrames);
        trace.ResolveSymbols();
        return trace;
    }

    const std::vector<StackFrame>& GetFrames() const { return m_frames; }
    int GetFrameCount() const { return static_cast<int>(m_frames.size()); }

    // Multi-line, one frame per line: "  #i <frame>".
    std::string ToString(const std::string& indent = "  ") const {
        std::ostringstream oss;
        for (size_t i = 0; i < m_frames.size(); ++i) {
            oss << indent << "#" << i << " " << m_frames[i].ToString() << "\n";
        }
        return oss.str();
    }

private:
    std::vector<StackFrame> m_frames;
    void*                   m_rawAddresses[MAX_FRAMES] = {};
    int                     m_capturedFrames           = 0;

    static int CaptureRaw(void** addresses, int skipFrames, int maxFrames) {
        if (maxFrames > MAX_FRAMES) maxFrames = MAX_FRAMES;
        if (skipFrames < 0) skipFrames = 0;

#ifdef _WIN32
        // CaptureStackBackTrace skips its own frame internally.
        USHORT captured = CaptureStackBackTrace(
            static_cast<DWORD>(skipFrames), static_cast<DWORD>(maxFrames),
            addresses, nullptr);
        return static_cast<int>(captured);

#elif defined(__unix__) || defined(__APPLE__)
        void* buffer[MAX_FRAMES + 32];
        int totalCapture = maxFrames + skipFrames;
        if (totalCapture > MAX_FRAMES + 32) totalCapture = MAX_FRAMES + 32;

        int captured = backtrace(buffer, totalCapture);
        if (skipFrames >= captured) return 0;

        int count = captured - skipFrames;
        if (count > maxFrames) count = maxFrames;
        for (int i = 0; i < count; ++i)
            addresses[i] = buffer[skipFrames + i];
        return count;
#else
        (void)addresses;
        (void)skipFrames;
        (void)maxFrames;
        return 0;
#endif
    }

#ifdef _WIN32
    // SymInitialize must run exactly once per process. Thread-safe via call_once.
    static void EnsureSymbolsInitialized() {
        static std::once_flag s_symInitFlag;
        std::call_once(s_symInitFlag, []() {
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);

            // Search path = directory of the running executable, so the PDB is
            // found even when the cwd differs.
            char exeDir[MAX_PATH] = {};
            GetModuleFileNameA(nullptr, exeDir, MAX_PATH);
            char* lastSlash = std::strrchr(exeDir, '\\');
            if (lastSlash) *lastSlash = '\0';

            SymInitialize(GetCurrentProcess(), exeDir[0] ? exeDir : nullptr, TRUE);
        });
    }
#endif

    void ResolveSymbols() {
        m_frames.resize(static_cast<size_t>(m_capturedFrames));

#ifdef _WIN32
        EnsureSymbolsInitialized();
        HANDLE process = GetCurrentProcess();

        for (size_t i = 0; i < m_frames.size(); ++i) {
            StackFrame& frame = m_frames[i];
            frame.address = reinterpret_cast<uintptr_t>(m_rawAddresses[i]);

            char symBuf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)] = {};
            SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(symBuf);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen   = MAX_SYM_NAME;

            DWORD64 disp64 = 0;
            if (SymFromAddr(process, frame.address, &disp64, symbol)) {
                frame.functionName = symbol->Name;
                frame.displacement = static_cast<int>(disp64);
            }

            IMAGEHLP_LINE64 lineInfo = {};
            lineInfo.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
            DWORD lineDisp = 0;
            if (SymGetLineFromAddr64(process, frame.address, &lineDisp, &lineInfo)) {
                frame.fileName   = lineInfo.FileName;
                frame.lineNumber = static_cast<int>(lineInfo.LineNumber);
            }

            IMAGEHLP_MODULE64 modInfo = {};
            modInfo.SizeOfStruct = sizeof(IMAGEHLP_MODULE64);
            if (SymGetModuleInfo64(process, frame.address, &modInfo)) {
                frame.moduleName = modInfo.ModuleName;
            }
        }

#elif defined(__unix__) || defined(__APPLE__)
        for (size_t i = 0; i < m_frames.size(); ++i) {
            StackFrame& frame = m_frames[i];
            frame.address = reinterpret_cast<uintptr_t>(m_rawAddresses[i]);

            Dl_info dlInfo = {};
            if (dladdr(m_rawAddresses[i], &dlInfo)) {
                if (dlInfo.dli_sname) {
                    int status = 0;
                    char* demangled =
                        abi::__cxa_demangle(dlInfo.dli_sname, nullptr, nullptr, &status);
                    if (status == 0 && demangled) {
                        frame.functionName = demangled;
                        free(demangled);
                    } else {
                        frame.functionName = dlInfo.dli_sname;
                    }
                    if (dlInfo.dli_saddr) {
                        frame.displacement = static_cast<int>(
                            reinterpret_cast<uintptr_t>(m_rawAddresses[i]) -
                            reinterpret_cast<uintptr_t>(dlInfo.dli_saddr));
                    }
                }
                if (dlInfo.dli_fname) {
                    const char* slash = std::strrchr(dlInfo.dli_fname, '/');
                    frame.moduleName = slash ? (slash + 1) : dlInfo.dli_fname;
                }
            } else {
                char** sym = backtrace_symbols(&m_rawAddresses[i], 1);
                if (sym) {
                    frame.functionName = sym[0];
                    free(sym);
                }
            }
        }
#endif
        // Other platforms: m_frames is sized 0 (no capture) — empty trace.
    }
};

}  // namespace rs2v
