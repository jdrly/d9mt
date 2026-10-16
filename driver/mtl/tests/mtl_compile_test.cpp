// Native (macOS, no Wine) stress test of the driver's compile task / worker
// pool (driver/mtl/mtl_compile.h), built with -fsanitize=thread by
// scripts/test-compile-mt.sh. Mirrors the D3D9 shader graph: programs
// (Phase A) are tasks; per (program, prev) function tasks wait on both
// programs, claim their "IR" in a per-program map and either compile or wait
// for the owner of an identical IR; a render thread requests functions in
// random order and waits on them (stealing queued ones), while pools are
// torn down with work still queued. Also: timed waits and the per-frame
// render-thread wait budget.

#include <cstdio>
#include <random>
#include <thread>
#include <unordered_map>

#include "../mtl_compile.h"

namespace dxvk {
  void Logger::info(const std::string& m) { std::fprintf(stderr, "info: %s\n", m.c_str()); }
  void Logger::warn(const std::string& m) { std::fprintf(stderr, "warn: %s\n", m.c_str()); }
  void Logger::err (const std::string& m) { std::fprintf(stderr, "err: %s\n",  m.c_str()); }
}

using namespace dxvk;

static std::atomic<int> g_errors = { 0 };

static void spin(uint32_t us) {
  auto end = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
  while (std::chrono::steady_clock::now() < end) { }
}

struct Program;

struct Function : public RcObject {
  uint64_t           value = 0u;   // the "library"; valid once task is done
  Rc<MtlCompileTask> task;
};

struct Program : public MtlCompileTask {
  MtlCompileWorkers* pool;
  uint32_t id;
  uint64_t phaseA = 0u;              // written by run()

  dxvk::mutex mutex;
  std::unordered_map<const Program*, Rc<Function>> functions;
  std::unordered_map<uint64_t, Rc<Function>> byIr;

  Program(MtlCompileWorkers* p, uint32_t i) : pool(p), id(i) { }

  void run() override {
    spin(50u + (id * 37u) % 200u);
    phaseA = uint64_t(id) * 1000003u + 1u;
  }

  Rc<Function> get(Program* prev);
};

struct FunctionTask : public MtlCompileTask {
  Rc<Program>  program, prev;
  Rc<Function> fn;

  FunctionTask(Program* p, Program* q, Function* f) : program(p), prev(q), fn(f) { }

  // Identical "IR" for several prevs, like a PS linked against compatible VSs.
  static uint64_t irOf(const Program* p, const Program* q) {
    return p->phaseA * 31u + (q != nullptr && q->id % 3u != 0u ? q->phaseA : 0u);
  }

  void run() override {
    program->wait();
    if (prev != nullptr)
      prev->wait();

    if (program->phaseA == 0u || (prev != nullptr && prev->phaseA == 0u))
      g_errors++;

    uint64_t ir = irOf(program.ptr(), prev.ptr());
    Rc<Function> owner;
    { std::lock_guard lock(program->mutex);
      owner = program->byIr.emplace(ir, fn).first->second;
    }

    if (owner != fn) {
      owner->task->wait();
      fn->value = owner->value;
    } else {
      spin(100u + uint32_t(ir % 300u));
      fn->value = ir * 7u + 3u;
    }

    fn = nullptr; prev = nullptr; program = nullptr;
  }
};

Rc<Function> Program::get(Program* prev) {
  Rc<Function> fn;
  { std::lock_guard lock(mutex);
    auto& slot = functions[prev];
    if (slot != nullptr)
      return slot;
    slot = fn = new Function();
    fn->task = new FunctionTask(this, prev, fn.ptr());
  }
  pool->submit(fn->task, true);
  return fn;
}

static void iteration(uint32_t seed, uint32_t threads, bool teardownEarly) {
  std::mt19937 rng(seed);
  auto* pool = new MtlCompileWorkers(threads);

  std::vector<Rc<Program>> programs;
  for (uint32_t i = 0u; i < 24u; i++) {
    programs.push_back(new Program(pool, i + 1u));
    pool->submit(programs.back(), false);
  }

  // Render thread + one extra "app thread" requesting concurrently.
  auto requester = [&] (uint32_t s, uint32_t n) {
    std::mt19937 r(s);
    for (uint32_t k = 0u; k < n; k++) {
      Program* ps = programs[r() % programs.size()].ptr();
      Program* vs = (r() % 5u) ? programs[r() % programs.size()].ptr() : nullptr;
      Rc<Function> fn = ps->get(vs);
      if (r() % 2u) {
        compile_stats::waitOnRenderThread(fn->task.ptr());
        uint64_t want = FunctionTask::irOf(ps, vs) * 7u + 3u;
        if (fn->value != want) {
          std::fprintf(stderr, "mismatch: %llu != %llu\n",
            (unsigned long long)fn->value, (unsigned long long)want);
          g_errors++;
        }
      }
    }
  };

  std::thread app(requester, seed * 3u + 1u, 150u);
  requester(seed, teardownEarly ? 40u : 300u);
  app.join();

  if (teardownEarly) {
    // Drop queued work; tasks still referenced must run inline when waited.
    delete pool;
    pool = nullptr;
    for (auto& p : programs) {
      p->pool = nullptr;
      p->wait();
      std::vector<Rc<Function>> fns;
      { std::lock_guard lock(p->mutex);
        for (auto& f : p->functions) fns.push_back(f.second);
      }
      for (auto& f : fns)
        f->task->wait();
    }
  } else {
    delete pool;
  }

  (void)rng;
}

// A task that runs until released.
struct GateTask : public MtlCompileTask {
  std::atomic<bool> open = { false };
  std::atomic<bool> ran  = { false };

  void run() override {
    ran = true;
    while (!open.load())
      std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
};

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_errors++; } } while (0)

static double msSince(uint64_t t0) {
  return double(compile_stats::nowNs() - t0) * 1e-6;
}

// MtlCompileTask::waitFor and the per-frame wait budget (D9MT_PSO_WAIT_MS).
// Lower bounds are exact; upper bounds leave room for a loaded machine.
static void budgetTests() {
  { // waitFor never runs a queued task inline; wait() still does.
    Rc<GateTask> t = new GateTask();
    uint64_t t0 = compile_stats::nowNs();
    CHECK(!t->waitFor(5000000u));
    CHECK(msSince(t0) >= 5.0);
    CHECK(!t->ran && !t->isDone());
    t->open = true;
    t->wait();
    CHECK(t->ran && t->isDone() && t->waitFor(0u));
  }

  { // Off (the default): wait() is the unbounded render-thread wait.
    MtlCompileWorkers pool(1u);
    MtlCompileWaitBudget budget;
    CHECK(!budget.enabled());
    Rc<GateTask> t = new GateTask();
    pool.submit(t, true);
    std::thread opener([&t] {
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      t->open = true;
    });
    uint64_t t0 = compile_stats::nowNs();
    CHECK(budget.wait(t.ptr()));
    CHECK(t->isDone() && msSince(t0) >= 25.0);
    opener.join();
    // Not on a worker yet / no workers: off runs it inline like before.
    Rc<GateTask> u = new GateTask();
    u->open = true;
    CHECK(budget.wait(u.ptr()) && u->ran);
  }

  { // On: one frame's waits together stay within the budget.
    MtlCompileWorkers pool(2u);
    MtlCompileWaitBudget budget;
    budget.setBudgetMs(40);
    CHECK(budget.enabled() && budget.leftNs() == 40000000u);

    Rc<GateTask> a = new GateTask(), b = new GateTask();
    pool.submit(a, true);
    pool.submit(b, true);

    uint64_t t0 = compile_stats::nowNs();
    CHECK(!budget.wait(a.ptr()));           // waits out the whole budget
    double first = msSince(t0);
    CHECK(first >= 40.0 && first < 200.0);
    CHECK(budget.leftNs() == 0u);
    t0 = compile_stats::nowNs();
    CHECK(!budget.wait(b.ptr()));           // nothing left: no wait at all
    CHECK(msSince(t0) < 5.0);
    CHECK(!a->isDone() && !b->isDone());    // both keep running

    // Next frame: a task that finishes within the budget is waited for and
    // only its time is charged.
    budget.onFrameEnd();
    CHECK(budget.leftNs() == 40000000u);
    std::thread opener([&a] {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      a->open = true;
    });
    t0 = compile_stats::nowNs();
    CHECK(budget.wait(a.ptr()));
    CHECK(a->isDone());
    opener.join();
    const uint64_t charged = 40000000u - budget.leftNs();
    CHECK(charged >= 5000000u && charged <= uint64_t(msSince(t0) * 1e6) + 1000000u);
    CHECK(budget.wait(nullptr));            // done / null: free
    CHECK(budget.wait(a.ptr()) && 40000000u - budget.leftNs() == charged);

    b->open = true;
    b->wait();
  }

  { // Budget 0: never waits.
    MtlCompileWaitBudget budget;
    budget.setBudgetMs(0);
    Rc<GateTask> t = new GateTask();
    CHECK(budget.enabled() && !budget.wait(t.ptr()) && !t->ran);
    budget.setBudgetMs(-1);
    CHECK(!budget.enabled());
    t->open = true;
    t->wait();
  }
}

int main() {
  budgetTests();

  for (uint32_t i = 0u; i < 40u; i++)
    iteration(i * 7919u + 1u, 1u + i % 5u, (i % 4u) == 3u);

  // Zero threads: everything runs inline on wait().
  iteration(12345u, 0u, false);

  if (g_errors) {
    std::fprintf(stderr, "FAIL: %d error(s)\n", g_errors.load());
    return 1;
  }
  std::printf("mtl_compile_test: OK\n");
  return 0;
}
