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

/// <summary>
/// CSharpEngine encapsulates embedding of the .NET CLR and provides
/// dynamic compilation and execution of C# source code via Roslyn/CodeDom.
/// </summary>
class CSharpEngine {
public:
    /// <summary>Constructor. Call Initialize() before other methods.</summary>
    CSharpEngine();

    /// <summary>Destructor. Calls Shutdown().</summary>
    ~CSharpEngine();

    /// <summary>Initialize the C# scripting engine and host the CLR.</summary>
    bool Initialize();

    /// <summary>Shut down the CLR and release resources.</summary>
    void Shutdown();

    /// <summary>Compile C# source code into an assembly.</summary>
    bool CompileFromSource(const std::wstring& sourceCode,
                           const std::wstring& assemblyName);

private:
#ifdef _WIN32
    ICLRMetaHost*    m_metaHost    = nullptr;
    ICLRRuntimeInfo* m_runtimeInfo = nullptr;
    ICorRuntimeHost* m_clrHost     = nullptr;
    _AppDomain*      m_appDomain   = nullptr;
#endif

    // Disallow copy/assignment
    CSharpEngine(const CSharpEngine&) = delete;
    CSharpEngine& operator=(const CSharpEngine&) = delete;
};