// signature.h - Authenticode signature classification with per-path cache
// and a background worker thread (WinVerifyTrust + CryptQueryObject are
// too slow to run on the UI thread; catalog (CAT) verification covers
// system binaries that carry no embedded signature).
#pragma once
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <string>
#include <map>
#include <deque>
#include <set>
#include <mutex>
#include <atomic>

enum class SigClass {
    Unknown,      // not verified yet / path missing / cannot verify
    System,       // valid signature whose subject contains "Microsoft"
    ThirdParty,   // valid non-Microsoft signature
    Unsigned      // no embedded signature and no catalog match
};

class SignatureCache {
public:
    static SignatureCache& Instance();

    // Thread-safe lookup: cached -> return; else enqueue for background
    // verification and return Unknown (UI shows "未知" until refreshed).
    SigClass Check(const std::wstring& path);

    // Background worker notifies the detail window when a batch completes.
    void SetNotifyHwnd(HWND hwnd) { m_notify_hwnd = hwnd; }
    void Shutdown();

private:
    SignatureCache();
    ~SignatureCache();
    SignatureCache(const SignatureCache&) = delete;
    SignatureCache& operator=(const SignatureCache&) = delete;

    static DWORD WINAPI WorkerProc(LPVOID);
    void WorkerLoop();
    SigClass VerifyPath(const std::wstring& path);
    void NotifyOnce();

    std::mutex m_mutex;
    std::map<std::wstring, SigClass> m_cache;
    std::deque<std::wstring> m_queue;
    std::set<std::wstring> m_pending;   // queued but not yet verified
    HANDLE m_event = nullptr;           // wake-up signal for the worker
    HANDLE m_worker = nullptr;
    std::atomic<bool> m_stop{ false };
    HWND m_notify_hwnd = nullptr;
};
