#include "Scripting/CSharpEngine.h"
#include "Utils/Logger.h"
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

#ifdef _WIN32
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
#endif
}

bool CSharpEngine::CompileFromSource(const std::wstring& sourceCode, const std::wstring& assemblyName) {
    Logger::Info("Compiling C# source to assembly: %ls", assemblyName.c_str());
    // Note: This example uses CodeDom via COM; for Roslyn you'd add proper metadata references
    HRESULT hr;
    _AppDomainPtr domain(m_appDomain);
    _TypePtr providerType = domain->GetType_2(_bstr_t(L"Microsoft.CSharp.CSharpCodeProvider"), true, true);
    if (!providerType) {
        Logger::Error("Failed to get CSharpCodeProvider type");
        return false;
    }

    _variant_t codeDomProvider = domain->CreateInstance_3(_bstr_t(L"Microsoft.CSharp.CSharpCodeProvider"));
    _ComPtr<IDispatch> pProviderDisp;
    codeDomProvider.pdispVal->QueryInterface(IID_IDispatch, (void**)&pProviderDisp);

    _TypePtr compilerParamsType = domain->GetType_2(_bstr_t(L"System.CodeDom.Compiler.CompilerParameters"), true, true);
    _variant_t paramsObj = domain->CreateInstance_3(_bstr_t(L"System.CodeDom.Compiler.CompilerParameters"));
    _ComPtr<IDispatch> pParamsDisp;
    paramsObj.pdispVal->QueryInterface(IID_IDispatch, (void**)&pParamsDisp);

    // GenerateExecutable = false
    {
        DISPID dispId;
        OLECHAR* name = L"GenerateExecutable";
        pParamsDisp->GetIDsOfNames(IID_NULL, &name, 1, LOCALE_USER_DEFAULT, &dispId);
        VARIANTARG var; VariantInit(&var);
        var.vt = VT_BOOL; var.boolVal = VARIANT_FALSE;
        DISPPARAMS dp{ &var, nullptr, 1, 0 };
        pParamsDisp->Invoke(dispId, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYPUT, &dp, nullptr, nullptr, nullptr);
    }
    // OutputAssembly
    {
        DISPID dispId;
        OLECHAR* name = L"OutputAssembly";
        pParamsDisp->GetIDsOfNames(IID_NULL, &name, 1, LOCALE_USER_DEFAULT, &dispId);
        VARIANTARG var; VariantInit(&var);
        var.vt = VT_BSTR; var.bstrVal = SysAllocString(assemblyName.c_str());
        DISPPARAMS dp{ &var, nullptr, 1, 0 };
        pParamsDisp->Invoke(dispId, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYPUT, &dp, nullptr, nullptr, nullptr);
    }

    // CompileAssemblyFromSource
    DISPID dispCompile;
    OLECHAR* compileName = L"CompileAssemblyFromSource";
    pProviderDisp->GetIDsOfNames(IID_NULL, &compileName, 1, LOCALE_USER_DEFAULT, &dispCompile);

    VARIANTARG args[2];
    VariantInit(&args[0]);
    SAFEARRAYBOUND sab = { 1, 0 };
    SAFEARRAY* psa = SafeArrayCreate(VT_BSTR, 1, &sab);
    LONG idx = 0;
    BSTR srcBstr = SysAllocString(sourceCode.c_str());
    SafeArrayPutElement(psa, &idx, srcBstr);
    args[0].vt = VT_ARRAY | VT_BSTR; args[0].parray = psa;

    VariantInit(&args[1]);
    args[1].vt = VT_DISPATCH; args[1].pdispVal = pParamsDisp;

    DISPPARAMS dpCompile{ args, nullptr, 2, 0 };
    VARIANT result; VariantInit(&result);
    hr = pProviderDisp->Invoke(dispCompile, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD, &dpCompile, &result, nullptr, nullptr);
    SafeArrayDestroy(psa);
    SysFreeString(srcBstr);

    if (FAILED(hr)) {
        Logger::Error("CompileAssemblyFromSource Invoke failed: 0x%08lx", hr);
        return false;
    }

    // Check errors count
    _ComPtr<IDispatch> pResultsDisp(result.pdispVal);
    DISPID dispErrors; OLECHAR* errorsName = L"Errors";
    pResultsDisp->GetIDsOfNames(IID_NULL, &errorsName, 1, LOCALE_USER_DEFAULT, &dispErrors);
    VARIANT varErrors; VariantInit(&varErrors);
    DISPPARAMS dpNoArgs{ nullptr, nullptr, 0,0 };
    pResultsDisp->Invoke(dispErrors, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET, &dpNoArgs, &varErrors, nullptr, nullptr);

    _variant_t vtCount;
    _ComPtr<IDispatch> pErrorsDisp(varErrors.pdispVal);
    DISPID dispCount; OLECHAR* countName = L"Count";
    pErrorsDisp->GetIDsOfNames(IID_NULL, &countName, 1, LOCALE_USER_DEFAULT, &dispCount);
    VARIANT varCount; VariantInit(&varCount);
    pErrorsDisp->Invoke(dispCount, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET, &dpNoArgs, &varCount, nullptr, nullptr);
    vtCount = varCount;

    long errorCount = vtCount.lVal;
    if (errorCount > 0) {
        Logger::Error("Compilation produced %ld errors", errorCount);
        return false;
    }

    Logger::Info("Compiled assembly: %ls successfully", assemblyName.c_str());
    return true;
}