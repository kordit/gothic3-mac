// Test66: ordering rules of the shader warm-up queue (metal/shader_warm_queue.h). No device, no game.
#include "../metal/shader_warm_queue.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>
#include <thread>
#include <vector>

static int failures = 0, checks = 0;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(c)) {                                                                                \
            ++failures;                                                                            \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                       \
        }                                                                                          \
    } while (0)


// Run 16 (review P1 follow-up): a fake compiler for ShaderAsyncCompiles, the production class ShaderWarm uses (its
// Metal compiler is the only part not run here). It holds every completion until the test delivers it, so a
// completion can arrive at any point, e.g. after destruction began. The class has no clock and no timeout, so
// "later than the old 5 s limit" is simulated: the test advances sim_ms past 5000 while destruction is blocked,
// with no real sleep.
struct FakeCompiler {
    struct Job {
        std::string src;
        ShaderAsyncCompiles<int>::Done done;
        bool delivered = false;
    };
    std::mutex m;
    std::vector<Job> jobs;
    int sync_result = -1;   // >= 0: complete inside the compile call itself, on the requesting thread
    std::atomic<int> deliveries{0};
    ShaderAsyncCompiles<int>::Compiler fn() {
        return [this](const std::string &src, ShaderAsyncCompiles<int>::Done done) {
            if (sync_result >= 0) {
                ++deliveries;
                done(sync_result);
                return;
            }
            std::lock_guard<std::mutex> lock(m);
            jobs.push_back({src, std::move(done), false});
        };
    }
    size_t count() {
        std::lock_guard<std::mutex> lock(m);
        return jobs.size();
    }
    // Delivers job i (0 = failure). Each job's completion runs exactly once.
    void deliver(size_t i, int lib) {
        ShaderAsyncCompiles<int>::Done d;
        {
            std::lock_guard<std::mutex> lock(m);
            if (i >= jobs.size() || jobs[i].delivered) {
                std::fprintf(stderr, "FAIL fake compiler: job %zu missing or delivered twice\n", i);
                std::abort();
            }
            jobs[i].delivered = true;
            d = jobs[i].done;
        }
        ++deliveries;
        d(lib);
    }
};
static std::atomic<long> sim_ms{0};   // simulated time (ms); nothing in the class reads a clock

// Runs f on a thread; fails the whole test (instead of hanging) when it has not finished within 20 s.
template <class F> static void no_deadlock(const char *what, F f) {
    auto fut = std::async(std::launch::async, f);
    if (fut.wait_for(std::chrono::seconds(20)) != std::future_status::ready) {
        std::fprintf(stderr, "FAIL deadlock: %s did not finish in 20 s\n", what);
        std::_Exit(2);
    }
    fut.get();
}

// (1)/(2)/(6): request one compile, begin destruction, pass the old 5 s limit (simulated), then deliver `lib`.
// Destruction must not finish before the delivery and must finish right after it.
static void delayed_completion_case(int lib) {
    using namespace std::chrono;
    ShaderWarmQueue<int> q;   // outlives the async object, as ShaderWarm's queue_ outlives async_
    FakeCompiler fc;
    auto a = std::make_unique<ShaderAsyncCompiles<int>>(q, fc.fn());
    bool pending = false;
    CHECK(a->request("late", "src-late", &pending) == 0 && pending);
    CHECK(fc.count() == 1 && a->outstanding() == 1);
    sim_ms = 0;
    std::atomic<bool> destroyed{false};
    std::atomic<long> destroyed_at_sim{-1};
    steady_clock::time_point destroyed_real;
    std::thread destroyer([&] {
        a.reset();   // ~ShaderAsyncCompiles: close() + drain(), as ~ShaderWarm does
        destroyed_real = steady_clock::now();
        destroyed_at_sim = sim_ms.load();
        destroyed = true;
    });
    std::this_thread::sleep_for(milliseconds(50));
    CHECK(!destroyed);               // an admitted completion still needs the state
    sim_ms = 6000;                   // past the old 5 s bounded wait
    std::this_thread::sleep_for(milliseconds(50));
    CHECK(!destroyed);
    const auto t_release = steady_clock::now();
    fc.deliver(0, lib);              // the late completion runs against live state
    destroyer.join();
    CHECK(destroyed && destroyed_at_sim.load() >= 6000);
    CHECK(duration_cast<milliseconds>(destroyed_real - t_release).count() < 500);   // finishes right after release
    CHECK(fc.deliveries == 1);       // one completion per request
    int out = -1;
    if (lib) {
        CHECK(q.poll("late", &out) == ShaderWarmQueue<int>::Poll::Ready && out == lib);
    } else {
        CHECK(q.poll("late", &out) == ShaderWarmQueue<int>::Poll::Absent);
    }
    CHECK(q.in_flight() == 0);
}

// Same, with the queue owned next to the async object and destroyed with it, in ShaderWarm's member order (queue_
// declared first, async_ after it; the owner's destructor closes and drains first). A completion still using the
// queue or the async state after destruction would be a use-after-free (run this test under ASan to see it).
struct AsyncOwner {
    ShaderWarmQueue<int> queue;
    ShaderAsyncCompiles<int> async;
    explicit AsyncOwner(ShaderAsyncCompiles<int>::Compiler c) : async(queue, std::move(c)) {}
    ~AsyncOwner() {
        async.close();
        async.drain();
    }
};
static void delayed_completion_owned_case(int lib) {
    using namespace std::chrono;
    FakeCompiler fc;
    auto o = std::make_unique<AsyncOwner>(fc.fn());
    bool pending = false;
    CHECK(o->async.request("late", "src-late", &pending) == 0 && pending);
    sim_ms = 0;
    std::atomic<bool> destroyed{false};
    std::thread destroyer([&] {
        o.reset();
        destroyed = true;
    });
    std::this_thread::sleep_for(milliseconds(50));
    sim_ms = 6000;
    CHECK(!destroyed);
    fc.deliver(0, lib);
    destroyer.join();
    CHECK(destroyed && fc.deliveries == 1);
}

int main() {
    using namespace std::chrono;
    {   // finished entry: handed over once
        ShaderWarmQueue<int> q;
        CHECK(q.add_known("a"));
        CHECK(!q.add_known("a"));
        CHECK(q.begin("a"));
        q.finish("a", 7, true);
        int lib = 0;
        CHECK(q.take("a", &lib) && lib == 7);
        CHECK(!q.take("a", &lib));   // second use: not handed out twice (functions are cached above)
        CHECK(q.hits() == 1 && q.coalesced() == 0);
    }
    {   // queued entry needed first: claimed, worker skips it
        ShaderWarmQueue<int> q;
        q.add_known("b");
        int lib = 0;
        CHECK(!q.take("b", &lib));
        CHECK(q.claimed() == 1);
        CHECK(!q.begin("b"));
        CHECK(!q.take("b", &lib));
        CHECK(q.claimed() == 1);     // counted once
    }
    {   // unknown key: plain miss, nothing claimed
        ShaderWarmQueue<int> q;
        int lib = 0;
        CHECK(!q.take("z", &lib));
        CHECK(q.claimed() == 0);
    }
    {   // in-flight entry: the draw path waits for the worker instead of compiling a second time
        ShaderWarmQueue<int> q;
        q.add_known("c");
        CHECK(q.begin("c"));
        std::atomic<bool> got{false};
        int lib = 0;
        const auto t0 = steady_clock::now();
        std::thread draw([&] { got = q.take("c", &lib); });
        std::this_thread::sleep_for(milliseconds(60));
        q.finish("c", 9, true);
        draw.join();
        const auto waited = duration_cast<milliseconds>(steady_clock::now() - t0).count();
        CHECK(got && lib == 9);
        CHECK(q.coalesced() == 1 && q.claimed() == 0);
        CHECK(waited >= 50 && waited < 1500);
    }
    {   // in-flight compile fails: the waiter is released and compiles itself
        ShaderWarmQueue<int> q;
        q.add_known("d");
        CHECK(q.begin("d"));
        bool got = true;
        int lib = 0;
        std::thread draw([&] { got = q.take("d", &lib); });
        std::this_thread::sleep_for(milliseconds(20));
        q.finish("d", 0, false);
        draw.join();
        CHECK(!got);
    }
    {   // stuck worker: the wait is bounded
        ShaderWarmQueue<int> q;
        q.add_known("e");
        CHECK(q.begin("e"));
        int lib = 0;
        const auto t0 = steady_clock::now();
        CHECK(!q.take("e", &lib, milliseconds(80)));
        const auto waited = duration_cast<milliseconds>(steady_clock::now() - t0).count();
        CHECK(waited >= 70 && waited < 1000);
        q.finish("e", 1, true);      // late result is still usable by a later draw
        CHECK(q.take("e", &lib) && lib == 1);
    }
    {   // a key the worker is NOT on does not wait
        ShaderWarmQueue<int> q;
        q.add_known("f");
        q.add_known("g");
        CHECK(q.begin("f"));
        int lib = 0;
        const auto t0 = steady_clock::now();
        CHECK(!q.take("g", &lib));
        CHECK(duration_cast<milliseconds>(steady_clock::now() - t0).count() < 20);
        CHECK(q.coalesced() == 0 && q.claimed() == 1);
    }
    {   // run 15: several worker compiles in flight; a waiter waits only for its own key
        ShaderWarmQueue<int> q;
        q.add_known("h");
        q.add_known("i");
        CHECK(q.begin("h") && q.begin("i"));
        CHECK(!q.begin("h"));          // never compiled twice
        CHECK(q.in_flight() == 2);
        q.finish("i", 3, true);
        int lib = 0;
        const auto t0 = steady_clock::now();
        CHECK(q.take("i", &lib) && lib == 3);
        CHECK(duration_cast<milliseconds>(steady_clock::now() - t0).count() < 20);
        q.finish("h", 4, true);
        CHECK(q.take("h", &lib) && lib == 4);
        CHECK(q.in_flight() == 0);
    }
    {   // run 15 poll: never waits; Pending while in flight, Ready once, then Absent
        ShaderWarmQueue<int> q;
        q.add_known("j");
        int lib = 0;
        CHECK(q.poll("j", &lib) == ShaderWarmQueue<int>::Poll::Absent);
        CHECK(q.claimed() == 0);       // poll alone claims nothing
        CHECK(q.begin("j"));
        const auto t0 = steady_clock::now();
        CHECK(q.poll("j", &lib) == ShaderWarmQueue<int>::Poll::Pending);
        CHECK(duration_cast<milliseconds>(steady_clock::now() - t0).count() < 20);
        q.finish("j", 5, true);
        CHECK(q.poll("j", &lib) == ShaderWarmQueue<int>::Poll::Ready && lib == 5);
        CHECK(q.poll("j", &lib) == ShaderWarmQueue<int>::Poll::Absent);
        CHECK(q.hits() == 1);
    }
    {   // run 15 begin_async: claims a queued key (worker skips it), refuses a second start
        ShaderWarmQueue<int> q;
        q.add_known("k");
        CHECK(q.begin_async("k"));
        CHECK(!q.begin_async("k"));
        CHECK(!q.begin("k"));          // worker skips the claimed key
        CHECK(q.claimed() == 1 && q.async_started() == 1);
        int lib = 0;
        CHECK(q.poll("k", &lib) == ShaderWarmQueue<int>::Poll::Pending);
        q.finish("k", 6, true);
        CHECK(q.poll("k", &lib) == ShaderWarmQueue<int>::Poll::Ready && lib == 6);
        CHECK(q.begin_async("u"));     // an unknown key can be compiled in the background too
        CHECK(q.claimed() == 1);
        q.finish("u", 0, false);       // failed: Absent again (the caller then compiles in the draw)
        CHECK(q.poll("u", &lib) == ShaderWarmQueue<int>::Poll::Absent);
    }
    {   // run 15: a worker key finished before a begin_async is not started again
        ShaderWarmQueue<int> q;
        q.add_known("m");
        CHECK(q.begin("m"));
        q.finish("m", 8, true);
        CHECK(!q.begin_async("m"));
        int lib = 0;
        CHECK(q.poll("m", &lib) == ShaderWarmQueue<int>::Poll::Ready && lib == 8);
    }
    // Run 16: ShaderAsyncCompiles lifetime (review P1 follow-up), through the production class with a fake compiler.
    delayed_completion_case(7);   // (1) success after destruction began and > 5 s (simulated)
    delayed_completion_case(0);   // (2) failure, same
    delayed_completion_owned_case(7);
    delayed_completion_owned_case(0);
    {   // (3) a request racing with closure: admitted before close (then drained) or refused (no compile at all)
        for (int round = 0; round < 200; ++round) {
            ShaderWarmQueue<int> q;
            FakeCompiler fc;
            ShaderAsyncCompiles<int> a(q, fc.fn());
            std::atomic<int> admitted{0};
            std::atomic<bool> closed{false}, late_admit{false};
            std::thread req([&] {
                for (int i = 0; i < 64; ++i) {
                    const bool was_closed = closed.load();
                    bool p = false;
                    a.request("r" + std::to_string(i), "s", &p);
                    if (p)
                        ++admitted;
                    if (p && was_closed)
                        late_admit = true;   // admitted although close() had already returned
                }
            });
            if (round & 1)
                std::this_thread::yield();
            a.close();
            closed = true;
            req.join();
            CHECK(!late_admit);
            CHECK((int)fc.count() == admitted.load() && a.outstanding() == fc.count());
            bool p = true;
            CHECK(a.request("after", "s", &p) == 0 && !p && (int)fc.count() == admitted.load());
            for (size_t i = 0; i < fc.count(); ++i)
                fc.deliver(i, i & 1 ? 0 : 3);
            no_deadlock("drain after race", [&] { a.drain(); });
            CHECK(a.outstanding() == 0 && q.in_flight() == 0);
        }
    }
    {   // (4) duplicate-key requests: one compile, sequential and concurrent
        ShaderWarmQueue<int> q;
        FakeCompiler fc;
        ShaderAsyncCompiles<int> a(q, fc.fn());
        bool p1 = false, p2 = false;
        CHECK(a.request("dup", "s", &p1) == 0 && p1);
        CHECK(a.request("dup", "s", &p2) == 0 && p2);   // in flight: pending, no second compile
        std::vector<std::thread> ts;
        for (int t = 0; t < 8; ++t)
            ts.emplace_back([&] {
                bool p = false;
                a.request("dup2", "s", &p);
            });
        for (auto &t : ts)
            t.join();
        CHECK(fc.count() == 2 && a.outstanding() == 2);
        CHECK(fc.jobs[0].src == "s");
        fc.deliver(0, 11);
        fc.deliver(1, 12);
        bool p = true;
        CHECK(a.request("dup", "s", &p) == 11 && !p);   // handed over once
        CHECK(a.request("dup2", "s", &p) == 12 && !p);
        CHECK(fc.deliveries == 2 && q.async_started() == 2);
    }
    {   // (5) shutdown with no pending work returns at once (also after completed work)
        const auto t0 = steady_clock::now();
        {
            ShaderWarmQueue<int> q;
            FakeCompiler fc;
            ShaderAsyncCompiles<int> a(q, fc.fn());
        }
        {
            ShaderWarmQueue<int> q;
            FakeCompiler fc;
            fc.sync_result = 5;
            ShaderAsyncCompiles<int> a(q, fc.fn());
            no_deadlock("synchronous success", [&] {
                bool p = false;
                a.request("x", "s", &p);
            });
            CHECK(a.outstanding() == 0);
        }
        CHECK(duration_cast<milliseconds>(steady_clock::now() - t0).count() < 50);
    }
    {   // a failed key is not compiled in the background again (the draw compiles it and reports the error)
        ShaderWarmQueue<int> q;
        FakeCompiler fc;
        ShaderAsyncCompiles<int> a(q, fc.fn());
        bool p = false;
        a.request("bad", "s", &p);
        fc.deliver(0, 0);
        CHECK(a.failed() == 1);
        CHECK(a.request("bad", "s", &p) == 0 && !p && fc.count() == 1);
    }
    {   // (7) failure path: no lock-order deadlock. Failures delivered on other threads while requests and polls
        // run, and failures delivered synchronously inside the compile call on the requesting thread.
        ShaderWarmQueue<int> q;
        FakeCompiler fc;
        ShaderAsyncCompiles<int> a(q, fc.fn());
        no_deadlock("concurrent failures", [&] {
            std::atomic<bool> done{false};
            std::thread deliverer([&] {
                size_t next = 0;
                while (!done || next < fc.count()) {
                    if (next < fc.count())
                        fc.deliver(next++, 0);
                    else
                        std::this_thread::yield();
                }
            });
            std::vector<std::thread> reqs;
            for (int t = 0; t < 4; ++t)
                reqs.emplace_back([&, t] {
                    for (int i = 0; i < 500; ++i) {
                        bool p = false;
                        a.request("f" + std::to_string(t) + "_" + std::to_string(i % 50), "s", &p);
                    }
                });
            for (auto &r : reqs)
                r.join();
            done = true;
            deliverer.join();
            a.close();
            a.drain();
        });
        CHECK(a.failed() == 200 && fc.count() == 200 && q.in_flight() == 0);
        ShaderWarmQueue<int> q2;
        FakeCompiler fs;
        fs.sync_result = 0;
        ShaderAsyncCompiles<int> b(q2, fs.fn());
        no_deadlock("synchronous failure", [&] {
            bool p = false;
            CHECK(b.request("sync", "s", &p) == 0);
            CHECK(b.request("sync", "s", &p) == 0 && !p);
        });
        CHECK(b.failed() == 1 && b.outstanding() == 0 && fs.deliveries == 1);
    }
    std::printf("shader_warm_queue_tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
