#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <userenv.h>

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

[[noreturn]] void fail(const char *operation, DWORD status = GetLastError()) {
    throw std::runtime_error(std::string(operation) + " failed (" + std::to_string(status) + ")");
}

struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

struct LocalAllocation {
    void *value = nullptr;
    ~LocalAllocation() { if (value) LocalFree(value); }
};

std::wstring profileName(std::wstring_view id) {
    if (id.size() != 36) throw std::runtime_error("Invalid CAD profile identifier");
    for (size_t i = 0; i < id.size(); ++i) {
        const auto ch = id[i];
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? ch != L'-' : !((ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f'))) {
            throw std::runtime_error("Invalid CAD profile identifier");
        }
    }
    return L"BambuCad." + std::wstring(id);
}

struct AppContainer {
    std::wstring name;
    PSID sid = nullptr;

    explicit AppContainer(const wchar_t *id) : name(profileName(id)) {
        const auto created = CreateAppContainerProfile(name.c_str(), L"Bambu CAD worker", L"One CAD operation", nullptr, 0, &sid);
        if (FAILED(created)) fail("CreateAppContainerProfile", created);
    }

    ~AppContainer() {
        FreeSid(sid);
        DeleteAppContainerProfile(name.c_str());
    }
};

class Attributes {
    std::vector<unsigned char> storage;
public:
    LPPROC_THREAD_ATTRIBUTE_LIST list = nullptr;

    explicit Attributes(DWORD count) {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, count, 0, &size);
        storage.resize(size);
        list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(list, count, 0, &size)) fail("InitializeProcThreadAttributeList");
    }

    ~Attributes() { DeleteProcThreadAttributeList(list); }

    template<typename T>
    void set(DWORD_PTR attribute, T &value) {
        if (!UpdateProcThreadAttribute(list, 0, attribute, &value, sizeof(value), nullptr, nullptr)) {
            fail("UpdateProcThreadAttribute");
        }
    }
};

void requireDirectory(const wchar_t *path) {
    const DWORD attributes = GetFileAttributesW(path);
    if (!std::filesystem::path(path).is_absolute() || attributes == INVALID_FILE_ATTRIBUTES ||
        !(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        throw std::runtime_error("CAD confinement requires a real absolute directory");
    }
}

void grantDirectory(wchar_t *path, PSID sid, DWORD permissions,
    DWORD inheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT, ACCESS_MODE mode = SET_ACCESS) {
    requireDirectory(path);
    LocalAllocation descriptor, entries;
    PACL previous = nullptr, updated = nullptr;
    PSECURITY_DESCRIPTOR security = nullptr;
    DWORD status = GetNamedSecurityInfoW(path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, &previous, nullptr, &security);
    descriptor.value = security;
    if (status != ERROR_SUCCESS) fail("GetNamedSecurityInfoW", status);
    if (!previous) throw std::runtime_error("CAD confinement requires a directory with an NTFS DACL");
    EXPLICIT_ACCESSW grant{};
    grant.grfAccessPermissions = permissions;
    grant.grfAccessMode = mode;
    grant.grfInheritance = inheritance;
    BuildTrusteeWithSidW(&grant.Trustee, sid);
    status = SetEntriesInAclW(1, &grant, previous, &updated);
    entries.value = updated;
    if (status != ERROR_SUCCESS) fail("SetEntriesInAclW", status);
    status = SetNamedSecurityInfoW(path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, updated, nullptr);
    if (status != ERROR_SUCCESS) fail("SetNamedSecurityInfoW (DACL)", status);
}

void allowLowIntegrityWrites(wchar_t *path) {
    LocalAllocation ownerDescriptor, descriptor;
    PSECURITY_DESCRIPTOR security = nullptr;
    PSID owner = nullptr;
    // An inherited Modify DACL often omits WRITE_OWNER, even for the creator.
    const auto ownerStatus = GetNamedSecurityInfoW(path, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
        &owner, nullptr, nullptr, nullptr, &security);
    ownerDescriptor.value = security;
    if (ownerStatus != ERROR_SUCCESS) fail("GetNamedSecurityInfoW (owner)", ownerStatus);
    grantDirectory(path, owner, WRITE_OWNER, NO_INHERITANCE, GRANT_ACCESS);
    security = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"S:(ML;OICI;NW;;;LW)", SDDL_REVISION_1, &security, nullptr)) {
        fail("ConvertStringSecurityDescriptorToSecurityDescriptorW");
    }
    descriptor.value = security;
    BOOL present = FALSE, defaulted = FALSE;
    PACL label = nullptr;
    if (!GetSecurityDescriptorSacl(security, &present, &label, &defaulted) || !present) fail("GetSecurityDescriptorSacl");
    const auto status = SetNamedSecurityInfoW(path, SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, label);
    if (status != ERROR_SUCCESS) fail("SetNamedSecurityInfoW (integrity label)", status);
}

void duplicateStandardHandle(DWORD kind, Handle &result) {
    if (!DuplicateHandle(GetCurrentProcess(), GetStdHandle(kind), GetCurrentProcess(), &result.value,
        0, TRUE, DUPLICATE_SAME_ACCESS)) fail("DuplicateHandle (CAD stdio)");
}

// The Windows CRT splits each argument again in the child. Escape quotes and
// trailing backslashes before handing the command line to CreateProcessW.
std::wstring quote(const std::wstring &argument) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (const auto ch : argument) {
        if (ch == L'\\') { ++slashes; continue; }
        result.append(slashes, L'\\');
        if (ch == L'"') result.append(slashes + 1, L'\\');
        result += ch;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}

void verifyToken(HANDLE process, PSID expected) {
    Handle token;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token.value)) fail("OpenProcessToken");
    DWORD container = 0, size = 0;
    if (!GetTokenInformation(token.value, TokenIsAppContainer, &container, sizeof(container), &size) || !container) {
        throw std::runtime_error("CAD worker did not receive an AppContainer token");
    }
    GetTokenInformation(token.value, TokenCapabilities, nullptr, 0, &size);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) fail("GetTokenInformation (capabilities size)");
    std::vector<unsigned char> buffer(size);
    if (!GetTokenInformation(token.value, TokenCapabilities, buffer.data(), size, &size)) fail("GetTokenInformation (capabilities)");
    if (reinterpret_cast<TOKEN_GROUPS *>(buffer.data())->GroupCount != 0) {
        throw std::runtime_error("CAD worker received unexpected AppContainer capabilities");
    }
    GetTokenInformation(token.value, TokenAppContainerSid, nullptr, 0, &size);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) fail("GetTokenInformation (package SID size)");
    buffer.resize(size);
    if (!GetTokenInformation(token.value, TokenAppContainerSid, buffer.data(), size, &size)) fail("GetTokenInformation (package SID)");
    if (!EqualSid(reinterpret_cast<TOKEN_APPCONTAINER_INFORMATION *>(buffer.data())->TokenAppContainer, expected)) {
        throw std::runtime_error("CAD worker received the wrong AppContainer identity");
    }
}

void verifyArtifacts(const wchar_t *root) {
    size_t count = 0;
    uintmax_t bytes = 0;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(root)) {
        const auto attributes = GetFileAttributesW(entry.path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            throw std::runtime_error("CAD work contains an unsafe filesystem entry");
        }
        if (++count > 4096) throw std::runtime_error("CAD work contains too many entries");
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            const auto size = entry.file_size();
            bytes += size;
            if (size > 128ULL * 1024 * 1024 || bytes > 512ULL * 1024 * 1024) {
                throw std::runtime_error("CAD work contains oversized artifacts");
            }
        }
    }
}

int runWorker(const wchar_t *id, int argc, wchar_t **argv) {
    AppContainer container(id);
    auto inputPath = argv[0], outputPath = argv[1], python = argv[2];
    const DWORD read = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
    grantDirectory(inputPath, container.sid, read);
    grantDirectory(outputPath, container.sid, read | FILE_GENERIC_WRITE | DELETE);
    allowLowIntegrityWrites(outputPath);

    Handle job{CreateJobObjectW(nullptr, nullptr)};
    if (!job.value) fail("CreateJobObjectW");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
        JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_PROCESS_TIME;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    limits.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart = 900LL * 10'000'000;
    limits.ProcessMemoryLimit = 8ULL * 1024 * 1024 * 1024;
    if (!SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        fail("SetInformationJobObject");
    }

    Handle input, output, error;
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    input.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
    if (input.value == INVALID_HANDLE_VALUE) fail("CreateFileW (NUL)");
    duplicateStandardHandle(STD_OUTPUT_HANDLE, output);
    duplicateStandardHandle(STD_ERROR_HANDLE, error);
    HANDLE stdio[] = {input.value, output.value, error.value};
    SECURITY_CAPABILITIES capabilities{};
    capabilities.AppContainerSid = container.sid;
    Attributes attributes(3);
    attributes.set(PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, capabilities);
    attributes.set(PROC_THREAD_ATTRIBUTE_HANDLE_LIST, stdio);
    attributes.set(PROC_THREAD_ATTRIBUTE_JOB_LIST, job.value);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.value;
    startup.StartupInfo.hStdOutput = output.value;
    startup.StartupInfo.hStdError = error.value;
    startup.lpAttributeList = attributes.list;
    PROCESS_INFORMATION created{};
    std::wstring command;
    for (int i = 2; i < argc; ++i) {
        if (i != 2) command += L' ';
        command += quote(argv[i]);
    }
    if (!CreateProcessW(python, command.data(), nullptr, nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, outputPath, &startup.StartupInfo, &created)) {
        fail("CreateProcessW (CAD AppContainer)");
    }
    Handle process{created.hProcess}, thread{created.hThread};
    verifyToken(process.value, container.sid);
    if (ResumeThread(thread.value) == static_cast<DWORD>(-1)) fail("ResumeThread");

    DWORD waiting;
    while ((waiting = WaitForSingleObject(process.value, 100)) == WAIT_TIMEOUT) {
        IO_COUNTERS io{};
        if (GetProcessIoCounters(process.value, &io) && io.WriteTransferCount > 512ULL * 1024 * 1024) {
            fail("CAD worker exceeded its write limit", ERROR_DISK_FULL);
        }
    }
    if (waiting != WAIT_OBJECT_0) fail("WaitForSingleObject");
    DWORD status = 0;
    if (!GetExitCodeProcess(process.value, &status)) fail("GetExitCodeProcess");
    if (status == 0) verifyArtifacts(outputPath);
    return static_cast<int>(status);
}

} // namespace

int wmain(int argc, wchar_t **argv) {
    try {
        if (argc == 5 && std::wstring(argv[1]) == L"--prepare-runtime") {
            LocalAllocation sid;
            PSID packages = nullptr;
            if (!ConvertStringSidToSidW(L"S-1-15-2-2", &packages)) fail("ConvertStringSidToSidW");
            sid.value = packages;
            // Only installed runtime files and shipped helpers are shared across CAD containers.
            for (int i = 2; i < argc; ++i) grantDirectory(argv[i], packages, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE);
            return 0;
        }
        if (argc == 3 && std::wstring_view(argv[1]) == L"--cleanup-profile") {
            const auto result = DeleteAppContainerProfile(profileName(argv[2]).c_str());
            if (FAILED(result) && result != HRESULT_FROM_WIN32(ERROR_NOT_FOUND) && result != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
                fail("DeleteAppContainerProfile", result);
            }
            return 0;
        }
        if (argc > 6 && std::wstring_view(argv[1]) == L"--run" && std::filesystem::path(argv[5]).is_absolute()) {
            return runWorker(argv[2], argc - 3, argv + 3);
        }
        throw std::runtime_error("Usage: bambu-cad-sandbox --prepare-runtime <python> <packages> <assets> | --run <id> <input> <output> <python> [args...] | --cleanup-profile <id>");
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
}
