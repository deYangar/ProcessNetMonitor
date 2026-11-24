#pragma once
#include <windows.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include "hourly_history.h"

namespace pnm {
class HistorySave {
public:
    enum Result { Pending, Saved, Failed, Superseded };

    // Only the final atomic replacement is serialized with invalidation.
    void Invalidate() {
        AcquireSRWLockExclusive(&state_->commit);
        ++state_->generation;
        ReleaseSRWLockExclusive(&state_->commit);
    }

    Result Status() const { return result_ ? result_->load() : Superseded; }

    bool Queue(const std::filesystem::path& path, const HourlyHistory& history, bool pinDll = true) {
        if (Status() == Pending) return false;
        auto job = MakeJob(path, history);
        if (pinDll && !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                reinterpret_cast<LPCWSTR>(&Run), &job->module)) return false;
        auto module = job->module;
        auto result = job->result;
        auto raw = job.release();
        const auto thread = CreateThread(nullptr, 0, Run, raw, 0, nullptr);
        if (!thread) {
            delete raw;
            if (module) FreeLibrary(module);
            return false;
        }
        CloseHandle(thread);
        result_ = std::move(result);
        return true;
    }

    bool Flush(const std::filesystem::path& path, const HourlyHistory& history) {
        Invalidate();
        return Write(*MakeJob(path, history), false) == Saved;
    }

    // At process termination the OS has stopped the other threads. On DLL
    // unload the worker's module reference prevents detach until it is done.
    // In particular, do not take a potentially abandoned lock here.
    bool FlushAtDetach(const std::filesystem::path& path, const HourlyHistory& history) {
        return Write(*MakeJob(path, history), false) == Saved;
    }

private:
    struct State { SRWLOCK commit = SRWLOCK_INIT; unsigned generation = 0, serial = 0; };
    struct Job {
        std::shared_ptr<State> state;
        std::shared_ptr<std::atomic<Result>> result;
        std::filesystem::path path, temporary;
        HourlyHistory history;
        unsigned generation;
        HMODULE module = nullptr;
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();
    std::shared_ptr<std::atomic<Result>> result_;

    std::unique_ptr<Job> MakeJob(const std::filesystem::path& path, const HourlyHistory& history) {
        auto temp = path;
        temp += L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
                std::to_wstring(++state_->serial) + L".tmp";
        return std::make_unique<Job>(Job{state_, std::make_shared<std::atomic<Result>>(Pending),
                                         path, std::move(temp), history, state_->generation});
    }

    static Result Write(const Job& job, bool background) {
        std::error_code error;
        if (job.path.has_parent_path()) std::filesystem::create_directories(job.path.parent_path(), error);
        if (error) return Failed;
        std::ofstream output(job.temporary, std::ios::binary | std::ios::trunc);
        const bool written = output && job.history.Write(output);
        output.close();
        Result result = Failed;
        if (written && output) {
            if (background) AcquireSRWLockExclusive(&job.state->commit);
            if (background && job.generation != job.state->generation) result = Superseded;
            else if (MoveFileExW(job.temporary.c_str(), job.path.c_str(),
                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                result = Saved;
            if (background) ReleaseSRWLockExclusive(&job.state->commit);
        }
        if (result != Saved) DeleteFileW(job.temporary.c_str());
        return result;
    }

    static DWORD WINAPI Run(void* arg) {
        auto job = std::unique_ptr<Job>(static_cast<Job*>(arg));
        const auto module = job->module;
        auto result = job->result;
        Result outcome = Failed;
        try { outcome = Write(*job, true); }
        catch (...) { DeleteFileW(job->temporary.c_str()); }
        job.reset();
        result->store(outcome);
        result.reset();
        if (module) FreeLibraryAndExitThread(module, 0);
        return 0;
    }
};
}