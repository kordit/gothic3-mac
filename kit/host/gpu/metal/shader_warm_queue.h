// Scheduling half of the Metal shader warm-up (d3d9_metal.mm, class ShaderWarm), kept free of
// Metal so its ordering rules can be tested without a device (tests/shader_warm_queue_tests.cpp).
//
// One background worker walks the recorded keys in first-use order (run 15: several compiles in
// flight at once); the draw path asks for a key when it first needs that library. The rules:
//  - a key the worker already finished is handed over once (take returns it and forgets it);
//  - a key being compiled right now is waited for (bounded), never compiled twice;
//  - a queued key the draw path needs before the worker reaches it is claimed: the caller compiles
//    it and the worker later skips it;
//  - nothing is held locked while a library compiles (compiles run between begin and finish).
// Run 15 (hitch): poll() is the non-blocking form for draws that have a plain fallback: Ready hands
// the library over, Pending means a compile of it is in flight (draw the fallback this frame), Absent
// means nobody compiles it (the caller may start its own background compile with begin_async).
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

template <class Lib> class ShaderWarmQueue {
  public:
    // Registers a key the worker will visit (seed or recorded list). Returns false if already known.
    bool add_known(const std::string &k) {
        std::lock_guard<std::mutex> lock(mutex_);
        return known_.insert(k).second;
    }
    bool known(const std::string &k) {
        std::lock_guard<std::mutex> lock(mutex_);
        return known_.count(k) != 0;
    }
    size_t known_count() {
        std::lock_guard<std::mutex> lock(mutex_);
        return known_.size();
    }
    // Worker: about to compile k. False when the draw path already claimed it (skip it).
    bool begin(const std::string &k) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (claimed_.count(k) || in_flight_.count(k) || ready_.count(k))
            return false;
        in_flight_.insert(k);
        return true;
    }
    // Draw path (run 15): starts its own background compile of k (known or not). False when k is
    // already in flight or finished (poll it instead). A known key is claimed so the worker skips it.
    bool begin_async(const std::string &k) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (in_flight_.count(k) || ready_.count(k))
            return false;
        if (known_.count(k) && claimed_.insert(k).second)
            ++claimed_count_;
        in_flight_.insert(k);
        ++async_count_;
        return true;
    }
    // Worker: compile of k finished (lib may be empty/null on failure).
    void finish(const std::string &k, Lib lib, bool ok) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (ok)
                ready_[k] = lib;
            in_flight_.erase(k);
        }
        done_.notify_all();
    }
    // Draw path. Returns true and sets *out when a finished library is available; otherwise false,
    // and the caller compiles the source itself.
    bool take(const std::string &k, Lib *out,
              std::chrono::milliseconds bound = std::chrono::milliseconds(2000)) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (in_flight_.count(k)) {
            ++coalesced_;
            done_.wait_for(lock, bound, [&] { return !in_flight_.count(k); });
        }
        auto it = ready_.find(k);
        if (it == ready_.end()) {
            if (known_.count(k) && claimed_.insert(k).second)
                ++claimed_count_;
            return false;
        }
        *out = it->second;
        ready_.erase(it);
        ++hits_;
        return true;
    }
    enum class Poll { Ready, Pending, Absent };
    // Draw path, never waits (run 15). Ready: *out is set and handed over once. Pending: in flight.
    // Absent: nobody is compiling k and nothing is ready (a known key is NOT claimed here).
    Poll poll(const std::string &k, Lib *out) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = ready_.find(k);
        if (it != ready_.end()) {
            *out = it->second;
            ready_.erase(it);
            ++hits_;
            return Poll::Ready;
        }
        return in_flight_.count(k) ? Poll::Pending : Poll::Absent;
    }
    uint32_t async_started() {
        std::lock_guard<std::mutex> lock(mutex_);
        return async_count_;
    }
    size_t in_flight() {
        std::lock_guard<std::mutex> lock(mutex_);
        return in_flight_.size();
    }
    uint32_t hits() {
        std::lock_guard<std::mutex> lock(mutex_);
        return hits_;
    }
    uint32_t coalesced() {
        std::lock_guard<std::mutex> lock(mutex_);
        return coalesced_;
    }
    uint32_t claimed() {
        std::lock_guard<std::mutex> lock(mutex_);
        return claimed_count_;
    }

  private:
    std::set<std::string> known_, claimed_;
    std::unordered_map<std::string, Lib> ready_;
    std::set<std::string> in_flight_;   // keys being compiled now (worker or draw-path background)
    std::mutex mutex_;
    std::condition_variable done_;
    uint32_t hits_ = 0, coalesced_ = 0, claimed_count_ = 0, async_count_ = 0;
};

// Run 16 (review P1 follow-up): the draw path's background compiles, kept free of Metal so their lifetime rules
// are tested through the code the renderer runs (tests/shader_warm_queue_tests.cpp drives it with a fake compiler;
// ShaderWarm passes the one Metal call, newLibraryWithSource:options:completionHandler:, as the compiler). Rules:
//  - request() admits a compile only while open: the closed/failed check, the queue's begin_async and the count of
//    admitted compiles are one locked step, so a request racing with close() is either admitted before it (and
//    drained) or refused (no compile, *pending clear: the caller compiles in the draw or never draws again);
//  - one compile per key at a time (begin_async); a key whose background compile failed is not retried here;
//  - close() refuses new work; drain() returns only after every admitted completion has finished with this object
//    and the queue. No timeout: memory safety over bounded shutdown (compiles usually take 0.1-0.3 s, which is no
//    bound); a completion that never arrives blocks drain() forever (the accepted tradeoff);
//  - lock order: mutex_ may be held while the queue takes its own mutex (admission), never the reverse, and a
//    completion holds neither while it calls into the queue. The completion's last access is the unlock of mutex_.
template <class Lib> class ShaderAsyncCompiles {
  public:
    using Done = std::function<void(Lib lib)>;   // empty/null lib = failed; called exactly once, on any thread
    using Compiler = std::function<void(const std::string &src, Done done)>;

    ShaderAsyncCompiles(ShaderWarmQueue<Lib> &queue, Compiler compiler)
        : queue_(queue), compiler_(std::move(compiler)) {}
    ShaderAsyncCompiles(const ShaderAsyncCompiles &) = delete;
    ShaderAsyncCompiles &operator=(const ShaderAsyncCompiles &) = delete;
    ~ShaderAsyncCompiles() {
        close();
        drain();
    }

    // The library when ready; empty with *pending set while a compile of k is in flight (the warm-up's, or one
    // started here now); empty with *pending clear when k's background compile failed or we are closing (the
    // caller then compiles in the draw, which reports any error as before).
    Lib request(const std::string &k, const std::string &src, bool *pending) {
        Lib lib{};
        *pending = false;
        switch (queue_.poll(k, &lib)) {
        case ShaderWarmQueue<Lib>::Poll::Ready:
            return lib;
        case ShaderWarmQueue<Lib>::Poll::Pending:
            *pending = true;
            return Lib{};
        case ShaderWarmQueue<Lib>::Poll::Absent:
            break;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (failed_.count(k) || closing_)
                return Lib{};
            *pending = true;
            if (!queue_.begin_async(k))
                return Lib{};   // started or finished meanwhile: polled again next draw
            ++admitted_;
        }
        const std::string key = k;
        compiler_(src, [this, key](Lib l) { complete(key, l); });
        return Lib{};
    }
    // No new compile is admitted from here on.
    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closing_ = true;
    }
    // Waits until every admitted compile's completion has finished (call close() first).
    void drain() {
        std::unique_lock<std::mutex> lock(mutex_);
        drained_.wait(lock, [&] { return admitted_ == 0; });
    }
    size_t outstanding() {
        std::lock_guard<std::mutex> lock(mutex_);
        return admitted_;
    }
    size_t failed() {
        std::lock_guard<std::mutex> lock(mutex_);
        return failed_.size();
    }

  private:
    void complete(const std::string &k, Lib l) {
        const bool ok = !(l == Lib{});
        if (!ok) {
            std::lock_guard<std::mutex> lock(mutex_);
            failed_.insert(k);
        }
        queue_.finish(k, l, ok);   // no lock of ours held here
        std::lock_guard<std::mutex> lock(mutex_);
        if (--admitted_ == 0)
            drained_.notify_all();   // under the lock: drain() cannot return (and free us) before our unlock
    }

    ShaderWarmQueue<Lib> &queue_;
    Compiler compiler_;
    std::mutex mutex_;
    std::condition_variable drained_;
    std::set<std::string> failed_;   // keys whose background compile failed
    size_t admitted_ = 0;            // admitted compiles whose completion has not finished
    bool closing_ = false;
};
