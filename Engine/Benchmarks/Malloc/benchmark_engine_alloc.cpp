#include <benchmark/benchmark.h>
#include <mimalloc.h>
#include <cstdlib>
#include <vector>
#include <list>
#include <map>
#include <thread>
#include <random>
#include <memory>
#include <new>

// ------------------------
// Custom engine allocator wrapper
// ------------------------
struct EngineMalloc {
    static void* Allocate(size_t size) { return std::malloc(size); }
    static void  Deallocate(void* ptr) { std::free(ptr); }
};

struct EngineMimalloc {
    static void* Allocate(size_t size) { return mi_malloc(size); }
    static void  Deallocate(void* ptr) { mi_free(ptr); }
};

// STL allocator encapsulation
template <typename T, typename AllocPolicy>
struct EngineAllocator {
    using value_type = T;

    EngineAllocator() = default;

    template <class U>
    EngineAllocator(const EngineAllocator<U, AllocPolicy>&) {}

    T* allocate(std::size_t n) {
        void* ptr = AllocPolicy::Allocate(n * sizeof(T));
        if (!ptr) throw std::bad_alloc();
        return static_cast<T*>(ptr);
    }

    void deallocate(T* p, std::size_t) noexcept {
        AllocPolicy::Deallocate(p);
    }
};

// ------------------------
// Benchmark: Single allocation/release
// ------------------------
template <typename AllocPolicy>
static void BM_SimpleAlloc(benchmark::State& state) {
    size_t size = state.range(0);
    for (auto _ : state) {
        void* ptr = AllocPolicy::Allocate(size);
        benchmark::DoNotOptimize(ptr);
        AllocPolicy::Deallocate(ptr);
    }
}

// ------------------------
// Benchmark: STL vector push_back
// ------------------------
template <typename AllocPolicy>
static void BM_VectorPushBack(benchmark::State& state) {
    size_t vec_size = state.range(0);
    using VecType = std::vector<int, EngineAllocator<int, AllocPolicy>>;

    for (auto _ : state) {
        VecType v;
        v.reserve(vec_size);
        for (size_t i = 0; i < vec_size; ++i) {
            v.push_back(static_cast<int>(i));
        }
        benchmark::DoNotOptimize(v.data());
    }
}

// ------------------------
// Benchmark: STL map insert
// ------------------------
template <typename AllocPolicy>
static void BM_MapInsert(benchmark::State& state) {
    size_t count = state.range(0);
    using MapType = std::map<int, int, std::less<int>, EngineAllocator<std::pair<const int, int>, AllocPolicy>>;

    for (auto _ : state) {
        MapType m;
        for (size_t i = 0; i < count; ++i) {
            m[i] = static_cast<int>(i);
        }
        benchmark::DoNotOptimize(m.size());
    }
}

// ------------------------
// Benchmark: Multi-thread allocation
// ------------------------
template <typename AllocPolicy>
static void BM_MultiThreadAlloc(benchmark::State& state) {
    size_t size = state.range(0);
    int threads = static_cast<int>(state.range(1));

    for (auto _ : state) {
        std::vector<std::thread> workers;
        for (int t = 0; t < threads; ++t) {
            workers.emplace_back([=]() {
                for (int i = 0; i < 1000; ++i) {
                    void* ptr = AllocPolicy::Allocate(size);
                    benchmark::DoNotOptimize(ptr);
                    AllocPolicy::Deallocate(ptr);
                }
                });
        }
        for (auto& w : workers) w.join();
    }
}

// ------------------------
// Benchmark: Object pool allocation/long-lived objects
// ------------------------
template <typename AllocPolicy>
static void BM_ObjectPool(benchmark::State& state) {
    constexpr size_t pool_size = 10000;

    for (auto _ : state) {
        std::vector<void*> pool;
        pool.reserve(pool_size);

        for (size_t i = 0; i < pool_size; ++i) {
            pool.push_back(AllocPolicy::Allocate(64));
        }

        benchmark::DoNotOptimize(pool.data());

        for (auto p : pool) AllocPolicy::Deallocate(p);
        pool.clear();
    }
}

// ------------------------
// Benchmark: Block allocation
// ------------------------
template <typename AllocPolicy>
static void BM_LargeAlloc(benchmark::State& state) {
    size_t size = state.range(0);
    for (auto _ : state) {
        void* ptr = AllocPolicy::Allocate(size);
        benchmark::DoNotOptimize(ptr);
        AllocPolicy::Deallocate(ptr);
    }
}

// ------------------------
// Benchmark: Simulation of fragmentation
// ------------------------
template <typename AllocPolicy>
static void BM_Fragmentation(benchmark::State& state) {
    size_t alloc_count = state.range(0);
    std::vector<void*> pool;
    pool.reserve(alloc_count);

    std::random_device rd;
    std::mt19937 rng(rd());

    for (auto _ : state) {
        pool.clear();
        for (size_t i = 0; i < alloc_count; ++i) {
            size_t sz = 16 + (rng() % 256); // Random patch
            pool.push_back(AllocPolicy::Allocate(sz));
        }
        std::shuffle(pool.begin(), pool.end(), rng);
        for (auto p : pool) AllocPolicy::Deallocate(p);
    }
}

// ------------------------
// Registering for benchmark Tests
// ------------------------
#define REGISTER_BASIC(AllocPolicy) \
BENCHMARK_TEMPLATE(BM_SimpleAlloc, AllocPolicy)->Arg(16)->Arg(64)->Arg(256)->Arg(1024)->Arg(4096); \
BENCHMARK_TEMPLATE(BM_VectorPushBack, AllocPolicy)->Arg(1000)->Arg(10000)->Arg(100000); \
BENCHMARK_TEMPLATE(BM_MapInsert, AllocPolicy)->Arg(1000)->Arg(10000); \
BENCHMARK_TEMPLATE(BM_MultiThreadAlloc, AllocPolicy)->Args({64, 4})->Args({64, 8}); \
BENCHMARK_TEMPLATE(BM_ObjectPool, AllocPolicy)->Arg(10000); \
BENCHMARK_TEMPLATE(BM_LargeAlloc, AllocPolicy)->Arg(1024*1024*10); \
BENCHMARK_TEMPLATE(BM_Fragmentation, AllocPolicy)->Arg(10000);

REGISTER_BASIC(EngineMalloc)
REGISTER_BASIC(EngineMimalloc)

BENCHMARK_MAIN();


/*
Run on (48 X 2496 MHz CPU s)
CPU Caches:
  L1 Data 48 KiB (x24)
  L1 Instruction 32 KiB (x24)
  L2 Unified 2048 KiB (x24)
  L3 Unified 46080 KiB (x1)
-----------------------------------------------------------------------------------
Benchmark                                         Time             CPU   Iterations
-----------------------------------------------------------------------------------
BM_SimpleAlloc<EngineMalloc>/16                39.7 ns         24.5 ns     24888889
BM_SimpleAlloc<EngineMalloc>/64                41.9 ns         30.8 ns     24888889
BM_SimpleAlloc<EngineMalloc>/256               38.0 ns         30.1 ns     32164102
BM_SimpleAlloc<EngineMalloc>/1024              34.2 ns         22.8 ns     30877538
BM_SimpleAlloc<EngineMalloc>/4096              39.3 ns         23.5 ns     49777778
BM_VectorPushBack<EngineMalloc>/1000            948 ns          610 ns       896000
BM_VectorPushBack<EngineMalloc>/10000         10563 ns         6719 ns       100000
BM_VectorPushBack<EngineMalloc>/100000       105920 ns        73438 ns        10000
BM_MapInsert<EngineMalloc>/1000               78075 ns        50000 ns        10000
BM_MapInsert<EngineMalloc>/10000             963272 ns       706215 ns         1239
BM_MultiThreadAlloc<EngineMalloc>/64/4      2910524 ns        15625 ns         1000
BM_MultiThreadAlloc<EngineMalloc>/64/8      6165736 ns        46875 ns         1000
BM_ObjectPool<EngineMalloc>/10000            433984 ns       302419 ns         2635
BM_LargeAlloc<EngineMalloc>/10485760          10129 ns         9521 ns        64000
BM_Fragmentation<EngineMalloc>/10000         884973 ns       593750 ns         1000

BM_SimpleAlloc<EngineMimalloc>/16              15.7 ns         11.0 ns    112000000
BM_SimpleAlloc<EngineMimalloc>/64              16.0 ns         9.84 ns    100000000
BM_SimpleAlloc<EngineMimalloc>/256             15.7 ns         11.1 ns     74666667
BM_SimpleAlloc<EngineMimalloc>/1024            17.8 ns         11.7 ns     64000000
BM_SimpleAlloc<EngineMimalloc>/4096            27.8 ns         15.1 ns     37333333
BM_VectorPushBack<EngineMimalloc>/1000          969 ns          663 ns       896000
BM_VectorPushBack<EngineMimalloc>/10000        9460 ns         6250 ns       100000
BM_VectorPushBack<EngineMimalloc>/100000     103713 ns        61384 ns        11200
BM_MapInsert<EngineMimalloc>/1000             43281 ns        26750 ns        34462
BM_MapInsert<EngineMimalloc>/10000           525485 ns       393031 ns         1948
BM_MultiThreadAlloc<EngineMimalloc>/64/4    4052120 ns        15625 ns         1000
BM_MultiThreadAlloc<EngineMimalloc>/64/8    9532928 ns        31250 ns         1000
BM_ObjectPool<EngineMimalloc>/10000           70203 ns        53125 ns        10000
BM_LargeAlloc<EngineMimalloc>/10485760          375 ns          272 ns      2986667
BM_Fragmentation<EngineMimalloc>/10000       254061 ns       175773 ns         4978
*/