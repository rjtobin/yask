/*****************************************************************************

YASK: Yet Another Stencil Kernel
Copyright (c) 2014-2016, Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to
deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
sell copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

* The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
IN THE SOFTWARE.

*****************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

// Stencil types.
#include "stencil.hpp"

// Base classes for stencil code.
#include "stencil_calc.hpp"

// Include auto-generated stencil code.
#include "stencil_code.hpp"

using namespace std;
using namespace yask;

// Set MODEL_CACHE to 1 or 2 to model that cache level
// and create a global cache object here.
#ifdef MODEL_CACHE
Cache cache(MODEL_CACHE);
#endif

// Fix bsize, if needed, to fit into rsize and be a multiple of mult.
// Return number of blocks.
idx_t findNumSubsets(idx_t& bsize, const string& bname,
                     idx_t rsize, const string& rname,
                     idx_t mult, string dim) {
    if (bsize < 1) bsize = rsize; // 0 => use full size.
    if (bsize > rsize) bsize = rsize;
    bsize = ROUND_UP(bsize, mult);
    idx_t nblks = (rsize + bsize - 1) / bsize;
    idx_t rem = rsize % bsize;
    idx_t nfull_blks = rem ? (nblks - 1) : nblks;

    cout << " In '" << dim << "' dimension, " << rname << " of size " <<
        rsize << " is divided into " << nfull_blks << " " << bname << "(s) of size " << bsize;
    if (rem)
        cout << " plus 1 remainder " << bname << " of size " << rem;
    cout << "." << endl;
    return nblks;
}
idx_t findNumBlocks(idx_t& bsize, idx_t rsize, idx_t mult, string dim) {
    return findNumSubsets(bsize, "block", rsize, "region", mult, dim);
}
idx_t findNumRegions(idx_t& rsize, idx_t dsize, idx_t mult, string dim) {
    return findNumSubsets(rsize, "region", dsize, "rank", mult, dim);
}

// Parse command-line args, run kernel, run validation if requested.
int main(int argc, char** argv)
{
    SEP_PAUSE;

    // MPI init.
    int my_rank = 0;
    int num_ranks = 1;
#ifdef USE_MPI
    MPI_Init(&argc, &argv);
    MPI_Comm comm = MPI_COMM_WORLD;
    MPI_Comm_rank(comm, &my_rank);
    MPI_Comm_size(comm, &num_ranks);
#else
    MPI_Comm comm = 0;
#endif
    bool is_leader = my_rank == 0;

    if (is_leader) {
        cout << "Invocation:";
        for (int i = 0; i < argc; i++)
            cout << " " << argv[i];
        cout << endl;

#ifdef DEBUG
        cout << "*** WARNING: binary compiled with DEBUG; ignore performance results.\n";
#endif
#if defined(NO_INTRINSICS) && (VLEN > 1)
        cout << "*** WARNING: binary compiled with NO_INTRINSICS; ignore performance results.\n";
#endif
#ifdef MODEL_CACHE
        cout << "*** WARNING: binary compiled with MODEL_CACHE; ignore performance results.\n";
#endif
#ifdef TRACE_MEM
        cout << "*** WARNING: binary compiled with TRACE_MEM; ignore performance results.\n";
#endif
#ifdef TRACE_INTRINSICS
        cout << "*** WARNING: binary compiled with TRACE_INTRINSICS; ignore performance results.\n";
#endif

        cout << endl <<
            "┌──────────────────────────────────────────┐\n"
            "│  Y.A.S.K. ── Yet Another Stencil Kernel  │\n"
            "│            https://01.org/yask           │\n"
            "│    Intel Corporation, copyright 2016     │\n"
            "└──────────────────────────────────────────┘\n"
            "\nStencil name: " STENCIL_NAME << endl;

    }

    // Stagger init messages in time.
    // TODO: create an MPI-safe I/O handler.
    sleep(my_rank);
    cout << endl;
#ifdef USE_MPI
    cout << "MPI rank " << my_rank << " of " << num_ranks << endl;
#else
    cout << "MPI not enabled." << endl;
#endif
    
    // options and their defaults.
    idx_t num_trials = 3; // number of trials.
    idx_t dt = 50;     // number of time-steps per trial.
    idx_t dn = 1, dx = DEF_RANK_SIZE, dy = DEF_RANK_SIZE, dz = DEF_RANK_SIZE;
    idx_t rt = 1;                         // wavefront time steps.
    idx_t rn = 0, rx = 0, ry = 0, rz = 0;  // region sizes (0 => use rank size).
    idx_t bt = 1;                          // temporal block size.
    idx_t bn = 1, bx = DEF_BLOCK_SIZE, by = DEF_BLOCK_SIZE, bz = DEF_BLOCK_SIZE;  // size of cache blocks.
    idx_t pn = 0, px = DEF_PAD, py = DEF_PAD, pz = DEF_PAD; // padding.
    idx_t nrn = 1, nrx = num_ranks, nry = 1, nrz = 1; // num ranks in each dim.
    bool validate = false;
    int  block_threads = DEF_BLOCK_THREADS; // number of threads for a block.
    bool doWarmup = true;
    int pre_trial_sleep_time = 1;   // sec to sleep before each trial.

    // parse options.
    bool help = false;
    for (int argi = 1; argi < argc; argi++) {
        if ( argv[argi][0] == '-' && argv[argi][1] ) {
            string opt = argv[argi];

            // options w/o values.
            if (opt == "-h" || opt == "-help" || opt == "--help") {
                cout << 
                    "Usage: [options]\n"
                    "Options:\n"
                    " -h:              print this help and the current settings, then exit\n"
                    " -t <n>           number of trials, default=" <<
                    num_trials << endl <<
                    " -dt <n>          rank domain size in temporal dimension (number of time steps), default=" <<
                    dt << endl <<
                    " -d{n,x,y,z} <n>  rank domain size in specified spatial dimension, defaults=" <<
                    dn << '*' << dx << '*' << dy << '*' << dz << endl <<
                    " -d <n>           set same rank size in 3 {x,y,z} spatial dimensions\n" <<
                    " -rt <n>          OpenMP region time steps (for wave-front tiling), default=" <<
                    rt << endl <<
                    " -r{n,x,y,z} <n>  OpenMP region size in specified spatial dimension, defaults=" <<
                    rn << '*' << rx << '*' << ry << '*' << rz << endl <<
                    " -r <n>           set same OpenMP region size in 3 {x,y,z} spatial dimensions\n"
                    " -b{n,x,y,z} <n>  cache block size in specified spatial dimension, defaults=" <<
                    bn << '*' << bx << '*' << by << '*' << bz << endl <<
                    " -b <n>           set same cache block size in 3 {x,y,z} spatial dimensions\n" <<
                    " -p{n,x,y,z} <n>  extra padding in specified spatial dimension, defaults=" <<
                    pn << '*' << px << '*' << py << '*' << pz << endl <<
                    " -p <n>           set same padding in 3 {x,y,z} spatial dimensions\n" <<
#ifdef USE_MPI
                    " -nr{n,x,y,z} <n> num ranks in specified spatial dimension, defaults=" <<
                    nrn << '*' << nrx << '*' << nry << '*' << nrz << endl <<
                    " -nr <n>          set same num ranks in 3 {x,y,z} spatial dimensions\n" <<
#endif
                    " -i <n>           equivalent to -dt, for backward compatibility\n" <<
                    " -bthreads <n>    set number of threads to use for a block, default=" <<
                    block_threads << endl <<
                    " -v               validate by comparing to a scalar run\n" <<
                    " -nw              skip warmup\n" <<
                    "Notes:\n"
#ifndef USE_MPI
                    " This binary has not been built with MPI support.\n"
#endif
                    " A block size of 0 => block size == region size in that dimension.\n"
                    " A region size of 0 => region size == rank size in that dimension.\n"
                    " Control the time steps in each temporal wave-front with -rt:\n"
                    "  1 effectively disables wave-front tiling.\n"
                    "  0 enables wave-front tiling across all time steps in one pass.\n"
                    "  Any value other than 1 also changes the region spatial-size defaults.\n"
                    " Temporal cache blocking is not yet supported => bt == 1.\n"
                    " Validation is very slow and uses 2x memory, so run with very small sizes.\n"
                    " If validation fails, it may be due to rounding error; try building with 8-byte reals.\n"
                    " Validation disables warmup and sets the default number of trials to 1.\n"
                    " The 'n' dimension only applies to stencils that use that variable.\n"
                    "Examples:\n" <<
                    " " << argv[0] << " -d 768 -dt 4\n" <<
                    " " << argv[0] << " -dx 512 -dy 256 -dz 128\n" <<
                    " " << argv[0] << " -d 2048 -dt 20 -r 512 -rt 10  # temporal tiling.\n" <<
                    " " << argv[0] << " -d 512 -npx 2 -npy 1 -npz 2   # multi-rank.\n" <<
                    " " << argv[0] << " -d 64 -v                      # validation.\n";
                help = true;
            }

            else if (opt == "-nw")
                doWarmup = false;

            // validation.
            else if (opt == "-v") {
                validate = true;
                num_trials = 1;
            }

            // options w/int values.
            else {

                if (argi >= argc) {
                    cerr << "error: no value for option '" << opt << "'." << endl;
                    exit(1);
                }
                int val = atoi(argv[++argi]);
                if (opt == "-t") num_trials = val;
                else if (opt == "-i") dt = val;
                else if (opt == "-dt") dt = val;
                else if (opt == "-dn") dn = val;
                else if (opt == "-dx") dx = val;
                else if (opt == "-dy") dy = val;
                else if (opt == "-dz") dz = val;
                else if (opt == "-d") dx = dy = dz = val;
                else if (opt == "-rt") rt = val;
                else if (opt == "-rn") rn = val;
                else if (opt == "-rx") rx = val;
                else if (opt == "-ry") ry = val;
                else if (opt == "-rz") rz = val;
                else if (opt == "-r") rx = ry = rz = val;
                else if (opt == "-bn") bn = val;
                else if (opt == "-bx") bx = val;
                else if (opt == "-by") by = val;
                else if (opt == "-bz") bz = val;
                else if (opt == "-b") bx = by = bz = val;
                else if (opt == "-pn") pn = val;
                else if (opt == "-px") px = val;
                else if (opt == "-py") py = val;
                else if (opt == "-pz") pz = val;
                else if (opt == "-p") px = py = pz = val;
#ifdef USE_MPI
                else if (opt == "-nrn") nrn = val;
                else if (opt == "-nrx") nrx = val;
                else if (opt == "-nry") nry = val;
                else if (opt == "-nrz") nrz = val;
                else if (opt == "-nr") nrx = nry = nrz = val;
#endif
                else if (opt == "-bthreads") block_threads = val;
                else {
                    cerr << "error: option '" << opt << "' not recognized." << endl;
                    exit(1);
                }
            }
        }
        else {
            cerr << "error: extraneous parameter '" <<
                argv[argi] << "'." << endl;
            exit(1);
        }
    }
    // done reading args.

    // TODO: check all dims.
#ifndef USING_DIM_N
    if (dn > 1) {
        cerr << "error: dn = " << dn << ", but stencil '"
            STENCIL_NAME "' doesn't use dimension 'n'." << endl;
        exit(1);
    }
#endif

    // Check ranks.
    idx_t req_ranks = nrn * nrx * nry * nrz;
    if (req_ranks != num_ranks) {
        cerr << "error: " << req_ranks << " rank(s) requested, but MPI reports " <<
            num_ranks << " rank(s) are active." << endl;
        exit(1);
    }
    
    // Context for evaluating results.
    STENCIL_CONTEXT context;
    context.num_ranks = num_ranks;
    context.my_rank = my_rank;
    context.comm = comm;

    // report threads.
    {
        cout << endl;
#if defined(_OPENMP)
        int omp_num_procs = omp_get_num_procs();
        cout << "Num OpenMP procs: " << omp_num_procs << endl;
        context.orig_max_threads = omp_get_max_threads();
        cout << "Num OpenMP threads: " << context.orig_max_threads << endl;

#if USE_CREW
        // Init Crew.
        cout << "Creating crews..." << endl;
        kmp_crew_create();
        int numThreads = omp_get_max_threads();
        cout << "Num OpenMP threads after crew creation: " << numThreads << endl;
        int crewSize = kmp_crew_get_max_size();
        int numWorkers = numThreads * crewSize;
        cout << "Total num crews: " << numWorkers << endl <<
            "  Num crew-leader threads: " << numThreads << endl <<
            "  Num threads per crew: " << crewSize << endl;
        if (numWorkers == context.orig_max_threads)
            cout << "Note: sanity check passed: num crews == num OpenMP threads before creating crews." << endl;
        else {
            cout << "Error: sanity check failed: num crews != num OpenMP threads before creating crews.\n"
                "This usually indicates your OpenMP library has a crew-initialization issue.\n"
                "Please update your OpenMP library or rebuild with crew disabled (make crew=0 ...).\n";
            exit(1);
        }
#else

        // Enable nesting and report nesting threads.
        assert(block_threads > 0);
        if (block_threads > 1)
            omp_set_nested(1);
        context.num_block_threads = block_threads;
        int rt = context.set_region_threads(); // Temporary; just for reporting.
        cout << "  Num threads per region: " << omp_get_max_threads() << endl;
        cout << "  Num threads per block: " << block_threads << endl;
        context.set_max_threads(); // Back to normal.
#endif
#else
        int numThreads = 1;
        cout << "Num threads: " << numThreads << endl;
#endif
    }

    // Adjust defaults for wavefronts.
    if (rt != 1) {
        if (!rn) rn = 1;
        if (!rx) rx = DEF_WAVEFRONT_REGION_SIZE;
        if (!ry) ry = DEF_WAVEFRONT_REGION_SIZE;
        if (!rz) rz = DEF_WAVEFRONT_REGION_SIZE;

        // TODO: enable this.
        if (num_ranks > 1) {
            cerr << "Sorry, MPI communication is not currently enabled with wave-front tiling." << endl;
        }
    }

    // Round up vars as needed.
    dt = roundUp(dt, CPTS_T, "rank size in t (time steps)");
    dn = roundUp(dn, CPTS_N, "rank size in n");
    dx = roundUp(dx, CPTS_X, "rank size in x");
    dy = roundUp(dy, CPTS_Y, "rank size in y");
    dz = roundUp(dz, CPTS_Z, "rank size in z");

    // Determine num regions based on region sizes.
    // Also fix up region sizes as needed.
    cout << "\nRegions:" << endl;
    idx_t nrgt = findNumRegions(rt, dt, CPTS_T, "t");
    idx_t nrgn = findNumRegions(rn, dn, CPTS_N, "n");
    idx_t nrgx = findNumRegions(rx, dx, CPTS_X, "x");
    idx_t nrgy = findNumRegions(ry, dy, CPTS_Y, "y");
    idx_t nrgz = findNumRegions(rz, dz, CPTS_Z, "z");
    idx_t nrg = nrgt * nrgn * nrgx * nrgy * nrgz;
    cout << " num-regions-per-rank: " << nrg << endl;

    // Determine num blocks based on block sizes.
    // Also fix up block sizes as needed.
    cout << "\nBlocks:" << endl;
    idx_t nbt = findNumBlocks(bt, rt, CPTS_T, "t");
    idx_t nbn = findNumBlocks(bn, rn, CPTS_N, "n");
    idx_t nbx = findNumBlocks(bx, rx, CPTS_X, "x");
    idx_t nby = findNumBlocks(by, ry, CPTS_Y, "y");
    idx_t nbz = findNumBlocks(bz, rz, CPTS_Z, "z");
    idx_t nb = nbt * nbn * nbx * nby * nbz;
    cout << " num-blocks-per-region: " << nb << endl;

    // Round up padding as needed.
    pn = roundUp(pn, VLEN_N, "extra padding in n");
    px = roundUp(px, VLEN_X, "extra padding in x");
    py = roundUp(py, VLEN_Y, "extra padding in y");
    pz = roundUp(pz, VLEN_Z, "extra padding in z");

    // Round up halos as needed.
    // TODO: get rid of this when grid-specific halos
    // are used throughout.
#ifdef USING_DIM_N
    idx_t hn = ROUND_UP(context.max_halo_n, VLEN_N);
#else
    idx_t hn = 0;
#endif
    idx_t hx = ROUND_UP(context.max_halo_x, VLEN_X);
    idx_t hy = ROUND_UP(context.max_halo_y, VLEN_Y);
    idx_t hz = ROUND_UP(context.max_halo_z, VLEN_Z);
    
    cout << "\nSizes in points per grid (t*n*x*y*z):\n"
        " vector-size: " << VLEN_T << '*' << VLEN_N << '*' << VLEN_X << '*' << VLEN_Y << '*' << VLEN_Z << endl <<
        " cluster-size: " << CPTS_T << '*' << CPTS_N << '*' << CPTS_X << '*' << CPTS_Y << '*' << CPTS_Z << endl <<
        " block-size: " << bt << '*' << bn << '*' << bx << '*' << by << '*' << bz << endl <<
        " region-size: " << rt << '*' << rn << '*' << rx << '*' << ry << '*' << rz << endl <<
        " rank-size: " << dt << '*' << dn << '*' << dx << '*' << dy << '*' << dz << endl <<
        " overall-size: " << dt << '*' << dn << '*' << (dx * num_ranks) << '*' << dy << '*' << dz << endl;
    cout << "\nOther settings:\n"
        " num-ranks: " << nrn << '*' << nrx << '*' << nry << '*' << nrz << endl <<
        " stencil-shape: " STENCIL_NAME << endl << 
        " time-dim-size: " << TIME_DIM_SIZE << endl <<
        " vector-len: " << VLEN << endl <<
        " padding: " << pn << '+' << px << '+' << py << '+' << pz << endl <<
        " max-halos: " << hn << '+' << hx << '+' << hy << '+' << hz << endl <<
        " manual-L1-prefetch-distance: " << PFDL1 << endl <<
        " manual-L2-prefetch-distance: " << PFDL2 << endl;

    if (help) {
        cout << "Exiting due to help option." << endl;
        exit(1);
    }

    // Save sizes in context struct.
    context.dt = dt;
    context.dn = dn;
    context.dx = dx;
    context.dy = dy;
    context.dz = dz;
    
    context.rt = rt;
    context.rn = rn;
    context.rx = rx;
    context.ry = ry;
    context.rz = rz;

    context.bt = bt;
    context.bn = bn;
    context.bx = bx;
    context.by = by;
    context.bz = bz;

    context.pn = pn;
    context.px = px;
    context.py = py;
    context.pz = pz;

    context.hn = hn;
    context.hx = hx;
    context.hy = hy;
    context.hz = hz;

    context.nrn = nrn;
    context.nrx = nrx;
    context.nry = nry;
    context.nrz = nrz;

    // Alloc memory, create lists of grids, etc.
    cout << endl;
    cout << "Allocating grids..." << endl;
    context.allocGrids();
    cout << "Allocating parameters..." << endl;
    context.allocParams();
#ifdef USE_MPI
    cout << "Allocating MPI buffers..." << endl;
    context.setupMPI();
#endif
    idx_t nbytes = context.get_num_bytes();
    cout << "Total rank-" << my_rank << " allocation in " <<
        context.gridPtrs.size() << " grid(s) (bytes): " << printWithPow2Multiplier(nbytes) << endl;
    const idx_t num_eqGrids = context.eqGridPtrs.size();
    cout << "Num grids: " << context.gridPtrs.size() << endl;
    cout << "Num grids to be updated: " << num_eqGrids << endl;

    // Stencil functions.
    idx_t scalar_fp_ops = 0;
    STENCIL_EQUATIONS stencils;
    idx_t num_stencils = stencils.stencils.size();
    cout << endl;
    cout << "Num stencil equations: " << num_stencils << endl <<
        "Est FP ops per point for each equation:" << endl;
    for (auto stencil : stencils.stencils) {
        idx_t fpos = stencil->get_scalar_fp_ops();
        cout << "  '" << stencil->get_name() << "': " << fpos << endl;
        scalar_fp_ops += fpos;
    }

    // Amount of work.
    const idx_t grid_numpts = dn*dx*dy*dz;
    const idx_t grids_numpts = grid_numpts * num_eqGrids;
    const idx_t grids_rank_numpts = dt * grids_numpts;
    const idx_t tot_numpts = grids_rank_numpts * num_ranks;
    const idx_t numFpOps = grid_numpts * scalar_fp_ops;
    const idx_t rank_numFpOps = dt * numFpOps;
    const idx_t tot_numFpOps = rank_numFpOps * num_ranks;
    
    // Print some stats from leader rank.
#ifdef USE_MPI
    cout << flush;
    sleep(1);
    MPI_Barrier(comm);
#endif
    if (is_leader) {
        cout << endl;
        cout << "Points to calculate per rank, time step, and grid: " <<
            printWithPow10Multiplier(grid_numpts) << endl;
        cout << "Points to calculate per rank and time step for all grids: " <<
            printWithPow10Multiplier(grids_numpts) << endl;
        cout << "Points to calculate per rank for all time steps and grids: " <<
            printWithPow10Multiplier(grids_rank_numpts) << endl;
        cout << "Points to calculate per time step for all ranks and grids: " <<
            printWithPow10Multiplier(grids_numpts * num_ranks) << endl;
        cout << "Points to calculate overall: " <<
            printWithPow10Multiplier(tot_numpts) << endl;
        cout << "Est FP ops per point and time step for all grids: " << scalar_fp_ops << endl;
        cout << "Est FP ops per rank and time step for all grids and points: " <<
            printWithPow10Multiplier(numFpOps) << endl;
        cout << "Est FP ops per time step for all grids, points, and ranks: " <<
            printWithPow10Multiplier(numFpOps * num_ranks) << endl;
        cout << "Est FP ops per rank for all grids, points, and time steps: " <<
            printWithPow10Multiplier(rank_numFpOps) << endl;
        cout << "Est FP ops overall: " <<
            printWithPow10Multiplier(tot_numFpOps) << endl;

        cout << "\nTotal overall allocation in " << num_ranks << " rank(s) (bytes): " <<
            printWithPow2Multiplier(nbytes * num_ranks) << endl;
    }
    
    // Exit if nothing to do.
    if (num_trials < 1) {
        cerr << "Exiting because no trials are specified." << endl;
        exit(1);
    }
    if (tot_numpts < 1) {
        cerr << "Exiting because there are zero points to evaluate." << endl;
        exit(1);
    }
    cout << flush;
    MPI_Barrier(comm);

    // This will initialize the grids before running the warmup.  If this is
    // not done, some operations may be done on zero pages, leading to
    // misleading performance or arithmetic exceptions.
    context.initSame();
    cout << flush;
    MPI_Barrier(comm);
    
    // warmup caches, threading, etc.
    if (doWarmup) {
        if (is_leader) cout << endl;

        // Temporarily set dt to a temp value.
        idx_t tmp_dt = min<idx_t>(dt, TIME_DIM_SIZE);
        context.dt = tmp_dt;

#ifdef MODEL_CACHE
        if (!is_leader)
            cache.disable();
        if (cache.isEnabled())
            cout << "Modeling cache...\n";
#endif
        if (is_leader)
            cout << "Warmup of " << context.dt << " time step(s)...\n" << flush;
        stencils.calc_rank_opt(context);

#ifdef MODEL_CACHE
        // print cache stats, then disable.
        if (cache.isEnabled()) {
            cout << "Done modeling cache...\n";
            cache.dumpStats();
            cache.disable();
        }
#endif

        // Replace temp setting with correct value.
        context.dt = dt;
        cout << flush;
        MPI_Barrier(comm);
    }

    // variables for measuring performance.
    double wstart, wstop;
    float best_elapsed_time=0.0f, best_pps=0.0f, best_flops=0.0f;

    // Performance runs.
    if (is_leader) {
        cout << "\nRunning " << num_trials << " performance trial(s) of " <<
            context.dt << " time step(s) each...\n" << flush;
    }
    for (idx_t tr = 0; tr < num_trials; tr++) {

        // init data for comparison if validating.
        if (validate)
            context.initDiff();

        sleep(pre_trial_sleep_time);
        MPI_Barrier(comm);
        SEP_RESUME;
        wstart = getTimeInSecs();

        // Actual work.
        stencils.calc_rank_opt(context);
        
        MPI_Barrier(comm);
        SEP_PAUSE;
        wstop =  getTimeInSecs();
            
        // calc and report perf.
        float elapsed_time = (float)(wstop - wstart);
        float pps = float(tot_numpts)/elapsed_time;
        float flops = float(tot_numFpOps)/elapsed_time;
        if (is_leader) {
            cout << "-----------------------------------------\n" <<
                "time (sec):              " << printWithPow10Multiplier(elapsed_time) << endl <<
                "throughput (points/sec): " << printWithPow10Multiplier(pps) << endl <<
                "throughput (est FLOPS):  " << printWithPow10Multiplier(flops) << endl;
        }

        if (pps > best_pps) {
            best_pps = pps;
            best_elapsed_time = elapsed_time;
            best_flops = flops;
        }
    }

    if (is_leader) {
        cout << "-----------------------------------------\n" <<
            "best-time (sec):              " << printWithPow10Multiplier(best_elapsed_time) << endl <<
            "best-throughput (points/sec): " << printWithPow10Multiplier(best_pps) << endl <<
            "best-throughput (est FLOPS):  " << printWithPow10Multiplier(best_flops) << endl <<
            "-----------------------------------------\n";
    }
    
    if (validate) {
        MPI_Barrier(comm);

        // check the correctness of one iteration.
        if (is_leader)
            cout << "Running validation trial...\n";

        // Make a ref context for comparisons w/new grids:
        // Copy the settings from context, then re-alloc grids.
        STENCIL_CONTEXT ref = context;
        ref.name += "-reference";
        ref.allocGrids();
        ref.allocParams();
        ref.setupMPI();

        // init to same value used in context.
        ref.initDiff();

#if CHECK_INIT
        {
            context.initDiff();
            idx_t errs = context.compare(ref);
            if( errs == 0 ) {
                cout << "INIT CHECK PASSED." << endl;
                exit(0);
            } else {
                cerr << "INIT CHECK FAILED: " << errs << " mismatch(es)." << endl;
                exit(1);
            }
        }
#endif

        // Ref trial.
        stencils.calc_rank_ref(ref);

        // check for equality.
#ifdef USE_MPI
        MPI_Barrier(comm);
        sleep(my_rank);
#endif
        cout << "Checking results on rank " << my_rank << "..." << endl;
        idx_t errs = context.compare(ref);
        if( errs == 0 ) {
            cout << "TEST PASSED." << endl;
        } else {
            cerr << "TEST FAILED: " << errs << " mismatch(es)." << endl;
            exit(1);
        }
    }
    else if (is_leader)
        cout << "\nRESULTS NOT VERIFIED.\n";

#ifdef USE_MPI
    MPI_Barrier(comm);
    MPI_Finalize();
#endif
    if (is_leader)
        cout << "YASK DONE." << endl;
    
    return 0;
}
