# DDS Implementation Fixes - Prioritized TODO

**Status**: Production-Blocking Issues Found
**Created**: 2026-02-17
**Author**: Code Review

---

## CRITICAL (Must Fix Before Production)

### 1. Memory Leak in `to_llama_request()` - ~500 bytes/request

**File**: `dds/dds_idl_wrapper.h:67-129`

**Problem**: All allocations via `strdup()` and `malloc()` are never freed after `dds_write()`.

**Fix Required**:
```cpp
// Add cleanup function
inline void free_llama_request(llama_ChatCompletionRequest& req) {
    dds_free(req.request_id);
    dds_free(req.model);
    dds_free(req.messages._buffer[i].role);  // loop
    dds_free(req.messages._buffer[i].content); // loop
    dds_free(req.messages._buffer);
    dds_free(req.top_p._buffer);
    dds_free(req.n._buffer);
    // ... other fields
}

// Call after dds_write() in send_response()
```

**Impact**: OOM after 10,000-100,000 requests.

---

### 2. Race Condition in `has_pending_requests()` / `pop_pending_request()`

**File**: `tools/server/server.cpp:280-303`

**Problem**:
```cpp
if (dds_bridge->has_pending_requests()) {  // CHECK
    auto req = dds_bridge->pop_pending_request(req);  // POP
    // Another thread may have consumed the request between CHECK and POP
}
```

**Fix Required**:
```cpp
// Atomic check-and-pop operation
bool try_pop_request(ChatCompletionRequest& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_requests_.empty()) return false;
    out = pending_requests_.begin()->second;
    pending_requests_.erase(pending_requests_.begin());
    return true;
}
```

**Impact**: Lost requests, incorrect responses to wrong clients.

---

### 3. Incomplete Cleanup on `start_server()` Failure

**File**: `dds/dds_transport.cpp:55-127`

**Problem**: If any topic/reader/writer creation fails, already-created entities leak.

**Fix Required**:
```cpp
bool start_server(...) {
    dds_entity_t participant = dds_create_participant(...);
    if (participant < 0) return false;

    auto cleanup = [&]() {
        if (request_topic_ > 0) dds_delete(request_topic_);
        // ... all other entities
        if (participant_ > 0) dds_delete(participant_);
    };

    request_topic_ = dds_create_topic(participant, ...);
    if (request_topic_ < 0) { cleanup(); return false; }
    // ... repeat for all entities
}
```

**Impact**: DDS domain leaks, eventual failure to create new participants.

---

### 4. Atomic Without Memory Order

**File**: `dds/dds_transport.cpp:231,255-258`

**Problem**:
```cpp
std::atomic<bool> running_{false};
// ...
running_ = true;  // Thread A
// ...
while (running_.load()) {  // Thread B - may never see true!
```

**Fix Required**:
```cpp
running_ = true;  // Thread A - release
while (running_.load(std::memory_order_acquire)) {  // Thread B - acquire
```

**Impact**: Undefined behavior, reader thread may never start.

---

### 5. Single Buffer Reuse in Read Loop

**File**: `dds/dds_transport.cpp:194-228`

**Problem**:
```cpp
void* sample = dds_alloc(sizeof(llama_ChatCompletionRequest));
// ...
dds_take(request_reader_, samples, infos, 1, 1);  // CONSUMES sample
// Next iteration: sample contains stale/invalid data!
```

**Fix Required**:
```cpp
// Option A: Reallocate after take
void* sample = nullptr;
while (running_) {
    dds_return_t n = dds_take(request_reader_, &sample, &info, 1, 1);
    if (n > 0 && info.valid_data) {
        process(sample);
        dds_sample_free(request_reader_, sample, DDS_FREE_ALL);
    }
}

// Option B: Use dds_read (non-consuming) with proper sample management
```

**Impact**: Corrupted data, crashes, or infinite loops.

---

## HIGH (Should Fix Before Release)

### 6. Memory Leaks in test_client.cpp

**File**: `dds/test_client.cpp:178-181`

**Problem**: All `dds_string_dup()` and `malloc()` allocations never freed.

**Fix Required**:
```cpp
// Before dds_delete(participant):
dds_free(req.request_id);
dds_free(req.model);
dds_free(req.messages._buffer[0].role);
dds_free(req.messages._buffer[0].content);
dds_free(req.messages._buffer);
```

---

### 7. Memory Leaks in benchmark_final.cpp

**File**: `dds/benchmark_final.cpp:48-77`

**Problem**: Every iteration leaks ~100+ bytes.

**Fix Required**: Create `free_llama_request()` helper and call it after each `dds_write()`.

---

### 8. Non-Compliant UUID v4 Generation

**File**: `dds/dds_transport.cpp:22-40` and `tools/server/server.cpp:117`

**Problem**: UUID variant bits don't conform to RFC 4122.

**Fix Required**:
```cpp
static std::string generate_request_id() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dis;

    uint8_t bytes[16];
    for (int i = 0; i < 16; i += 4) {
        uint32_t val = dis(gen);
        bytes[i] = (val >> 0) & 0xFF;
        bytes[i+1] = (val >> 8) & 0xFF;
        bytes[i+2] = (val >> 16) & 0xFF;
        bytes[i+3] = (val >> 24) & 0xFF;
    }

    // Set version (4) and variant (8, 9, A, or B)
    bytes[6] = (bytes[6] & 0x0F) | 0x40;  // Version 4
    bytes[8] = (bytes[8] & 0x3F) | 0x80;  // Variant RFC 4122

    // Format as UUID string
    snprintf(buf, sizeof(buf),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        bytes[0], bytes[1], bytes[2], bytes[3],
        bytes[4], bytes[5], bytes[6], bytes[7],
        bytes[8], bytes[9], bytes[10], bytes[11],
        bytes[12], bytes[13], bytes[14], bytes[15]);
}
```

---

### 9. Ineffective try-catch Block

**File**: `dds/dds_transport.cpp:56-127`

**Problem**: DDS API returns error codes, doesn't throw exceptions.

**Fix Required**:
```cpp
bool start_server(...) {
    // Remove try-catch entirely
    // DDS functions return negative codes on error
    dds_entity_t participant = dds_create_participant(domain_id_, nullptr, nullptr);
    if (participant < 0) {
        fprintf(stderr, "[DDS] Failed to create participant: %d\n", participant);
        return false;
    }
    // ... rest of function
}
```

---

### 10. Missing QoS in test_client and benchmark

**File**: `dds/test_client.cpp:77-79` and `dds/benchmark_final.cpp`

**Problem**: `HISTORY` QoS not set, may cause sample loss under load.

**Fix Required**:
```cpp
dds_qos_t* qos = dds_create_qos();
dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(10));
dds_qset_durability(qos, DDS_DURABILITY_TRANSIENT_LOCAL);
dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, 8);  // ADD THIS
```

---

## MEDIUM (Technical Debt)

### 11. strdup() Portability Issue

**File**: `dds/dds_idl_wrapper.h:27-28`

**Problem**: `strdup()` is POSIX, not standard C++. Fails on some compilers/platforms.

**Fix Required**:
```cpp
// Use std::string + c_str() where possible, or
// Use platform-specific alternatives
#ifdef _WIN32
#define strdup _strdup
#endif
```

---

### 12. Stub Functions Should Be Asserts

**File**: `dds/dds_transport.cpp:275-286`

**Problem**: `start_client()` and `send_request()` are stubs that silently fail.

**Fix Required**:
```cpp
bool DDSTransport::start_client() {
    LOG_ERROR("[DDS] Client mode not implemented");
    return false;
}
```

Or use `[[nodiscard]]` attribute.

---

### 13. Code Duplication

**File**: `dds/dds_transport.cpp:55-127`

**Problem**: Same error handling pattern repeated 6 times.

**Fix Required**: Create helper macro:
```cpp
#define DDS_CHECK_ENTITY(name, expr) \
    name ## _ = expr; \
    if (name ## _ < 0) { \
        fprintf(stderr, "[DDS] Failed to create " #name ": %d\n", name ## _); \
        return false; \
    }
```

---

## TESTING GAPS

### 14. No Unit Tests

**Status**: No unit tests exist for DDS module.

**Required Tests**:
- `test_dds_types.cpp`: Type conversion tests
- `test_dds_transport.cpp`: Transport lifecycle tests
- `test_dds_bridge.cpp`: Bridge integration tests

---

### 15. No Error Path Tests

**Status**: Only happy path tested.

**Required Tests**:
- Participant creation failure
- Topic creation failure
- Writer/Reader creation failure
- Write failure scenarios
- Timeout scenarios

---

### 16. No Concurrency Tests

**Status**: No thread safety testing.

**Required Tests**:
- Multiple simultaneous requests
- Concurrent read/write
- Callback thread safety
- Race condition scenarios

---

## BENCHMARKS

### Performance Observations

| Metric | HTTP | DDS | Notes |
|--------|------|-----|-------|
| Mean Latency | 5,160ms | 2,020ms | DDS 2.55x faster |
| Std Dev | 562ms | 3ms | DDS much more consistent |
| p95 | 5,944ms | 2,023ms | DDS predictable |
| Throughput | 0.19/s | 0.50/s | DDS 2.63x more |

### Potential Optimizations (Post-Fix)

1. Replace polling with Waitset
2. Add streaming support
3. Enable SHM transport by default
4. Batch multiple requests
5. Zero-copy serialization

---

## FILES TO MODIFY

1. `dds/dds_idl_wrapper.h` - Add cleanup functions
2. `dds/dds_transport.cpp` - Fix race conditions, atomics, cleanup
3. `dds/test_client.cpp` - Add cleanup, QoS history
4. `dds/benchmark_final.cpp` - Add cleanup
5. `tools/server/server.cpp` - Fix race condition, UUID
6. `docs/dds.md` - Update troubleshooting section

---

## ESTIMATED EFFORT

| Priority | Tasks | Estimated Time |
|----------|-------|----------------|
| CRITICAL | 5 | 4-6 hours |
| HIGH | 5 | 2-4 hours |
| MEDIUM | 3 | 1-2 hours |
| TESTING | 3 | 4-8 hours |

**Total**: 11-20 hours for production-ready code.

---

## REFERENCES

- [CycloneDDS Documentation](https://docs.cyclonedds.io/)
- [RFC 4122 - UUID Version 4](https://tools.ietf.org/html/rfc4122)
- [C++ Atomics and Memory Model](https://en.cppreference.com/w/cpp/atomic/memory_order)
