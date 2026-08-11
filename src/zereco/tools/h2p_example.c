/* ZERECO motivating microbenchmark.
 *
 * Reproduces the GAP BC (betweenness centrality) PBFS inner loop that we
 * extracted from a real Scarab retired-instruction stream:
 *
 *     for (p = neigh; p != end; ++p) {     // sequential, stride +4
 *         v = *p;                          // TARGET LOAD 1 : perfectly strided
 *         d = depths[v];                   // TARGET LOAD 2 : irregular indirect
 *         if (d == -1)    ...              // H2P #1
 *         if (d == depth) ...              // H2P #2  (shares both loads)
 *     }
 *
 * The two loads form a pointer-chasing pair: load 1's VALUE is load 2's
 * ADDRESS.  Load 1 is trivially address-predictable, load 2 is not.  Both feed
 * the same hard-to-predict branches.  That split is the whole reason ZERECO
 * pairs RF prefetching (for load 1) with priority issue (for the residual
 * slice that load 2 sits in).
 *
 * Build:   gcc -O2 -fno-unroll-loops -o h2p_example h2p_example.c
 * Inspect: gcc -O2 -fno-unroll-loops -S h2p_example.c && less h2p_example.s
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Sized so depths[] does not fit in L1/L2, matching the real workload where
 * the indirect access misses.  Shrink DEPTHS_N to make load 2 an L1 hit. */
#define DEPTHS_N   (1 << 22)      /* 4 M ints = 16 MB */
#define NEIGH_N    (1 << 20)      /* 1 M neighbour ids */
#define ITERS      64

/* Keep the compiler from hoisting or vectorising the loop away. */
static int  *depths;
static int  *neigh;
static long  sink;

/* `scores` gives the second branch a side effect heavy enough that gcc emits a
 * real branch instead of if-converting it to cmov, and the store on the first
 * path forces the address of dep[v] into a register -- both matching what the
 * real BC binary does. */
static double *scores;

__attribute__((noinline))
static long pbfs_inner(const int *p, const int *end, int *dep, int depth,
                       double *sc) {
    long hits = 0;
    for (; p != end; ++p) {
        int v = *p;               /* TARGET LOAD 1: stride +4, predictable   */
        int d = dep[v];           /* TARGET LOAD 2: indirect, unpredictable  */
        if (d == -1) {            /* H2P #1                                  */
            dep[v] = depth;       /*   forces &dep[v] to live in a register  */
            hits += 1;
        }
        if (d == depth) {         /* H2P #2, reuses the same loaded value    */
            sc[v] += sc[v] * 0.5 + 1.0;
            hits += 2;
        }
    }
    return hits;
}

static unsigned long rng_state = 88172645463325252ULL;
static unsigned long xorshift(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

int main(void) {
    depths = malloc((size_t)DEPTHS_N * sizeof(int));
    neigh  = malloc((size_t)NEIGH_N  * sizeof(int));
    if (!depths || !neigh) {
        fprintf(stderr, "alloc failed\n");
        return 1;
    }

    /* depths[] holds a mix of -1 / depth / other so both branches are
     * genuinely data-dependent and neither is biased. */
    for (long i = 0; i < DEPTHS_N; i++)
        depths[i] = (int)(xorshift() % 4) - 1;   /* -1, 0, 1, 2 */

    /* Neighbour ids are scanned sequentially but their VALUES are scattered,
     * so load 1 is strided while load 2 is irregular. */
    for (long i = 0; i < NEIGH_N; i++)
        neigh[i] = (int)(xorshift() % DEPTHS_N);

    scores = malloc((size_t)DEPTHS_N * sizeof(double));
    if (!scores) { fprintf(stderr, "alloc failed\n"); return 1; }
    memset(scores, 0, (size_t)DEPTHS_N * sizeof(double));

    long total = 0;
    for (int it = 0; it < ITERS; it++)
        total += pbfs_inner(neigh, neigh + NEIGH_N, depths, 1, scores);

    sink = total;
    printf("hits=%ld\n", sink);
    free(depths);
    free(neigh);
    free(scores);
    return 0;
}
