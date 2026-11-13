#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "simulator.h"

/*
 * Predictive pager.
 *
 * Idea:
 *  1. The simulated programs are a small known set (see programs.o). Their
 *     control-flow skeleton (size, exit PC, branch sites/targets/types) is
 *     hardcoded below.
 *  2. Each tick we watch every process's PC. Observed PC transitions
 *     (sequential step vs. jump) let us eliminate program types that could
 *     not have produced the trace. What remains is the set of "candidate"
 *     programs for that process.
 *  3. For every program and every PC we precompute dist[k][pc][page]: the
 *     minimum number of ticks until the program could first touch `page`,
 *     following every possible branch outcome (GOTO is always taken, FOR/IF
 *     may go either way).
 *  4. For each process, a page is "needed" if, for some candidate program,
 *     it can be touched within LOOKAHEAD ticks and is paged in (most urgent
 *     first) if not resident. A resident page is only paged out once it
 *     cannot be needed within KEEP ticks: a page-out must finish (100 ticks)
 *     before the page can be paged back in (another 100 ticks), so evicting
 *     a page that a loop revisits soon would guarantee a miss.
 *
 * If the observed trace contradicts every candidate, we fall back to
 * considering all programs plus a simple sequential prefetch, so the pager
 * still behaves sensibly (reactive paging of the current page is always done).
 */

#define NPROG      10
#define MAXBR       4
#define INF       255          /* "not reachable within the horizon" */
#ifndef LOOKAHEAD
#define LOOKAHEAD 104          /* ticks of lead time (PAGEWAIT=100, + slack) */
#endif
#ifndef KEEP
#define KEEP      215          /* keep a page resident if it is needed again
                                  within this many ticks: evicting it would
                                  cost a page-out (100) + page-in (100)     */
#endif
#define HORIZON   (KEEP + 1)

#define B_GOTO 0
#define B_FOR  1
#define B_IF   3

typedef struct {
    int from, to, type;
} Br;

typedef struct {
    int size;
    int exitpc;
    int nbr;
    Br  br[MAXBR];
} Prog;

static const Prog progs[NPROG] = {
    /* 0 */ {1435, 1434, 3, {{ 500, 1402, B_IF  }, {1301, 1433, B_GOTO}, {1433,    0, B_FOR}}},
    /* 1 */ {2535, 2534, 3, {{1500, 2402, B_IF  }, {2401, 2533, B_GOTO}, {2533,    0, B_FOR}}},
    /* 2 */ {1431, 1430, 1, {{1429,    0, B_FOR }}},
    /* 3 */ {2131, 2130, 1, {{2129,    0, B_FOR }}},
    /* 4 */ {1285, 1284, 2, {{1282, 1066, B_FOR }, {1283,    0, B_FOR}}},
    /* 5 */ {2485, 2484, 2, {{2482, 2166, B_FOR }, {2483,    0, B_FOR}}},
    /* 6 */ { 912,  911, 0, {{0}}},
    /* 7 */ {2500, 2499, 0, {{0}}},
    /* 8 */ { 705,  704, 4, {{ 700,  703, B_IF  }, { 701,    0, B_GOTO},
                             { 702,  703, B_GOTO}, { 703,    0, B_FOR}}},
    /* 9 */ {1505, 1504, 4, {{1500, 1503, B_IF  }, {1501,    0, B_GOTO},
                             {1502, 1503, B_GOTO}, {1503,    0, B_FOR}}},
};

#define ALLMASK ((1u << NPROG) - 1)
#define MAXSZ   2535

/* dist[k][pc][page]: min ticks from pc until page is first executed */
static unsigned char dist[NPROG][MAXSZ][MAXPROCPAGES];

/* branch lookup: index of branch at pc for program k, or -1 */
static signed char brat[NPROG][MAXSZ];

typedef struct {
    int req_proc, req_page, req_dist;
} Req;

static int req_cmp(const void *a, const void *b) {
    const Req *x = a, *y = b;
    return x->req_dist - y->req_dist;
}

static void build_tables(void) {
    static int dq[MAXSZ + 1], dd[MAXSZ + 1];
    static short seen[MAXSZ];
    static short stamp = 0;

    for (int k = 0; k < NPROG; k++) {
        const Prog *p = &progs[k];
        for (int pc = 0; pc < MAXSZ; pc++) brat[k][pc] = -1;
        for (int b = 0; b < p->nbr; b++) brat[k][p->br[b].from] = (signed char)b;

        for (int s = 0; s < p->size; s++) {
            for (int g = 0; g < MAXPROCPAGES; g++) dist[k][s][g] = INF;
            /* BFS over pcs, unit cost per tick */
            stamp++;
            int head = 0, tail = 0;
            dq[tail] = s; dd[tail++] = 0; seen[s] = stamp;
            while (head < tail) {
                int pc = dq[head], d = dd[head++];
                int pg = pc / PAGESIZE;
                if (pg < MAXPROCPAGES && dist[k][s][pg] == INF)
                    dist[k][s][pg] = (unsigned char)d;
                if (d >= HORIZON) continue;
                if (pc == p->exitpc) continue;      /* process leaves */
                int nxt[2], nn = 0;
                int b = brat[k][pc];
                if (b < 0) {
                    nxt[nn++] = pc + 1;
                } else {
                    nxt[nn++] = p->br[b].to;
                    if (p->br[b].type != B_GOTO) nxt[nn++] = pc + 1;
                }
                for (int i = 0; i < nn; i++) {
                    int n = nxt[i];
                    if (n < 0 || n >= p->size) n = 0;
                    if (seen[n] != stamp) {
                        seen[n] = stamp;
                        dq[tail] = n; dd[tail++] = d + 1;
                    }
                }
            }
        }
    }
}

/* Update the candidate-program mask given an observed PC transition. */
static unsigned update_mask(unsigned mask, int prev, int pc) {
    unsigned out = 0;
    if (pc == prev) return mask;

    if (pc == prev + 1) {
        /* sequential step: rule out programs that must branch here, or
         * that would already have exited */
        for (int k = 0; k < NPROG; k++) {
            if (!(mask & (1u << k))) continue;
            if (prev == progs[k].exitpc) continue;
            int b = brat[k][prev];
            if (b >= 0 && progs[k].br[b].type == B_GOTO &&
                progs[k].br[b].to != pc) continue;
            out |= 1u << k;
        }
        return out ? out : ALLMASK;
    }

    /* a jump: keep programs with a matching branch. If none matches, this
     * is either a freshly loaded process (pc == 0 after an exit) or an
     * inconsistent trace; in both cases start over with all candidates. */
    for (int k = 0; k < NPROG; k++) {
        if (!(mask & (1u << k))) continue;
        int b = brat[k][prev];
        if (b >= 0 && progs[k].br[b].to == pc) out |= 1u << k;
    }
    return out ? out : ALLMASK;
}

void pageit(Pentry q[MAXPROCESSES]) {

    /* Static vars */
    static int initialized = 0;
    static int tick = 1; // artificial time
    static unsigned mask[MAXPROCESSES];
    static int prevpc[MAXPROCESSES];
    static int prevactive[MAXPROCESSES];

    /* Local vars */
    Req reqs[MAXPROCESSES * MAXPROCPAGES];
    int nreq = 0;
    int i, g;

    /* initialize static vars on first run */
    if (!initialized) {
        build_tables();
        for (i = 0; i < MAXPROCESSES; i++) {
            mask[i] = ALLMASK;
            prevpc[i] = 0;
            prevactive[i] = 0;
        }
        initialized = 1;
    }

    for (i = 0; i < MAXPROCESSES; i++) {
        if (!q[i].active) { prevactive[i] = 0; continue; }

        int pc = (int)q[i].pc;
        int cur = pc / PAGESIZE;

        /* ---- classify the process ---- */
        if (!prevactive[i])
            mask[i] = ALLMASK;
        else
            mask[i] = update_mask(mask[i], prevpc[i], pc);
        prevpc[i] = pc;
        prevactive[i] = 1;

        /* ---- which pages will be touched soon? ---- */
        int best[MAXPROCPAGES];
        for (g = 0; g < MAXPROCPAGES; g++) best[g] = INF;
        for (int k = 0; k < NPROG; k++) {
            if (!(mask[i] & (1u << k))) continue;
            if (pc >= progs[k].size) continue;
            for (g = 0; g < MAXPROCPAGES; g++) {
                int d = dist[k][pc][g];
                if (d < best[g]) best[g] = d;
            }
        }
        best[cur] = 0;
        /* generic fallback: the sequentially next page */
        if (cur + 1 < MAXPROCPAGES) {
            int d = PAGESIZE - (pc % PAGESIZE);
            if (d < best[cur + 1]) best[cur + 1] = d;
        }

        for (g = 0; g < MAXPROCPAGES; g++) {
            if (q[i].pages[g]) {
                if (best[g] > KEEP) pageout(i, g);
            } else if (best[g] <= LOOKAHEAD) {
                reqs[nreq].req_proc = i;
                reqs[nreq].req_page = g;
                reqs[nreq].req_dist = best[g];
                nreq++;
            }
        }
    }

    /* most urgent page-ins first */
    qsort(reqs, nreq, sizeof(Req), req_cmp);
    for (i = 0; i < nreq; i++)
        pagein(reqs[i].req_proc, reqs[i].req_page);

    /* advance time for next pageit iteration */
    tick++;
}
