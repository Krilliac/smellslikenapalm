// src/Scripting/CSharpEngine.cpp

#include "Scripting/CSharpEngine.h"
#include "Utils/Logger.h"
#include <mscorlib.tlb>
#include <metahost.h>
#include <windows.h>

#pragma comment(lib, "MSCorEE.lib")

CSharpEngine::CSharpEngine()
    : m_metaHost(nullptr), m_runtimeInfo(nullptr),
      m_clrHost(nullptr), m_appDomain(nullptr)
{
    Logger::Info("CSharpEngine constructed");
}

CSharpEngine::~CSharpEngine()
{
    Shutdown();
}

bool CSharpEngine::Initialize()
{
    Logger::Info("Initializing C# scripting engine (Roslyn through CLR)...");

    HRESULT hr = CLRCreateInstance(CLSID_CLRMetaHost, IID_ICLRMetaHost, (LPVOID*)&m_metaHost);
    if (FAILED(hr) || !m_metaHost) {
        Logger::Error("CLRCreateInstance failed: 0x%08lx", hr);
        return false;
    }

    hr = m_metaHost->GetRuntime(L"v4.0.30319", IID_ICLRRuntimeInfo, (LPVOID*)&m_runtimeInfo);
    if (FAILED(hr) || !m_runtimeInfo) {
        Logger::Error("ICLRMetaHost::GetRuntime failed: 0x%08lx", hr);
        return false;
    }

    BOOL loadable = FALSE;
    hr = m_runtimeInfo->IsLoadable(&loadable);
    if (FAILED(hr) || !loadable) {
        Logger::Error("CLR runtime not loadable: 0x%08lx", hr);
        return false;
    }

    hr = m_runtimeInfo->GetInterface(CLSID_CorRuntimeHost, IID_ICorRuntimeHost, (LPVOID*)&m_clrHost);
    if (FAILED(hr) || !m_clrHost) {
        Logger::Error("ICLRRuntimeInfo::GetInterface failed: 0x%08lx", hr);
        return false;
    }

    hr = m_clrHost->Start();
    if (FAILED(hr)) {
        Logger::Error("ICorRuntimeHost::Start failed: 0x%08lx", hr);
        return false;
    }

    IUnknown* pAppDomainThunk = nullptr;
    hr = m_clrHost->GetDefaultDomain(&pAppDomainThunk);
    if (FAILED(hr) || !pAppDomainThunk) {
        Logger::Error("ICorRuntimeHost::GetDefaultDomain failed: 0x%08lx", hr);
        return false;
    }

    hr = pAppDomainThunk->QueryInterface(IID__AppDomain, (LPVOID*)&m_appDomain);
    pAppDomainThunk->Release();
    if (FAILED(hr) || !m_appDomain) {
        Logger::Error("Failed to get default AppDomain: 0x%08lx", hr);
        return false;
    }

    Logger::Info("C# scripting engine initialized successfully");
    return true;
}

void CSharpEngine::Shutdown()
{
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

bool CSharpEngine::CompileFromSource(const std::wstring& sourceCode, const std::wstring& assemblyName)
{
    Logger::Info("Compiling C# source to assembly: %ws", assemblyName.c_str());

    // Note: For true Roslyn integration, you'd host the C# compiler via
    // a .NET Core or .NET Framework scripting API. Here we use CodeDom via CLR.

    HRESULT hr;
    m_compilerResults.clear();

    _AssemblyPtr pAssembly;
    _AppDomainPtr domain(m_appDomain);

    // Use Microsoft.CSharp.CSharpCodeProvider in-appdomain:
    _TypePtr codeDomProviderType = domain->GetType_2(_bstr_t(L"Microsoft.CSharp.CSharpCodeProvider"), true, true);
    if (!codeDomProviderType) {
        Logger::Error("Failed to get CSharpCodeProvider type");
        return false;
    }

    VARIANT vEmpty;
    VariantInit(&vEmpty);
    _variant_t provider = domain->GetType_2(_bstr_t(L"System.CodeDom.Compiler.CodeDomProvider"), true, true);
    _variant_t compiler = domain->CreateInstance_3(_bstr_t(L"Microsoft.CSharp.CSharpCodeProvider"));

    _ComPtr<IDispatch> pProviderDisp;
    compiler.pdispVal->QueryInterface(IID_IDispatch, (void**)&pProviderDisp);

    // Prepare CompilerParameters
    _TypePtr compilerParamsType = domain->GetType_2(_bstr_t(L"System.CodeDom.Compiler.CompilerParameters"), true, true);
    _variant_t paramsObj = domain->CreateInstance_3(_bstr_t(L"System.CodeDom.Compiler.CompilerParameters"));
    _ComPtr<IDispatch> pParamsDisp;
    paramsObj.pdispVal->QueryInterface(IID_IDispatch, (void**)&pParamsDisp);

    // Set GenerateInMemory = false, OutputAssembly
    {
        DISPID dispId;
        OLECHAR* name = L"GenerateExecutable";
        pParamsDisp->GetIDsOfNames(IID_NULL, &name, 1, LOCALE_USER_DEFAULT, &dispId);
        VARIANTARG arg; VariantInit(&arg);
        arg.vt = VT_BOOL; arg.boolVal = VARIANT_FALSE;
        DISPPARAMS dp{ &arg, nullptr, 1, 0 };
        pParamsDisp->Invoke(dispId, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYPUT, &dp, nullptr, nullptr, nullptr);
    }
    {
        DISPID dispId;
        OLECHAR* name = L"OutputAssembly";
        pParamsDisp->GetIDsOfNames(IID_NULL, &name, 1, LOCALE_USER_DEFAULT, &dispId);
        VARIANTARG arg; VariantInit(&arg);
        arg.vt = VT_BSTR; arg.bstrVal = SysAllocString(assemblyName.c_str());
        DISPPARAMS dp{ &arg, nullptr, 1, 0 };
        pParamsDisp->Invoke(dispId, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYPUT, &dp, nullptr, nullptr, nullptr);
    }

    // Invoke CompileAssemblyFromSource
    DISPID dispCompile;
    OLECHAR* nameCompile = L"CompileAssemblyFromSource";
    pProviderDisp->GetIDsOfNames(IID_NULL, &nameCompile, 1, LOCALE_USER_DEFAULT, &dispCompile);

    VARIANTARG args[2];
    VariantInit(&args[0]); // array of sources
    SAFEARRAYBOUND sab = { 1, 0 };
    SAFEARRAY* psa = SafeArrayCreate(VT_BSTR, 1, &sab);
    LONG idx = 0;
    BSTR bstrSrc = SysAllocString(sourceCode.c_str());
    SafeArrayPutElement(psa, &idx, bstrSrc);
    args[0].vt = VT_ARRAY | VT_BSTR;
    args[0].parray = psa;

    VariantInit(&args[1]);
    args[1].vt = VT_DISPATCH;
    args[1].pdispVal = pParamsDisp;

    DISPPARAMS dpCompile{ args, nullptr, 2, 0 };
    VARIANT result;
    VariantInit(&result);
    hr = pProviderDisp->Invoke(dispCompile, IID_NULL, LOCALE_USER_DEFAULT,
                               DISPATCH_METHOD, &dpCompile, &result, nullptr, nullptr);
    if (FAILED(hr)) {
        Logger::Error("CompileAssemblyFromSource Invoke failed: 0x%08lx", hr);
        return false;
    }

    // result is a CompilerResults object; check Errors property
    _ComPtr<IDispatch> pResultsDisp(result.pdispVal);
    DISPID dispErrors;
    OLECHAR* nameErrors = L"Errors";
    pResultsDisp->GetIDsOfNames(IID_NULL, &nameErrors, 1, LOCALE_USER_DEFAULT, &dispErrors);

    VARIANT varErrors;
    VariantInit(&varErrors);
    DISPPARAMS dpNoArgs{ nullptr, nullptr, 0, 0 };
    pResultsDisp->Invoke(dispErrors, IID_NULL, LOCALE_USER_DEFAULT,
                         DISPATCH_PROPERTYGET, &dpNoArgs, &varErrors, nullptr, nullptr);

    // varErrors is a ReadOnlyCollection<CompilerError>
    _variant_t vtCount;
    {
        _ComPtr<IDispatch> pErrorsDisp(varErrors.pdispVal);
        DISPID dispCount;
        OLECHAR* nameCount = L"Count";
        pErrorsDisp->GetIDsOfNames(IID_NULL, &nameCount, 1, LOCALE_USER_DEFAULT, &dispCount);
        VARIANT varCount; VariantInit(&varCount);
        pErrorsDisp->Invoke(dispCount, IID_NULL, LOCALE_USER_DEFAULT,
                            DISPATCH_PROPERTYGET, &dpNoArgs, &varCount, nullptr, nullptr);
        vtCount = varCount;
    }

    long errorCount = vtCount.lVal;
    if (errorCount > 0) {
        Logger::Error("Compilation produced %ld errors", errorCount);
        // Could iterate and log each error
        return false;
    }

    Logger::Info("Compiled assembly: %ws successfully", assemblyName.c_str());
    return true;
}