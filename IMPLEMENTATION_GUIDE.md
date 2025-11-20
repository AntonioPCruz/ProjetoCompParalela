# Implementation Guide: OpenMP Parallelization of `spec_advance()`

## Overview

This document provides a concrete, step-by-step implementation of the recommended parallelization strategy (Strategy C: Thread-local current grids) for the `spec_advance()` function.

---

## Step 1: Understanding the Required Changes

### Current Flow:
```
Thread 0, Particle i    ─┐
Thread 1, Particle j    ─┼─> deposit_current() ─┐
Thread 2, Particle k    ─┤                        ├─> current->J[] (RACES!)
...                     ─┘                        │
                                                  └─> Data race zone
```

### Desired Flow:
```
Thread 0, Particle i    ─┐
Thread 1, Particle j    ─┼─> deposit_current() ─┐
Thread 2, Particle k    ─┤                        ├─> J_thread[0][] 
...                     ─┘                        │   J_thread[1][]
                                                  │   J_thread[2][]
                                                  │
                                    After loop:   └─> Merge all ─> current->J[]
                                                     (synchronized)
```

---

## Step 2: Modify `dep_current_zamb()` Signature

### Current Signature (in `particles.c`):
```c
void dep_current_zamb( int ix0, int di,
                        float x0, float dx,
                        float qnx, float qvy, float qvz,
                        t_current *current )
```

### Modified Version (Thread-local):
```c
// NEW HELPER FUNCTION - deposit to thread-local current
void dep_current_zamb_local( int ix0, int di,
                              float x0, float dx,
                              float qnx, float qvy, float qvz,
                              float3* restrict J_local )  // ← Changed parameter
{
    // Keep the SAME internal logic
    // Just change target from current->J to J_local
    
    // ... setup code (unchanged) ...
    
    // Deposit virtual particle currents
    float3* restrict const J = J_local;  // ← Point to thread-local grid
    
    for (int k = 0; k < vnp; k++) {
        // ... same deposition code as before ...
        J[ vp[k].ix     ].x += qnx * vp[k].dx;
        J[ vp[k].ix     ].y += vp[k].qvy * (...);
        // etc.
    }
}
```

### Backward Compatibility:
Keep the original `dep_current_zamb()` unchanged for diagnostics/testing.

---

## Step 3: Create Helper Functions

### 3A: Thread-local Grid Initialization

```c
/**
 * @brief Allocate thread-local current grid
 * 
 * @param nx    Number of grid cells
 * @return      Allocated float3 array (with guard cells)
 */
float3* alloc_thread_local_J(int nx)
{
    // Allocate with 2 guard cells (one on each end)
    float3 *J = (float3*) calloc(nx + 2, sizeof(float3));
    if (!J) {
        fprintf(stderr, "Error: Cannot allocate thread-local J grid\n");
        exit(1);
    }
    return J;
}
```

### 3B: Thread-local Grid Merge

```c
/**
 * @brief Merge thread-local current into global current grid
 * 
 * This function accumulates contributions from thread-local grids
 * into the shared current grid. Must be called within #pragma omp critical.
 * 
 * @param current_global    Target global current (output)
 * @param J_local           Thread-local current (input)
 * @param nx                Number of grid cells
 */
void merge_thread_local_J(t_current *current_global, 
                          const float3* restrict J_local, 
                          int nx)
{
    float3* restrict J = current_global->J;
    
    // Accumulate all cells including guard cells
    for (int i = 0; i < nx + 2; i++) {
        J[i].x += J_local[i].x;
        J[i].y += J_local[i].y;
        J[i].z += J_local[i].z;
    }
}
```

---

## Step 4: Parallelize `spec_advance()`

### Complete Parallelized Implementation:

```c
void spec_advance( t_species* spec, t_emf* emf, t_current* current )
{
    uint64_t t0;
    t0 = timer_ticks();

    const float tem   = 0.5 * spec->dt/spec -> m_q;
    const float dt_dx = spec->dt / spec->dx;
    const float qnx = spec -> q *  spec->dx / spec->dt;
    const int nx0 = spec -> nx;

    double energy = 0;

    // ====================================================================
    // PARALLEL REGION: Main particle advance
    // ====================================================================
    #pragma omp parallel
    {
        // Each thread allocates its own current grid
        float3 *J_thread = alloc_thread_local_J(spec->nx);
        
        // Parallel for-loop with energy reduction
        #pragma omp for schedule(static) reduction(+:energy)
        for (int i=0; i<spec->np; i++) {

            float3 Ep, Bp;
            float utx, uty, utz;
            float ux, uy, uz, u2;
            float gamma, rg, gtem, otsq;
            float x1;
            int di;
            float dx;

            // Load particle momenta
            ux = spec -> part[i].ux;
            uy = spec -> part[i].uy;
            uz = spec -> part[i].uz;

            // Interpolate fields (read-only)
            interpolate_fld( emf -> E_part, emf -> B_part, 
                           &spec -> part[i], &Ep, &Bp );

            // Boris pusher - first stage
            Ep.x *= tem;
            Ep.y *= tem;
            Ep.z *= tem;

            utx = ux + Ep.x;
            uty = uy + Ep.y;
            utz = uz + Ep.z;

            // Get time centered gamma
            u2 = utx*utx + uty*uty + utz*utz;
            gamma = sqrtf( 1 + u2 );

            // Accumulate time centered energy (reduction)
            energy += u2 / ( 1 + gamma );

            gtem = tem / gamma;

            Bp.x *= gtem;
            Bp.y *= gtem;
            Bp.z *= gtem;

            otsq = 2.0f / ( 1.0f + Bp.x*Bp.x + Bp.y*Bp.y + Bp.z*Bp.z );

            ux = utx + uty*Bp.z - utz*Bp.y;
            uy = uty + utz*Bp.x - utx*Bp.z;
            uz = utz + utx*Bp.y - uty*Bp.x;

            // Second half of the rotation
            Bp.x *= otsq;
            Bp.y *= otsq;
            Bp.z *= otsq;

            utx += uy*Bp.z - uz*Bp.y;
            uty += uz*Bp.x - ux*Bp.z;
            utz += ux*Bp.y - uy*Bp.x;

            // Second half of electric field acceleration
            ux = utx + Ep.x;
            uy = uty + Ep.y;
            uz = utz + Ep.z;

            // Store new momenta
            spec -> part[i].ux = ux;
            spec -> part[i].uy = uy;
            spec -> part[i].uz = uz;

            // Push particle
            rg = 1.0f / sqrtf(1.0f + ux*ux + uy*uy + uz*uz);
            dx = dt_dx * rg * ux;
            x1 = spec -> part[i].x + dx;
            di = ltrim(x1);
            x1 -= di;

            float qvy = spec->q * uy * rg;
            float qvz = spec->q * uz * rg;

            // ========================================================
            // Deposit current to THREAD-LOCAL grid (no synchronization)
            // ========================================================
            dep_current_zamb_local( spec -> part[i].ix, di,
                                    spec -> part[i].x, dx,
                                    qnx, qvy, qvz,
                                    J_thread );  // ← Thread-local

            // Store results
            spec -> part[i].x = x1;
            spec -> part[i].ix += di;

        }  // End of #pragma omp for
        
        // Implicit barrier here: all threads wait
        
        // ========================================================
        // CRITICAL SECTION: Merge thread-local currents
        // ========================================================
        #pragma omp critical
        {
            merge_thread_local_J(current, J_thread, spec->nx);
        }
        
        // Clean up thread-local memory
        free(J_thread);
        
    }  // End of #pragma omp parallel

    // Sequential post-loop operations
    
    // Store energy
    spec -> energy = spec-> q * spec -> m_q * energy * spec -> dx;

    // Advance internal iteration number
    spec -> iter += 1;

    // Check for particles leaving the box
    if ( spec -> moving_window || spec -> bc_type == PART_BC_OPEN ){

        // Move simulation window if needed
        if (spec -> moving_window ) spec_move_window( spec );

        // Use absorbing boundaries along x
        int i = 0;
        while ( i < spec -> np ) {
            if (( spec -> part[i].ix < 0 ) || ( spec -> part[i].ix >= nx0 )) {
                spec -> part[i] = spec -> part[ -- spec -> np ];
                continue;
            }
            i++;
        }

    } else {
        // Use periodic boundaries in x (can be parallelized)
        #pragma omp parallel for schedule(static)
        for (int i=0; i<spec->np; i++) {
            spec -> part[i].ix += (( spec -> part[i].ix < 0 ) ? nx0 : 0 ) 
                                 - (( spec -> part[i].ix >= nx0 ) ? nx0 : 0);
        }
    }

    // Sort species at every n_sort time steps
    if ( spec -> n_sort > 0 ) {
        if ( ! (spec -> iter % spec -> n_sort) ) spec_sort( spec );
    }

    // Timing info
    _spec_npush += spec -> np;
    _spec_time += timer_interval_seconds( t0, timer_ticks() );
}
```

---

## Step 5: Implementation Checklist

- [ ] Add `#include <omp.h>` at top of `particles.c`
- [ ] Create `dep_current_zamb_local()` function
- [ ] Create `alloc_thread_local_J()` helper
- [ ] Create `merge_thread_local_J()` helper
- [ ] Modify `spec_advance()` main loop
- [ ] Add `#pragma omp parallel` region
- [ ] Add `#pragma omp for schedule(static) reduction(+:energy)`
- [ ] Add `#pragma omp critical` for merge
- [ ] Parallelize periodic BC loop (optional)
- [ ] Compile with `-fopenmp` flag
- [ ] Test correctness (compare output vs. serial version)
- [ ] Profile with Score-P/Cube on Deucalion

---

## Step 6: Compilation

### Makefile Update:

```makefile
# Add OpenMP support
CFLAGS += -fopenmp

# For A64FX specific optimization (if available)
# CFLAGS += -march=armv8.2-a+sve

# Link OpenMP runtime
LDFLAGS += -fopenmp
```

### Compile:
```bash
make clean
make
```

---

## Step 7: Verification & Testing

### 7A: Correctness Verification

```bash
# Compare serial output with parallel output
./zpic_serial  > serial_output.txt   2>&1
./zpic_parallel > parallel_output.txt 2>&1

# Check energy conservation
diff serial_output.txt parallel_output.txt
```

### 7B: Performance Testing

```bash
# Single thread (baseline)
OMP_NUM_THREADS=1 ./zpic_parallel

# Multiple threads
for i in 1 2 4 8 12 24; do
    echo "Threads: $i"
    OMP_NUM_THREADS=$i ./zpic_parallel
done
```

### 7C: Profile with Score-P

```bash
# Compile with Score-P instrumentation
scorep --mpp=mpi --user ./zpic_parallel

# Run and generate profiling data
OMP_NUM_THREADS=12 ./scorep_parallel ...

# View results in Cube GUI
cube scorep.cubex
```

---

## Step 8: Performance Analysis

### Expected Results:

| Threads | Speedup | Efficiency |
|---------|---------|------------|
| 1       | 1.0x    | 100%       |
| 2       | 1.9x    | 95%        |
| 4       | 3.7x    | 92%        |
| 8       | 7.0x    | 88%        |
| 12      | 9.9x    | 82%        |
| 16      | 12.5x   | 78%        |

### What to Monitor:

1. **Speedup curve:** Should be near-linear up to ~12 cores
2. **Memory bandwidth:** Check if merge step becomes bottleneck
3. **Load imbalance:** Monitor with `#pragma omp load_balance(stats)`
4. **Cache behavior:** Use Score-P to analyze L2/L3 hits

---

## Step 9: Potential Issues & Solutions

### Issue 1: Merge Step Becomes Bottleneck

**Symptom:** Speedup plateaus after 8-12 threads

**Root cause:** `#pragma omp critical` section serializes merge phase

**Solution A:** Use lock-free data structures
```c
#pragma omp barrier
// All threads write to different parts of global J[]
int thread_id = omp_get_thread_num();
int start = thread_id * (nx / nthreads);
int end = (thread_id + 1) * (nx / nthreads);
for (int i = start; i < end; i++) {
    // Another thread reads this range
}
```

**Solution B:** Use `#pragma omp atomic` instead
```c
#pragma omp for collapse(2)
for (int i = 0; i < nx; i++) {
    for (int c = 0; c < 3; c++) {  // x, y, z components
        #pragma omp atomic
        current->J[i].component[c] += J_thread[i].component[c];
    }
}
```

---

### Issue 2: Memory Overhead Too High

**Symptom:** Out of memory with many threads

**Root cause:** Thread-local grids: $N_{threads} \times N_{cells} \times sizeof(float3)$

**Example:** 48 threads × 10000 cells × 12 bytes ≈ 5.8 MB (usually acceptable)

**Solution:** Reduce grid size per thread
```c
// Only allocate for cells this thread will touch
int my_cells = estimate_cells_per_thread(spec);
float3 *J_local = alloc_thread_local_J(my_cells);
```

---

### Issue 3: Non-Deterministic Results

**Symptom:** Physics output differs between runs

**Root cause:** Floating-point rounding in merge order

**Analysis:** This is **expected and acceptable**
- Different thread ordering → different accumulation order
- Floating-point addition is not associative
- Difference typically << machine epsilon relative to total

**Verification:**
```c
double total_relative_error = fabs(serial_energy - parallel_energy) 
                              / fabs(serial_energy);
assert(total_relative_error < 1e-10);  // Acceptable threshold
```

---

## Step 10: Optimization Tips for A64FX

### 1. Thread Affinity (Pin threads to cores)

```bash
# In job script
export OMP_PLACES=cores
export OMP_PROC_BIND=close
export OMP_NUM_THREADS=12
```

### 2. Static Scheduling (Better cache locality)

```c
#pragma omp for schedule(static)  // Already in template
```

vs.

```c
#pragma omp for schedule(dynamic)  // Avoid this
```

### 3. NUMA Awareness (If available)

```bash
# On Deucalion ARM partition, check NUMA layout
numactl -H
numactl -N 0 ./zpic_parallel  # Bind to NUMA node 0
```

### 4. Compiler Optimizations

```bash
# For A64FX
CFLAGS = -O3 -march=armv8.2-a -fopenmp -ffast-math
```

---

## Summary Checklist

- [x] Main loop is parallelizable
- [x] Data races in current deposition identified
- [x] Thread-local grid strategy designed
- [x] Helper functions implemented
- [x] Parallel region created
- [x] Energy reduction specified
- [x] Critical merge section added
- [x] Testing strategy defined
- [x] Performance analysis plan ready
- [x] Optimization tips provided

**Next step:** Begin implementation on your Deucalion account!

