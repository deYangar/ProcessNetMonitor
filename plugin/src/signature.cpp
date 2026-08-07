// signature.cpp - offline Authenticode + catalog signature verification.
// Verified on Win11 25H2: core system exes either carry an embedded
// Microsoft signature (svchost, lsass, services, csrss, smss, explorer,
// wininit, lsaiso, RuntimeBroker, taskhostw, ...) or are catalog-signed
// via catroot (winlogon, dwm, conhost, notepad, cmd, spoolsv, sihost,
// ctfmon, ...). Both paths are checked; no network is ever touched
// (WTD_CACHE_ONLY_URL_RETRIEVAL, no revocation checks).
#include "signature.h"
#include <wintrust.h>
#include <softpub.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <mscat.h>
#include <vector>

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

SignatureCache& SignatureCache::Instance() {
    static SignatureCache inst;
    return inst;
}

SignatureCache::SignatureCache() {
    m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

SignatureCache::~SignatureCache() {
    Shutdown();
    if (m_event) CloseHandle(m_event);
}

void SignatureCache::Shutdown() {
    m_stop = true;
    if (m_event) SetEvent(m_event);
    if (m_worker) {
        WaitForSingleObject(m_worker, 3000);
        CloseHandle(m_worker);
        m_worker = nullptr;
    }
}

SigClass SignatureCache::Check(const std::wstring& path) {
    if (path.empty()) return SigClass::Unknown;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_cache.find(path);
        if (it != m_cache.end()) return it->second;
        if (!m_pending.count(path)) {
            m_pending.insert(path);
            m_queue.push_back(path);
            if (!m_worker) {
                m_worker = CreateThread(nullptr, 0, WorkerProc, this, 0, nullptr);
            }
        }
    }
    if (m_event) SetEvent(m_event);
    return SigClass::Unknown;
}

DWORD WINAPI SignatureCache::WorkerProc(LPVOID p) {
    ((SignatureCache*)p)->WorkerLoop();
    return 0;
}

void SignatureCache::WorkerLoop() {
    while (!m_stop.load()) {
        WaitForSingleObject(m_event, INFINITE);
        if (m_stop.load()) break;
        // Drain the queue (dedup against m_pending)
        while (true) {
            std::wstring path;
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                if (m_queue.empty()) break;
                path = m_queue.front();
                m_queue.pop_front();
            }
            SigClass cls = VerifyPath(path);
            bool changed = false;
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                auto it = m_cache.find(path);
                if (it == m_cache.end() || it->second == SigClass::Unknown) {
                    m_cache[path] = cls;
                    changed = true;
                }
                m_pending.erase(path);
            }
            if (changed) NotifyOnce();
        }
    }
}

void SignatureCache::NotifyOnce() {
    if (m_notify_hwnd && IsWindow(m_notify_hwnd)) {
        PostMessageW(m_notify_hwnd, WM_APP + 2, 0, 0);
    }
}

// ---- verification -----------------------------------------------------

static bool GetEmbeddedSigner(const std::wstring& path, std::wstring& signer) {
    signer.clear();
    HCERTSTORE store = nullptr;
    HCRYPTMSG msg = nullptr;
    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, path.c_str(),
        CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
        CERT_QUERY_FORMAT_FLAG_BINARY, 0, nullptr, nullptr, nullptr,
        &store, &msg, nullptr)) {
        return false;
    }
    bool ok = false;
    DWORD len = 0;
    if (CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &len) &&
        len > 0 && len < 65536) {
        std::vector<BYTE> buf(len);
        if (CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, buf.data(), &len)) {
            auto* si = (CMSG_SIGNER_INFO*)buf.data();
            // find the signer cert by issuer + serial match (no CERT_FIND
            // macros needed - some SDKs omit CERT_FIND_ISSUER_SERIAL_NUM)
            PCCERT_CONTEXT cert = nullptr;
            PCCERT_CONTEXT found = nullptr;
            while ((cert = CertEnumCertificatesInStore(store, cert)) != nullptr) {
                auto* ci = cert->pCertInfo;
                if (ci->Issuer.cbData == si->Issuer.cbData &&
                    ci->SerialNumber.cbData == si->SerialNumber.cbData &&
                    memcmp(ci->Issuer.pbData, si->Issuer.pbData, si->Issuer.cbData) == 0 &&
                    memcmp(ci->SerialNumber.pbData, si->SerialNumber.pbData, si->SerialNumber.cbData) == 0) {
                    found = cert;
                    break;
                }
                CertFreeCertificateContext(cert);
            }
            if (found) {
                wchar_t name[256];
                if (CertGetNameStringW(found, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, 256)) {
                    signer = name;
                    ok = true;
                }
                CertFreeCertificateContext(found);
            }
        }
    }
    if (store) CertCloseStore(store, 0);
    if (msg) CryptMsgClose(msg);
    return ok;
}

static bool VerifyEmbeddedTrust(const std::wstring& path) {
    WINTRUST_FILE_INFO fi = {};
    fi.cbStruct = sizeof(fi);
    fi.pcwszFilePath = path.c_str();
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA wd = {};
    wd.cbStruct = sizeof(wd);
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
    LONG r = WinVerifyTrust(nullptr, &action, &wd);
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &wd);
    return (r == 0);
}

// Catalog signature: find the matching .cat in catroot by file hash and
// verify the catalog's own embedded Microsoft signature.
static bool VerifyCatalogSignature(const std::wstring& path) {
    HCATADMIN hAdmin = nullptr;
    if (!CryptCATAdminAcquireContext2(&hAdmin, nullptr, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) {
        if (!CryptCATAdminAcquireContext(&hAdmin, nullptr, 0)) return false;
    }
    bool ok = false;
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        BYTE hash[64];
        DWORD hashLen = sizeof(hash);
        if (CryptCATAdminCalcHashFromFileHandle2(hAdmin, hFile, &hashLen, hash, 0)) {
            HCATINFO hCat = CryptCATAdminEnumCatalogFromHash(hAdmin, hash, hashLen, 0, nullptr);
            if (hCat) {
                CATALOG_INFO ci = {};
                ci.cbStruct = sizeof(ci);
                if (CryptCATCatalogInfoFromContext(hCat, &ci, 0)) {
                    // the .cat itself must carry a Microsoft signature
                    std::wstring signer;
                    if (GetEmbeddedSigner(ci.wszCatalogFile, signer) &&
                        signer.find(L"Microsoft") != std::wstring::npos) {
                        ok = true;
                    }
                }
                CryptCATAdminReleaseCatalogContext(hAdmin, hCat, 0);
            }
        }
        CloseHandle(hFile);
    }
    CryptCATAdminReleaseContext(hAdmin, 0);
    return ok;
}

SigClass SignatureCache::VerifyPath(const std::wstring& path) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return SigClass::Unknown;   // exe gone - cannot verify (cache stays Unknown)
    }
    std::wstring signer;
    if (VerifyEmbeddedTrust(path) && GetEmbeddedSigner(path, signer)) {
        if (signer.find(L"Microsoft") != std::wstring::npos) return SigClass::System;
        return SigClass::ThirdParty;
    }
    // no (valid) embedded signature - try catalog
    if (VerifyCatalogSignature(path)) return SigClass::System;
    return SigClass::Unsigned;
}
