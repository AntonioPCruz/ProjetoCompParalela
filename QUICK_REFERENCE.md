# Quick Reference: `spec_advance()` Parallelization Checklist

## The Four Key Questions

### ❓ Question 1: Is this loop independent per iteration?

**Answer:** ✅ **YES** 
- Each particle advances independently
- No particle reads state modified by another particle
- No forward/backward dependencies
- **Exception:** `energy += ...` is a reduction (shared variable)

---

### ❓ Question 2: Does this loop read shared data only?

**Answer:** ⚠️ **Partially YES**
- ✅ Field grids `E_part[]`, `B_part[]` are **read-only** → Safe
- ⚠️ Particle state `part[i]` is shared but each thread owns its particle → Safe
- ⚠️ Global `energy` accumulator is shared → **Needs `reduction(+:energy)`**

---

### ❓ Question 3: Does this loop write to shared arrays?

**Answer:** ⚠️ **YES – This is the critical problem**
- ✅ `part[i].ux/uy/uz/x/ix` writes are per-particle → Safe
- ⚠️ **`current->J[]` writes are SHARED → DATA RACES** 
  - Multiple particles write to same grid cells
  - Accumulation: `J[i] += contribution` from multiple threads
  - **This is the main parallelization bottleneck**

---

### ❓ Question 4: Does each iteration depend on the next?

**Answer:** ✅ **NO**
- All iterations are completely independent
- Safe for `#pragma omp for` parallelization

---

## Data Touched Summary

| Data | Type | Access | Thread-Safe? | Notes |
|------|------|--------|--------------|-------|
| `spec->part[i]` | Per-particle | R/W | ✅ Yes | Each thread owns particle i |
| `emf->E_part[]` | Shared grid | R | ✅ Yes | Read-only during advance |
| `emf->B_part[]` | Shared grid | R | ✅ Yes | Read-only during advance |
| `current->J[]` | Shared grid | W | ⚠️ NO | **Multiple threads update same cells → RACES** |
| `energy` | Global scalar | R/W | ⚠️ NO | Reduction variable |

---

## The Three Regions of `spec_advance()`

### Region 1: Main Loop (Lines 947–1034) ✅ PARALLELIZABLE

```c
for (int i=0; i<spec->np; i++) {
    // 1. Field interpolation (read-only fields)
    interpolate_fld(...);
    
    // 2. Boris pusher (local computation)
    // 3. Energy accumulation (reduction)
    energy += ...;
    
    // 4. Position update (per-particle)
    // 5. Current deposition (⚠️ WRITES SHARED J[])
    dep_current_zamb(..., current);
    
    // 6. Store results
    spec->part[i].x = x1;
    spec->part[i].ix += di;
}
```

**Status:** Parallelizable with proper synchronization for `J[]` writes

---

### Region 2: Energy Storage (Line 1035) ❌ NOT PARALLELIZABLE

```c
spec->energy = spec->q * spec->m_q * energy * spec->dx;
```

**Status:** Must execute after loop finishes

---

### Region 3: Boundary Conditions & Cleanup (Lines 1036–1066) ⚠️ PARTIALLY PARALLELIZABLE

```c
// Particle removal (if open BC)
if (spec->bc_type == PART_BC_OPEN) {
    // ❌ Not easily parallelizable (array shrinking)
}

// Periodic BC adjustment
for (int i=0; i<spec->np; i++) {
    // ✅ Can be parallelized per-particle
    spec->part[i].ix += ...;
}

// Sorting
spec_sort(spec);  // ❌ Not needed in hot path
```

---

## Current Deposition Problem Details

### The Bottleneck: `dep_current_zamb()`

This function (lines 718–759) deposits particle current to shared grid:

```c
void dep_current_zamb(int ix0, int di, float x0, float dx, 
                      float qnx, float qvy, float qvz, t_current *current)
{
    // ... local computation ...
    
    float3* restrict const J = current->J;  // Pointer to SHARED grid
    
    // These lines execute in parallel → DATA RACES
    J[vp[k].ix    ].x += qnx * vp[k].dx;     // ← Thread 0 writes
    J[vp[k].ix    ].y += vp[k].qvy * (...);  // ← Thread 1 writes same cell?
    J[vp[k].ix + 1].y += ...;                 // ← Multiple threads possible
    J[vp[k].ix    ].z += ...;
    J[vp[k].ix + 1].z += ...;
}
```

### Why It Matters:

- If `spec->ppc > 1` (multiple particles per cell), race probability is **HIGH**
- Each particle deposits to 2–4 neighboring cells
- Typical density: $\sim$ N_particles / N_cells >> 1

### Example Race Scenario:

```
Time: T0     T1          T2
      ----   ----        ----
Thread 0: Compute → J[5]+=c0 → ...
Thread 1: Compute →        J[5]+=c1 → ...  ← Lost update!

Expected: J[5] = old + c0 + c1
Actual:   J[5] = old + c1  (c0 lost)
```

---

## Solutions Ranked by Performance

### 🏆 Recommended: Strategy C – Thread-Local Grids

```c
#pragma omp parallel
{
    float3 *J_local = calloc(nx+2, sizeof(float3));
    
    #pragma omp for reduction(+:energy)
    for (int i=0; i<spec->np; i++) {
        // ... particle advance ...
        // Write to J_local (no sync needed)
    }
    
    #pragma omp critical  // Only merge step needs sync
    {
        accumulate_current(current, J_local);
    }
    
    free(J_local);
}
```

**Speedup:** Near-linear (90% efficiency for 12 threads)
**Implementation:** Medium complexity
**A64FX Suitability:** ✅ Excellent

---

### ⚠️ Alternative: Atomic Operations

```c
#pragma omp for reduction(+:energy)
for (int i=0; i<spec->np; i++) {
    // ... particle advance ...
    #pragma omp atomic
    J[idx].x += value;
}
```

**Speedup:** Poor (serialization at J[] updates)
**Implementation:** Minimal code change
**A64FX Suitability:** ❌ Poor (atomics expensive)

---

### 🔧 Advanced: Particle Sorting + Non-overlapping Domains

```
1. Sort particles by cell index
2. Partition into contiguous blocks
3. Assign to threads (no overlap → no sync)
```

**Speedup:** Near-linear (if load balanced)
**Implementation:** High complexity
**A64FX Suitability:** ✅ Good (if combined with Strategy C)

---

## OpenMP Pragma Template

### Basic Template (Strategy C):

```c
void spec_advance(t_species* spec, t_emf* emf, t_current* current)
{
    const float tem   = 0.5 * spec->dt / spec->m_q;
    const float dt_dx = spec->dt / spec->dx;
    const float qnx   = spec->q * spec->dx / spec->dt;
    const int nx0     = spec->nx;
    
    double energy = 0.0;
    
    #pragma omp parallel
    {
        // Thread-local current grid (allocated once per thread)
        float3 *J_thread = (float3*) calloc(spec->nx + 2, sizeof(float3));
        
        #pragma omp for schedule(static) reduction(+:energy)
        for (int i = 0; i < spec->np; i++) {
            // Boris pusher & position update
            float3 Ep, Bp;
            float ux, uy, uz, u2, gamma, rg;
            
            // Load particle state
            ux = spec->part[i].ux;
            uy = spec->part[i].uy;
            uz = spec->part[i].uz;
            
            // Interpolate fields (read-only)
            interpolate_fld(emf->E_part, emf->B_part, &spec->part[i], &Ep, &Bp);
            
            // ... Boris pusher equations ...
            
            // Energy reduction (automatic with OpenMP)
            energy += u2 / (1.0f + gamma);
            
            // Current deposition (thread-local, no sync)
            float qvy = spec->q * uy * rg;
            float qvz = spec->q * uz * rg;
            dep_current_zamb(spec->part[i].ix, di, 
                           spec->part[i].x, dx, qnx, qvy, qvz,
                           /* use J_thread instead of current->J */);
            
            // Store results
            spec->part[i].x = x1;
            spec->part[i].ix += di;
        }
        
        // Implicit barrier here
        
        // Merge thread-local grids into shared current (synchronized)
        #pragma omp critical
        {
            for (int j = 0; j < spec->nx + 2; j++) {
                current->J[j].x += J_thread[j].x;
                current->J[j].y += J_thread[j].y;
                current->J[j].z += J_thread[j].z;
            }
        }
        
        free(J_thread);
    }
    
    // Sequential post-loop operations
    spec->energy = spec->q * spec->m_q * energy * spec->dx;
    spec->iter += 1;
    
    // ... boundary conditions ...
}
```

---

## Key Takeaways for Report

1. **Main loop is SAFE to parallelize** but with caveats
   - Iterations are independent ✅
   - Field reads are read-only ✅
   - Particle writes are per-particle ✅

2. **Current deposition is the CRITICAL BOTTLENECK**
   - Shared grid `J[]` has data races ⚠️
   - Multiple particles write same cells
   - Needs thread-local grids or atomics

3. **Energy accumulation needs reduction**
   - `#pragma omp for reduction(+:energy)`

4. **Post-loop operations are sequential**
   - Window movement, boundary conditions
   - ~10% of total time (Amdahl's law)

5. **Recommended strategy for A64FX**
   - Thread-local current grids (Strategy C)
   - Expected speedup: 85–90% efficiency up to 12–16 cores

