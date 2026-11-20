#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <ncrypt.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <mscat.h>
#include <mssip.h>
#include <sddl.h>
#include <shlobj.h>
#include <array>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <vector>
#include <utility>
#include "local_signing.h"
#include "machine_protocol.h"

namespace fs = std::filesystem;
namespace xiaoji {
namespace {
using Bytes = std::vector<BYTE>;
constexpr const wchar_t* files[] = {L"xiaoji.inf", L"xiaoji.dll", L"xiaoji.cat"};
constexpr wchar_t settings[] = L"SOFTWARE\\XiaojiUmdf2LocalSigning";
constexpr wchar_t subject[] = L"CN=xiaoji UMDF2 Local Test";
constexpr wchar_t cachePrefix[] = L"xiaoji-umdf2-cli-";
constexpr DWORD encoding = X509_ASN_ENCODING | PKCS_7_ASN_ENCODING;

[[noreturn]] void Error(const char* text, DWORD code = GetLastError()) {
    throw std::runtime_error(std::string(text) + " (code " + std::to_string(code) + ")");
}
void Check(LONG status, const char* text) { if (status != ERROR_SUCCESS) Error(text, status); }
void Require(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }

template<class T, auto close> struct Resource {
    T value{};
    Resource() = default;
    explicit Resource(T handle) : value(handle) {}
    Resource(const Resource&) = delete;
    Resource(Resource&& other) noexcept : value(std::exchange(other.value, {})) {}
    Resource& operator=(Resource&& other) noexcept { std::swap(value, other.value); return *this; }
    ~Resource() { if (value) close(value); }
};
using File = Resource<HANDLE, CloseHandle>;
using Key = Resource<NCRYPT_KEY_HANDLE, NCryptFreeObject>;
using Cert = Resource<PCCERT_CONTEXT, CertFreeCertificateContext>;
void CloseStore(HCERTSTORE store) { CertCloseStore(store, 0); }
using Store = Resource<HCERTSTORE, CloseStore>;
using Registry = Resource<HKEY, RegCloseKey>;
using Descriptor = Resource<HLOCAL, LocalFree>;

std::wstring Hex(const Bytes& bytes) {
    constexpr wchar_t digits[] = L"0123456789ABCDEF";
    std::wstring result;
    for (BYTE byte : bytes) { result += digits[byte >> 4]; result += digits[byte & 15]; }
    return result;
}
bool IsHex(const std::wstring& text, size_t length) {
    return text.size() == length && text.find_first_not_of(L"0123456789ABCDEFabcdef") == text.npos;
}
std::wstring UniqueName() {
    GUID id{};
    Check(CoCreateGuid(&id), "Generate unique name");
    auto bytes = reinterpret_cast<const BYTE*>(&id);
    return Hex(Bytes(bytes, bytes + sizeof(id)));
}
fs::path ProgramFiles() {
    PWSTR path{};
    Check(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &path), "Locate Program Files");
    Resource<PWSTR, CoTaskMemFree> storage(path);
    return path;
}
Descriptor Security(const wchar_t* sddl) {
    PSECURITY_DESCRIPTOR raw{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &raw, nullptr)) Error("Build security descriptor");
    return Descriptor(raw);
}
void PlainPath(const fs::path& path, bool directory) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) Error("Inspect package path");
    Require(!(attributes & FILE_ATTRIBUTE_REPARSE_POINT) && !!(attributes & FILE_ATTRIBUTE_DIRECTORY) == directory,
            "Unexpected directory, file or reparse point in package path");
}
bool CacheExists(const fs::path& path) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) { PlainPath(path, true); return true; }
    auto error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return false;
    Error("Inspect signing cache", error);
}
class WorkDirectory {
public:
    fs::path path;
    WorkDirectory(const fs::path& parent, bool elevated) : path(parent / (L"xiaoji-session-" + UniqueName())) {
        auto acl = Security(L"O:BAG:BAD:P(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)");
        SECURITY_ATTRIBUTES security{sizeof(security), acl.value, FALSE};
        if (!CreateDirectoryW(path.c_str(), elevated ? &security : nullptr)) Error("Create private signing directory");
    }
    void Remove() {
        if (path.empty()) return;
        PlainPath(path, true);
        for (auto name : files) fs::remove(path/name);
        fs::remove(path);
        path.clear();
    }
};

template<class Action> void WithWork(const fs::path& parent, bool elevated, Action action) {
    WorkDirectory work(parent, elevated);
    try { action(work); }
    catch (const std::exception& original) {
        try { work.Remove(); }
        catch (const std::exception& cleanup) {
            throw std::runtime_error(std::string(original.what()) + "; temporary directory remains: " + work.path.u8string() + ": " + cleanup.what());
        }
        throw;
    }
    work.Remove();
}

class Digest {
    static void CloseAlgorithm(BCRYPT_ALG_HANDLE provider) { BCryptCloseAlgorithmProvider(provider, 0); }
    Resource<BCRYPT_ALG_HANDLE, CloseAlgorithm> algorithm;
    Resource<BCRYPT_HASH_HANDLE, BCryptDestroyHash> hash;
public:
    Digest() {
        Check(BCryptOpenAlgorithmProvider(&algorithm.value, BCRYPT_SHA256_ALGORITHM, nullptr, 0), "Open SHA256");
        Check(BCryptCreateHash(algorithm.value, &hash.value, nullptr, 0, nullptr, 0, 0), "Create SHA256");
    }
    void Add(const BYTE* data, DWORD size) { Check(BCryptHashData(hash.value, const_cast<BYTE*>(data), size, 0), "Hash data"); }
    Bytes Finish() { Bytes result(32); Check(BCryptFinishHash(hash.value, result.data(), 32, 0), "Finish SHA256"); return result; }
};
Bytes FileHash(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    Digest hash;
    std::array<BYTE, 65536> buffer{};
    while (input.read(reinterpret_cast<char*>(buffer.data()), buffer.size()) || input.gcount())
        hash.Add(buffer.data(), static_cast<DWORD>(input.gcount()));
    Require(input.eof() && !input.bad(), "Cannot read entire package file");
    return hash.Finish();
}
using Hashes = std::array<Bytes, 3>;
Hashes Snapshot(const fs::path& source, const fs::path& work) {
    Hashes hashes;
    for (size_t index = 0; index < hashes.size(); ++index) {
        PlainPath(source/files[index], false);
        hashes[index] = FileHash(source/files[index]);
    }
    for (size_t index = 0; index < hashes.size(); ++index) {
        auto from = source/files[index], to = work/files[index];
        std::ifstream input(from, std::ios::binary);
        std::ofstream output(to, std::ios::binary);
        output << input.rdbuf();
        output.close();
        Require(input && output && FileHash(to) == hashes[index], "Package changed or failed to copy");
    }
    return hashes;
}

Bytes ArtifactHash(const fs::path& path, bool sha1 = false) {
    HCATADMIN admin{};
    if (!CryptCATAdminAcquireContext2(&admin, nullptr, sha1 ? L"SHA1" : L"SHA256", nullptr, 0)) Error("Open catalog hash context");
    auto release = [](HCATADMIN handle) { CryptCATAdminReleaseContext(handle, 0); };
    std::unique_ptr<void, decltype(release)> context(admin, release);
    HANDLE raw = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (raw == INVALID_HANDLE_VALUE) Error("Open catalog member");
    File file(raw);
    Bytes digest(sha1 ? 20 : 32);
    DWORD size = static_cast<DWORD>(digest.size());
    if (!CryptCATAdminCalcHashFromFileHandle2(admin, raw, &size, digest.data(), 0) || size != digest.size()) Error("Hash catalog member");
    return digest;
}
void CheckMembers(const fs::path& folder) {
    auto path = folder/files[2];
    HANDLE raw = CryptCATOpen(const_cast<PWSTR>(path.c_str()), CRYPTCAT_OPEN_EXISTING, 0, CRYPTCAT_VERSION_2, 0);
    if (raw == INVALID_HANDLE_VALUE) Error("Open package catalog");
    Resource<HANDLE, CryptCATClose> catalog(raw);
    std::set<std::wstring> allowed;
    for (auto file : {files[0], files[1]}) {
        auto hash = ArtifactHash(folder/file);
        auto tag = Hex(hash);
        auto member = CryptCATGetMemberInfo(raw, tag.data());
        Require(member && member->pIndirectData && member->pIndirectData->Digest.cbData == hash.size() &&
            !memcmp(member->pIndirectData->Digest.pbData, hash.data(), hash.size()), "Catalog does not contain the supplied INF/DLL hash");
        allowed.insert(tag); allowed.insert(Hex(ArtifactHash(folder/file, true)));
    }
    for (CRYPTCATMEMBER* member = nullptr; (member = CryptCATEnumerateMember(raw, member)) != nullptr;) {
        std::wstring tag = member->pwszReferenceTag ? member->pwszReferenceTag : L"";
        if (!tag.empty()) CharUpperBuffW(tag.data(), static_cast<DWORD>(tag.size()));
        Require(allowed.erase(tag) == 1, "Catalog contains an unrelated or duplicate member; refusing to sign");
    }
}

Bytes EncodedName() {
    DWORD size{};
    if (!CertStrToNameW(X509_ASN_ENCODING, subject, CERT_X500_NAME_STR, nullptr, nullptr, &size, nullptr)) Error("Size certificate subject");
    Bytes name(size);
    if (!CertStrToNameW(X509_ASN_ENCODING, subject, CERT_X500_NAME_STR, nullptr, name.data(), &size, nullptr)) Error("Encode certificate subject");
    return name;
}
Bytes Encode(LPCSTR type, const void* object) {
    DWORD size{};
    if (!CryptEncodeObjectEx(X509_ASN_ENCODING, type, object, 0, nullptr, nullptr, &size)) Error("Size certificate extension");
    Bytes data(size);
    if (!CryptEncodeObjectEx(X509_ASN_ENCODING, type, object, 0, nullptr, data.data(), &size)) Error("Encode certificate extension");
    return data;
}
Cert SelfSigned(NCRYPT_KEY_HANDLE key, CRYPT_KEY_PROV_INFO* provider) {
    auto name = EncodedName();
    CERT_NAME_BLOB blob{static_cast<DWORD>(name.size()), name.data()};
    CERT_BASIC_CONSTRAINTS2_INFO constraints{};
    BYTE signing = CERT_DIGITAL_SIGNATURE_KEY_USAGE;
    CRYPT_BIT_BLOB usage{1, &signing, 7};
    LPSTR codeSigning = const_cast<LPSTR>(szOID_PKIX_KP_CODE_SIGNING);
    CERT_ENHKEY_USAGE eku{1, &codeSigning};
    Bytes values[] = {Encode(X509_BASIC_CONSTRAINTS2, &constraints), Encode(X509_KEY_USAGE, &usage), Encode(X509_ENHANCED_KEY_USAGE, &eku)};
    LPCSTR oids[] = {szOID_BASIC_CONSTRAINTS2, szOID_KEY_USAGE, szOID_ENHANCED_KEY_USAGE};
    CERT_EXTENSION extensions[3]{};
    for (int index = 0; index < 3; ++index)
        extensions[index] = {const_cast<LPSTR>(oids[index]), TRUE, {static_cast<DWORD>(values[index].size()), values[index].data()}};
    CERT_EXTENSIONS all{3, extensions};
    CRYPT_ALGORITHM_IDENTIFIER algorithm{const_cast<LPSTR>(szOID_RSA_SHA256RSA), {}};
    SYSTEMTIME start{}, end{};
    GetSystemTime(&start); end = start;
    // Offline signatures have no timestamp. Ten years avoids unnecessary expiry on later driver reinstalls.
    end.wYear += 10;
    if (end.wMonth == 2 && end.wDay == 29) end.wDay = 28;
    Cert cert(CertCreateSelfSignCertificate(key, &blob, provider ? 0 : CERT_CREATE_SELFSIGN_NO_KEY_INFO,
                                            provider, &algorithm, &start, &end, &all));
    if (!cert.value) Error("Create self-signed code signing certificate");
    return cert;
}
Key NewKey(const wchar_t* name) {
    Key provider;
    Check(NCryptOpenStorageProvider(&provider.value, MS_KEY_STORAGE_PROVIDER, 0), "Open Microsoft Software KSP");
    Key key;
    Check(NCryptCreatePersistedKey(provider.value, &key.value, NCRYPT_RSA_ALGORITHM, name, 0, name ? NCRYPT_MACHINE_KEY_FLAG : 0), "Create RSA key");
    auto set = [&](LPCWSTR property, DWORD value) {
        Check(NCryptSetProperty(key.value, property, reinterpret_cast<BYTE*>(&value), sizeof(value), 0), "Configure RSA signing key");
    };
    set(NCRYPT_LENGTH_PROPERTY, 3072); set(NCRYPT_EXPORT_POLICY_PROPERTY, 0); set(NCRYPT_KEY_USAGE_PROPERTY, NCRYPT_ALLOW_SIGNING_FLAG);
    Check(NCryptFinalizeKey(key.value, 0), "Finalize RSA key");
    if (name) {
        try {
            auto acl = Security(L"O:BAG:BAD:P(A;;GA;;;BA)(A;;GA;;;SY)");
            Check(NCryptSetProperty(key.value, NCRYPT_SECURITY_DESCR_PROPERTY, static_cast<BYTE*>(acl.value),
                GetSecurityDescriptorLength(acl.value), OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION), "Restrict machine key ACL");
        } catch (...) { Check(NCryptDeleteKey(key.value, 0), "Remove incomplete signing key"); key.value = 0; throw; }
    }
    return key;
}

// Microsoft Learn: SignerSignEx2 and SIGNER_{FILE_INFO,SUBJECT_INFO,CERT_STORE_INFO,CERT,SIGNATURE_INFO}.
// https://learn.microsoft.com/windows/win32/seccrypto/signersignex2 (Mssign32.dll, no SDK import library/header).
struct SignFile { DWORD size; LPCWSTR name; HANDLE file; };
struct SignSubject { DWORD size; DWORD* index; DWORD choice; union { SignFile* file; void* blob; }; };
struct SignStore { DWORD size; PCCERT_CONTEXT cert; DWORD policy; HCERTSTORE store; };
struct SignCert { DWORD size; DWORD choice; union { LPCWSTR spc; SignStore* store; void* chain; }; HWND window; };
struct SignSignature { DWORD size; ALG_ID algorithm; DWORD choice; void* attribute; PCRYPT_ATTRIBUTES auth, unauth; };
static_assert(sizeof(SignFile) == 24 && sizeof(SignSubject) == 32 && sizeof(SignStore) == 32 &&
              sizeof(SignCert) == 24 && sizeof(SignSignature) == 40);
using SignEx2 = HRESULT (WINAPI*)(DWORD, SignSubject*, SignCert*, SignSignature*, void*, DWORD, LPCSTR, LPCWSTR,
                                  PCRYPT_ATTRIBUTES, void*, void**, PCERT_STRONG_SIGN_PARA, void*);
void Sign(const fs::path& path, PCCERT_CONTEXT cert) {
    Resource<HMODULE, FreeLibrary> library(LoadLibraryExW(L"mssign32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
    if (!library.value) Error("Load system mssign32.dll");
    auto address = GetProcAddress(library.value, "SignerSignEx2");
    if (!address) Error("Resolve SignerSignEx2");
    SignEx2 sign{};
    static_assert(sizeof(sign) == sizeof(address));
    memcpy(&sign, &address, sizeof(sign));
    DWORD index{};
    SignFile file{sizeof(file), path.c_str(), nullptr};
    SignSubject target{sizeof(target), &index, 1, {&file}};
    SignStore store{sizeof(store), cert, 4 /* SIGNER_CERT_POLICY_SPC */, nullptr};
    SignCert signer{};
    signer.size = sizeof(signer); signer.choice = 2; signer.store = &store;
    SignSignature signature{sizeof(signature), CALG_SHA_256, 0, nullptr, nullptr, nullptr};
    Check(sign(0, &target, &signer, &signature, nullptr, 0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr), "SignerSignEx2");
}
LONG FileTrust(const fs::path& path) {
    WINTRUST_FILE_INFO file{sizeof(file), path.c_str(), nullptr, nullptr};
    WINTRUST_DATA trust{};
    trust.cbStruct = sizeof(trust); trust.dwUIChoice = WTD_UI_NONE; trust.dwUnionChoice = WTD_CHOICE_FILE;
    trust.pFile = &file; trust.dwStateAction = WTD_STATEACTION_VERIFY; trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    LONG status = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &policy, &trust);
    trust.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &policy, &trust);
    return status;
}
void CheckSigner(const fs::path& path, PCCERT_CONTEXT expected) {
    Store store;
    Resource<HCRYPTMSG, CryptMsgClose> msg;
    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, path.c_str(), CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED |
        CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED, CERT_QUERY_FORMAT_FLAG_BINARY, 0, nullptr, nullptr, nullptr,
        &store.value, &msg.value, nullptr)) Error("Read embedded signature");
    DWORD size{};
    if (!CryptMsgGetParam(msg.value, CMSG_SIGNER_CERT_INFO_PARAM, 0, nullptr, &size)) Error("Read signer identity");
    Bytes data(size);
    if (!CryptMsgGetParam(msg.value, CMSG_SIGNER_CERT_INFO_PARAM, 0, data.data(), &size)) Error("Read signer certificate");
    Cert signer(CertFindCertificateInStore(store.value, encoding, 0, CERT_FIND_SUBJECT_CERT, data.data(), nullptr));
    Require(signer.value && signer.value->cbCertEncoded == expected->cbCertEncoded &&
        !memcmp(signer.value->pbCertEncoded, expected->pbCertEncoded, expected->cbCertEncoded), "Unexpected signing certificate");
    if (!CryptMsgControl(msg.value, 0, CMSG_CTRL_VERIFY_SIGNATURE, expected->pCertInfo)) Error("Invalid signer signature");
}

Bytes Property(PCCERT_CONTEXT cert, DWORD id) {
    DWORD size{};
    if (!CertGetCertificateContextProperty(cert, id, nullptr, &size)) Error("Read signing certificate property");
    Bytes data(size);
    if (!CertGetCertificateContextProperty(cert, id, data.data(), &size)) Error("Read signing certificate property");
    return data;
}
Store MachineStore(LPCWSTR name, bool write) {
    Store result(CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE |
        CERT_STORE_OPEN_EXISTING_FLAG | (write ? 0 : CERT_STORE_READONLY_FLAG), name));
    if (!result.value) Error("Open machine certificate store");
    return result;
}
Cert Find(HCERTSTORE store, const Bytes& thumb) {
    CRYPT_HASH_BLOB hash{static_cast<DWORD>(thumb.size()), const_cast<BYTE*>(thumb.data())};
    Cert cert(CertFindCertificateInStore(store, encoding, 0, CERT_FIND_SHA1_HASH, &hash, nullptr));
    if (!cert.value) {
        if (GetLastError() != static_cast<DWORD>(CRYPT_E_NOT_FOUND)) Error("Find signing certificate");
    } else {
        wchar_t text[128]{};
        if (!CertNameToStrW(X509_ASN_ENCODING, &cert.value->pCertInfo->Subject, CERT_X500_NAME_STR, text, ARRAYSIZE(text)) ||
            wcscmp(text, subject)) throw std::runtime_error("Recorded thumbprint has an unexpected subject; inspect machine stores");
    }
    return cert;
}
Bytes RecordedThumb() {
    Registry record;
    auto rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, settings, 0, KEY_READ | KEY_WOW64_64KEY, &record.value);
    if (rc == ERROR_FILE_NOT_FOUND) return {};
    Check(rc, "Read signing record");
    wchar_t text[41]{};
    DWORD size = sizeof(text);
    Check(RegGetValueW(record.value, nullptr, L"Thumbprint", RRF_RT_REG_SZ, nullptr, text, &size), "Missing or invalid recorded thumbprint");
    Require(size == sizeof(text) && IsHex(text, 40), "Invalid recorded thumbprint");
    Bytes thumb(20);
    DWORD count = 20;
    if (!CryptStringToBinaryW(text, 40, CRYPT_STRING_HEXRAW, thumb.data(), &count, nullptr, nullptr) || count != 20) Error("Decode thumbprint");
    return thumb;
}
std::wstring ObjectName(NCRYPT_HANDLE object) {
    DWORD size{};
    Check(NCryptGetProperty(object, NCRYPT_NAME_PROPERTY, nullptr, 0, &size, 0), "Size KSP object name");
    Require(size >= sizeof(wchar_t) && size % sizeof(wchar_t) == 0, "Invalid KSP object name length");
    std::wstring name(size / sizeof(wchar_t), L'\0');
    DWORD read{};
    Check(NCryptGetProperty(object, NCRYPT_NAME_PROPERTY, reinterpret_cast<BYTE*>(name.data()), size, &read, 0), "Read KSP object name");
    Require(read == size && name.find(L'\0') == name.size() - 1, "Invalid KSP object name");
    name.pop_back();
    return name;
}
Key MachineKey(PCCERT_CONTEXT cert) {
    auto info = Property(cert, CERT_KEY_PROV_INFO_PROP_ID);
    Cert uncached(CertCreateCertificateContext(cert->dwCertEncodingType, cert->pbCertEncoded, cert->cbCertEncoded));
    if (!uncached.value || !CertSetCertificateContextProperty(uncached.value, CERT_KEY_PROV_INFO_PROP_ID, 0, info.data()))
        Error("Prepare uncached certificate for private key comparison");
    BOOL owned{};
    DWORD spec{};
    NCRYPT_KEY_HANDLE raw{};
    if (!CryptAcquireCertificatePrivateKey(uncached.value, CRYPT_ACQUIRE_ONLY_NCRYPT_KEY_FLAG | CRYPT_ACQUIRE_NO_HEALING |
        CRYPT_ACQUIRE_COMPARE_KEY_FLAG | CRYPT_ACQUIRE_SILENT_FLAG, nullptr, &raw, &spec, &owned)) Error("Recorded private key unavailable or mismatched; not replacing it");
    Require(owned != FALSE, "Refusing borrowed certificate key handle");
    if (spec != CERT_NCRYPT_KEY_SPEC) { CryptReleaseContext(raw, 0); throw std::runtime_error("Expected a CNG signing key"); }
    Key key(raw);
    DWORD flags{}, read{};
    Check(NCryptGetProperty(raw, NCRYPT_KEY_TYPE_PROPERTY, reinterpret_cast<BYTE*>(&flags), sizeof(flags), &read, 0), "Read signing key scope");
    Require(read == sizeof(flags) && (flags & NCRYPT_MACHINE_KEY_FLAG), "Recorded key is not a machine key");
    Key provider;
    Check(NCryptGetProperty(raw, NCRYPT_PROVIDER_HANDLE_PROPERTY, reinterpret_cast<BYTE*>(&provider.value), sizeof(provider.value), &read, 0), "Read signing key provider");
    Require(read == sizeof(provider.value) && !_wcsicmp(ObjectName(provider.value).c_str(), MS_KEY_STORAGE_PROVIDER), "Unexpected signing key provider");
    auto name = ObjectName(raw);
    Require(name.compare(0, 7, L"xiaoji-") == 0 && IsHex(name.substr(7), 32), "Unexpected machine key container name");
    return key;
}
void Trust(PCCERT_CONTEXT cert, const Bytes& thumb) {
    for (auto name : {L"Root", L"TrustedPublisher"}) {
        auto store = MachineStore(name, true);
        if (!Find(store.value, thumb).value && !CertAddEncodedCertificateToStore(store.value, X509_ASN_ENCODING,
            cert->pbCertEncoded, cert->cbCertEncoded, CERT_STORE_ADD_NEW, nullptr)) Error("Add public signing certificate");
    }
}
void EraseEntries(const Bytes& thumb, Key& key) {
    auto publisher = MachineStore(L"TrustedPublisher", true), root = MachineStore(L"Root", true), personal = MachineStore(L"My", true);
    auto publisherCert = Find(publisher.value, thumb), rootCert = Find(root.value, thumb), myCert = Find(personal.value, thumb);
    auto remove = [](Cert& cert, const char* residual) {
        if (cert.value && !CertDeleteCertificateFromStore(std::exchange(cert.value, {}))) Error(residual);
    };
    try {
        remove(publisherCert, "TrustedPublisher removal failed; Root, My and private key not removed");
        remove(rootCert, "Root removal failed; My and private key not removed");
        Check(NCryptDeleteKey(key.value, NCRYPT_SILENT_FLAG), "Private key removal failed; My certificate retained");
        key.value = 0;
        remove(myCert, "Private key removed but My certificate remains");
    } catch (const std::exception& err) {
        throw std::runtime_error("Certificate " + fs::path(Hex(thumb)).u8string() + ": " + err.what());
    }
}
Cert LocalSigner() {
    auto old = RecordedThumb();
    auto personal = MachineStore(L"My", true);
    Cert previous = old.empty() ? Cert() : Find(personal.value, old);
    Require(old.empty() || previous.value, "Recorded My certificate missing; not creating a new root");
    Key retired = previous.value ? MachineKey(previous.value) : Key();
    if (previous.value) {
        auto validity = CertVerifyTimeValidity(nullptr, previous.value->pCertInfo);
        Require(validity >= 0, "Recorded certificate is not yet valid; not replacing it");
        if (validity == 0) { Trust(previous.value, old); return previous; }
    }
    auto name = L"xiaoji-" + UniqueName();
    auto key = NewKey(name.c_str());
    Bytes added;
    Cert signer;
    try {
        CRYPT_KEY_PROV_INFO provider{};
        provider.pwszProvName = const_cast<PWSTR>(MS_KEY_STORAGE_PROVIDER); provider.pwszContainerName = name.data();
        provider.dwFlags = CRYPT_MACHINE_KEYSET;
        signer = SelfSigned(key.value, &provider);
        auto thumb = Property(signer.value, CERT_SHA1_HASH_PROP_ID);
        if (!CertAddCertificateContextToStore(personal.value, signer.value, CERT_STORE_ADD_NEW, nullptr)) Error("Save machine signing certificate");
        added.swap(thumb);
        Trust(signer.value, added);
        auto hex = Hex(added);
        Check(RegSetKeyValueW(HKEY_LOCAL_MACHINE, settings, L"Thumbprint", REG_SZ, hex.c_str(), static_cast<DWORD>((hex.size()+1)*sizeof(wchar_t))), "Save signer thumbprint");
    } catch (...) {
        if (!added.empty()) EraseEntries(added, key);
        else if (key.value) { Check(NCryptDeleteKey(key.value, 0), "Remove incomplete machine key"); key.value = 0; }
        throw;
    }
    if (previous.value) EraseEntries(old, retired);
    return signer;
}
std::wstring CacheId(const Hashes& hashes, const Bytes& thumb) {
    Digest digest;
    for (const auto& hash : hashes) digest.Add(hash.data(), static_cast<DWORD>(hash.size()));
    digest.Add(thumb.data(), static_cast<DWORD>(thumb.size()));
    return Hex(digest.Finish());
}
constexpr DWORD cacheReceiptSize = 64;
using CacheRecords = std::map<std::wstring, Bytes>;
struct ValueInfo { std::wstring name; DWORD type, size; };

bool IsCacheValue(const ValueInfo& value) {
    return !_wcsnicmp(value.name.c_str(), L"Cache-", 6) && IsHex(value.name.substr(6), 64) &&
           value.type == REG_BINARY && value.size == cacheReceiptSize;
}
bool CanRemoveRecord(bool hasSubkey, const std::vector<ValueInfo>& values) {
    return !hasSubkey && std::all_of(values.begin(), values.end(), [](const ValueInfo& value) {
        if (value.name.find(L'\0') != std::wstring::npos) return false;
        if (!_wcsicmp(value.name.c_str(), L"Thumbprint")) return value.type == REG_SZ;
        if (!_wcsicmp(value.name.c_str(), L"InstalledHashes")) return value.type == REG_MULTI_SZ;
        return IsCacheValue(value);
    });
}
Bytes CacheReceipt(Bytes dllHash, const Bytes& catHash) {
    Require(dllHash.size() == 32 && catHash.size() == 32, "Expected two SHA256 digests");
    dllHash.insert(dllHash.end(), catHash.begin(), catHash.end());
    return dllHash;
}
Bytes SignedReceipt(const fs::path& folder) { return CacheReceipt(FileHash(folder/files[1]), FileHash(folder/files[2])); }

CacheRecords OwnedCaches(bool forRemoval) {
    Registry key;
    Check(RegOpenKeyExW(HKEY_LOCAL_MACHINE, settings, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY |
        (forRemoval ? KEY_ENUMERATE_SUB_KEYS : 0), &key.value), "Read signing cache receipts");
    CacheRecords records;
    std::vector<ValueInfo> values;
    for (DWORD index = 0;; ++index) {
        wchar_t name[256]{};
        DWORD size = ARRAYSIZE(name), type{}, bytes{};
        auto rc = RegEnumValueW(key.value, index, name, &size, nullptr, &type, nullptr, &bytes);
        if (rc == ERROR_NO_MORE_ITEMS) break;
        Check(rc, "Read signing cache receipt");
        ValueInfo value{std::wstring(name, size), type, bytes};
        if (forRemoval) values.push_back(value);
        if (!_wcsnicmp(name, L"Cache-", 6)) {
            Require(IsCacheValue(value), "Invalid signing cache receipt; caches retained");
            Bytes digests(cacheReceiptSize);
            Check(RegGetValueW(key.value, nullptr, name, RRF_RT_REG_BINARY, nullptr, digests.data(), &bytes), "Read signed DLL/CAT hashes");
            Require(bytes == cacheReceiptSize, "Signing cache receipt length changed");
            CharUpperBuffW(name+6, 64);
            records.emplace(name+6, std::move(digests));
        }
    }
    if (forRemoval) {
        wchar_t subkey[256];
        DWORD length = ARRAYSIZE(subkey);
        auto rc = RegEnumKeyExW(key.value, 0, subkey, &length, nullptr, nullptr, nullptr, nullptr);
        if (rc != ERROR_NO_MORE_ITEMS) Check(rc, "Enumerate signing record subkeys");
        Require(CanRemoveRecord(rc == ERROR_SUCCESS, values), "Unexpected signing record value or subkey; caches, certificate, key and record retained");
    }
    return records;
}
void CheckCache(const fs::path& cache) {
    PlainPath(cache, true);
    for (const auto& file : fs::directory_iterator(cache)) {
        auto name = file.path().filename().wstring();
        Require(std::any_of(std::begin(files), std::end(files), [&](auto allowed) { return !_wcsicmp(name.c_str(), allowed); }),
                "Unexpected file in signing cache; cache retained");
        PlainPath(file.path(), false);
    }
}
fs::path PrepareCopy(WorkDirectory& work, const fs::path& source, PackageCheck identity, PackageCheck verify, const Progress& progress) {
    auto hashes = Snapshot(source, work.path);
    identity(work.path/files[0]); CheckMembers(work.path);
    progress("certificate", "Check the local signing certificate");
    auto cert = LocalSigner();
    auto id = CacheId(hashes, Property(cert.value, CERT_SHA1_HASH_PROP_ID));
    auto cache = work.path.parent_path()/(cachePrefix + id);
    progress("sign", "Sign and verify the package");
    auto validate = [&](const fs::path& folder) {
        for (auto name : {files[1], files[2]}) {
            CheckSigner(folder/name, cert.value); Check(FileTrust(folder/name), "Untrusted DLL or CAT signature");
        }
        verify(folder/files[0]);
    };
    if (CacheExists(cache)) {
        auto receipts = OwnedCaches(false);
        auto recorded = receipts.find(id);
        Require(recorded != receipts.end(), "Unrecorded cache path already exists");
        CheckCache(cache);
        Require(recorded->second == SignedReceipt(cache), "Signed DLL or CAT differs from cache receipt");
        validate(cache);
        Require(FileHash(cache/files[0]) == hashes[0] && ArtifactHash(cache/files[1]) == ArtifactHash(work.path/files[1]), "Cache does not match requested package");
    } else {
        Sign(work.path/files[1], cert.value); Sign(work.path/files[2], cert.value);
        validate(work.path);
        auto receipt = SignedReceipt(work.path);
        auto published = cache;
        if (!MoveFileW(work.path.c_str(), published.c_str())) Error("Publish new signing cache without replacement");
        work.path.swap(published);
        Check(RegSetKeyValueW(HKEY_LOCAL_MACHINE, settings, (L"Cache-"+id).c_str(), REG_BINARY, receipt.data(), cacheReceiptSize), "Record signing cache");
        work.path.clear();
    }
    return cache/files[0];
}
}

fs::path PrepareLocalPackage(const fs::path& source, PackageCheck identity, PackageCheck verify, const Progress& progress) {
    identity(source/files[0]);
    fs::path result;
    WithWork(ProgramFiles(), true, [&](WorkDirectory& work) { result = PrepareCopy(work, source, identity, verify, progress); });
    return result;
}
bool IsMissingPublishedInf(unsigned long error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND || error == ERROR_NOT_FOUND;
}
void RemoveLocalTrust() {
    auto thumb = RecordedThumb();
    if (thumb.empty()) return;
    auto records = OwnedCaches(true);
    auto personal = MachineStore(L"My", true);
    auto cert = Find(personal.value, thumb);
    Require(cert.value != nullptr, "Recorded My certificate missing; private key cannot be identified, trust and record retained");
    auto key = MachineKey(cert.value);
    auto parent = ProgramFiles();
    for (const auto& entry : records) {
        auto cache = parent/(cachePrefix + entry.first);
        try {
            if (!CacheExists(cache)) continue;
            CheckCache(cache);
            for (auto name : files) fs::remove(cache/name);
            fs::remove(cache);
        } catch (const std::exception& err) {
            throw std::runtime_error("Cache cleanup stopped at " + cache.u8string() + "; trust and record retained: " + err.what());
        }
    }
    EraseEntries(thumb, key);
    Check(RegDeleteKeyExW(HKEY_LOCAL_MACHINE, settings, KEY_WOW64_64KEY, 0), "Trust removed but signing registry key remains");
}

#ifdef XIAOJI_SESSION_TEST
static void TestTrustCleanup() {
    const auto cacheName = L"Cache-" + std::wstring(64, L'A');
    std::vector<ValueInfo> values{{L"Thumbprint", REG_SZ, 82}, {L"InstalledHashes", REG_MULTI_SZ, 392}, {cacheName, REG_BINARY, 64}};
    Require(CanRemoveRecord(false, values) && !CanRemoveRecord(true, values), "Signing record subkey policy failed");
    for (const ValueInfo& invalid : std::vector<ValueInfo>{{L"", REG_SZ, 0}, {L"FutureSetting", REG_BINARY, 64},
        {L"InstalledHashes", REG_SZ, 82}, {L"Cache-unknown", REG_BINARY, 64}, {cacheName, REG_NONE, 0},
        {cacheName, REG_MULTI_SZ, 64}, {cacheName, REG_BINARY, 63}, {cacheName, REG_BINARY, 65}}) {
        values.push_back(invalid);
        Require(!CanRemoveRecord(false, values), "Unexpected signing record accepted");
        values.pop_back();
    }
    Bytes dll(32, 0xa1), cat(32, 0xc2);
    const auto receipt = CacheReceipt(dll, cat);
    Require(receipt.size() == cacheReceiptSize && std::equal(dll.begin(), dll.end(), receipt.begin()) &&
        std::equal(cat.begin(), cat.end(), receipt.begin()+32), "Cache receipt hash layout mismatch");
    auto changedDll = dll, changedCat = cat;
    changedDll.front() ^= 1; changedCat.back() ^= 1;
    Require(receipt != CacheReceipt(dll, changedCat) && receipt != CacheReceipt(changedDll, cat), "Cache receipt did not detect substitution");
    Require(IsMissingPublishedInf(ERROR_FILE_NOT_FOUND) && IsMissingPublishedInf(ERROR_PATH_NOT_FOUND) && IsMissingPublishedInf(ERROR_NOT_FOUND) &&
        !IsMissingPublishedInf(ERROR_SUCCESS) && !IsMissingPublishedInf(ERROR_ACCESS_DENIED) && !IsMissingPublishedInf(ERROR_INSUFFICIENT_BUFFER),
        "Unexpected missing OEM package policy");
}
static void TestMachineProtocol() {
    using State = DeviceState;
    const State states[] = {State::absent, State::current, State::legacy, State::not_started, State::unknown};
    const char* expected[] = {"NBBBB", "BRUBB", "BUUBB", "BBBBB", "BBBBB"};
    for (size_t keyboard = 0; keyboard < 5; ++keyboard) for (size_t mouse = 0; mouse < 5; ++mouse) {
        char code = expected[keyboard][mouse];
        Require(std::string_view(InstallationState(states[keyboard], states[mouse])) == (code == 'N' ? "not_installed" :
            code == 'R' ? "ready" : code == 'U' ? "needs_upgrade" : "broken"), "Status classification failed");
    }
    Require(JsonString(std::string("\0\n\t\"\\\x1f", 6) + u8"\u5c0f") == u8"\"\\u0000\\u000A\\u0009\\u0022\\u005C\\u001F\u5c0f\"", "JSON escaping failed");
}
static void TestPackageSignature(const fs::path& source, WorkDirectory& work) {
    Snapshot(source, work.path);
    auto dll = work.path/files[1], cat = work.path/files[2];
    auto hash = ArtifactHash(dll);
    auto unsignedReceipt = SignedReceipt(work.path);
    CheckMembers(work.path);
    for (auto& path : {dll, cat}) Require(FileTrust(path) == TRUST_E_NOSIGNATURE, "Expected an unsigned Inf2Cat build as test input");
    auto key = NewKey(nullptr);
    auto cert = SelfSigned(key.value, nullptr);
    Store store(CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, nullptr));
    Cert signer;
    if (!store.value || !CertAddCertificateContextToStore(store.value, cert.value, CERT_STORE_ADD_NEW, &signer.value)) Error("Create in-memory signer store");
    CERT_KEY_CONTEXT context{};
    context.cbSize = sizeof(context); context.hNCryptKey = key.value; context.dwKeySpec = CERT_NCRYPT_KEY_SPEC;
    if (!CertSetCertificateContextProperty(signer.value, CERT_KEY_CONTEXT_PROP_ID, CERT_STORE_NO_CRYPT_RELEASE_FLAG, &context)) Error("Attach temporary signing key");
    for (auto& path : {dll, cat}) {
        Sign(path, signer.value);
        CheckSigner(path, signer.value);
        auto status = FileTrust(path);
        if (status != CERT_E_UNTRUSTEDROOT) Error("Expected WinVerifyTrust to reject only the untrusted test root", status);
    }
    Require(hash == ArtifactHash(dll), "Signing changed PE catalog hash");
    auto signedReceipt = SignedReceipt(work.path);
    Require(signedReceipt == CacheReceipt(FileHash(dll), FileHash(cat)) &&
        !std::equal(signedReceipt.begin(), signedReceipt.begin()+32, unsignedReceipt.begin()) &&
        !std::equal(signedReceipt.begin()+32, signedReceipt.end(), unsignedReceipt.begin()+32), "Receipt did not capture both signed files");
    CheckMembers(work.path);
    std::fstream edit(dll, std::ios::in | std::ios::out | std::ios::binary);
    edit.seekg(0x40); int previous = edit.get();
    edit.seekp(0x40); edit.put(static_cast<char>(previous ^ 1)); edit.close();
    Require(previous != EOF && edit && ArtifactHash(dll) != hash, "DLL mutation did not change member hash");
    try { CheckMembers(work.path); }
    catch (const std::runtime_error& err) {
        if (std::string_view(err.what()) != "Catalog does not contain the supplied INF/DLL hash") throw;
        std::cout << "Ephemeral RSA signing, signer identity, untrusted root, CAT membership/tampering, JSON and 25 states: OK\n";
        return;
    }
    throw std::runtime_error("Catalog accepted the modified DLL");
}
void SigningTest(const fs::path& source) {
    TestTrustCleanup();
    TestMachineProtocol();
    WithWork(fs::temp_directory_path(), false, [&](WorkDirectory& work) { TestPackageSignature(source, work); });
}
#endif
}
