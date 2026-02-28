#include "Scripting/CSharpEngine.h"
#include "Utils/Logger.h"

#ifdef _WIN32
#include <comdef.h>   // _bstr_t, _variant_t
#include <mscorlib.tlb>
#include <metahost.h>
#include <windows.h>
#pragma comment(lib, "MSCorEE.lib")

CSharpEngine::CSharpEngine() {
    Logger::Trace("[CSharpEngine::CSharpEngine] Entering constructor (Windows platform)");
    Logger::Info("CSharpEngine constructed");
    Logger::Trace("[CSharpEngine::CSharpEngine] Constructor complete, member pointers default-initialized");
}

CSharpEngine::~CSharpEngine() {
    Logger::Trace("[CSharpEngine::~CSharpEngine] Entering destructor");
    Logger::Debug("[CSharpEngine::~CSharpEngine] Calling Shutdown() to release CLR resources");
    Shutdown();
    Logger::Trace("[CSharpEngine::~CSharpEngine] Destructor complete");
}

bool CSharpEngine::Initialize() {
    Logger::Trace("[CSharpEngine::Initialize] Entering Initialize()");
    Logger::Info("Initializing C# scripting engine (Roslyn through CLR)...");

    Logger::Debug("[CSharpEngine::Initialize] Attempting CLRCreateInstance for CLSID_CLRMetaHost");
    HRESULT hr = CLRCreateInstance(CLSID_CLRMetaHost, IID_ICLRMetaHost, (LPVOID*)&m_metaHost);
    if (FAILED(hr) || !m_metaHost) {
        Logger::Error("CLRCreateInstance failed: 0x%08lx", hr);
        Logger::Error("[CSharpEngine::Initialize] Unable to create CLR meta host instance, m_metaHost is %s", m_metaHost ? "non-null" : "null");
        Logger::Trace("[CSharpEngine::Initialize] Exiting Initialize() -> returning false (CLRCreateInstance failure)");
        return false;
    }
    Logger::Debug("[CSharpEngine::Initialize] CLRCreateInstance succeeded, m_metaHost=%p, hr=0x%08lx", (void*)m_metaHost, hr);

    Logger::Debug("[CSharpEngine::Initialize] Requesting runtime v4.0.30319 via GetRuntime");
    hr = m_metaHost->GetRuntime(L"v4.0.30319", IID_ICLRRuntimeInfo, (LPVOID*)&m_runtimeInfo);
    if (FAILED(hr) || !m_runtimeInfo) {
        Logger::Error("ICLRMetaHost::GetRuntime failed: 0x%08lx", hr);
        Logger::Error("[CSharpEngine::Initialize] Could not obtain runtime info for v4.0.30319, m_runtimeInfo is %s", m_runtimeInfo ? "non-null" : "null");
        Logger::Debug("[CSharpEngine::Initialize] Releasing m_metaHost before returning");
        m_metaHost->Release();
        Logger::Trace("[CSharpEngine::Initialize] Exiting Initialize() -> returning false (GetRuntime failure)");
        return false;
    }
    Logger::Debug("[CSharpEngine::Initialize] GetRuntime succeeded, m_runtimeInfo=%p, hr=0x%08lx", (void*)m_runtimeInfo, hr);

    BOOL loadable = FALSE;
    Logger::Debug("[CSharpEngine::Initialize] Checking if runtime is loadable via IsLoadable");
    hr = m_runtimeInfo->IsLoadable(&loadable);
    if (FAILED(hr) || !loadable) {
        Logger::Error("CLR runtime not loadable: 0x%08lx", hr);
        Logger::Error("[CSharpEngine::Initialize] Runtime loadable check failed, loadable=%d, hr=0x%08lx", (int)loadable, hr);
        Logger::Debug("[CSharpEngine::Initialize] Releasing m_runtimeInfo and m_metaHost before returning");
        m_runtimeInfo->Release();
        m_metaHost->Release();
        Logger::Trace("[CSharpEngine::Initialize] Exiting Initialize() -> returning false (not loadable)");
        return false;
    }
    Logger::Debug("[CSharpEngine::Initialize] Runtime is loadable, loadable=%d", (int)loadable);

    Logger::Debug("[CSharpEngine::Initialize] Getting ICorRuntimeHost interface via GetInterface");
    hr = m_runtimeInfo->GetInterface(CLSID_CorRuntimeHost, IID_ICorRuntimeHost, (LPVOID*)&m_clrHost);
    if (FAILED(hr) || !m_clrHost) {
        Logger::Error("GetInterface failed: 0x%08lx", hr);
        Logger::Error("[CSharpEngine::Initialize] Failed to get ICorRuntimeHost, m_clrHost is %s", m_clrHost ? "non-null" : "null");
        Logger::Debug("[CSharpEngine::Initialize] Releasing m_runtimeInfo and m_metaHost before returning");
        m_runtimeInfo->Release();
        m_metaHost->Release();
        Logger::Trace("[CSharpEngine::Initialize] Exiting Initialize() -> returning false (GetInterface failure)");
        return false;
    }
    Logger::Debug("[CSharpEngine::Initialize] GetInterface succeeded, m_clrHost=%p", (void*)m_clrHost);

    Logger::Debug("[CSharpEngine::Initialize] Starting CLR host via m_clrHost->Start()");
    hr = m_clrHost->Start();
    if (FAILED(hr)) {
        Logger::Error("CLR Start failed: 0x%08lx", hr);
        Logger::Error("[CSharpEngine::Initialize] CLR host failed to start, hr=0x%08lx", hr);
        Logger::Debug("[CSharpEngine::Initialize] Releasing m_clrHost, m_runtimeInfo, m_metaHost before returning");
        m_clrHost->Release();
        m_runtimeInfo->Release();
        m_metaHost->Release();
        Logger::Trace("[CSharpEngine::Initialize] Exiting Initialize() -> returning false (CLR Start failure)");
        return false;
    }
    Logger::Debug("[CSharpEngine::Initialize] CLR host started successfully");

    IUnknown* pAppDomainThunk = nullptr;
    Logger::Debug("[CSharpEngine::Initialize] Requesting default AppDomain via GetDefaultDomain");
    hr = m_clrHost->GetDefaultDomain(&pAppDomainThunk);
    if (FAILED(hr) || !pAppDomainThunk) {
        Logger::Error("GetDefaultDomain failed: 0x%08lx", hr);
        Logger::Error("[CSharpEngine::Initialize] Could not obtain default AppDomain thunk, pAppDomainThunk is %s", pAppDomainThunk ? "non-null" : "null");
        Logger::Debug("[CSharpEngine::Initialize] Releasing m_clrHost, m_runtimeInfo, m_metaHost before returning");
        m_clrHost->Release();
        m_runtimeInfo->Release();
        m_metaHost->Release();
        Logger::Trace("[CSharpEngine::Initialize] Exiting Initialize() -> returning false (GetDefaultDomain failure)");
        return false;
    }
    Logger::Debug("[CSharpEngine::Initialize] GetDefaultDomain succeeded, pAppDomainThunk=%p", (void*)pAppDomainThunk);

    Logger::Debug("[CSharpEngine::Initialize] Querying IID__AppDomain interface from AppDomain thunk");
    hr = pAppDomainThunk->QueryInterface(IID__AppDomain, (LPVOID*)&m_appDomain);
    pAppDomainThunk->Release();
    Logger::Debug("[CSharpEngine::Initialize] Released pAppDomainThunk after QueryInterface");
    if (FAILED(hr) || !m_appDomain) {
        Logger::Error("QueryInterface for AppDomain failed: 0x%08lx", hr);
        Logger::Error("[CSharpEngine::Initialize] AppDomain QueryInterface failed, m_appDomain is %s, hr=0x%08lx", m_appDomain ? "non-null" : "null", hr);
        Logger::Debug("[CSharpEngine::Initialize] Calling Shutdown() to clean up partial initialization");
        Shutdown();
        Logger::Trace("[CSharpEngine::Initialize] Exiting Initialize() -> returning false (QueryInterface failure)");
        return false;
    }
    Logger::Debug("[CSharpEngine::Initialize] AppDomain obtained successfully, m_appDomain=%p", (void*)m_appDomain);

    Logger::Info("C# scripting engine initialized successfully");
    Logger::Trace("[CSharpEngine::Initialize] Exiting Initialize() -> returning true");
    return true;
}

void CSharpEngine::Shutdown() {
    Logger::Trace("[CSharpEngine::Shutdown] Entering Shutdown()");
    Logger::Info("Shutting down C# scripting engine");

    if (m_appDomain) {
        Logger::Debug("[CSharpEngine::Shutdown] Releasing m_appDomain=%p", (void*)m_appDomain);
        m_appDomain->Release();
        m_appDomain = nullptr;
        Logger::Debug("[CSharpEngine::Shutdown] m_appDomain released and set to nullptr");
    } else {
        Logger::Debug("[CSharpEngine::Shutdown] m_appDomain is already null, skipping release");
    }

    if (m_clrHost) {
        Logger::Debug("[CSharpEngine::Shutdown] Stopping and releasing m_clrHost=%p", (void*)m_clrHost);
        m_clrHost->Stop();
        m_clrHost->Release();
        m_clrHost = nullptr;
        Logger::Debug("[CSharpEngine::Shutdown] m_clrHost stopped, released, and set to nullptr");
    } else {
        Logger::Debug("[CSharpEngine::Shutdown] m_clrHost is already null, skipping stop/release");
    }

    if (m_runtimeInfo) {
        Logger::Debug("[CSharpEngine::Shutdown] Releasing m_runtimeInfo=%p", (void*)m_runtimeInfo);
        m_runtimeInfo->Release();
        m_runtimeInfo = nullptr;
        Logger::Debug("[CSharpEngine::Shutdown] m_runtimeInfo released and set to nullptr");
    } else {
        Logger::Debug("[CSharpEngine::Shutdown] m_runtimeInfo is already null, skipping release");
    }

    if (m_metaHost) {
        Logger::Debug("[CSharpEngine::Shutdown] Releasing m_metaHost=%p", (void*)m_metaHost);
        m_metaHost->Release();
        m_metaHost = nullptr;
        Logger::Debug("[CSharpEngine::Shutdown] m_metaHost released and set to nullptr");
    } else {
        Logger::Debug("[CSharpEngine::Shutdown] m_metaHost is already null, skipping release");
    }

    Logger::Trace("[CSharpEngine::Shutdown] Shutdown() complete");
}

bool CSharpEngine::CompileFromSource(const std::wstring& sourceCode, const std::wstring& assemblyName) {
    Logger::Trace("[CSharpEngine::CompileFromSource] Entering CompileFromSource(sourceCode.length=%zu, assemblyName.length=%zu)",
                  sourceCode.length(), assemblyName.length());
    Logger::Debug("[CSharpEngine::CompileFromSource] Windows COM implementation omitted for brevity, returning false");
    // Windows COM implementation omitted for brevity
    Logger::Trace("[CSharpEngine::CompileFromSource] Exiting CompileFromSource() -> returning false");
    return false;
}

#else // Non-Windows stub

CSharpEngine::CSharpEngine() {
    Logger::Trace("[CSharpEngine::CSharpEngine] Entering constructor (non-Windows stub platform)");
    Logger::Info("CSharpEngine constructed (stub - not available on this platform)");
    Logger::Trace("[CSharpEngine::CSharpEngine] Constructor complete (stub)");
}

CSharpEngine::~CSharpEngine() {
    Logger::Trace("[CSharpEngine::~CSharpEngine] Entering destructor (non-Windows stub)");
    Logger::Debug("[CSharpEngine::~CSharpEngine] Calling Shutdown() from destructor (stub)");
    Shutdown();
    Logger::Trace("[CSharpEngine::~CSharpEngine] Destructor complete (stub)");
}

bool CSharpEngine::Initialize() {
    Logger::Trace("[CSharpEngine::Initialize] Entering Initialize() (non-Windows stub)");
    Logger::Warn("CSharpEngine: C# scripting is not available on non-Windows platforms");
    Logger::Debug("[CSharpEngine::Initialize] Platform does not support CLR hosting, initialization is a no-op");
    Logger::Trace("[CSharpEngine::Initialize] Exiting Initialize() -> returning false (non-Windows stub)");
    return false;
}

void CSharpEngine::Shutdown() {
    Logger::Trace("[CSharpEngine::Shutdown] Entering Shutdown() (non-Windows stub)");
    Logger::Debug("[CSharpEngine::Shutdown] No-op on non-Windows platform, nothing to release");
    // No-op on non-Windows
    Logger::Trace("[CSharpEngine::Shutdown] Exiting Shutdown() (non-Windows stub)");
}

bool CSharpEngine::CompileFromSource(const std::wstring& /*sourceCode*/, const std::wstring& /*assemblyName*/) {
    Logger::Trace("[CSharpEngine::CompileFromSource] Entering CompileFromSource() (non-Windows stub)");
    Logger::Warn("CSharpEngine: CompileFromSource not available on non-Windows platforms");
    Logger::Debug("[CSharpEngine::CompileFromSource] CLR compilation requires Windows platform, returning false");
    Logger::Trace("[CSharpEngine::CompileFromSource] Exiting CompileFromSource() -> returning false (non-Windows stub)");
    return false;
}

#endif // _WIN32
