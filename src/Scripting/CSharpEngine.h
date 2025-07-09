// src/Scripting/CSharpEngine.h

#pragma once

#ifdef _WIN32
  #include <windows.h>
  #include <metahost.h>         // CLR hosting interfaces
  #import "mscorlib.tlb" raw_interfaces_only                \
           high_property_prefixes("_get","_put","_putref") \
           rename("ReportEvent", "InteropServices_ReportEvent")
#else
  // Mono hosting headers would go here for non-Windows platforms
#endif

#include <string>
#include <vector>

/// <summary>
/// CSharpEngine encapsulates the embedding of the .NET CLR and provides
/// dynamic compilation and execution of C# source code via Roslyn/CodeDom.
/// </summary>
class CSharpEngine {
public:
    /// <summary>
    /// Constructor. Does not initialize CLR; call Initialize() first.
    /// </summary>
    CSharpEngine();

    /// <summary>
    /// Destructor. Calls Shutdown() to tear down CLR if still running.
    /// </summary>
    ~CSharpEngine();

    /// <summary>
    /// Initialize the C# scripting engine and host the CLR in-process.
    /// Must be called before any other methods.
    /// </summary>
    /// <returns>True on success, false on failure.</returns>
    bool Initialize();

    /// <summary>
    /// Shut down the CLR and release all hosting resources.
    /// Safe to call multiple times.
    /// </summary>
    void Shutdown();

    /// <summary>
    /// Compile the given C# source code into an assembly on disk or in-memory.
    /// </summary>
    /// <param name="sourceCode">Full C# source text.</param>
    /// <param name="assemblyName">Desired name/path of the output assembly.</param>
    /// <returns>True if compilation succeeded without errors.</returns>
    bool CompileFromSource(const std::wstring& sourceCode,
                           const std::wstring& assemblyName);

private:
#ifdef _WIN32
    // CLR hosting interfaces
    ICLRMetaHost*        m_metaHost;    // CLR MetaHost
    ICLRRuntimeInfo*     m_runtimeInfo; // .NET runtime info
    ICorRuntimeHost*     m_clrHost;     // Runtime host interface
    _AppDomain*          m_appDomain;   // Default AppDomain
#endif

    // Disallow copy and assignment
    CSharpEngine(const CSharpEngine&) = delete;
    CSharpEngine& operator=(const CSharpEngine&) = delete;
};