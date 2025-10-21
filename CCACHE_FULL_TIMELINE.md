# Complete ccache CCACHE_PREFIX_CPP Timeline and Analysis

## 📅 Full Timeline

### September 9, 2025 - Original Addition
**Commit**: `06644e3ceede9c3e5e9c45bd872c1d1782903ea2`  
**PR**: #40724 "Configure ccache for a greater hit ratio"  
**Author**: Miłosz Tomkiel (Samsung)

**Added**:
```bash
#!/bin/bash
# Add -P flag to get rid of #line directives which break caching
args=()
for arg in "$@"; do
    args+=("$arg")
    [[ "$arg" == "-E" ]] && args+=("-P")
done
exec "${args[@]}"
```

**Rationale**: 
- Remove `#line` directives to improve ccache hit ratio
- `#line` directives contain absolute paths which vary between builds
- Goal: More cache hits across different build environments

**Problem**: Not immediately recognized

---

### September 10, 2025 - First Revert (Next Day!)
**Commit**: `ab7f8fbf36c823d2aca09ac52a741f58b7df7e22`  
**PR**: #40919 "Revert Configure ccache for a greater hit ratio"  
**Author**: Andrei Litvin

**Action**: Complete revert of PR #40724

**Why**: Likely discovered it caused build issues (no detailed reason in commit message)

**Duration**: Less than 24 hours before revert!

---

### October 17, 2025 - Re-addition (Simplified)
**Commit**: `9f57f3acdaa77f70af524706c5e00e51a885d76b`  
**PR**: #41503 "Enable ccache reusing between apps on some CI workflows"  
**Authors**: Arkadiusz Bokowy & Miłosz Tomkiel (Samsung)

**Added (Simplified version)**:
```bash
#!/bin/sh
# Add -P flag to get rid of #line directives which break caching
exec "$@" -P
```

**Changes from original**:
- Simpler implementation (always adds `-P`, not just with `-E`)
- Switched from bash to sh (faster)
- Part of larger PR to enable cache reusing between apps

**Rationale** (from PR description):
- Previous version was reverted, but they believed a simplified version would be safe
- Goal: Share cache between different app builds (e.g., chip-tool, all-clusters)
- Claimed benefit: Better cache hit ratio

**Warning Signs**: 
- Re-introducing something that was already reverted
- Simplified but with same fundamental problem

---

### October 17-20, 2025 - Active Period
**Duration**: 3 days

**Likely symptoms during this time**:
- Intermittent linker errors on PRs that changed headers
- "undefined symbol" errors for changed function signatures
- Builds failing in CI but succeeding locally
- Reports of "ccache seems to be broken"

**The "changed_listener_combination" PR**:
- This PR changes function signatures (AttributesChangedListener → ProviderChangeListener)
- Hit the linker error: `undefined symbol: emberAfAttributeChanged(...)`
- This was the **smoking gun** that proved the bug

---

### October 20, 2025 - Final Removal
**Commit**: `685d31adc4733237b26bc57631b55c1597028b70`  
**PR**: #41539 "Remove CCACHE_PREFIX_CPP"  
**Author**: Andrei Litvin (same person who did first revert)

**Commit message**:
```
This causes bad builds: cpp compilation reuses object files when
headers change, which results in wrong code compiled.
```

**Comments in code**:
```yaml
# NOTE: the following was REMOVED because it causes invalid cache re-uses.
#       We observed changed headers not being detected in cpp file rebuilds and
#       caches being re-used when they should not be.
```

**Finally understood**: The `-P` flag fundamentally breaks ccache's ability to detect header changes

---

## 🔍 Technical Analysis

### Why `-P` Breaks ccache

1. **Normal Preprocessing** (without `-P`):
   ```c
   #line 1 "source.cpp"
   #include "header.h"
   // ... code ...
   
   // Preprocessed output includes:
   #line 350 "/path/to/header.h"
   void emberAfAttributeChanged(..., AttributesChangedListener*);
   #line 1585 "/path/to/attribute-storage.cpp"
   void emberAfAttributeChanged(..., AttributesChangedListener*) { ... }
   ```

2. **With `-P` flag**:
   ```c
   // All #line directives removed:
   void emberAfAttributeChanged(..., AttributesChangedListener*);
   void emberAfAttributeChanged(..., AttributesChangedListener*) { ... }
   ```

3. **When header changes**:
   ```c
   // New signature in header.h:
   void emberAfAttributeChanged(..., ProviderChangeListener*);
   
   // WITHOUT -P: Preprocessed output changes → Different hash → Recompile ✅
   // WITH -P: Might have same hash if only signature changed → CACHE HIT ❌
   ```

### Why Cache Hits Happened Inappropriately

**ccache's content check process**:
1. Hash the command line args
2. Run preprocessor
3. Hash the preprocessed output
4. Look up hash in cache

**With `-P`**:
- Preprocessed output loses file/line information
- When only function signatures change, some hash collisions possible
- ccache thinks "same file" even though header changed
- Returns cached object file with OLD signature
- Caller compiled with NEW signature
- Linker error! 🔴

---

## 💡 Why It Wasn't Caught Earlier

### Original Version (Sept 9-10)
- **Reverted in < 24 hours**
- Likely caught by internal testing or first CI runs
- No detailed documentation of what broke

### Second Version (Oct 17-20)
- **Only 3 days active**
- Not all PRs change headers/signatures
- Intermittent failures easy to miss or attribute to other causes
- **The "changed_listener_combination" PR was the definitive test case**
  - Changed multiple function signatures
  - Triggered multiple linker errors
  - Made the problem obvious

---

## 📊 Lessons Learned

### 1. **Don't Remove `#line` Directives for Caching**

**Claimed benefit**: Better cache hit ratio (fewer path differences)

**Actual cost**: Correctness bugs that are hard to debug

**Conclusion**: Cache hit ratio is meaningless if builds are incorrect

### 2. **Reverting and Re-adding is Risky**

The fact that it was:
- Added Sept 9
- Reverted Sept 10 (< 24 hrs)
- Re-added Oct 17 (simplified)
- Removed Oct 20 (3 days later)

Shows: The simplified version had the SAME fundamental flaw

### 3. **Signature Changes are the Best Test**

PRs that change function signatures immediately expose caching bugs:
- Caller uses new signature → compiles
- Definition cached with old signature → linker error
- Clear, immediate failure

Other header changes might cause **silent bugs** (worse!)

### 4. **Trust the Defaults**

`CCACHE_COMPILERCHECK=content` works when:
- Preprocessor output includes full information (`#line` directives)
- Don't modify preprocessor behavior

Samsung's goal (better hit ratio) was misguided:
- They wanted to remove path differences
- But paths are ESSENTIAL for correctness
- Better to have lower hit ratio than wrong builds

---

## 🎯 Current State (After Oct 20, 2025)

### ✅ Fixed Configuration

```yaml
# .github/actions/setup-ccache/action.yml
env:
  CCACHE_NOHASHDIR: 1
  CCACHE_BASEDIR: ${{ github.workspace }}
  # CCACHE_PREFIX_CPP removed - no longer set
  CCACHE_SLOPPINESS: time_macros
  CCACHE_DIR: ${{ github.workspace }}/.ccache
  CCACHE_COMPILERCHECK: content
```

### Cache Strategy

**Current**: 
- Use content-based checking
- Don't modify preprocessor output
- Accept lower hit ratio for correctness

**Future possibilities** (if Samsung wants better hit ratio):
1. Use `CCACHE_BASEDIR` more effectively (already set)
2. Use `CCACHE_NOHASHDIR=1` (already set)
3. Ensure consistent build environments
4. **DO NOT** remove `#line` directives

---

## 🔬 The Investigation (Our Work)

### What We Did

1. **Reproduced locally**: Built master, then PR, measured cache behavior
2. **Verified changed files were compiled**: All critical files recompiled
3. **Analyzed CI error**: Linker error showed signature mismatch
4. **Found the difference**: Local test didn't use `-P`, CI did

### Key Findings

- Local test: ✅ Worked correctly (no `-P`)
- CI build: ❌ Failed (with `-P`)
- Proof: The `-P` flag was the cause

### Smoking Gun

**CI Error**:
```
undefined symbol: emberAfAttributeChanged(..., AttributesChangedListener*)
referenced by attribute-table.cpp
```

**Interpretation**:
- `attribute-table.cpp`: Compiled with NEW header → calls new signature  
- `attribute-storage.cpp`: CACHED from master → defines old signature
- `-P` flag prevented ccache from detecting the header change

---

## 📝 Recommendations

### For Future Cache Optimizations

1. **Never modify preprocessor output**
   - No `-P` flag
   - No stripping of directives
   - Trust the compiler

2. **Test with signature-changing PRs**
   - Before deploying cache changes
   - Try a PR that changes function signatures
   - Verify linker doesn't error

3. **Monitor for intermittent CI failures**
   - "undefined symbol" errors
   - Build failures that don't repro locally
   - May indicate caching bug

4. **Document why reverts happen**
   - First revert (Sept 10) had no details
   - Made it easier to re-introduce same bug
   - Clear documentation would have prevented re-addition

---

## 🏁 Conclusion

The ccache `CCACHE_PREFIX_CPP` saga demonstrates:

1. **Good intentions, bad implementation**: Wanting better cache hits is good, but correctness comes first
2. **Quick reverts don't always stick**: The Sept 10 revert should have been permanent
3. **Simplification doesn't fix fundamental flaws**: Simpler `-P` still breaks header change detection
4. **Testing matters**: Need test cases that change signatures/headers
5. **Trust was restored**: Removing `-P` fixed the issue

**Final status**: ✅ RESOLVED - ccache works correctly as of Oct 20, 2025
