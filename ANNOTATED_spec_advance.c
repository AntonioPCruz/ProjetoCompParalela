/*
 * ANNOTATED CODE: spec_advance() with Parallelization Analysis
 * 
 * This file shows the spec_advance() function with inline annotations
 * highlighting parallelization concerns and opportunities.
 * 
 * Legend:
 * [P] = Parallelizable
 * [S] = Sequential / Non-parallelizable
 * [⚠️] = Needs special handling
 * [R] = Reduction operation
 * [X] = Data race risk
 */

void spec_advance( t_species* spec, t_emf* emf, t_current* current )
{
    // [S] Timing - must be sequential
    uint64_t t0;
    t0 = timer_ticks();

    // [P] Constants - read-only
    const float tem   = 0.5 * spec->dt/spec -> m_q;
    const float dt_dx = spec->dt / spec->dx;

    // [P] Auxiliary values - read-only
    const float qnx = spec -> q *  spec->dx / spec->dt;
    const int nx0 = spec -> nx;

    // [R] Energy accumulation - reduction variable
    //     PROBLEM: Multiple threads will write this
    //     SOLUTION: Use #pragma omp for reduction(+:energy)
    double energy = 0;

    // ========================================================================
    // MAIN PARTICLE ADVANCE LOOP
    // ========================================================================
    // Status: [P] PARALLELIZABLE but with synchronization needed for J[]
    // 
    // Each iteration processes ONE particle independently.
    // The loop can be parallelized with:
    //   #pragma omp for schedule(static) reduction(+:energy)
    //
    // However, current deposition inside has shared writes.
    // See annotations below.
    // ========================================================================

    for (int i=0; i<spec->np; i++) {  // [P] Loop variable - parallelizable iteration

        float3 Ep, Bp;                  // [P] Per-thread local variables
        float utx, uty, utz;            // [P] Per-thread local variables
        float ux, uy, uz, u2;           // [P] Per-thread local variables
        float gamma, rg, gtem, otsq;    // [P] Per-thread local variables

        float x1;                       // [P] Per-thread local variable

        int di;                         // [P] Per-thread local variable
        float dx;                       // [P] Per-thread local variable

        // Load particle momenta
        // [P] SAFE: Each thread reads only spec->part[i]
        ux = spec -> part[i].ux;
        uy = spec -> part[i].uy;
        uz = spec -> part[i].uz;

        // Interpolate fields at particle position
        // [P] SAFE: emf->E_part[] and emf->B_part[] are READ-ONLY
        //          Linear interpolation has no cross-thread dependencies
        //          Results stored in thread-local Ep, Bp
        interpolate_fld( emf -> E_part, emf -> B_part, &spec -> part[i], &Ep, &Bp );

        // Boris pusher - first stage
        // [P] SAFE: Pure local computation on temporary variables
        Ep.x *= tem;
        Ep.y *= tem;
        Ep.z *= tem;

        utx = ux + Ep.x;
        uty = uy + Ep.y;
        utz = uz + Ep.z;

        // Calculate gamma factor for relativistic effects
        // [P] SAFE: Local computation
        u2 = utx*utx + uty*uty + utz*utz;
        gamma = sqrtf( 1 + u2 );

        // Energy accumulation
        // [R] SHARED VARIABLE: energy
        //     PROBLEM: Multiple threads write this simultaneously
        //     SOLUTION: #pragma omp for reduction(+:energy)
        energy += u2 / ( 1 + gamma );

        // Boris pusher - rotation
        // [P] SAFE: Local computation
        gtem = tem / gamma;

        Bp.x *= gtem;
        Bp.y *= gtem;
        Bp.z *= gtem;

        otsq = 2.0f / ( 1.0f + Bp.x*Bp.x + Bp.y*Bp.y + Bp.z*Bp.z );

        ux = utx + uty*Bp.z - utz*Bp.y;
        uy = uty + utz*Bp.x - utx*Bp.z;
        uz = utz + utx*Bp.y - uty*Bp.x;

        // Second rotation
        // [P] SAFE: Local computation
        Bp.x *= otsq;
        Bp.y *= otsq;
        Bp.z *= otsq;

        utx += uy*Bp.z - uz*Bp.y;
        uty += uz*Bp.x - ux*Bp.z;
        utz += ux*Bp.y - uy*Bp.x;

        // Final electric field acceleration
        // [P] SAFE: Local computation
        ux = utx + Ep.x;
        uy = uty + Ep.y;
        uz = utz + Ep.z;

        // Store new momenta to particle
        // [P] SAFE: Each thread writes only spec->part[i]
        //           (different particles have different indices i)
        spec -> part[i].ux = ux;
        spec -> part[i].uy = uy;
        spec -> part[i].uz = uz;

        // Calculate displacement
        // [P] SAFE: Local computation
        rg = 1.0f / sqrtf(1.0f + ux*ux + uy*uy + uz*uz);
        dx = dt_dx * rg * ux;

        // Calculate new position
        // [P] SAFE: Local computation
        x1 = spec -> part[i].x + dx;

        // Check if particle crossed cell boundary
        // [P] SAFE: Local computation
        di = ltrim(x1);
        x1 -= di;

        // Calculate charge-weighted velocities for current deposition
        // [P] SAFE: Local computation
        float qvy = spec->q * uy * rg;
        float qvz = spec->q * uz * rg;

        // ====================================================================
        // CURRENT DEPOSITION - THE CRITICAL BOTTLENECK
        // ====================================================================
        // Function: dep_current_zamb()
        // 
        // Status: [X] DATA RACE - Multiple threads write shared grid J[]
        //
        // Problem Scenario:
        //   Thread 0: Deposits particle 0 to cell 5: J[5].x += 0.5
        //   Thread 1: Deposits particle 1 to cell 5: J[5].x += 0.3
        //   
        //   Sequential result: J[5].x += 0.8
        //   Parallel result:   J[5].x += 0.3 or 0.5 (lost update!)
        //
        // Why it occurs:
        //   - Multiple particles can be in same cell (ppc > 1)
        //   - Current weighted to 2-4 neighboring grid cells
        //   - No explicit synchronization between threads
        //
        // Current deposition writes to these locations:
        //   current->J[vp[k].ix    ].x += qnx * vp[k].dx;
        //   current->J[vp[k].ix    ].y += vp[k].qvy * (...);
        //   current->J[vp[k].ix + 1].y += vp[k].qvy * (...);
        //   current->J[vp[k].ix    ].z += vp[k].qvz * (...);
        //   current->J[vp[k].ix + 1].z += vp[k].qvz * (...);
        //
        // Solutions:
        //   1. [⚠️] Atomic operations: #pragma omp atomic (SLOW)
        //   2. [✅] Thread-local grids: Each thread gets J_local[] (FAST)
        //   3. [✅] Particle sorting: Partition by cell (COMPLEX)
        // ====================================================================

        dep_current_zamb( spec -> part[i].ix, di,
                         spec -> part[i].x, dx,
                         qnx, qvy, qvz,
                         current );  // ← current->J[] written here with races

        // Store updated position
        // [P] SAFE: Each thread writes only spec->part[i]
        spec -> part[i].x = x1;
        spec -> part[i].ix += di;

    }
    // End of main loop

    // ========================================================================
    // POST-LOOP OPERATIONS
    // ========================================================================

    // Store energy
    // [S] SEQUENTIAL: Must execute after loop completes
    //     Cannot be moved into loop (needs final energy value)
    spec -> energy = spec-> q * spec -> m_q * energy * spec -> dx;

    // Advance internal iteration number
    // [S] SEQUENTIAL: Must execute after loop (iteration counter)
    spec -> iter += 1;

    // Check for particles leaving the box
    // [S/⚠️] MOSTLY SEQUENTIAL with some parallelizable parts
    if ( spec -> moving_window || spec -> bc_type == PART_BC_OPEN ){

        // Move simulation window if needed
        // [S] SEQUENTIAL: Modifies global state (n_move counter)
        if (spec -> moving_window ) spec_move_window( spec );

        // Use absorbing boundaries along x
        // [S] SEQUENTIAL: Particle removal requires synchronized array shrinking
        //     Could be parallelized with thread-local buffers + merge
        int i = 0;
        while ( i < spec -> np ) {
            if (( spec -> part[i].ix < 0 ) || ( spec -> part[i].ix >= nx0 )) {
                spec -> part[i] = spec -> part[ -- spec -> np ];
                continue;
            }
            i++;
        }

    } else {
        // Use periodic boundaries in x
        // [P] PARALLELIZABLE: Each particle updated independently
        //     #pragma omp for
        for (int i=0; i<spec->np; i++) {
            spec -> part[i].ix += (( spec -> part[i].ix < 0 ) ? nx0 : 0 ) 
                                 - (( spec -> part[i].ix >= nx0 ) ? nx0 : 0);
        }
    }

    // Sort species at every n_sort time steps
    // [S] SEQUENTIAL: Particle sorting (not in hot path, only every n_sort iterations)
    if ( spec -> n_sort > 0 ) {
        if ( ! (spec -> iter % spec -> n_sort) ) spec_sort( spec );
    }

    // Timing info
    // [S] SEQUENTIAL: Global timing statistics
    _spec_npush += spec -> np;
    _spec_time += timer_interval_seconds( t0, timer_ticks() );
}

/*
 * SUMMARY OF FINDINGS
 * 
 * Loop-level parallelization potential: 90% (most of computation in loop)
 * 
 * Independent iterations:           ✅ YES
 * Read-only shared data:            ✅ YES (E_part, B_part)
 * Write-shared data:                ⚠️ YES (J[]) - DATA RACE
 * Cross-iteration dependencies:     ✅ NO
 * 
 * Critical bottleneck: current->J[] deposition
 * 
 * Recommended approach: Thread-local current grids
 * Expected efficiency: 85-90% for 12-16 cores on A64FX
 */
