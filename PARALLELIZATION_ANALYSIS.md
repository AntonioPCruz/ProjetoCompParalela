# Parallelization Analysis: `spec_advance()` Function

## Executive Summary

The `spec_advance()` function in `particles.c` is the computational hotspot and has **two distinct regions with different parallelization profiles**:

1. **Main particle advance loop** (lines 947–1034): ✅ **PARALLELIZABLE** with caveats
2. **Current deposition kernel** (inside loop): ⚠️ **DATA RACE RISK** → needs special handling
3. **Post-loop operations**: ⚠️ **NON-PARALLELIZABLE** (sequential dependencies)

---

## Detailed Analysis

### 📍 Location of Main Loop

**Function:** `spec_advance()` in `particles.c` (lines 910–1066)

**Main computational loop (lines 947–1034):**
```c
for (int i=0; i<spec->np; i++) {
    // ... particle update code ...
    spec->part[i].ux = ux;
    spec->part[i].uy = uy;
    spec->part[i].uz = uz;
    // ... position update ...
    spec->part[i].x = x1;
    spec->part[i].ix += di;
}
```

---

## Question 1: Is this loop independent per iteration?

### ✅ Answer: **YES, mostly – with ONE critical exception**

#### Independent Aspects (Per-Particle Operations):

| Operation | Per-Particle? | Independence |
|-----------|---------------|--------------|
| Load particle momenta `(ux, uy, uz)` | ✅ Yes | Each particle reads only its own state |
| Field interpolation | ✅ Yes | Each particle reads from **read-only** field grids `E_part[]`, `B_part[]` |
| Boris pusher (velocity update) | ✅ Yes | Pure computation on local variables |
| Update particle momenta | ✅ Yes | Each particle writes only to `part[i].(ux,uy,uz)` |
| Position update | ✅ Yes | Each particle updates only `part[i].(x, ix)` |
| Energy accumulation | ⚠️ **NO** | Shared variable `energy` (see below) |

#### **Critical Dependency: Energy Accumulation**

```c
energy += u2 / ( 1 + gamma );  // Line 977
```

- **Type:** Reduction operation
- **Issue:** Multiple threads would update the same `energy` variable → **data race**
- **Solution:** OpenMP `#pragma omp for reduction(+:energy)`

---

## Question 2: Does this loop read shared data only?

### ⚠️ Answer: **Partially – Read-only field data YES, but shared particle state exists**

#### Read-Only Field Data (SAFE to parallelize):

```c
interpolate_fld( emf -> E_part, emf -> B_part, &spec -> part[i], &Ep, &Bp );
```

- **`emf->E_part[]`** (Electric field grid): ✅ Read-only during particle advance
- **`emf->B_part[]`** (Magnetic field grid): ✅ Read-only during particle advance
- Each particle performs **linear interpolation** at its own position
- **No cross-particle dependencies** on field reads

#### Shared Particle Data (Requires care):

```c
ux = spec -> part[i].ux;  // Read from own particle
...
spec -> part[i].ux = ux;  // Write to own particle
```

- ✅ **Safe:** Each iteration `i` reads and writes only to `spec->part[i]`
- ✅ **No false sharing concerns** in 1D PIC with proper loop distribution

---

## Question 3: Does this loop write to shared arrays?

### ⚠️ Answer: **YES – Shared array `J[]` (current grid) has DATA RACES**

This is the **main parallelization challenge**.

#### Writes Inside the Loop:

| Target | Scope | Issue |
|--------|-------|-------|
| `spec->part[i].ux/uy/uz` | Per-particle | ✅ Safe (each thread owns its particle) |
| `spec->part[i].x` | Per-particle | ✅ Safe (each thread owns its particle) |
| `spec->part[i].ix` | Per-particle | ✅ Safe (each thread owns its particle) |
| **`current->J[]`** | **Shared grid** | ⚠️ **DATA RACE** |

#### The Current Deposition Problem:

Inside the main loop, `dep_current_zamb()` is called (lines 1023–1026):

```c
dep_current_zamb( spec -> part[i].ix, di,
                 spec -> part[i].x, dx,
                 qnx, qvy, qvz,
                 current );
```

This function writes to the **shared current grid** `current->J[]`:

```c
// From dep_current_zamb() - lines 752-758
J[ vp[k].ix     ].x += qnx * vp[k].dx;
J[ vp[k].ix     ].y += vp[k].qvy * (S0x[0]+S1x[0]+...);
J[ vp[k].ix + 1 ].y += vp[k].qvy * (S0x[1]+S1x[1]+...);
J[ vp[k].ix     ].z += vp[k].qvz * (S0x[0]+S1x[0]+...);
J[ vp[k].ix  +1 ].z += vp[k].qvz * (S0x[1]+S1x[1]+...);
```

#### Why Data Races Occur:

- **Multiple particles can deposit to the same grid cell**
- Example: Particle 0 at cell 5 and Particle 1 at cell 5 both write `J[5]` simultaneously
- **Result:** Lost updates, non-deterministic behavior

#### Probability of Race:

- **High** in typical simulations where:
  - Number of particles >> Number of cells (multiple particles per cell)
  - PPC (particles per cell) > 1
  - Multiple threads processing overlapping cell indices

---

## Question 4: Does each iteration depend on the next?

### ✅ Answer: **NO – All iterations are independent**

**Verification:**

```
Iteration i:   Reads only part[i], computes locally, writes part[i] and J[]
Iteration j:   Reads only part[j], computes locally, writes part[j] and J[]
               (where i ≠ j)
```

**No forward/backward dependencies:**
- No particle reads state modified by another particle
- No particle reads its own previous state
- ✅ **Safe for OpenMP parallel for-loop** (with proper synchronization for shared `J[]`)

---

## Data Dependency Summary Table

```
┌─────────────────────────┬──────────────┬─────────────┬──────────────────┐
│ Data Accessed           │ Per-Particle │ Read/Write  │ Thread-Safe?     │
├─────────────────────────┼──────────────┼─────────────┼──────────────────┤
│ part[i].(ux,uy,uz)      │ Yes          │ Read        │ ✅ YES           │
│ part[i].(x,ix)          │ Yes          │ Read/Write  │ ✅ YES           │
│ emf->E_part[]           │ No (shared)  │ Read        │ ✅ YES (RO)      │
│ emf->B_part[]           │ No (shared)  │ Read        │ ✅ YES (RO)      │
│ current->J[]            │ No (shared)  │ Write       │ ⚠️ DATA RACE     │
│ energy (global)         │ No (shared)  │ Read/Write  │ ⚠️ DATA RACE     │
│ spec->iter              │ No (global)  │ Read        │ ✅ YES (RO)      │
│ spec->np                │ No (global)  │ Read        │ ✅ YES (RO)      │
└─────────────────────────┴──────────────┴─────────────┴──────────────────┘
```

---

## Parallelization Strategies

### Strategy A: Naive Parallelization (❌ INCORRECT – Data Races)

```c
#pragma omp parallel for
for (int i=0; i<spec->np; i++) {
    // ... loop body ...
    dep_current_zamb(..., current);  // ← RACE CONDITION!
}
```

**Problems:**
- Multiple threads write `current->J[]` simultaneously
- Results are non-deterministic and incorrect

---

### Strategy B: Atomic Operations (⚠️ SAFE but SLOW)

```c
#pragma omp parallel for reduction(+:energy)
for (int i=0; i<spec->np; i++) {
    // ... per-particle computation ...
    
    // Use atomic operations for current deposition
    // This requires modifying dep_current_zamb() to use #pragma omp atomic
    #pragma omp atomic
    J[idx].x += value;
}
```

**Pros:**
- Correctness guaranteed
- Minimal code changes

**Cons:**
- ⚠️ **Performance degradation:** Atomics are expensive, especially on ARM
- Each `J[idx]` write requires synchronization → **bottleneck**
- Not recommended for A64FX with high core count

---

### Strategy C: Thread-Local Current Grid (✅ RECOMMENDED)

```c
#pragma omp parallel
{
    // Each thread allocates its own current grid
    t_current *thread_J = create_thread_local_J(current->nx);
    
    #pragma omp for reduction(+:energy)
    for (int i=0; i<spec->np; i++) {
        // ... per-particle computation ...
        dep_current_zamb(..., thread_J);  // Write to LOCAL grid
    }
    
    // Barrier implicit here, then reduce
    #pragma omp critical
    {
        merge_current_grids(current, thread_J);  // Accumulate global J
    }
}
```

**Pros:**
- ✅ **No synchronization during loop** → maximal performance
- ✅ **Cache-efficient:** Each thread works on its own grid
- ✅ **Scalable:** Performance improves with core count
- ✅ **Recommended for A64FX (ARM)**

**Cons:**
- Memory overhead: O(n_threads × grid_size)
- Requires merge step after loop (typically fast)

---

### Strategy D: Particle Sorting + Task Parallelism (Advanced)

```
1. Sort particles by cell index (already done every n_sort iterations)
2. Partition particles into non-overlapping groups per thread
3. Each thread processes its group without synchronization
```

**Pros:**
- ✅ Zero synchronization needed
- ✅ Cache-optimal data layout

**Cons:**
- Requires particle reordering (already in code as `spec_sort()`)
- More complex to implement
- May not be necessary if Strategy C is used

---

## Post-Loop Operations (NOT Parallelizable)

### Lines 1035–1066: Sequential Dependencies

```c
// Store energy
spec -> energy = spec-> q * spec -> m_q * energy * spec -> dx;

// Advance internal iteration number
spec -> iter += 1;  // ← Depends on all loop iterations done

// Check for particles leaving the box
if ( spec -> moving_window || spec -> bc_type == PART_BC_OPEN ){
    // ... window movement, boundary handling ...
    // ← Depends on all particle positions updated
}

// Periodic boundary conditions
for (int i=0; i<spec->np; i++) {
    spec -> part[i].ix += ...;  // ← Sequential loop, can be parallelized
}

// Sort species
if ( spec -> n_sort > 0 ) {
    if ( ! (spec -> iter % spec -> n_sort) ) spec_sort( spec );
}
```

**Analysis:**

| Operation | Parallelizable? | Reason |
|-----------|-----------------|--------|
| Energy storage | ❌ No | Sequential after loop |
| Iteration increment | ❌ No | Must happen after loop |
| Window movement | ❌ No | Depends on all particles updated |
| Absorbing BC | ✅ Partially | Particle removal requires synchronization |
| Periodic BC loop | ✅ Yes | Independent per particle |
| Sorting | ❌ Not in hot path | Only every `n_sort` iterations |

---

## Recommended Implementation

### For ZPIC on A64FX (ARM):

```c
void spec_advance( t_species* spec, t_emf* emf, t_current* current )
{
    uint64_t t0 = timer_ticks();
    
    const float tem   = 0.5 * spec->dt / spec->m_q;
    const float dt_dx = spec->dt / spec->dx;
    const float qnx   = spec->q * spec->dx / spec->dt;
    const int nx0     = spec->nx;
    
    double energy = 0.0;
    
    // STRATEGY C: Thread-local current deposition
    #pragma omp parallel
    {
        // Allocate thread-local current grid
        float3 *J_local = (float3*) calloc(spec->nx + 2, sizeof(float3));
        
        #pragma omp for reduction(+:energy) schedule(static)
        for (int i = 0; i < spec->np; i++) {
            // ... Boris pusher code ...
            
            // Deposit to thread-local grid (no synchronization needed)
            dep_current_zamb(spec->part[i].ix, di,
                           spec->part[i].x, dx,
                           qnx, qvy, qvz,
                           /* temporary struct */ );
        }
        
        // Reduction phase: merge thread-local grids
        #pragma omp critical
        {
            for (int j = 0; j < spec->nx + 2; j++) {
                current->J[j].x += J_local[j].x;
                current->J[j].y += J_local[j].y;
                current->J[j].z += J_local[j].z;
            }
        }
        
        free(J_local);
    }
    
    // Sequential post-loop operations
    spec->energy = spec->q * spec->m_q * energy * spec->dx;
    spec->iter += 1;
    
    // ... boundary conditions and sorting ...
}
```

---

## Performance Expectations

### Speedup Estimate (Amdahl's Law):

Assuming:
- 90% of time in parallelizable particle loop
- 10% in non-parallelizable post-operations
- $N$ threads on A64FX

$$\text{Speedup} = \frac{1}{0.1 + \frac{0.9}{N}}$$

| Threads | Estimated Speedup | Efficiency |
|---------|-------------------|-----------|
| 1       | 1.0x              | 100%      |
| 4       | 3.6x              | 90%       |
| 8       | 7.1x              | 89%       |
| 12      | 9.9x              | 82%       |
| 24      | 15.8x             | 66%       |
| 48      | 20.8x             | 43%       |

**Note:** Actual speedup depends on:
- Memory bandwidth availability on A64FX
- Cache line contention in thread-local grid merge
- Load balance (uniform particle distribution assumed)

---

## Conclusions

### Summary of Findings:

✅ **Loop iterations are INDEPENDENT** → Parallelizable

✅ **Read-only field data** → No synchronization needed for field reads

⚠️ **Shared grid writes** → Current deposition is the critical bottleneck

⚠️ **Shared reduction variable** → Energy accumulation needs `reduction(+:energy)`

❌ **Post-loop operations are SEQUENTIAL** → Cannot be parallelized

### Recommended Action:

1. **Implement Strategy C (Thread-local grids)** for maximum performance on A64FX
2. **Profile with Score-P/Cube** to verify bottleneck elimination
3. **Monitor memory bandwidth** → thread-local grid merge may become bottleneck at high core count
4. **Consider particle sorting + static scheduling** for better cache locality

### Next Steps:

- [ ] Implement OpenMP parallelization with thread-local current grid
- [ ] Benchmark speedup vs. core count on Deucalion ARM partition
- [ ] Profile with Score-P to identify remaining hotspots
- [ ] Compare against Strategies A, B for reference
- [ ] Validate correctness with regression tests (physics output comparison)

