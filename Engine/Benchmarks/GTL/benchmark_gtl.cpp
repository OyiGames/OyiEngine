#include <unordered_map>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <numeric>
#include <random>
#include <algorithm>

#include <benchmark/benchmark.h>

// GTL headers (确保 include path 中包含 gtl/include)
#include <gtl/phmap.hpp>    // flat_hash_map, parallel_flat_hash_map
#include <gtl/vector.hpp>   // gtl::vector

//using gtl::flat_hash_map;
//using gtl::parallel_flat_hash_map;
//using gtl::vector;

// ------------------------------------------------------
// 模拟游戏引擎用例：Entity 数据
// ------------------------------------------------------
struct EntityData {
    int id;
    float x, y, z;
    int health;
};

// ------------------------------------------------------
// 生成实体数据（返回 std::vector 以便作为统一的数据源）
// ------------------------------------------------------
static std::vector<EntityData> GenerateEntities(size_t count) {
    std::vector<EntityData> entities;
    entities.reserve(count);
    std::mt19937 rng{ 42 };
    std::uniform_real_distribution<float> pos(-500.f, 500.f);
    for (size_t i = 0; i < count; ++i) {
        entities.push_back({ static_cast<int>(i), pos(rng), pos(rng), pos(rng), 100 });
    }
    return entities;
}

// ------------------------------------------------------
// 全局参数（你可以根据机器调整）
// ------------------------------------------------------
constexpr size_t ENTITY_COUNT = 200'000;

// ------------------------------------------------------
// 测试 1：单线程插入性能（Entity 注册）
// ------------------------------------------------------
static void BM_STL_UnorderedMap_Insert(benchmark::State& state) {
    auto entities = GenerateEntities(ENTITY_COUNT);
    for (auto _ : state) {
        std::unordered_map<int, EntityData> table;
        table.reserve(ENTITY_COUNT);
        for (const auto& e : entities)
            table.emplace(e.id, e);
        benchmark::DoNotOptimize(table);
    }
}
BENCHMARK(BM_STL_UnorderedMap_Insert);

static void BM_GTL_FlatHashMap_Insert(benchmark::State& state) {
    auto entities = GenerateEntities(ENTITY_COUNT);
    for (auto _ : state) {
        gtl::flat_hash_map<int, EntityData> table;
        table.reserve(ENTITY_COUNT);
        for (const auto& e : entities)
            table.emplace(e.id, e);
        benchmark::DoNotOptimize(table);
    }
}
BENCHMARK(BM_GTL_FlatHashMap_Insert);

// ------------------------------------------------------
// 测试 2：查找性能（Entity 查询）
// ------------------------------------------------------
static void BM_STL_UnorderedMap_Find(benchmark::State& state) {
    auto entities = GenerateEntities(ENTITY_COUNT);
    std::unordered_map<int, EntityData> table;
    table.reserve(ENTITY_COUNT);
    for (const auto& e : entities) table.emplace(e.id, e);

    std::vector<int> keys(ENTITY_COUNT);
    std::iota(keys.begin(), keys.end(), 0);

    for (auto _ : state) {
        volatile float sum = 0.f;
        for (int k : keys) sum += table[k].x;
        benchmark::DoNotOptimize(sum);
    }
}
BENCHMARK(BM_STL_UnorderedMap_Find);

static void BM_GTL_FlatHashMap_Find(benchmark::State& state) {
    auto entities = GenerateEntities(ENTITY_COUNT);
    gtl::flat_hash_map<int, EntityData> table;
    table.reserve(ENTITY_COUNT);
    for (const auto& e : entities) table.emplace(e.id, e);

    std::vector<int> keys(ENTITY_COUNT);
    std::iota(keys.begin(), keys.end(), 0);

    for (auto _ : state) {
        volatile float sum = 0.f;
        for (int k : keys) sum += table[k].x;
        benchmark::DoNotOptimize(sum);
    }
}
BENCHMARK(BM_GTL_FlatHashMap_Find);

// ------------------------------------------------------
// 测试 3：多线程并发插入（使用 gtl::parallel_flat_hash_map）
// ------------------------------------------------------
static void BM_GTL_ParallelFlatHashMap_Insert(benchmark::State& state) {
    auto entities = GenerateEntities(ENTITY_COUNT);
    const size_t threads = state.threads();
    const size_t per_thread = ENTITY_COUNT / threads;

    for (auto _ : state) {
        // 每轮迭代都创建新的 map
        gtl::parallel_flat_hash_map<int, EntityData> table;

        size_t begin = per_thread * state.thread_index();
        size_t end = (state.thread_index() == threads - 1) ? ENTITY_COUNT : begin + per_thread;

        for (size_t i = begin; i < end; ++i) {
            table.emplace(entities[i].id, entities[i]);
        }

        benchmark::DoNotOptimize(table);
    }
}
BENCHMARK(BM_GTL_ParallelFlatHashMap_Insert)
->Threads(1)->Threads(2)->Threads(4)->Threads(8);

// ------------------------------------------------------
// 测试 4：带外部锁的 std::unordered_map（模拟线程安全容器）
// ------------------------------------------------------
static void BM_STL_UnorderedMap_LockedInsert(benchmark::State& state) {
    auto entities = GenerateEntities(ENTITY_COUNT);
    const size_t threads = static_cast<size_t>(state.threads());
    const size_t per_thread = ENTITY_COUNT / threads;

    for (auto _ : state) {
        // 每轮 iteration 创建一个新的 map，保证线程安全
        std::unordered_map<int, EntityData> table;
        std::mutex mtx;

        size_t begin = per_thread * state.thread_index();
        size_t end = (state.thread_index() == static_cast<int>(threads - 1)) ?
            ENTITY_COUNT : begin + per_thread;

        for (size_t i = begin; i < end; ++i) {
            std::lock_guard<std::mutex> lock(mtx);
            table.emplace(entities[i].id, entities[i]);
        }

        benchmark::DoNotOptimize(table);
    }
}

BENCHMARK(BM_STL_UnorderedMap_LockedInsert)
->Threads(1)->Threads(2)->Threads(4)->Threads(8);

// ------------------------------------------------------
// 测试 5：向量性能（常见游戏 ECS 组件存储）
// ------------------------------------------------------
static void BM_STL_Vector_PushBack(benchmark::State& state) {
    auto entities = GenerateEntities(ENTITY_COUNT);
    for (auto _ : state) {
        std::vector<EntityData> vec;
        vec.reserve(ENTITY_COUNT);
        for (const auto& e : entities)
            vec.push_back(e);
        benchmark::DoNotOptimize(vec);
    }
}
BENCHMARK(BM_STL_Vector_PushBack);

static void BM_GTL_Vector_PushBack(benchmark::State& state) {
    auto entities = GenerateEntities(ENTITY_COUNT);
    for (auto _ : state) {
        gtl::vector<EntityData> vec;
        vec.reserve(ENTITY_COUNT);
        for (const auto& e : entities)
            vec.push_back(e);
        benchmark::DoNotOptimize(vec);
    }
}
BENCHMARK(BM_GTL_Vector_PushBack);

// ------------------------------------------------------
BENCHMARK_MAIN();

/*
Run on (48 X 2496 MHz CPU s)
CPU Caches:
  L1 Data 48 KiB (x24)
  L1 Instruction 32 KiB (x24)
  L2 Unified 2048 KiB (x24)
  L3 Unified 46080 KiB (x1)
***WARNING*** Library was built as DEBUG. Timings may be affected.
--------------------------------------------------------------------------------------
Benchmark                                            Time             CPU   Iterations
--------------------------------------------------------------------------------------
BM_STL_UnorderedMap_Insert                    33908788 ns     23437500 ns           26
BM_GTL_FlatHashMap_Insert                      4915045 ns      4329819 ns          166

BM_STL_UnorderedMap_Find                       2540289 ns      1725653 ns          498
BM_GTL_FlatHashMap_Find                        1252503 ns       906808 ns          896

BM_GTL_ParallelFlatHashMap_Insert/threads:1   12184390 ns      7343750 ns          100
BM_GTL_ParallelFlatHashMap_Insert/threads:2    4603627 ns      2845014 ns          346
BM_GTL_ParallelFlatHashMap_Insert/threads:4    2478440 ns      1318359 ns          640
BM_GTL_ParallelFlatHashMap_Insert/threads:8    1236606 ns       522226 ns         1496
BM_STL_UnorderedMap_LockedInsert/threads:1    31498079 ns     19761029 ns           34
BM_STL_UnorderedMap_LockedInsert/threads:2    15966561 ns      7812500 ns           64
BM_STL_UnorderedMap_LockedInsert/threads:4     7918007 ns      4333496 ns          256
BM_STL_UnorderedMap_LockedInsert/threads:8     3350376 ns      1098633 ns          896

BM_STL_Vector_PushBack                         1093642 ns       562500 ns         1000
BM_GTL_Vector_PushBack                         1130190 ns       718750 ns         1000
*/