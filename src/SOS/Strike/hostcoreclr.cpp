// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

// ==++==
// 

// 
// ==--==
#include "sos.h"
#include "disasm.h"
#include <dbghelp.h>

#include "corhdr.h"
#include "cor.h"
#include "dacprivate.h"
#include "sospriv.h"
#include "corerror.h"
#include "safemath.h"

#include <psapi.h>
#include <tchar.h>
#include <limits.h>

#ifdef FEATURE_PAL
#include <sys/stat.h>
#include <dlfcn.h>
#include <unistd.h>
#endif // !FEATURE_PAL

#include <coreclrhost.h>
#include <set>
#include <string>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#ifndef IfFailRet
#define IfFailRet(EXPR) do { Status = (EXPR); if(FAILED(Status)) { return (Status); } } while (0)
#endif

static bool g_hostingInitialized = false;
static bool g_symbolStoreInitialized = false;
LPCSTR g_hostRuntimeDirectory = nullptr;
LPCSTR g_dacFilePath = nullptr;
LPCSTR g_dbiFilePath = nullptr;
LPCSTR g_tmpPath = nullptr;
SOSNetCoreCallbacks g_SOSNetCoreCallbacks;

#ifdef FEATURE_PAL
#define TPALIST_SEPARATOR_STR_A ":"
#else
#define TPALIST_SEPARATOR_STR_A ";"
#endif

//
// Build the TPA list of assemblies for the runtime hosting api.
//
static void AddFilesFromDirectoryToTpaList(const char* directory, std::string& tpaList)
{
    const char * const tpaExtensions[] = {
        "*.ni.dll",      // Probe for .ni.dll first so that it's preferred if ni and il coexist in the same dir
        "*.dll",
    };
    std::set<std::string> addedAssemblies;

    // Don't add this file to the list because we don't want to the one from the hosting runtime
    addedAssemblies.insert(SOSManagedDllName);

    // Walk the directory for each extension separately so that we first get files with .ni.dll extension,
    // then files with .dll extension, etc.
    for (int extIndex = 0; extIndex < sizeof(tpaExtensions) / sizeof(tpaExtensions[0]); extIndex++)
    {
        const char* ext = tpaExtensions[extIndex];
        size_t extLength = strlen(ext) - 1;         // don't count the "*"

        std::string assemblyPath(directory);
        assemblyPath.append(DIRECTORY_SEPARATOR_STR_A);
        assemblyPath.append(tpaExtensions[extIndex]);

        WIN32_FIND_DATAA data;
        HANDLE findHandle = FindFirstFileA(assemblyPath.c_str(), &data);

        if (findHandle != INVALID_HANDLE_VALUE) 
        {
            do
            {
                if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                {
                    std::string filename(data.cFileName);
                    size_t extPos = filename.length() - extLength;
                    std::string filenameWithoutExt(filename.substr(0, extPos));

                    // Make sure if we have an assembly with multiple extensions present,
                    // we insert only one version of it.
                    if (addedAssemblies.find(filenameWithoutExt) == addedAssemblies.end())
                    {
                        addedAssemblies.insert(filenameWithoutExt);

                        tpaList.append(directory);
                        tpaList.append(DIRECTORY_SEPARATOR_STR_A);
                        tpaList.append(filename);
                        tpaList.append(TPALIST_SEPARATOR_STR_A);
                    }
                }
            } 
            while (0 != FindNextFileA(findHandle, &data));

            FindClose(findHandle);
        }
    }
}

#ifdef FEATURE_PAL

#if defined(__linux__)
#define symlinkEntrypointExecutable "/proc/self/exe"
#elif !defined(__APPLE__)
#define symlinkEntrypointExecutable "/proc/curproc/exe"
#endif

static bool GetAbsolutePath(const char* path, std::string& absolutePath)
{
    bool result = false;

    char realPath[PATH_MAX];
    if (realpath(path, realPath) != nullptr && realPath[0] != '\0')
    {
        absolutePath.assign(realPath);
        // realpath should return canonicalized path without the trailing slash
        assert(absolutePath.back() != '/');

        result = true;
    }

    return result;
}

static bool GetEntrypointExecutableAbsolutePath(std::string& entrypointExecutable)
{
    bool result = false;
    
    entrypointExecutable.clear();

    // Get path to the executable for the current process using
    // platform specific means.
#if defined(__APPLE__)
    
    // On Mac, we ask the OS for the absolute path to the entrypoint executable
    uint32_t lenActualPath = 0;
    if (_NSGetExecutablePath(nullptr, &lenActualPath) == -1)
    {
        // OSX has placed the actual path length in lenActualPath,
        // so re-attempt the operation
        std::string resizedPath(lenActualPath, '\0');
        char *pResizedPath = const_cast<char *>(resizedPath.c_str());
        if (_NSGetExecutablePath(pResizedPath, &lenActualPath) == 0)
        {
            entrypointExecutable.assign(pResizedPath);
            result = true;
        }
    }
#elif defined (__FreeBSD__)
    static const int name[] = {
        CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1
    };
    char path[PATH_MAX];
    size_t len;

    len = sizeof(path);
    if (sysctl(name, 4, path, &len, nullptr, 0) == 0)
    {
        entrypointExecutable.assign(path);
        result = true;
    }
    else
    {
        // ENOMEM
        result = false;
    }
#elif defined(__NetBSD__) && defined(KERN_PROC_PATHNAME)
    static const int name[] = {
        CTL_KERN, KERN_PROC_ARGS, -1, KERN_PROC_PATHNAME,
    };
    char path[MAXPATHLEN];
    size_t len;

    len = sizeof(path);
    if (sysctl(name, __arraycount(name), path, &len, NULL, 0) != -1)
    {
        entrypointExecutable.assign(path);
        result = true;
    }
    else
    {
        result = false;
    }
#else
    // On other OSs, return the symlink that will be resolved by GetAbsolutePath
    // to fetch the entrypoint EXE absolute path, inclusive of filename.
    result = GetAbsolutePath(symlinkEntrypointExecutable, entrypointExecutable);
#endif 

    return result;
}

#else // FEATURE_PAL

static bool GetEntrypointExecutableAbsolutePath(std::string& entrypointExecutable)
{
    ArrayHolder<char> hostPath = new char[MAX_LONGPATH+1];
    if (::GetModuleFileName(NULL, hostPath, MAX_LONGPATH) == 0)
    {
        return false;
    }

    entrypointExecutable.clear();
    entrypointExecutable.append(hostPath);

    return true;
}

#endif // FEATURE_PAL

/**********************************************************************\
 * Returns the coreclr module/runtime directory of the target.
\**********************************************************************/
static HRESULT GetCoreClrDirectory(std::string& coreClrDirectory)
{
#ifdef FEATURE_PAL
    LPCSTR directory = g_ExtServices->GetCoreClrDirectory();
    if (directory == NULL)
    {
        ExtErr("Error: Runtime module (%s) not loaded yet\n", MAKEDLLNAME_A("coreclr"));
        return E_FAIL;
    }
    if (!GetAbsolutePath(directory, coreClrDirectory))
    {
        return E_FAIL;
    }
#else
    ULONG index;
    HRESULT Status = g_ExtSymbols->GetModuleByModuleName(MAIN_CLR_MODULE_NAME_A, 0, &index, NULL);
    if (FAILED(Status))
    {
        ExtErr("Error: Runtime module (%s) not loaded yet\n", MAKEDLLNAME_A("coreclr"));
        return Status;
    }
    ArrayHolder<char> szModuleName = new char[MAX_LONGPATH + 1];
    Status = g_ExtSymbols->GetModuleNames(index, 0, szModuleName, MAX_LONGPATH, NULL, NULL, 0, NULL, NULL, 0, NULL);
    if (FAILED(Status))
    {
        ExtErr("Error: Failed to get coreclr module name\n");
        return Status;
    }
    if (GetFileAttributesA(szModuleName) == INVALID_FILE_ATTRIBUTES)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    coreClrDirectory = szModuleName;

    // Parse off the module name to get just the path
    size_t lastSlash = coreClrDirectory.rfind(DIRECTORY_SEPARATOR_CHAR_A);
    if (lastSlash == std::string::npos)
    {
        return E_FAIL;
    }
    coreClrDirectory.assign(coreClrDirectory, 0, lastSlash);
#endif
    return S_OK;
}

/**********************************************************************\
 * Returns the coreclr module/runtime directory of the target.
\**********************************************************************/
HRESULT GetCoreClrDirectory(LPWSTR modulePath, int modulePathSize)
{
    std::string coreclrDirectory;
    HRESULT hr = GetCoreClrDirectory(coreclrDirectory);
    if (FAILED(hr))
    {
        return hr;
    }
    int length = MultiByteToWideChar(CP_ACP, 0, coreclrDirectory.c_str(), -1, modulePath, modulePathSize);
    if (0 >= length)
    {
        ExtErr("MultiByteToWideChar(coreclrDirectory) failed. Last error = 0x%x\n", GetLastError());
        return HRESULT_FROM_WIN32(GetLastError());
    }
    return S_OK;
}

//
// Searches the runtime directory for a .NET Core runtime version
//
static bool FindDotNetVersion(int majorFilter, int minorFilter, std::string& hostRuntimeDirectory)
{
    std::string directory(hostRuntimeDirectory);
    directory.append("*");
    std::string versionFound;

    WIN32_FIND_DATAA data;
    HANDLE findHandle = FindFirstFileA(directory.c_str(), &data);

    if (findHandle != INVALID_HANDLE_VALUE) 
    {
        int highestRevision = 0;
        do
        {
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                int major = 0;
                int minor = 0;
                int revision = 0;
                if (sscanf_s(data.cFileName, "%d.%d.%d", &major, &minor, &revision) == 3)
                {
                    if (major == majorFilter && minor == minorFilter)
                    {
                        if (revision >= highestRevision)
                        {
                            highestRevision = revision;
                            versionFound.assign(data.cFileName);
                        }
                    }
                }
            }
        } 
        while (0 != FindNextFileA(findHandle, &data));

        FindClose(findHandle);
    }

    if (versionFound.length() > 0)
    {
        hostRuntimeDirectory.append(versionFound);
        return true;
    }

    return false;
}

#ifdef FEATURE_PAL
const char *g_linuxPaths[] = {
//  "/rh-dotnet22/root/usr/bin/dotnet/shared/Microsoft.NETCore.App",
    "/rh-dotnet21/root/usr/bin/dotnet/shared/Microsoft.NETCore.App",
    "/rh-dotnet20/root/usr/bin/dotnet/shared/Microsoft.NETCore.App",
    "/usr/share/dotnet/shared/Microsoft.NETCore.App",
};
#endif

/**********************************************************************\
 * Returns the path to the coreclr to use for hosting and it's
 * directory. Attempts to use the best installed version of the 
 * runtime, otherwise it defaults to the target's runtime version.
\**********************************************************************/
static HRESULT GetHostRuntime(std::string& coreClrPath, std::string& hostRuntimeDirectory)
{
    // If the hosting runtime isn't already set, use the runtime we are debugging
    if (g_hostRuntimeDirectory == nullptr)
    {
#ifdef FEATURE_PAL
#if defined(__APPLE__)
        hostRuntimeDirectory.assign("/usr/local/share/dotnet/shared/Microsoft.NETCore.App");
#elif defined (__FreeBSD__) || defined(__NetBSD__)
        ExtErr("FreeBSD or NetBSD not supported\n");
        return E_FAIL;
#else
        // Start with the possible RHEL's locations, then the regular Linux path
        for (int i = 0; i < _countof(g_linuxPaths); i++)
        {
            hostRuntimeDirectory.assign(g_linuxPaths[i]);
            if (access(hostRuntimeDirectory.c_str(), F_OK) == 0)
            {
                break;
            }
        }
#endif
#else
        ArrayHolder<CHAR> programFiles = new CHAR[MAX_LONGPATH];
        if (GetEnvironmentVariableA("PROGRAMFILES", programFiles, MAX_LONGPATH) == 0)
        {
            ExtErr("PROGRAMFILES environment variable not found\n");
            return E_FAIL;
        }
        hostRuntimeDirectory.assign(programFiles);
        hostRuntimeDirectory.append("\\dotnet\\shared\\Microsoft.NETCore.App");
#endif
        hostRuntimeDirectory.append(DIRECTORY_SEPARATOR_STR_A);

        // First attempt find the highest 2.1.x version. We want to start with the LTS
        // and only use the higher versions if it isn't installed.
        if (!FindDotNetVersion(2, 1, hostRuntimeDirectory))
        {
            // Find highest 2.2.x version
            if (!FindDotNetVersion(2, 2, hostRuntimeDirectory))
            {
                // Find highest 3.0.x version
                if (!FindDotNetVersion(3, 0, hostRuntimeDirectory))
                {
                    // If an installed runtime can not be found, use the target coreclr version
                    HRESULT hr = GetCoreClrDirectory(hostRuntimeDirectory);
                    if (FAILED(hr))
                    {
                        return hr;
                    }
                }
            }
        }

        // Save away the runtime version we are going to use to host the SOS managed code
        g_hostRuntimeDirectory = _strdup(hostRuntimeDirectory.c_str());
    }
    hostRuntimeDirectory.assign(g_hostRuntimeDirectory);
    coreClrPath.assign(g_hostRuntimeDirectory);
    coreClrPath.append(DIRECTORY_SEPARATOR_STR_A);
    coreClrPath.append(MAIN_CLR_DLL_NAME_A);
    return S_OK;
}

#ifndef FEATURE_PAL
/**********************************************************************\
 * Returns the path to the runtime directory to use for hosting. 
\**********************************************************************/
LPCSTR
GetHostRuntimeDirectory()
{
    std::string hostRuntimeDirectory;
    std::string coreClrPath;

    HRESULT Status = GetHostRuntime(coreClrPath, hostRuntimeDirectory);
    if (FAILED(Status))
    {
        return nullptr;
    }
    return hostRuntimeDirectory.c_str();
}
#endif // FEATURE_PAL

//
// Returns the unique temporary directory for this instnace of SOS
//
static LPCSTR GetTempDirectory()
{
    if (g_tmpPath == nullptr)
    {
        char tmpPath[MAX_LONGPATH];
        if (::GetTempPathA(MAX_LONGPATH, tmpPath) == 0)
        {
            strcpy_s(tmpPath, MAX_LONGPATH, ".");
            strcat_s(tmpPath, MAX_LONGPATH, DIRECTORY_SEPARATOR_STR_A);
        }
        char pidstr[128];
        sprintf_s(pidstr, _countof(pidstr), "sos%d", GetCurrentProcessId());
        strcat_s(tmpPath, MAX_LONGPATH, pidstr);
        strcat_s(tmpPath, MAX_LONGPATH, DIRECTORY_SEPARATOR_STR_A);

        CreateDirectoryA(tmpPath, NULL);
        g_tmpPath = _strdup(tmpPath);
    }
    return g_tmpPath;
}

/**********************************************************************\
 * Clean up the temporary directory files and DAC symlink.
\**********************************************************************/
#ifdef FEATURE_PAL
__attribute__((destructor)) 
#endif
void SOSShutdown()
{
    LPCSTR tmpPath = (LPCSTR)InterlockedExchangePointer((PVOID *)&g_tmpPath, nullptr);
    if (tmpPath != nullptr)
    {
        std::string directory(tmpPath);
        directory.append("*");

        WIN32_FIND_DATAA data;
        HANDLE findHandle = FindFirstFileA(directory.c_str(), &data);

        if (findHandle != INVALID_HANDLE_VALUE) 
        {
            do
            {
                if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                {
                    std::string file(tmpPath);
                    file.append(data.cFileName);
                    DeleteFileA(file.c_str());
                }
            } 
            while (0 != FindNextFileA(findHandle, &data));

            FindClose(findHandle);
        }

        RemoveDirectoryA(tmpPath);
        free((void*)tmpPath);
    }
}

/**********************************************************************\
 * Returns the DAC module path to the rest of SOS.
\**********************************************************************/
LPCSTR GetDacFilePath()
{
    // If the DAC path hasn't been set by the symbol download support, use the one in the runtime directory.
    if (g_dacFilePath == nullptr)
    {
        std::string dacModulePath;
        HRESULT hr = GetCoreClrDirectory(dacModulePath);
        if (SUCCEEDED(hr))
        {
            dacModulePath.append(DIRECTORY_SEPARATOR_STR_A);
            dacModulePath.append(MAKEDLLNAME_A("mscordaccore"));
#ifdef FEATURE_PAL
            // if DAC file exists
            if (access(dacModulePath.c_str(), F_OK) == 0)
#endif
            {
#if defined(__linux__)
                // We are creating a symlink to the DAC in a temp directory
                // where libcoreclrtraceptprovider.so doesn't exist so it 
                // doesn't get loaded by the DAC causing a LTTng-UST exception.
                //
                // Issue #https://github.com/dotnet/coreclr/issues/20205
                LPCSTR tmpPath = GetTempDirectory();
                if (tmpPath != nullptr) 
                {
                    std::string dacSymLink(tmpPath);
                    dacSymLink.append(MAKEDLLNAME_A("mscordaccore"));

                    int error = symlink(dacModulePath.c_str(), dacSymLink.c_str());
                    if (error == 0)
                    {
                        dacModulePath.assign(dacSymLink);
                    }
                    else
                    {
                        ExtErr("symlink(%s, %s) FAILED %s\n", dacModulePath.c_str(), dacSymLink.c_str(), strerror(errno));
                    }
                }
#endif
                g_dacFilePath = _strdup(dacModulePath.c_str());
            }
        }

        if (g_dacFilePath == nullptr)
        {
            // Attempt to only load the DAC/DBI modules
            LoadNativeSymbols(true);
        }
    }
    return g_dacFilePath;
}

/**********************************************************************\
 * Returns the DBI module path to the rest of SOS.
\**********************************************************************/
LPCSTR GetDbiFilePath()
{
    if (g_dbiFilePath == nullptr)
    {
        std::string dbiModulePath;
        HRESULT hr = GetCoreClrDirectory(dbiModulePath);
        if (SUCCEEDED(hr))
        {
            dbiModulePath.append(DIRECTORY_SEPARATOR_STR_A);
            dbiModulePath.append(MAKEDLLNAME_A("mscordbi"));
#ifdef FEATURE_PAL
            // if DBI file exists
            if (access(dbiModulePath.c_str(), F_OK) == 0)
#endif
            {
                g_dbiFilePath = _strdup(dbiModulePath.c_str());
            }
        }

        if (g_dbiFilePath == nullptr)
        {
            // Attempt to only load the DAC/DBI modules
            LoadNativeSymbols(true);
        }
    }
    return g_dbiFilePath;
}

/**********************************************************************\
 * Called when the managed SOS Host loads/initializes SOS.
\**********************************************************************/
extern "C" HRESULT SOSInitializeByHost(SOSNetCoreCallbacks* callbacks, int callbacksSize, LPCSTR tempDirectory, LPCSTR dacFilePath, LPCSTR dbiFilePath, bool symbolStoreEnabled)
{
    if (memcpy_s(&g_SOSNetCoreCallbacks, sizeof(g_SOSNetCoreCallbacks), callbacks, callbacksSize) != 0)
    {
        return E_INVALIDARG;
    }
    if (tempDirectory != nullptr)
    {
        g_tmpPath = _strdup(tempDirectory);
    }
    if (dacFilePath != nullptr)
    {
        g_dacFilePath = _strdup(dacFilePath);
    }
    if (dbiFilePath != nullptr)
    {
        g_dbiFilePath = _strdup(dbiFilePath);
    }
#ifndef FEATURE_PAL
    // When SOS is hosted on dotnet-dump, the ExtensionApis are not set so 
    // the expression evaluation function needs to be supplied.
    GetExpression = (PWINDBG_GET_EXPRESSION64)callbacks->GetExpressionDelegate;
#endif
    g_symbolStoreInitialized = symbolStoreEnabled;
    g_hostingInitialized = true;
    return S_OK;
}

/**********************************************************************\
 * Returns true if the host runtime has already been initialized.
\**********************************************************************/
BOOL IsHostingInitialized()
{
    return g_hostingInitialized;
}

/**********************************************************************\
 * Initializes the host coreclr runtime and gets the managed entry 
 * points delegates.
\**********************************************************************/
HRESULT InitializeHosting()
{
    if (g_hostingInitialized)
    {
        return S_OK;
    }
    coreclr_initialize_ptr initializeCoreCLR = nullptr;
    coreclr_create_delegate_ptr createDelegate = nullptr;
    std::string hostRuntimeDirectory;
    std::string sosModuleDirectory;
    std::string coreClrPath;

    HRESULT Status = GetHostRuntime(coreClrPath, hostRuntimeDirectory);
    if (FAILED(Status))
    {
        return Status;
    }
#ifdef FEATURE_PAL
    ArrayHolder<char> szSOSModulePath = new char[MAX_LONGPATH + 1];
    UINT cch = MAX_LONGPATH;
    if (!PAL_GetPALDirectoryA(szSOSModulePath, &cch)) {
        ExtErr("Error: Failed to get SOS module directory\n");
        return E_FAIL;
    }
    sosModuleDirectory = szSOSModulePath;

    void* coreclrLib = dlopen(coreClrPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (coreclrLib == nullptr)
    {
        ExtErr("Error: Failed to load %s\n", coreClrPath.c_str());
        return E_FAIL;
    }
    initializeCoreCLR = (coreclr_initialize_ptr)dlsym(coreclrLib, "coreclr_initialize");
    createDelegate = (coreclr_create_delegate_ptr)dlsym(coreclrLib, "coreclr_create_delegate");
#else
    ArrayHolder<char> szSOSModulePath = new char[MAX_LONGPATH + 1];
    if (GetModuleFileNameA(g_hInstance, szSOSModulePath, MAX_LONGPATH) == 0)
    {
        ExtErr("Error: Failed to get SOS module directory\n");
        return E_FAIL;
    }
    sosModuleDirectory = szSOSModulePath;

    // Get just the sos module directory
    size_t lastSlash = sosModuleDirectory.rfind(DIRECTORY_SEPARATOR_CHAR_A);
    if (lastSlash == std::string::npos)
    {
        ExtErr("Error: Failed to parse sos module name\n");
        return E_FAIL;
    }
    sosModuleDirectory.erase(lastSlash);

    HMODULE coreclrLib = LoadLibraryA(coreClrPath.c_str());
    if (coreclrLib == nullptr)
    {
        ExtErr("Error: Failed to load %s\n", coreClrPath.c_str());
        return E_FAIL;
    }
    initializeCoreCLR = (coreclr_initialize_ptr)GetProcAddress(coreclrLib, "coreclr_initialize");
    createDelegate = (coreclr_create_delegate_ptr)GetProcAddress(coreclrLib, "coreclr_create_delegate");
#endif // FEATURE_PAL

    if (initializeCoreCLR == nullptr || createDelegate == nullptr)
    {
        ExtErr("Error: coreclr_initialize or coreclr_create_delegate not found\n");
        return E_FAIL;
    }

    // Trust The SOS managed and dependent assemblies from the sos directory
    std::string tpaList;
    AddFilesFromDirectoryToTpaList(sosModuleDirectory.c_str(), tpaList);

    // Trust the runtime assemblies
    AddFilesFromDirectoryToTpaList(hostRuntimeDirectory.c_str(), tpaList);

    std::string appPaths;
    appPaths.append(sosModuleDirectory);
    appPaths.append(TPALIST_SEPARATOR_STR_A);
    appPaths.append(hostRuntimeDirectory);

    const char *propertyKeys[] = {
        "TRUSTED_PLATFORM_ASSEMBLIES", "APP_PATHS", "APP_NI_PATHS",
        "NATIVE_DLL_SEARCH_DIRECTORIES", "AppDomainCompatSwitch"};

    const char *propertyValues[] = {// TRUSTED_PLATFORM_ASSEMBLIES
                                    tpaList.c_str(),
                                    // APP_PATHS
                                    appPaths.c_str(),
                                    // APP_NI_PATHS
                                    hostRuntimeDirectory.c_str(),
                                    // NATIVE_DLL_SEARCH_DIRECTORIES
                                    appPaths.c_str(),
                                    // AppDomainCompatSwitch
                                    "UseLatestBehaviorWhenTFMNotSpecified"};

    std::string entryPointExecutablePath;
    if (!GetEntrypointExecutableAbsolutePath(entryPointExecutablePath))
    {
        ExtErr("Could not get full path to current executable");
        return E_FAIL;
    }

    void *hostHandle;
    unsigned int domainId;
    Status = initializeCoreCLR(entryPointExecutablePath.c_str(), "sos", 
        sizeof(propertyKeys) / sizeof(propertyKeys[0]), propertyKeys, propertyValues, &hostHandle, &domainId);

    if (FAILED(Status))
    {
        ExtErr("Error: Fail to initialize CoreCLR %08x\n", Status);
        return Status;
    }

    IfFailRet(createDelegate(hostHandle, domainId, SOSManagedDllName, SymbolReaderClassName, "InitializeSymbolStore", (void **)&g_SOSNetCoreCallbacks.InitializeSymbolStoreDelegate));
    IfFailRet(createDelegate(hostHandle, domainId, SOSManagedDllName, SymbolReaderClassName, "DisplaySymbolStore", (void **)&g_SOSNetCoreCallbacks.DisplaySymbolStoreDelegate));
    IfFailRet(createDelegate(hostHandle, domainId, SOSManagedDllName, SymbolReaderClassName, "DisableSymbolStore", (void **)&g_SOSNetCoreCallbacks.DisableSymbolStoreDelegate));
    IfFailRet(createDelegate(hostHandle, domainId, SOSManagedDllName, SymbolReaderClassName, "LoadNativeSymbols", (void **)&g_SOSNetCoreCallbacks.LoadNativeSymbolsDelegate));
    IfFailRet(createDelegate(hostHandle, domainId, SOSManagedDllName, SymbolReaderClassName, "LoadSymbolsForModule", (void **)&g_SOSNetCoreCallbacks.LoadSymbolsForModuleDelegate));
    IfFailRet(createDelegate(hostHandle, domainId, SOSManagedDllName, SymbolReaderClassName, "Dispose", (void **)&g_SOSNetCoreCallbacks.DisposeDelegate));
    IfFailRet(createDelegate(hostHandle, domainId, SOSManagedDllName, SymbolReaderClassName, "ResolveSequencePoint", (void **)&g_SOSNetCoreCallbacks.ResolveSequencePointDelegate));
    IfFailRet(createDelegate(hostHandle, domainId, SOSManagedDllName, SymbolReaderClassName, "GetLocalVariableName", (void **)&g_SOSNetCoreCallbacks.GetLocalVariableNameDelegate));
    IfFailRet(createDelegate(hostHandle, domainId, SOSManagedDllName, SymbolReaderClassName, "GetLineByILOffset", (void **)&g_SOSNetCoreCallbacks.GetLineByILOffsetDelegate));
    IfFailRet(createDelegate(hostHandle, domainId, SOSManagedDllName, MetadataHelperClassName, "GetMetadataLocator", (void **)&g_SOSNetCoreCallbacks.GetMetadataLocatorDelegate));

    g_hostingInitialized = true;
    return Status;
}

//
// Pass to managed helper code to read in-memory PEs/PDBs.
// Returns the number of bytes read.
//
static int ReadMemoryForSymbols(ULONG64 address, uint8_t *buffer, int cb)
{
    ULONG read;
    if (SafeReadMemory(TO_TADDR(address), (PVOID)buffer, cb, &read))
    {
        return read;
    }
    return 0;
}

/**********************************************************************\
 * Setup and initialize the symbol server support.
\**********************************************************************/
HRESULT InitializeSymbolStore(BOOL logging, BOOL msdl, BOOL symweb, const char* symbolServer, const char* cacheDirectory)
{
    HRESULT Status = S_OK;
    IfFailRet(InitializeHosting());
    _ASSERTE(g_SOSNetCoreCallbacks.InitializeSymbolStoreDelegate != nullptr);

    if (!g_SOSNetCoreCallbacks.InitializeSymbolStoreDelegate(logging, msdl, symweb, symbolServer, cacheDirectory, nullptr))
    {
        ExtErr("Error initializing symbol server support\n");
        return E_FAIL;
    }
    g_symbolStoreInitialized = true;
    return S_OK;
}

/**********************************************************************\
 * Setup and initialize the symbol server support using the .sympath
\**********************************************************************/
void InitializeSymbolStore()
{
    _ASSERTE(g_SOSNetCoreCallbacks.InitializeSymbolStoreDelegate != nullptr);

#ifndef FEATURE_PAL
    if (!g_symbolStoreInitialized)
    {
        g_symbolStoreInitialized = true;

        ArrayHolder<char> symbolPath = new char[MAX_LONGPATH];
        if (SUCCEEDED(g_ExtSymbols->GetSymbolPath(symbolPath, MAX_LONGPATH, nullptr)))
        {
            if (strlen(symbolPath) > 0)
            {
                if (!g_SOSNetCoreCallbacks.InitializeSymbolStoreDelegate(false, false, false, nullptr, nullptr, symbolPath))
                {
                    ExtErr("Windows symbol path parsing FAILED\n");
                }
            }
        }
    }
#endif
}

//
// Symbol downloader callback
//
static void SymbolFileCallback(void* param, const char* moduleFileName, const char* symbolFilePath)
{
    if (strcmp(moduleFileName, MAIN_CLR_DLL_NAME_A) == 0) {
        return;
    }
    if (strcmp(moduleFileName, MAKEDLLNAME_A("mscordaccore")) == 0) {
        if (g_dacFilePath == nullptr) {
            g_dacFilePath = _strdup(symbolFilePath);
        }
        return;
    }
    if (strcmp(moduleFileName, MAKEDLLNAME_A("mscordbi")) == 0) {
        if (g_dbiFilePath == nullptr) {
            g_dbiFilePath = _strdup(symbolFilePath);
        }
        return;
    }
#ifdef FEATURE_PAL
    ToRelease<ILLDBServices2> services2(NULL);
    HRESULT Status = g_ExtServices->QueryInterface(__uuidof(ILLDBServices2), (void**)&services2);
    if (SUCCEEDED(Status))
    {
        services2->AddModuleSymbol(param, symbolFilePath);
    }
#endif
}

//
// Enumerate native module callback
//
static void LoadNativeSymbolsCallback(void* param, const char* moduleFilePath, ULONG64 moduleAddress, int moduleSize)
{
    _ASSERTE(g_hostingInitialized);
    _ASSERTE(g_SOSNetCoreCallbacks.LoadNativeSymbolsDelegate != nullptr);
    g_SOSNetCoreCallbacks.LoadNativeSymbolsDelegate(SymbolFileCallback, param, GetTempDirectory(), moduleFilePath, moduleAddress, moduleSize, ReadMemoryForSymbols);
}

/**********************************************************************\
 * Enumerate the native modules and attempt to download the symbols
 * for them. Depends on the lldb callback to enumerate modules. Not
 * necessary on dbgeng because it already downloads native symbols.
\**********************************************************************/
HRESULT LoadNativeSymbols(bool runtimeOnly)
{
    HRESULT hr = S_OK;
    if (g_symbolStoreInitialized)
    {
#ifdef FEATURE_PAL
        ToRelease<ILLDBServices2> services2(NULL);
        hr = g_ExtServices->QueryInterface(__uuidof(ILLDBServices2), (void**)&services2);
        if (SUCCEEDED(hr))
        {
            hr = services2->LoadNativeSymbols(runtimeOnly, LoadNativeSymbolsCallback);
        }
#else
        if (runtimeOnly)
        {
            ULONG index;
            ULONG64 moduleAddress;
            HRESULT hr = g_ExtSymbols->GetModuleByModuleName(MAIN_CLR_MODULE_NAME_A, 0, &index, &moduleAddress);
            if (SUCCEEDED(hr))
            {
                ArrayHolder<char> moduleFilePath = new char[MAX_LONGPATH + 1];
                hr = g_ExtSymbols->GetModuleNames(index, 0, moduleFilePath, MAX_LONGPATH, NULL, NULL, 0, NULL, NULL, 0, NULL);
                if (SUCCEEDED(hr))
                {
                    DEBUG_MODULE_PARAMETERS moduleParams;
                    hr = g_ExtSymbols->GetModuleParameters(1, &moduleAddress, 0, &moduleParams);
                    if (SUCCEEDED(hr))
                    {
                        LoadNativeSymbolsCallback(nullptr, moduleFilePath, moduleAddress, moduleParams.Size);
                    }
                }
            }
        }
#endif
    }
    return hr;
}

/**********************************************************************\
 * Displays the symbol server and cache status.
\**********************************************************************/
void DisplaySymbolStore()
{
    if (g_symbolStoreInitialized)
    {
        _ASSERTE(g_SOSNetCoreCallbacks.DisplaySymbolStoreDelegate != nullptr);
        g_SOSNetCoreCallbacks.DisplaySymbolStoreDelegate();
    }
}

/**********************************************************************\
 * Turns off the symbol server support.
\**********************************************************************/
void DisableSymbolStore()
{
    if (g_symbolStoreInitialized)
    {
        g_symbolStoreInitialized = false;

        _ASSERTE(g_SOSNetCoreCallbacks.DisableSymbolStoreDelegate != nullptr);
        g_SOSNetCoreCallbacks.DisableSymbolStoreDelegate();
    }
}

/**********************************************************************\
 * Load symbols for an ICorDebugModule. Used by "clrstack -i".
\**********************************************************************/
HRESULT SymbolReader::LoadSymbols(___in IMetaDataImport* pMD, ___in ICorDebugModule* pModule)
{
    HRESULT Status = S_OK;

    BOOL isDynamic = FALSE;
    IfFailRet(pModule->IsDynamic(&isDynamic));
    if (isDynamic)
    {
        // Dynamic and in memory assemblies are a special case which we will ignore for now
        ExtWarn("SOS Warning: Loading symbols for dynamic assemblies is not yet supported\n");
        return E_FAIL;
    }

    ULONG64 peAddress = 0;
    IfFailRet(pModule->GetBaseAddress(&peAddress));

    IXCLRDataModule* pClrModule;
    IfFailRet(GetModuleFromAddress(peAddress, &pClrModule));

    return LoadSymbols(pMD, pClrModule);
}

/**********************************************************************\
 * Load symbols for a module.
\**********************************************************************/
HRESULT SymbolReader::LoadSymbols(___in IMetaDataImport* pMD, ___in IXCLRDataModule* pModule)
{
    ULONG32 flags;
    HRESULT hr = pModule->GetFlags(&flags);
    if (FAILED(hr)) 
    {
        ExtOut("LoadSymbols IXCLRDataModule->GetFlags FAILED 0x%08x\n", hr);
        return hr;
    }

    if (flags & CLRDATA_MODULE_IS_DYNAMIC)
    {
        ExtWarn("SOS Warning: Loading symbols for dynamic assemblies is not yet supported\n");
        return E_FAIL;
    }

    DacpGetModuleData moduleData;
    hr = moduleData.Request(pModule);
    if (FAILED(hr))
    {
        ExtOut("LoadSymbols moduleData.Request FAILED 0x%08x\n", hr);
        return hr;
    }

    ArrayHolder<WCHAR> pModuleName = new WCHAR[MAX_LONGPATH + 1];
    ULONG32 nameLen = 0;
    hr = pModule->GetFileName(MAX_LONGPATH, &nameLen, pModuleName);
    if (FAILED(hr))
    {
        ExtOut("LoadSymbols: IXCLRDataModule->GetFileName FAILED 0x%08x\n", hr);
        return hr;
    }

#ifndef FEATURE_PAL
    // TODO: in-memory windows PDB not supported
    hr = LoadSymbolsForWindowsPDB(pMD, moduleData.LoadedPEAddress, pModuleName, moduleData.IsFileLayout);
    if (SUCCEEDED(hr))
    {
        return hr;
    }
#endif // FEATURE_PAL

    return LoadSymbolsForPortablePDB(
        pModuleName, 
        moduleData.IsInMemory,
        moduleData.IsFileLayout,
        moduleData.LoadedPEAddress,
        moduleData.LoadedPESize, 
        moduleData.InMemoryPdbAddress,
        moduleData.InMemoryPdbSize);
}

#ifndef FEATURE_PAL

/**********************************************************************\
 * Attempts to load Windows PDBs on Windows.
\**********************************************************************/
HRESULT SymbolReader::LoadSymbolsForWindowsPDB(___in IMetaDataImport* pMD, ___in ULONG64 peAddress, __in_z WCHAR* pModuleName, ___in BOOL isFileLayout)
{
    HRESULT Status = S_OK;

    if (m_pSymReader != NULL) 
        return S_OK;

    // Ignore errors to be able to run under a managed host (dotnet-dump).
    CoInitialize(NULL);

    // We now need a binder object that will take the module and return a 
    ToRelease<ISymUnmanagedBinder3> pSymBinder;
    if (FAILED(Status = CreateInstanceCustom(CLSID_CorSymBinder_SxS, 
                        IID_ISymUnmanagedBinder3, 
                        NATIVE_SYMBOL_READER_DLL,
                        cciDacColocated|cciDbgPath, 
                        (void**)&pSymBinder)))
    {
        ExtOut("SOS Error: Unable to CoCreateInstance class=CLSID_CorSymBinder_SxS, interface=IID_ISymUnmanagedBinder3, hr=0x%x\n", Status);
        ExtOut("This usually means SOS was unable to locate a suitable version of DiaSymReader. The dll searched for was '%S'\n", NATIVE_SYMBOL_READER_DLL);
        return Status;
    }

    ToRelease<IDebugSymbols3> spSym3(NULL);
    Status = g_ExtSymbols->QueryInterface(__uuidof(IDebugSymbols3), (void**)&spSym3);
    if (FAILED(Status))
    {
        ExtOut("SOS Error: Unable to query IDebugSymbols3 HRESULT=0x%x.\n", Status);
        return Status;
    }

    ULONG pathSize = 0;
    Status = spSym3->GetSymbolPathWide(NULL, 0, &pathSize);
    if (FAILED(Status)) //S_FALSE if the path doesn't fit, but if the path was size 0 perhaps we would get S_OK?
    {
        ExtOut("SOS Error: Unable to get symbol path length. IDebugSymbols3::GetSymbolPathWide HRESULT=0x%x.\n", Status);
        return Status;
    }

    ArrayHolder<WCHAR> symbolPath = new WCHAR[pathSize];
    Status = spSym3->GetSymbolPathWide(symbolPath, pathSize, NULL);
    if (S_OK != Status)
    {
        ExtOut("SOS Error: Unable to get symbol path. IDebugSymbols3::GetSymbolPathWide HRESULT=0x%x.\n", Status);
        return Status;
    }

    ToRelease<IUnknown> pCallback = NULL;
    if (isFileLayout)
    {
        pCallback = (IUnknown*) new PEOffsetMemoryReader(TO_TADDR(peAddress));
    }
    else
    {
        pCallback = (IUnknown*) new PERvaMemoryReader(TO_TADDR(peAddress));
    }

    // TODO: this should be better integrated with windbg's symbol lookup
    Status = pSymBinder->GetReaderFromCallback(pMD, pModuleName, symbolPath, 
        AllowRegistryAccess | AllowSymbolServerAccess | AllowOriginalPathAccess | AllowReferencePathAccess, pCallback, &m_pSymReader);

    if (FAILED(Status) && m_pSymReader != NULL)
    {
        m_pSymReader->Release();
        m_pSymReader = NULL;
    }
    return Status;
}

#endif // FEATURE_PAL

/**********************************************************************\
 * Attempts to load a portable or embeded PDB. Both Windows and xplat.
\**********************************************************************/
HRESULT SymbolReader::LoadSymbolsForPortablePDB(__in_z WCHAR* pModuleName, ___in BOOL isInMemory, ___in BOOL isFileLayout,
    ___in ULONG64 peAddress, ___in ULONG64 peSize, ___in ULONG64 inMemoryPdbAddress, ___in ULONG64 inMemoryPdbSize)
{
    HRESULT Status = S_OK;

    IfFailRet(InitializeHosting());
    InitializeSymbolStore();

    _ASSERTE(g_SOSNetCoreCallbacks.LoadSymbolsForModuleDelegate != nullptr);

    // The module name needs to be null for in-memory PE's.
    ArrayHolder<char> szModuleName = nullptr;
    if (!isInMemory && pModuleName != nullptr)
    {
        szModuleName = new char[MAX_LONGPATH];
        if (WideCharToMultiByte(CP_ACP, 0, pModuleName, (int)(_wcslen(pModuleName) + 1), szModuleName, MAX_LONGPATH, NULL, NULL) == 0)
        {
            return E_FAIL;
        }
    }

    m_symbolReaderHandle = g_SOSNetCoreCallbacks.LoadSymbolsForModuleDelegate(szModuleName, isFileLayout, peAddress, 
        (int)peSize, inMemoryPdbAddress, (int)inMemoryPdbSize, ReadMemoryForSymbols);

    if (m_symbolReaderHandle == 0)
    {
        return E_FAIL;
    }

    return Status;
}

/**********************************************************************\
 * Return the source/line number info for method/il offset.
\**********************************************************************/
HRESULT SymbolReader::GetLineByILOffset(___in mdMethodDef methodToken, ___in ULONG64 ilOffset,
    ___out ULONG *pLinenum, __out_ecount(cchFileName) WCHAR* pwszFileName, ___in ULONG cchFileName)
{
    HRESULT Status = S_OK;

    if (m_symbolReaderHandle != 0)
    {
        _ASSERTE(g_hostingInitialized);
        _ASSERTE(g_SOSNetCoreCallbacks.GetLineByILOffsetDelegate != nullptr);

        BSTR bstrFileName = SysAllocStringLen(0, MAX_LONGPATH);
        if (bstrFileName == nullptr)
        {
            return E_OUTOFMEMORY;
        }
        // Source lines with 0xFEEFEE markers are filtered out on the managed side.
        if ((g_SOSNetCoreCallbacks.GetLineByILOffsetDelegate(m_symbolReaderHandle, methodToken, ilOffset, pLinenum, &bstrFileName) == FALSE) || (*pLinenum == 0))
        {
            SysFreeString(bstrFileName);
            return E_FAIL;
        }
        wcscpy_s(pwszFileName, cchFileName, bstrFileName);
        SysFreeString(bstrFileName);
        return S_OK;
    }

#ifndef FEATURE_PAL
    if (m_pSymReader == NULL)
        return E_FAIL;

    ToRelease<ISymUnmanagedMethod> pSymMethod(NULL);
    IfFailRet(m_pSymReader->GetMethod(methodToken, &pSymMethod));

    ULONG32 seqPointCount = 0;
    IfFailRet(pSymMethod->GetSequencePointCount(&seqPointCount));

    if (seqPointCount == 0)
        return E_FAIL;

    // allocate memory for the objects to be fetched
    ArrayHolder<ULONG32> offsets(new ULONG32[seqPointCount]);
    ArrayHolder<ULONG32> lines(new ULONG32[seqPointCount]);
    ArrayHolder<ULONG32> columns(new ULONG32[seqPointCount]);
    ArrayHolder<ULONG32> endlines(new ULONG32[seqPointCount]);
    ArrayHolder<ULONG32> endcolumns(new ULONG32[seqPointCount]);
    ArrayHolder<ToRelease<ISymUnmanagedDocument>> documents(new ToRelease<ISymUnmanagedDocument>[seqPointCount]);

    ULONG32 realSeqPointCount = 0;
    IfFailRet(pSymMethod->GetSequencePoints(seqPointCount, &realSeqPointCount, offsets, &(documents[0]), lines, columns, endlines, endcolumns));

    const ULONG32 HiddenLine = 0x00feefee;
    int bestSoFar = -1;

    for (int i = 0; i < (int)realSeqPointCount; i++)
    {
        if (offsets[i] > ilOffset)
            break;

        if (lines[i] != HiddenLine)
            bestSoFar = i;
    }

    if (bestSoFar != -1)
    {
        ULONG32 cchNeeded = 0;
        IfFailRet(documents[bestSoFar]->GetURL(cchFileName, &cchNeeded, pwszFileName));

        *pLinenum = lines[bestSoFar];
        return S_OK;
    }
#endif // FEATURE_PAL

    return E_FAIL;
}

HRESULT SymbolReader::GetNamedLocalVariable(___in ISymUnmanagedScope * pScope, ___in ICorDebugILFrame * pILFrame, ___in mdMethodDef methodToken, 
    ___in ULONG localIndex, __out_ecount(paramNameLen) WCHAR* paramName, ___in ULONG paramNameLen, ICorDebugValue** ppValue)
{
    HRESULT Status = S_OK;

    if (m_symbolReaderHandle != 0)
    {
        _ASSERTE(g_hostingInitialized);
        _ASSERTE(g_SOSNetCoreCallbacks.GetLocalVariableNameDelegate != nullptr);

        BSTR wszParamName = SysAllocStringLen(0, mdNameLen);
        if (wszParamName == NULL)
        {
            return E_OUTOFMEMORY;
        }

        if (g_SOSNetCoreCallbacks.GetLocalVariableNameDelegate(m_symbolReaderHandle, methodToken, localIndex, &wszParamName) == FALSE)
        {
            SysFreeString(wszParamName);
            return E_FAIL;
        }

        wcscpy_s(paramName, paramNameLen, wszParamName);
        SysFreeString(wszParamName);

        if (FAILED(pILFrame->GetLocalVariable(localIndex, ppValue)) || (*ppValue == NULL))
        {
            *ppValue = NULL;
            return E_FAIL;
        }
        return S_OK;
    }

#ifndef FEATURE_PAL
    if (m_pSymReader == NULL)
        return E_FAIL;

    if (pScope == NULL)
    {
        ToRelease<ISymUnmanagedMethod> pSymMethod;
        IfFailRet(m_pSymReader->GetMethod(methodToken, &pSymMethod));

        ToRelease<ISymUnmanagedScope> pScope;
        IfFailRet(pSymMethod->GetRootScope(&pScope));

        return GetNamedLocalVariable(pScope, pILFrame, methodToken, localIndex, paramName, paramNameLen, ppValue);
    }
    else
    {
        ULONG32 numVars = 0;
        IfFailRet(pScope->GetLocals(0, &numVars, NULL));

        ArrayHolder<ISymUnmanagedVariable*> pLocals = new ISymUnmanagedVariable*[numVars];
        IfFailRet(pScope->GetLocals(numVars, &numVars, pLocals));

        for (ULONG i = 0; i < numVars; i++)
        {
            ULONG32 varIndexInMethod = 0;
            if (SUCCEEDED(pLocals[i]->GetAddressField1(&varIndexInMethod)))
            {
                if (varIndexInMethod != localIndex)
                    continue;

                ULONG32 nameLen = 0;
                if (FAILED(pLocals[i]->GetName(paramNameLen, &nameLen, paramName)))
                        swprintf_s(paramName, paramNameLen, W("local_%d\0"), localIndex);

                if (SUCCEEDED(pILFrame->GetLocalVariable(varIndexInMethod, ppValue)) && (*ppValue != NULL))
                {
                    for(ULONG j = 0; j < numVars; j++) pLocals[j]->Release();
                    return S_OK;
                }
                else
                {
                    *ppValue = NULL;
                    for(ULONG j = 0; j < numVars; j++) pLocals[j]->Release();
                    return E_FAIL;
                }
            }
        }

        ULONG32 numChildren = 0;
        IfFailRet(pScope->GetChildren(0, &numChildren, NULL));

        ArrayHolder<ISymUnmanagedScope*> pChildren = new ISymUnmanagedScope*[numChildren];
        IfFailRet(pScope->GetChildren(numChildren, &numChildren, pChildren));

        for (ULONG i = 0; i < numChildren; i++)
        {
            if (SUCCEEDED(GetNamedLocalVariable(pChildren[i], pILFrame, methodToken, localIndex, paramName, paramNameLen, ppValue)))
            {
                for (ULONG j = 0; j < numChildren; j++) pChildren[j]->Release();
                return S_OK;
            }
        }

        for (ULONG j = 0; j < numChildren; j++) pChildren[j]->Release();
    }
#endif // FEATURE_PAL

    return E_FAIL;
}

/**********************************************************************\
 * Returns the name of the local variable from a PDB. 
\**********************************************************************/
HRESULT SymbolReader::GetNamedLocalVariable(___in ICorDebugFrame * pFrame, ___in ULONG localIndex, __out_ecount(paramNameLen) WCHAR* paramName, 
    ___in ULONG paramNameLen, ___out ICorDebugValue** ppValue)
{
    HRESULT Status = S_OK;

    *ppValue = NULL;
    paramName[0] = L'\0';

    ToRelease<ICorDebugILFrame> pILFrame;
    IfFailRet(pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID*) &pILFrame));

    ToRelease<ICorDebugFunction> pFunction;
    IfFailRet(pFrame->GetFunction(&pFunction));

    mdMethodDef methodDef;
    ToRelease<ICorDebugClass> pClass;
    ToRelease<ICorDebugModule> pModule;
    IfFailRet(pFunction->GetClass(&pClass));
    IfFailRet(pFunction->GetModule(&pModule));
    IfFailRet(pFunction->GetToken(&methodDef));

    return GetNamedLocalVariable(NULL, pILFrame, methodDef, localIndex, paramName, paramNameLen, ppValue);
}

/**********************************************************************\
 * Returns the sequence point to bind breakpoints.
\**********************************************************************/
HRESULT SymbolReader::ResolveSequencePoint(__in_z WCHAR* pFilename, ___in ULONG32 lineNumber, ___out mdMethodDef* pToken, ___out ULONG32* pIlOffset)
{
    HRESULT Status = S_OK;

    if (m_symbolReaderHandle != 0)
    {
        _ASSERTE(g_hostingInitialized);
        _ASSERTE(g_SOSNetCoreCallbacks.ResolveSequencePointDelegate != nullptr);

        char szName[mdNameLen];
        if (WideCharToMultiByte(CP_ACP, 0, pFilename, (int)(_wcslen(pFilename) + 1), szName, mdNameLen, NULL, NULL) == 0)
        { 
            return E_FAIL;
        }
        if (g_SOSNetCoreCallbacks.ResolveSequencePointDelegate(m_symbolReaderHandle, szName, lineNumber, pToken, pIlOffset) == FALSE)
        {
            return E_FAIL;
        }
        return S_OK;
    }

#ifndef FEATURE_PAL
    if (m_pSymReader == NULL)
        return E_FAIL;

    ULONG32 cDocs = 0;
    ULONG32 cDocsNeeded = 0;
    ArrayHolder<ToRelease<ISymUnmanagedDocument>> pDocs = NULL;

    IfFailRet(m_pSymReader->GetDocuments(cDocs, &cDocsNeeded, NULL));
    pDocs = new ToRelease<ISymUnmanagedDocument>[cDocsNeeded];
    cDocs = cDocsNeeded;
    IfFailRet(m_pSymReader->GetDocuments(cDocs, &cDocsNeeded, &(pDocs[0])));

    ULONG32 filenameLen = (ULONG32) _wcslen(pFilename);

    for (ULONG32 i = 0; i < cDocs; i++)
    {
        ULONG32 cchUrl = 0;
        ULONG32 cchUrlNeeded = 0;
        ArrayHolder<WCHAR> pUrl = NULL;
        IfFailRet(pDocs[i]->GetURL(cchUrl, &cchUrlNeeded, pUrl));
        pUrl = new WCHAR[cchUrlNeeded];
        cchUrl = cchUrlNeeded;
        IfFailRet(pDocs[i]->GetURL(cchUrl, &cchUrlNeeded, pUrl));

        // If the URL is exactly as long as the filename then compare the two names directly
        if (cchUrl-1 == filenameLen)
        {
            if (0!=_wcsicmp(pUrl, pFilename))
                continue;
        }
        // does the URL suffix match [back]slash + filename?
        else if (cchUrl-1 > filenameLen)
        {
            WCHAR* slashLocation = pUrl + (cchUrl - filenameLen - 2);
            if (*slashLocation != L'\\' && *slashLocation != L'/')
                continue;
            if (0 != _wcsicmp(slashLocation+1, pFilename))
                continue;
        }
        // URL is too short to match
        else
            continue;

        ULONG32 closestLine = 0;
        if (FAILED(pDocs[i]->FindClosestLine(lineNumber, &closestLine)))
            continue;

        ToRelease<ISymUnmanagedMethod> pSymUnmanagedMethod;
        IfFailRet(m_pSymReader->GetMethodFromDocumentPosition(pDocs[i], closestLine, 0, &pSymUnmanagedMethod));
        IfFailRet(pSymUnmanagedMethod->GetToken(pToken));
        IfFailRet(pSymUnmanagedMethod->GetOffset(pDocs[i], closestLine, 0, pIlOffset));

        // If this IL 
        if (*pIlOffset == -1)
        {
            return E_FAIL;
        }
        return S_OK;
    }
#endif // FEATURE_PAL

    return E_FAIL;
}
