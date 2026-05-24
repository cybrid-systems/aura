# runtime.c 单元测试计划

**目标**：独立测试 `lib/runtime.c` 的 Bump Allocator、Drop 函数族、闭包捕获等运行时功能，不依赖 Aura 编译器管线。

---

## 测试框架

用纯 C 写测试 harness，编译 + 链接 `runtime.c` 运行：

```bash
gcc -g tests/runtime_test_harness.c lib/runtime.c -o /tmp/runtime_test -lm
/tmp/runtime_test
```

每个测试函数以 `test_` 开头，通过 `assert()` 或自定义检查验证结果。

---

## 测试用例

### 1. Bump Allocator

```c
void test_bump_basic() {
    aura_bump_init();
    void* p1 = aura_bump_alloc(64, 8);
    void* p2 = aura_bump_alloc(64, 8);
    assert(p1 != NULL);
    assert(p2 != NULL);
    assert((char*)p2 - (char*)p1 == 64); // contiguous
    aura_bump_reset();
    // After reset, next alloc should reuse memory
    void* p3 = aura_bump_alloc(64, 8);
    assert(p3 == p1); // reuses from start
    printf("  ✅ bump_basic\n");
}
```

- `test_bump_basic` — 基本分配 + 连续 + reset 重用
- `test_bump_alignment` — 不同对齐（4/8/16 字节）
- `test_bump_overflow` — 动态扩容（分配超过初始 64MB → realloc）
- `test_bump_multi_reset` — 多次 init/reset 循环

### 2. Pair 分配 + Drop + Free List

```c
void test_pair_drop_reuse() {
    int64_t p1 = aura_alloc_pair(10, 20);
    int64_t p2 = aura_alloc_pair(30, 40);
    assert(aura_pair_car(p1) == 10);
    assert(aura_pair_cdr(p2) == 40);
    aura_drop_pair(p1);
    // After drop, p1's slot should be reused
    int64_t p3 = aura_alloc_pair(50, 60);
    assert(p3 == p1); // same slot reused
    assert(aura_pair_car(p3) == 50);
    printf("  ✅ pair_drop_reuse\n");
}
```

- `test_pair_basic` — 创建 + car/cdr 读取
- `test_pair_drop_reuse` — drop 后复用
- `test_pair_double_drop` — 幂等性（重复 drop 不崩溃）
- `test_pair_oob` — 越界 car/cdr 返回 0
- `test_pair_large_count` — 大量分配 + drop 循环

### 3. Cell 分配 + Drop + Free List

类似 pair 测试：

- `test_cell_basic` — 创建 + get/set
- `test_cell_drop_reuse` — drop 后复用
- `test_cell_double_drop` — 幂等性
- `test_cell_oob` — 越界 get/set 不崩溃

### 4. 闭包捕获 + 调用

```c
int64_t test_fn(int64_t* args, uint32_t argc) {
    return args[0] + args[1];
}

void test_closure_capture_and_call() {
    int64_t cid = aura_alloc_closure(0);
    aura_register_closure_fn(cid, (int64_t)(intptr_t)test_fn, 2);
    aura_closure_capture(cid, 0, 42); // capture value into env[0]
    int64_t args[2] = {10, 20};
    int64_t result = aura_closure_call(cid, args, 2);
    assert(result == 30);          // fn result
    // env[0] should be 42 (captured)
    printf("  ✅ closure_capture_and_call\n");
}
```

- `test_closure_basic` — 创建 + 调用
- `test_closure_capture` — 捕获 + 验证
- `test_closure_drop` — drop 后调用返回 0
- `test_closure_oob` — 越界 ID 安全
- `test_closure_multi_capture` — 最多 8 个捕获槽

### 5. 字符串

```c
void test_string_alloc() {
    int64_t s1 = aura_alloc_string("hello");
    int64_t s2 = aura_alloc_string("world");
    const char* r1 = aura_string_ref(s1);
    const char* r2 = aura_string_ref(s2);
    assert(strcmp(r1, "hello") == 0);
    assert(strcmp(r2, "world") == 0);
    // Empty string
    int64_t s3 = aura_alloc_string("");
    assert(strcmp(aura_string_ref(s3), "") == 0);
    printf("  ✅ string_alloc\n");
}
```

- `test_string_basic` — 创建 + 读取
- `test_string_empty` — 空字符串
- `test_string_oob` — 越界 ref 返回 ""
- `test_string_large` — 大字符串（~100KB）

### 6. Bump + Drop 混合场景

```c
void test_bump_and_drop_mixed() {
    aura_bump_init();
    // Bump alloc some memory
    void* buf = aura_bump_alloc(256, 8);
    // Use pair/cell APIs (which use their own internal Free List)
    int64_t p1 = aura_alloc_pair(1, 2);
    int64_t p2 = aura_alloc_pair(3, 4);
    aura_drop_pair(p1);
    aura_bump_reset(); // reset doesn't affect pair heap
    // Bump memory is reset, pair memory still valid
    assert(aura_pair_car(p2) == 3);
    printf("  ✅ bump_and_drop_mixed\n");
}
```

---

## 执行

```bash
# 编译并运行所有测试
gcc -g tests/runtime_test_harness.c lib/runtime.c -o /tmp/runtime_test -lm
/tmp/runtime_test

# 带 AddressSanitizer
gcc -g -fsanitize=address tests/runtime_test_harness.c lib/runtime.c -o /tmp/runtime_test_asan -lm
/tmp/runtime_test_asan
```

**预期输出**：
```
=== runtime.c unit tests ===
  ✅ bump_basic
  ✅ bump_alignment
  ✅ pair_drop_reuse
  ...
  ✅ all (18/18 passed)
```
