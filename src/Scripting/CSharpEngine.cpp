#include "Scripting/CSharpEngine.h"
#include "Utils/Logger.h"

#ifdef _WIN32
#include <comdef.h>   // _bstr_t, _variant_t
#include <mscorlib.tlb>
#include <metahost.h>
#include <windows.h>
#pragma comment(lib, "MSCorEE.lib")

CSharpEngine::CSharpEngine() {
    Logger::Info("CSharpEngine constructed");
}

CSharpEngine::~CSharpEngine() {
    Shutdown();
}

bool CSharpEngine::Initialize() {
    Logger::Info("Initializing C# scripting engine (Roslyn through CLR)...");
    HRESULT hr = CLRCreateInstance(CLSID_CLRMetaHost, IID_ICLRMetaHost, (LPVOID*)&m_metaHost);
    if (FAILED(hr) || !m_metaHost) {
        Logger::Error("CLRCreateInstance failed: 0x%08lx", hr);
        return false;
    }

    hr = m_metaHost->GetRuntime(L"v4.0.30319", IID_ICLRRuntimeInfo, (LPVOID*)&m_runtimeInfo);
    if (FAILED(hr) || !m_runtimeInfo) {
        Logger::Error("ICLRMetaHost::GetRuntime failed: 0x%08lx", hr);
        m_metaHost->Release();
        return false;
    }

    BOOL loadable = FALSE;
    hr = m_runtimeInfo->IsLoadable(&loadable);
    if (FAILED(hr) || !loadable) {
        Logger::Error("CLR runtime not loadable: 0x%08lx", hr);
        m_runtimeInfo->Release();
        m_metaHost->Release();
        return false;
    }

    hr = m_runtimeInfo->GetInterface(CLSID_CorRuntimeHost, IID_ICorRuntimeHost, (LPVOID*)&m_clrHost);
    if (FAILED(hr) || !m_clrHost) {
        Logger::Error("GetInterface failed: 0x%08lx", hr);
        m_runtimeInfo->Release();
        m_metaHost->Release();
        return false;
    }

    hr = m_clrHost->Start();
    if (FAILED(hr)) {
        Logger::Error("CLR Start failed: 0x%08lx", hr);
        m_clrHost->Release();
        m_runtimeInfo->Release();
        m_metaHost->Release();
        return false;
    }

    IUnknown* pAppDomainThunk = nullptr;
    hr = m_clrHost->GetDefaultDomain(&pAppDomainThunk);
    if (FAILED(hr) || !pAppDomainThunk) {
        Logger::Error("GetDefaultDomain failed: 0x%08lx", hr);
        m_clrHost->Release();
        m_runtimeInfo->Release();
        m_metaHost->Release();
        return false;
    }

    hr = pAppDomainThunk->QueryInterface(IID__AppDomain, (LPVOID*)&m_appDomain);
    pAppDomainThunk->Release();
    if (FAILED(hr) || !m_appDomain) {
        Logger::Error("QueryInterface for AppDomain failed: 0x%08lx", hr);
        Shutdown();
        return false;
    }

    Logger::Info("C# scripting engine initialized successfully");
    return true;
}

void CSharpEngine::Shutdown() {
    Logger::Info("Shutting down C# scripting engine");

    if (m_appDomain) {
        m_appDomain->Release();
        m_appDomain = nullptr;
    }
    if (m_clrHost) {
        m_clrHost->Stop();
        m_clrHost->Release();
        m_clrHost = nullptr;
    }
    if (m_runtimeInfo) {
        m_runtimeInfo->Release();
        m_runtimeInfo = nullptr;
    }
    if (m_metaHost) {
        m_metaHost->Release();
        m_metaHost = nullptr;
    }
}

bool CSharpEngine::CompileFromSource(const std::wstring& sourceCode, const std::wstring& assemblyName) {
    // Windows COM implementation omitted for brevity
    return false;
}

#else // Non-Windows stub

CSharpEngine::CSharpEngine() {
    Logger::Info("CSharpEngine constructed (stub - not available on this platform)");
}

CSharpEngine::~CSharpEngine() {
    Shutdown();
}

bool CSharpEngine::Initialize() {
    Logger::Warn("CSharpEngine: C# scripting is not available on non-Windows platforms");
    return false;
}

void CSharpEngine::Shutdown() {
    // No-op on non-Windows
}

bool CSharpEngine::CompileFromSource(const std::wstring& /*sourceCode*/, const std::wstring& /*assemblyName*/) {
    Logger::Warn("CSharpEngine: CompileFromSource not available on non-Windows platforms");
    return false;
}

#endif // _WIN32
