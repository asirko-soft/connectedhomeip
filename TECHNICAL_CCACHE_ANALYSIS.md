# Technical Analysis: ccache, Preprocessor, and the `-P` Flag Bug

## Table of Contents
1. [How ccache Works](#how-ccache-works)
2. [The Preprocessor and #line Directives](#the-preprocessor-and-line-directives)
3. [What the `-P` Flag Does](#what-the--p-flag-does)
4. [Why This Breaks ccache](#why-this-breaks-ccache)
5. [Proper Optimization Strategies](#proper-optimization-strategies)

---

## How ccache Works

### Basic Mechanism

ccache is a compiler cache that speeds up recompilation by caching compilation results. Here's the process:

```
Source File → Preprocessor → Hash Input → Check Cache
                                ↓
                              Hit? → Return Cached .o
                                ↓
                              Miss → Compile → Store in Cache
```

### Key Steps:

1. **Invocation**: ccache intercepts compiler calls
2. **Preprocessing**: Runs the preprocessor (`cpp`) on the source file
3. **Hashing**: Creates a hash from:
   - Compiler command line
   - Preprocessed output
   - Compiler binary
4. **Cache Lookup**: Checks if hash exists in cache
5. **Result**: Returns cached object file OR compiles and caches

### Configuration: `CCACHE_COMPILERCHECK`

This setting controls how ccache detects compiler changes:

```bash
# mtime (default in older versions) - check compiler modification time
CCACHE_COMPILERCHECK=mtime

# content - hash the compiler binary
CCACHE_COMPILERCHECK=content  # ← Used in connectedhomeip

# none - assume compiler never changes
CCACHE_COMPILERCHECK=none

# string - use a custom string
CCACHE_COMPILERCHECK="gcc-9.3.0"
```

**Content mode** is best for correctness but requires hashing the preprocessed output accurately.

---

## The Preprocessor and #line Directives

### Normal Preprocessing

When you run the C preprocessor on a file:

```cpp
// source.cpp
#include "header.h"

void function() {
    doSomething();
}
```

The preprocessor produces:

```cpp
# 1 "source.cpp"
# 1 "<built-in>"
# 1 "<command-line>"
# 1 "source.cpp"
# 1 "header.h" 1
// ... header contents ...
# 350 "header.h"
void doSomething(int x, NewType* param);
# 2 "source.cpp" 2

void function() {
    doSomething();
}
```

### What are `#line` Directives?

```cpp
# 350 "header.h"
```

This means: "The following code came from line 350 of header.h"

**Purpose**:
1. **Error messages**: Compiler reports errors at correct file:line
2. **Debugging**: Debugger shows correct source locations
3. **Dependency tracking**: Shows which headers were included
4. **Change detection**: Shows when header content changes

---

## What the `-P` Flag Does

### GCC/Clang `-P` Flag Documentation

From GCC manual:
```
-P  Inhibit generation of linemarkers in the output from the preprocessor.
    This might be useful when running the preprocessor on something that
    is not C code, and will be sent to a program which might be confused
    by the linemarkers.
```

### Example:

**Without `-P`**:
```cpp
# 1 "source.cpp"
# 1 "header.h" 1
# 350 "header.h"
void func(int x, NewType* ptr);
# 2 "source.cpp" 2
void func(int x, NewType* ptr) { }
```

**With `-P`**:
```cpp
void func(int x, NewType* ptr);
void func(int x, NewType* ptr) { }
```

**Notice**: All `#line` directives removed!

---

## Why This Breaks ccache

### The Problem: Loss of Change Detection

#### Scenario 1: Header Changes (The connectedhomeip Bug)

**Initial State (master branch)**:

```cpp
// attribute-storage.h
void emberAfAttributeChanged(..., AttributesChangedListener* listener);

// attribute-storage.cpp
#include "attribute-storage.h"
void emberAfAttributeChanged(..., AttributesChangedListener* listener) {
    // implementation
}
```

**Preprocessed WITH -P**:
```cpp
void emberAfAttributeChanged(..., AttributesChangedListener* listener);
void emberAfAttributeChanged(..., AttributesChangedListener* listener) {
    // implementation
}
```

**Hash**: `abc123def456` (cached)

**After PR Changes Header**:

```cpp
// attribute-storage.h (NEW)
void emberAfAttributeChanged(..., ProviderChangeListener* listener);

// attribute-storage.cpp (UNCHANGED)
#include "attribute-storage.h"
void emberAfAttributeChanged(..., ProviderChangeListener* listener) {
    // implementation
}
```

**Preprocessed WITH -P**:
```cpp
void emberAfAttributeChanged(..., ProviderChangeListener* listener);
void emberAfAttributeChanged(..., ProviderChangeListener* listener) {
    // implementation
}
```

**Hash**: `abc123def456` ← **SAME HASH!**

**Result**: ccache thinks nothing changed → returns OLD object file → LINKER ERROR

#### Why the Hash is the Same

Without `#line` directives, ccache sees:
- Same function name ✓
- Same implementation ✓
- No indication of which header was included ✗
- No indication header content changed ✗

**With `#line` directives**, the preprocessed output would be:

```cpp
# 350 "attribute-storage.h"
void emberAfAttributeChanged(..., ProviderChangeListener* listener);
# 1585 "attribute-storage.cpp"
void emberAfAttributeChanged(..., ProviderChangeListener* listener) {
```

The line number and filename in the directive would be part of the hash, making it different!

---

### Scenario 2: Why Samsung Wanted `-P`

**The Goal**: Improve cache hit rate across different build locations

**Problem they tried to solve**:

```cpp
# 1 "/home/user1/project/source.cpp"
# 1 "/home/user1/project/header.h"
```

vs.

```cpp
# 1 "/home/user2/project/source.cpp"
# 1 "/home/user2/project/header.h"
```

Different absolute paths → different hashes → cache miss

**Their "solution"**: Remove `#line` directives with `-P`
- Both become just the code
- Same hash
- Cache hit! 🎉

**But**: This breaks change detection! 🔥

---

## The Complete Failure Mode

### Step-by-Step Breakdown

1. **Master branch build** (Oct 17-20, with `-P` flag active):
   ```
   attribute-storage.cpp → Preprocess with -P → Hash = abc123
   → Compile → Cache object file with hash abc123
   ```

2. **PR changes header**:
   ```cpp
   // attribute-storage.h
   - void func(..., AttributesChangedListener*)
   + void func(..., ProviderChangeListener*)
   ```

3. **PR builds attribute-table.cpp** (calls the function):
   ```
   Preprocessor sees NEW header → Different code → CACHE MISS
   → Compiles with NEW signature → Expects to link with NEW signature
   ```

4. **PR builds attribute-storage.cpp** (defines the function):
   ```
   Preprocessor with -P → Output looks same → Hash = abc123
   → CACHE HIT! → Returns OLD object file (OLD signature)
   ```

5. **Linker**:
   ```
   attribute-table.o: "Call void func(..., ProviderChangeListener*)"
   attribute-storage.o: "Define void func(..., AttributesChangedListener*)"
   
   ERROR: undefined symbol: func(..., ProviderChangeListener*)
   ```

---

## Proper Optimization Strategies

### ✅ Correct Ways to Improve ccache Hit Rate

#### 1. Use `CCACHE_BASEDIR` Correctly

**Purpose**: Make paths relative to a base directory

```bash
export CCACHE_BASEDIR=/workspace/project
```

**How it works**:
- ccache rewrites absolute paths in `#line` directives to be relative
- `/workspace/project/src/file.cpp` becomes `src/file.cpp` in the hash
- Cache portable across different base paths

**Configuration in connectedhomeip**:
```yaml
CCACHE_BASEDIR: ${{ github.workspace }}  # ✅ Already doing this
```

#### 2. Use `CCACHE_NOHASHDIR`

**Purpose**: Don't include current directory in hash

```bash
export CCACHE_NOHASHDIR=1  # ✅ Already enabled
```

This helps when building from different directories.

#### 3. Use `CCACHE_SLOPPINESS` Appropriately

**Purpose**: Ignore certain changes that don't affect output

```bash
CCACHE_SLOPPINESS=time_macros  # ✅ Already enabled
```

Options:
- `time_macros`: Ignore `__TIME__`, `__DATE__`, `__TIMESTAMP__`
- `file_macro`: Ignore `__FILE__` paths
- `include_file_mtime`: Ignore header mtimes (use content)
- `pch_defines`: Handle precompiled headers better

**What NOT to do**: `file_stat_matches` - can cause stale cache

#### 4. Ensure Deterministic Builds

**Avoid**:
- Random values in builds
- Timestamps (unless ignored by sloppiness)
- Environment-dependent paths

**Use**:
- Reproducible builds
- Fixed seeds for any randomness
- Relative paths

#### 5. Share Cache Across Jobs (Carefully)

**Current connectedhomeip approach**:
```yaml
key: ccache-${{ runner.os }}-${{ variant }}-w${{ week }}
```

**Better approaches**:

**Option A**: Include commit in key (safest)
```yaml
key: ccache-${{ runner.os }}-${{ variant }}-${{ github.sha }}
restore-keys: |
  ccache-${{ runner.os }}-${{ variant }}-${{ github.base_ref }}-
```

**Option B**: Branch-based cache
```yaml
key: ccache-${{ runner.os }}-${{ variant }}-${{ github.ref }}-${{ github.sha }}
restore-keys: |
  ccache-${{ runner.os }}-${{ variant }}-${{ github.ref }}-
  ccache-${{ runner.os }}-${{ variant }}-refs/heads/master-
```

---

### ❌ What NOT to Do

#### 1. **Don't Remove `#line` Directives**

```bash
# BAD - breaks change detection
CCACHE_PREFIX_CPP=script-that-adds-P-flag.sh
```

#### 2. **Don't Use `mtime` Mode with Shared Cache**

```bash
# BAD - not reliable across machines
CCACHE_COMPILERCHECK=mtime
```

#### 3. **Don't Use Excessive Sloppiness**

```bash
# BAD - too sloppy, might miss real changes
CCACHE_SLOPPINESS=file_macro,file_stat_matches,include_file_ctime,include_file_mtime,system_headers,time_macros,pch_defines
```

#### 4. **Don't Symlink Compilers to ccache**

```bash
# BAD - can confuse build systems
ln -s /usr/bin/ccache /usr/local/bin/gcc
```

Instead, use explicit wrapping or launcher:
```bash
export CC="ccache gcc"  # ✅ Better
```

Or:
```cmake
set(CMAKE_C_COMPILER_LAUNCHER ccache)  # ✅ Best
```

---

## Best Practices Summary

### Required Configuration (connectedhomeip has these ✅)

```bash
CCACHE_COMPILERCHECK=content  # Hash compiler binary
CCACHE_BASEDIR=/workspace     # Make paths relative
CCACHE_NOHASHDIR=1           # Don't hash current dir
CCACHE_SLOPPINESS=time_macros # Ignore timestamp macros
```

### Additional Recommendations

```bash
CCACHE_MAXSIZE=5G            # Set appropriate size
CCACHE_DIR=/cache/location   # Persistent location
CCACHE_COMPRESS=1            # Save disk space (optional)
CCACHE_COMPRESSLEVEL=6       # Balance compression vs speed
```

### Cache Key Strategy

**For CI**:
```yaml
# Option 1: Commit-based (safest)
key: ccache-${{ os }}-${{ variant }}-${{ sha }}
restore-keys: |
  ccache-${{ os }}-${{ variant }}-${{ base_ref }}-

# Option 2: Week-based (what was used, acceptable with correct config)
key: ccache-${{ os }}-${{ variant }}-w${{ week }}
```

**Critical**: With week-based keys, `CCACHE_COMPILERCHECK=content` MUST work correctly (no `-P` flag!)

---

## Verification Steps

### 1. Test with Signature Changes

Create a test that:
1. Builds with function signature X
2. Changes header to signature Y
3. Rebuilds
4. Verifies linker succeeds (proves cache didn't inappropriately reuse)

### 2. Monitor ccache Stats

```bash
ccache -s
```

Look for:
- Hit rate (should be high for unchanged code)
- Miss rate (should increase when headers change)
- Errors (should be zero)

### 3. Enable Debug Logging

```bash
export CCACHE_DEBUG=1
export CCACHE_LOGFILE=/tmp/ccache.log
```

Check logs for cache decisions.

---

## Conclusion

### The Root Cause

The `-P` flag removed `#line` directives, which are ESSENTIAL for:
1. Detecting header changes
2. Tracking dependencies
3. Ensuring correct compilation

### The Fix

Remove `CCACHE_PREFIX_CPP` entirely. Use proper ccache configuration instead.

### The Lesson

**Performance optimizations must not compromise correctness.**

The samsung team wanted better cache hits, but:
- ✅ Goal: Improve build performance
- ❌ Method: Remove essential information
- ✅ Result: Faster builds... sometimes
- ❌ Side effect: Incorrect builds... sometimes

**Correct approach**: Use ccache's built-in features (BASEDIR, NOHASHDIR) which are designed to improve portability WITHOUT breaking change detection.

### Current Status

✅ connectedhomeip has correct configuration as of Oct 20, 2025:
- No `-P` flag
- `CCACHE_COMPILERCHECK=content`
- `CCACHE_BASEDIR` set
- `CCACHE_NOHASHDIR=1`
- Appropriate sloppiness settings

**No further action needed** - the caching is now both FAST and CORRECT.
