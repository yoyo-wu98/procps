/*
 * slabinfo.c - slabinfo related functions for libproc
 *
 * Chris Rivera <cmrivera@ufl.edu>
 * Robert Love <rml@tech9.net>
 *
 * Copyright (C) 2003 Chris Rivera
 * Copyright (C) 2004 Albert Cahalan
 * Copyright (C) 2015 Craig Small <csmall@enc.com.au>
 * Copyright (C) 2016 Jim Warnerl <james.warner@comcast.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>

#include "procps-private.h"
#include <proc/slabinfo.h>

#define SLABINFO_FILE        "/proc/slabinfo"
#define SLABINFO_LINE_LEN    2048
#define SLAB_INFO_NAME_LEN   128


/*
   Because 'select' could, at most, return only node[0] values and since 'reap'
   would be forced to duplicate global slabs stuff in every node results stack,
   the following #define can be used to enforce strictly logical return values.
      select: allow only PROCPS_SLABINFO & PROCPS_SLABS items
      reap:   allow only PROCPS_SLABINFO & PROCPS_SLABNODE items
   Without the #define, these functions always return something (even if just 0)
      get:    return only PROCPS_SLABS results, else 0
      select: return only PROCPS_SLABINFO & PROCPS_SLABS results, else zero
      reap:   return any requested, even when duplicated in each node's stack */
//#define ENFORCE_LOGICAL


struct slabs_summ {
    unsigned int  nr_objs;          /* number of objects, among all caches */
    unsigned int  nr_active_objs;   /* number of active objects, among all caches */
    unsigned int  nr_pages;         /* number of pages consumed by all objects */
    unsigned int  nr_slabs;         /* number of slabs, among all caches */
    unsigned int  nr_active_slabs;  /* number of active slabs, among all caches */
    unsigned int  nr_caches;        /* number of caches */
    unsigned int  nr_active_caches; /* number of active caches */
    unsigned int  avg_obj_size;     /* average object size */
    unsigned int  min_obj_size;     /* size of smallest object */
    unsigned int  max_obj_size;     /* size of largest object */
    unsigned long active_size;      /* size of all active objects */
    unsigned long total_size;       /* size of all objects */
};

struct slabs_node {
    char name[SLAB_INFO_NAME_LEN];  /* name of this cache */
    unsigned long cache_size;       /* size of entire cache */
    unsigned int  nr_objs;          /* number of objects in this cache */
    unsigned int  nr_active_objs;   /* number of active objects */
    unsigned int  obj_size;         /* size of each object */
    unsigned int  objs_per_slab;    /* number of objects per slab */
    unsigned int  pages_per_slab;   /* number of pages per slab */
    unsigned int  nr_slabs;         /* number of slabs in this cache */
    unsigned int  nr_active_slabs;  /* number of active slabs */
    unsigned int  use;              /* percent full: total / active */
};

struct slabs_hist {
    struct slabs_summ new;
    struct slabs_summ old;
};

struct stacks_extent {
    int ext_numstacks;
    struct stacks_extent *next;
    struct slabinfo_stack **stacks;
};

struct ext_support {
    int numitems;                   // includes 'logical_end' delimiter
    enum slabinfo_item *items;      // includes 'logical_end' delimiter
    struct stacks_extent *extents;  // anchor for these extents
#ifdef ENFORCE_LOGICAL
    enum slabinfo_item lowest;      // range of allowable enums
    enum slabinfo_item highest;
#endif
    int dirty_stacks;
};

struct fetch_support {
    struct slabinfo_stack **anchor; // fetch consolidated extents
    int n_alloc;                    // number of above pointers allocated
    int n_inuse;                    // number of above pointers occupied
    int n_alloc_save;               // last known reaped.stacks allocation
    struct slabinfo_reap results;   // count + stacks for return to caller
};

struct procps_slabinfo {
    int refcount;
    FILE *slabinfo_fp;
    int slabinfo_was_read;
    int nodes_alloc;                /* nodes alloc()ed */
    int nodes_used;                 /* nodes using alloced memory */
    struct slabs_node *nodes;       /* first slabnode of this list */
    struct slabs_hist hist;         /* new/old slabs_summ data */
    struct ext_support select_ext;  /* supports concurrent select/reap */
    struct ext_support fetch_ext;   /* supports concurrent select/reap */
    struct fetch_support fetch;     /* support for procps_slabinfo_reap */
    struct slabs_node nul_node;     /* used by procps_slabinfo_select */
};


// ___ Results 'Set' Support ||||||||||||||||||||||||||||||||||||||||||||||||||


#define setNAME(e) set_results_ ## e
#define setDECL(e) static void setNAME(e) \
    (struct slabinfo_result *R, struct slabs_hist *S, struct slabs_node *N)

// regular assignment
#define REG_set(e,t,x) setDECL(e) { (void)N; R->result. t = S->new . x; }
#define NOD_set(e,t,x) setDECL(e) { (void)S; R->result. t = N-> x; }
// delta assignment
#define HST_set(e,t,x) setDECL(e) { (void)N; R->result. t = (signed long)S->new . x - S->old. x; }

setDECL(noop)  { (void)R; (void)S; (void)N; }
setDECL(extra) { (void)R; (void)S; (void)N; }

REG_set(SLABS_OBJS,               u_int,  nr_objs)
REG_set(SLABS_AOBJS,              u_int,  nr_active_objs)
REG_set(SLABS_PAGES,              u_int,  nr_pages)
REG_set(SLABS_SLABS,              u_int,  nr_slabs)
REG_set(SLABS_ASLABS,             u_int,  nr_active_slabs)
REG_set(SLABS_CACHES,             u_int,  nr_caches)
REG_set(SLABS_ACACHES,            u_int,  nr_active_caches)
REG_set(SLABS_SIZE_AVG,           u_int,  avg_obj_size)
REG_set(SLABS_SIZE_MIN,           u_int,  min_obj_size)
REG_set(SLABS_SIZE_MAX,           u_int,  max_obj_size)
REG_set(SLABS_SIZE_ACTIVE,       ul_int,  active_size)
REG_set(SLABS_SIZE_TOTAL,        ul_int,  total_size)

HST_set(SLABS_DELTA_OBJS,         s_int,  nr_objs)
HST_set(SLABS_DELTA_AOBJS,        s_int,  nr_active_objs)
HST_set(SLABS_DELTA_PAGES,        s_int,  nr_pages)
HST_set(SLABS_DELTA_SLABS,        s_int,  nr_slabs)
HST_set(SLABS_DELTA_ASLABS,       s_int,  nr_active_slabs)
HST_set(SLABS_DELTA_CACHES,       s_int,  nr_caches)
HST_set(SLABS_DELTA_ACACHES,      s_int,  nr_active_caches)
HST_set(SLABS_DELTA_SIZE_AVG,     s_int,  avg_obj_size)
HST_set(SLABS_DELTA_SIZE_MIN,     s_int,  min_obj_size)
HST_set(SLABS_DELTA_SIZE_MAX,     s_int,  max_obj_size)
HST_set(SLABS_DELTA_SIZE_ACTIVE,  s_int,  active_size)
HST_set(SLABS_DELTA_SIZE_TOTAL,   s_int,  total_size)

NOD_set(SLABNODE_NAME,              str,  name)
NOD_set(SLABNODE_OBJS,            u_int,  nr_objs)
NOD_set(SLABNODE_AOBJS,           u_int,  nr_active_objs)
NOD_set(SLABNODE_OBJ_SIZE,        u_int,  obj_size)
NOD_set(SLABNODE_OBJS_PER_SLAB,   u_int,  objs_per_slab)
NOD_set(SLABNODE_PAGES_PER_SLAB,  u_int,  pages_per_slab)
NOD_set(SLABNODE_SLABS,           u_int,  nr_slabs)
NOD_set(SLABNODE_ASLABS,          u_int,  nr_active_slabs)
NOD_set(SLABNODE_USE,             u_int,  use)
NOD_set(SLABNODE_SIZE,           ul_int,  cache_size)


// ___ Results 'Get' Support ||||||||||||||||||||||||||||||||||||||||||||||||||

#define getNAME(e) get_results_ ## e
#define getDECL(e) static signed long getNAME(e) \
    (struct procps_slabinfo *I)

// regular get
#define REG_get(e,t,x) getDECL(e) { return I->hist.new. x; }
// delta get
#define HST_get(e,t,x) getDECL(e) { return (signed long)I->hist.new. x - I->hist.old. x; }
// illogical get
#define NOT_get(e) getDECL(e)     { (void)I; return 0; }

NOT_get(noop)
NOT_get(extra)

REG_get(SLABS_OBJS,               u_int,  nr_objs)
REG_get(SLABS_AOBJS,              u_int,  nr_active_objs)
REG_get(SLABS_PAGES,              u_int,  nr_pages)
REG_get(SLABS_SLABS,              u_int,  nr_slabs)
REG_get(SLABS_ASLABS,             u_int,  nr_active_slabs)
REG_get(SLABS_CACHES,             u_int,  nr_caches)
REG_get(SLABS_ACACHES,            u_int,  nr_active_caches)
REG_get(SLABS_SIZE_AVG,           u_int,  avg_obj_size)
REG_get(SLABS_SIZE_MIN,           u_int,  min_obj_size)
REG_get(SLABS_SIZE_MAX,           u_int,  max_obj_size)
REG_get(SLABS_SIZE_ACTIVE,       ul_int,  active_size)
REG_get(SLABS_SIZE_TOTAL,        ul_int,  total_size)

HST_get(SLABS_DELTA_OBJS,         s_int,  nr_objs)
HST_get(SLABS_DELTA_AOBJS,        s_int,  nr_active_objs)
HST_get(SLABS_DELTA_PAGES,        s_int,  nr_pages)
HST_get(SLABS_DELTA_SLABS,        s_int,  nr_slabs)
HST_get(SLABS_DELTA_ASLABS,       s_int,  nr_active_slabs)
HST_get(SLABS_DELTA_CACHES,       s_int,  nr_caches)
HST_get(SLABS_DELTA_ACACHES,      s_int,  nr_active_caches)
HST_get(SLABS_DELTA_SIZE_AVG,     s_int,  avg_obj_size)
HST_get(SLABS_DELTA_SIZE_MIN,     s_int,  min_obj_size)
HST_get(SLABS_DELTA_SIZE_MAX,     s_int,  max_obj_size)
HST_get(SLABS_DELTA_SIZE_ACTIVE,  s_int,  active_size)
HST_get(SLABS_DELTA_SIZE_TOTAL,   s_int,  total_size)

NOT_get(SLABNODE_NAME)
NOT_get(SLABNODE_OBJS)
NOT_get(SLABNODE_AOBJS)
NOT_get(SLABNODE_OBJ_SIZE)
NOT_get(SLABNODE_OBJS_PER_SLAB)
NOT_get(SLABNODE_PAGES_PER_SLAB)
NOT_get(SLABNODE_SLABS)
NOT_get(SLABNODE_ASLABS)
NOT_get(SLABNODE_USE)
NOT_get(SLABNODE_SIZE)


// ___ Sorting Support ||||||||||||||||||||||||||||||||||||||||||||||||||||||||

struct sort_parms {
    int offset;
    enum slabinfo_sort_order order;
};

#define srtNAME(e) sort_results_ ## e

#define REG_srt(T) static int srtNAME(T) ( \
  const struct pids_stack **A, const struct pids_stack **B, struct sort_parms *P) { \
    const struct pids_result *a = (*A)->head + P->offset; \
    const struct pids_result *b = (*B)->head + P->offset; \
    if ( a->result. T > b->result. T ) return P->order > 0 ?  1 : -1; \
    if ( a->result. T < b->result. T ) return P->order > 0 ? -1 :  1; \
    return 0; }

REG_srt(u_int)
REG_srt(ul_int)

static int srtNAME(str) (
  const struct pids_stack **A, const struct pids_stack **B, struct sort_parms *P) {
    const struct pids_result *a = (*A)->head + P->offset;
    const struct pids_result *b = (*B)->head + P->offset;
    return P->order * strcoll(a->result.str, b->result.str);
}

static int srtNAME(noop) (
  const struct pids_stack **A, const struct pids_stack **B, enum pids_item *O) {
    (void)A; (void)B; (void)O;
    return 0;
}

#undef REG_srt


// ___ Controlling Table ||||||||||||||||||||||||||||||||||||||||||||||||||||||

typedef void (*SET_t)(struct slabinfo_result *, struct slabs_hist *, struct slabs_node *);
#define RS(e) (SET_t)setNAME(e)

typedef signed long (*GET_t)(struct procps_slabinfo *);
#define RG(e) (GET_t)getNAME(e)

typedef int  (*QSR_t)(const void *, const void *, void *);
#define QS(t) (QSR_t)srtNAME(t)

        /*
         * Need it be said?
         * This table must be kept in the exact same order as
         * those *enum slabinfo_item* guys ! */
static struct {
    SET_t setsfunc;              // the actual result setting routine
    GET_t getsfunc;              // a routine to return single result
    QSR_t sortfunc;              // sort cmp func for a specific type
} Item_table[] = {
/*  setsfunc                     getsfunc                     sortfunc
    ---------------------------  ---------------------------  ---------  */
  { RS(noop),                    RG(noop),                    QS(noop)   },
  { RS(extra),                   RG(extra),                   QS(noop)   },

  { RS(SLABS_OBJS),              RG(SLABS_OBJS),              QS(noop)   },
  { RS(SLABS_AOBJS),             RG(SLABS_AOBJS),             QS(noop)   },
  { RS(SLABS_PAGES),             RG(SLABS_PAGES),             QS(noop)   },
  { RS(SLABS_SLABS),             RG(SLABS_SLABS),             QS(noop)   },
  { RS(SLABS_ASLABS),            RG(SLABS_ASLABS),            QS(noop)   },
  { RS(SLABS_CACHES),            RG(SLABS_CACHES),            QS(noop)   },
  { RS(SLABS_ACACHES),           RG(SLABS_ACACHES),           QS(noop)   },
  { RS(SLABS_SIZE_AVG),          RG(SLABS_SIZE_AVG),          QS(noop)   },
  { RS(SLABS_SIZE_MIN),          RG(SLABS_SIZE_MIN),          QS(noop)   },
  { RS(SLABS_SIZE_MAX),          RG(SLABS_SIZE_MAX),          QS(noop)   },
  { RS(SLABS_SIZE_ACTIVE),       RG(SLABS_SIZE_ACTIVE),       QS(noop)   },
  { RS(SLABS_SIZE_TOTAL),        RG(SLABS_SIZE_TOTAL),        QS(noop)   },

  { RS(SLABS_DELTA_OBJS),        RG(SLABS_DELTA_OBJS),        QS(noop)   },
  { RS(SLABS_DELTA_AOBJS),       RG(SLABS_DELTA_AOBJS),       QS(noop)   },
  { RS(SLABS_DELTA_PAGES),       RG(SLABS_DELTA_PAGES),       QS(noop)   },
  { RS(SLABS_DELTA_SLABS),       RG(SLABS_DELTA_SLABS),       QS(noop)   },
  { RS(SLABS_DELTA_ASLABS),      RG(SLABS_DELTA_ASLABS),      QS(noop)   },
  { RS(SLABS_DELTA_CACHES),      RG(SLABS_DELTA_CACHES),      QS(noop)   },
  { RS(SLABS_DELTA_ACACHES),     RG(SLABS_DELTA_ACACHES),     QS(noop)   },
  { RS(SLABS_DELTA_SIZE_AVG),    RG(SLABS_DELTA_SIZE_AVG),    QS(noop)   },
  { RS(SLABS_DELTA_SIZE_MIN),    RG(SLABS_DELTA_SIZE_MIN),    QS(noop)   },
  { RS(SLABS_DELTA_SIZE_MAX),    RG(SLABS_DELTA_SIZE_MAX),    QS(noop)   },
  { RS(SLABS_DELTA_SIZE_ACTIVE), RG(SLABS_DELTA_SIZE_ACTIVE), QS(noop)   },
  { RS(SLABS_DELTA_SIZE_TOTAL),  RG(SLABS_DELTA_SIZE_TOTAL),  QS(noop)   },

  { RS(SLABNODE_NAME),           RG(SLABNODE_NAME),           QS(str)    },
  { RS(SLABNODE_OBJS),           RG(SLABNODE_OBJS),           QS(u_int)  },
  { RS(SLABNODE_AOBJS),          RG(SLABNODE_AOBJS),          QS(u_int)  },
  { RS(SLABNODE_OBJ_SIZE),       RG(SLABNODE_OBJ_SIZE),       QS(u_int)  },
  { RS(SLABNODE_OBJS_PER_SLAB),  RG(SLABNODE_OBJS_PER_SLAB),  QS(u_int)  },
  { RS(SLABNODE_PAGES_PER_SLAB), RG(SLABNODE_PAGES_PER_SLAB), QS(u_int)  },
  { RS(SLABNODE_SLABS),          RG(SLABNODE_SLABS),          QS(u_int)  },
  { RS(SLABNODE_ASLABS),         RG(SLABNODE_ASLABS),         QS(u_int)  },
  { RS(SLABNODE_USE),            RG(SLABNODE_USE),            QS(u_int)  },
  { RS(SLABNODE_SIZE),           RG(SLABNODE_SIZE),           QS(ul_int) },

  { NULL,                        NULL,                        NULL       }
};

    /* please note,
     * this enum MUST be 1 greater than the highest value of any enum */
enum slabinfo_item PROCPS_SLABINFO_logical_end = PROCPS_SLABNODE_SIZE + 1;

#undef setNAME
#undef setDECL
#undef REG_set
#undef NOD_set
#undef HST_set
#undef getNAME
#undef getDECL
#undef REG_get
#undef HST_get
#undef NOT_get
#undef srtNAME
#undef RS
#undef RG
#undef QS


// ___ Private Functions ||||||||||||||||||||||||||||||||||||||||||||||||||||||
// --- slabnode specific support ----------------------------------------------

/* Alloc up more slabnode memory, if required
 */
static int alloc_slabnodes (
        struct procps_slabinfo *info)
{
    struct slabs_node *new_nodes;
    int new_count;

    if (info->nodes_used < info->nodes_alloc)
        return 0;
    /* Increment the allocated number of slabs */
    new_count = info->nodes_alloc * 5/4+30;

    new_nodes = realloc(info->nodes, sizeof(struct slabs_node) * new_count);
    if (!new_nodes)
        return -ENOMEM;
    info->nodes = new_nodes;
    info->nodes_alloc = new_count;
    return 0;
} // end: alloc_slabnodes


/*
 * get_slabnode - allocate slab_info structures using a free list
 *
 * In the fast path, we simply return a node off the free list.  In the slow
 * list, we malloc() a new node.  The free list is never automatically reaped,
 * both for simplicity and because the number of slab caches is fairly
 * constant.
 */
static int get_slabnode (
        struct procps_slabinfo *info,
        struct slabs_node **node)
{
    int retval;

    if (info->nodes_used == info->nodes_alloc) {
        if ((retval = alloc_slabnodes(info)) < 0)
            return retval;
    }
    *node = &(info->nodes[info->nodes_used++]);
    return 0;
} // end: get_slabnode


/* parse_slabinfo20:
 *
 * sactual parse routine for slabinfo 2.x (2.6 kernels)
 * Note: difference between 2.0 and 2.1 is in the ": globalstat" part where version 2.1
 * has extra column <nodeallocs>. We don't use ": globalstat" part in both versions.
 *
 * Formats (we don't use "statistics" extensions)
 *
 *  slabinfo - version: 2.1
 *  # name            <active_objs> <num_objs> <objsize> <objperslab> <pagesperslab> \
 *  : tunables <batchcount> <limit> <sharedfactor> \
 *  : slabdata <active_slabs> <num_slabs> <sharedavail>
 *
 *  slabinfo - version: 2.1 (statistics)
 *  # name            <active_objs> <num_objs> <objsize> <objperslab> <pagesperslab> \
 *  : tunables <batchcount> <limit> <sharedfactor> \
 *  : slabdata <active_slabs> <num_slabs> <sharedavail> \
 *  : globalstat <listallocs> <maxobjs> <grown> <reaped> <error> <maxfreeable> <freelimit> <nodeallocs> \
 *  : cpustat <allochit> <allocmiss> <freehit> <freemiss>
 *
 *  slabinfo - version: 2.0
 *  # name            <active_objs> <num_objs> <objsize> <objperslab> <pagesperslab> \
 *  : tunables <batchcount> <limit> <sharedfactor> \
 *  : slabdata <active_slabs> <num_slabs> <sharedavail>
 *
 *  slabinfo - version: 2.0 (statistics)
 *  # name            <active_objs> <num_objs> <objsize> <objperslab> <pagesperslab> \
 *  : tunables <batchcount> <limit> <sharedfactor> \
 *  : slabdata <active_slabs> <num_slabs> <sharedavail> \
 *  : globalstat <listallocs> <maxobjs> <grown> <reaped> <error> <maxfreeable> <freelimit> \
 *  : cpustat <allochit> <allocmiss> <freehit> <freemiss>
 */
static int parse_slabinfo20 (
        struct procps_slabinfo *info)
{
    struct slabs_node *node;
    char buffer[SLABINFO_LINE_LEN];
    int retval;
    int page_size = getpagesize();
    struct slabs_summ *hist = &(info->hist.new);

    hist->min_obj_size = INT_MAX;
    hist->max_obj_size = 0;

    while (fgets(buffer, SLABINFO_LINE_LEN, info->slabinfo_fp )) {
        if (buffer[0] == '#')
            continue;

        if ((retval = get_slabnode(info, &node)) < 0)
            return retval;

        if (sscanf(buffer,
                   "%" STRINGIFY(SLAB_INFO_NAME_LEN) "s" \
                   "%u %u %u %u %u : tunables %*u %*u %*u : slabdata %u %u %*u",
                   node->name,
                   &node->nr_active_objs, &node->nr_objs,
                   &node->obj_size, &node->objs_per_slab,
                   &node->pages_per_slab, &node->nr_active_slabs,
                   &node->nr_slabs) < 8) {
            if (errno != 0)
                return -errno;
            return -EINVAL;
        }

        if (!node->name[0])
            snprintf(node->name, sizeof(node->name), "%s", "unknown");

        if (node->obj_size < hist->min_obj_size)
            hist->min_obj_size = node->obj_size;
        if (node->obj_size > hist->max_obj_size)
            hist->max_obj_size = node->obj_size;

        node->cache_size = (unsigned long)node->nr_slabs * node->pages_per_slab
            * page_size;

        if (node->nr_objs) {
            node->use = (unsigned int)100 * (node->nr_active_objs / node->nr_objs);
            hist->nr_active_caches++;
        } else
            node->use = 0;

        hist->nr_objs += node->nr_objs;
        hist->nr_active_objs += node->nr_active_objs;
        hist->total_size += (unsigned long)node->nr_objs * node->obj_size;
        hist->active_size += (unsigned long)node->nr_active_objs * node->obj_size;
        hist->nr_pages += node->nr_slabs * node->pages_per_slab;
        hist->nr_slabs += node->nr_slabs;
        hist->nr_active_slabs += node->nr_active_slabs;
        hist->nr_caches++;
    }

    if (hist->nr_objs)
        hist->avg_obj_size = hist->total_size / hist->nr_objs;

    return 0;
} // end: parse_slabinfo20


/* read_slabinfo_failed():
 *
 * Read the data out of /proc/slabinfo putting the information
 * into the supplied info container
 *
 * Returns: 0 on success, negative on error
 */
static int read_slabinfo_failed (
        struct procps_slabinfo *info)
{
    char line[SLABINFO_LINE_LEN];
    int retval, major, minor;

    memcpy(&info->hist.old, &info->hist.new, sizeof(struct slabs_summ));
    memset(&(info->hist.new), 0, sizeof(struct slabs_summ));
    if ((retval = alloc_slabnodes(info)) < 0)
        return retval;

    memset(info->nodes, 0, sizeof(struct slabs_node)*info->nodes_alloc);
    info->nodes_used = 0;

    if (NULL == info->slabinfo_fp
    && (info->slabinfo_fp = fopen(SLABINFO_FILE, "r")) == NULL)
        return -errno;

    if (fseek(info->slabinfo_fp, 0L, SEEK_SET) < 0)
        return -errno;

    /* Parse the version string */
    if (!fgets(line, SLABINFO_LINE_LEN, info->slabinfo_fp))
        return -errno;

    if (sscanf(line, "slabinfo - version: %d.%d", &major, &minor) != 2)
        return -EINVAL;

    if (major == 2)
        retval = parse_slabinfo20(info);
    else
        return -ERANGE;

    if (!info->slabinfo_was_read)
        memcpy(&info->hist.old, &info->hist.new, sizeof(struct slabs_summ));
    info->slabinfo_was_read = 1;

    return retval;
} // end: read_slabinfo_failed


// ___ Private Functions ||||||||||||||||||||||||||||||||||||||||||||||||||||||
// --- generalized support ----------------------------------------------------

static inline void assign_results (
        struct slabinfo_stack *stack,
        struct slabs_hist *summ,
        struct slabs_node *node)
{
    struct slabinfo_result *this = stack->head;

    for (;;) {
        enum slabinfo_item item = this->item;
        if (item >= PROCPS_SLABINFO_logical_end)
            break;
        Item_table[item].setsfunc(this, summ, node);
        ++this;
    }
    return;
} // end: assign_results


static inline void cleanup_stack (
        struct slabinfo_result *this)
{
    for (;;) {
        if (this->item >= PROCPS_SLABINFO_logical_end)
            break;
        if (this->item > PROCPS_SLABINFO_noop)
            this->result.ul_int = 0;
        ++this;
    }
} // end: cleanup_stack


static inline void cleanup_stacks_all (
        struct ext_support *this)
{
    int i;
    struct stacks_extent *ext = this->extents;

    while (ext) {
        for (i = 0; ext->stacks[i]; i++)
            cleanup_stack(ext->stacks[i]->head);
        ext = ext->next;
    };
    this->dirty_stacks = 0;
} // end: cleanup_stacks_all


static void extents_free_all (
        struct ext_support *this)
{
    do {
        struct stacks_extent *p = this->extents;
        this->extents = this->extents->next;
        free(p);
    } while (this->extents);
} // end: extents_free_all


static inline struct slabinfo_result *itemize_stack (
        struct slabinfo_result *p,
        int depth,
        enum slabinfo_item *items)
{
    struct slabinfo_result *p_sav = p;
    int i;

    for (i = 0; i < depth; i++) {
        p->item = items[i];
        p->result.ul_int = 0;
        ++p;
    }
    return p_sav;
} // end: itemize_stack


static void itemize_stacks_all (
        struct ext_support *this)
{
    struct stacks_extent *ext = this->extents;

    while (ext) {
        int i;
        for (i = 0; ext->stacks[i]; i++)
            itemize_stack(ext->stacks[i]->head, this->numitems, this->items);
        ext = ext->next;
    };
    this->dirty_stacks = 0;
} // end: static void itemize_stacks_all


static inline int items_check_failed (
        struct ext_support *this,
        enum slabinfo_item *items,
        int numitems)
{
    int i;

    /* if an enum is passed instead of an address of one or more enums, ol' gcc
     * will silently convert it to an address (possibly NULL).  only clang will
     * offer any sort of warning like the following:
     *
     * warning: incompatible integer to pointer conversion passing 'int' to parameter of type 'enum slabinfo_item *'
     * my_stack = procps_slabinfo_select(info, PROCPS_SLABINFO_noop, num);
     *                                         ^~~~~~~~~~~~~~~~
     */
    if (numitems < 1
    || (void *)items < (void *)(unsigned long)(2 * PROCPS_SLABINFO_logical_end))
        return -1;

    for (i = 0; i < numitems; i++) {
#ifdef ENFORCE_LOGICAL
        if (items[i] == PROCPS_SLABINFO_noop
        || (items[i] == PROCPS_SLABINFO_extra))
            continue;
        if (items[i] < this->lowest
        || (items[i] > this->highest))
            return -1;
#else
        // a slabinfo_item is currently unsigned, but we'll protect our future
        if (items[i] < 0)
            return -1;
        if (items[i] >= PROCPS_SLABINFO_logical_end)
            return -1;
#endif
    }

    return 0;
} // end: items_check_failed


static struct stacks_extent *stacks_alloc (
        struct ext_support *this,
        int maxstacks)
{
    struct stacks_extent *p_blob;
    struct slabinfo_stack **p_vect;
    struct slabinfo_stack *p_head;
    size_t vect_size, head_size, list_size, blob_size;
    void *v_head, *v_list;
    int i;

    if (this == NULL || this->items == NULL)
        return NULL;
    if (maxstacks < 1)
        return NULL;

    vect_size  = sizeof(void *) * maxstacks;                   // size of the addr vectors |
    vect_size += sizeof(void *);                               // plus NULL addr delimiter |
    head_size  = sizeof(struct slabinfo_stack);                // size of that head struct |
    list_size  = sizeof(struct slabinfo_result)*this->numitems;// any single results stack |
    blob_size  = sizeof(struct stacks_extent);                 // the extent anchor itself |
    blob_size += vect_size;                                    // plus room for addr vects |
    blob_size += head_size * maxstacks;                        // plus room for head thing |
    blob_size += list_size * maxstacks;                        // plus room for our stacks |

    /* note: all of our memory is allocated in a single blob, facilitating a later free(). |
             as a minimum, it is important that the result structures themselves always be |
             contiguous for every stack since they are accessed through relative position. | */
    if (NULL == (p_blob = calloc(1, blob_size)))
        return NULL;

    p_blob->next = this->extents;                              // push this extent onto... |
    this->extents = p_blob;                                    // ...some existing extents |
    p_vect = (void *)p_blob + sizeof(struct stacks_extent);    // prime our vector pointer |
    p_blob->stacks = p_vect;                                   // set actual vectors start |
    v_head = (void *)p_vect + vect_size;                       // prime head pointer start |
    v_list = v_head + (head_size * maxstacks);                 // prime our stacks pointer |

    for (i = 0; i < maxstacks; i++) {
        p_head = (struct slabinfo_stack *)v_head;
        p_head->head = itemize_stack((struct slabinfo_result *)v_list, this->numitems, this->items);
        p_blob->stacks[i] = p_head;
        v_list += list_size;
        v_head += head_size;
    }
    p_blob->ext_numstacks = maxstacks;
    return p_blob;
} // end: stacks_alloc


static int stacks_fetch (
        struct procps_slabinfo *info)
{
 #define memINCR  64
 #define n_alloc  info->fetch.n_alloc
 #define n_inuse  info->fetch.n_inuse
 #define n_saved  info->fetch.n_alloc_save
    struct stacks_extent *ext;

    // initialize stuff -----------------------------------
    if (!info->fetch.anchor) {
        if (!(info->fetch.anchor = calloc(sizeof(void *), memINCR)))
            return -ENOMEM;
        n_alloc = memINCR;
    }
    if (!info->fetch_ext.extents) {
        if (!(ext = stacks_alloc(&info->fetch_ext, n_alloc)))
            return -ENOMEM;
        memset(info->fetch.anchor, 0, sizeof(void *) * n_alloc);
        memcpy(info->fetch.anchor, ext->stacks, sizeof(void *) * n_alloc);
        itemize_stacks_all(&info->fetch_ext);
    }
    cleanup_stacks_all(&info->fetch_ext);

    // iterate stuff --------------------------------------
    n_inuse = 0;
    while (n_inuse < info->nodes_used) {
        if (!(n_inuse < n_alloc)) {
            n_alloc += memINCR;
            if ((!(info->fetch.anchor = realloc(info->fetch.anchor, sizeof(void *) * n_alloc)))
            || (!(ext = stacks_alloc(&info->fetch_ext, memINCR))))
                return -1;
            memcpy(info->fetch.anchor + n_inuse, ext->stacks, sizeof(void *) * memINCR);
        }
        assign_results(info->fetch.anchor[n_inuse], &info->hist, &info->nodes[n_inuse]);
        ++n_inuse;
    }

    // finalize stuff -------------------------------------
    if (n_saved < n_alloc + 1) {
        n_saved = n_alloc + 1;
        if (!(info->fetch.results.stacks = realloc(info->fetch.results.stacks, sizeof(void *) * n_saved)))
            return -1;
    }
    memcpy(info->fetch.results.stacks, info->fetch.anchor, sizeof(void *) * n_inuse);
    info->fetch.results.stacks[n_inuse] = NULL;
    info->fetch.results.total = n_inuse;

    return n_inuse;
 #undef memINCR
 #undef n_alloc
 #undef n_inuse
 #undef n_saved
} // end: stacks_fetch


static int stacks_reconfig_maybe (
        struct ext_support *this,
        enum slabinfo_item *items,
        int numitems)
{
    if (items_check_failed(this, items, numitems))
        return -EINVAL;
    /* is this the first time or have things changed since we were last called?
       if so, gotta' redo all of our stacks stuff ... */
    if (this->numitems != numitems + 1
    || memcmp(this->items, items, sizeof(enum slabinfo_item) * numitems)) {
        // allow for our PROCPS_SLABINFO_logical_end
        if (!(this->items = realloc(this->items, sizeof(enum slabinfo_item) * (numitems + 1))))
            return -ENOMEM;
        memcpy(this->items, items, sizeof(enum slabinfo_item) * numitems);
        this->items[numitems] = PROCPS_SLABINFO_logical_end;
        this->numitems = numitems + 1;
        if (this->extents)
            extents_free_all(this);
        return 1;
    }
    return 0;
} // end: stacks_reconfig_maybe


// ___ Public Functions |||||||||||||||||||||||||||||||||||||||||||||||||||||||

/*
 * procps_slabinfo_new():
 *
 * @info: location of returned new structure
 *
 * Returns: 0 on success <0 on failure
 */
PROCPS_EXPORT int procps_slabinfo_new (
        struct procps_slabinfo **info)
{
    struct procps_slabinfo *p;
    int rc;

    if (info == NULL)
        return -EINVAL;

    if (!(p = calloc(1, sizeof(struct procps_slabinfo))))
        return -ENOMEM;

#ifdef ENFORCE_LOGICAL
    p->select_ext.lowest  = PROCPS_SLABS_OBJS;
    p->select_ext.highest = PROCPS_SLABS_DELTA_SIZE_TOTAL;
    p->fetch_ext.lowest   = PROCPS_SLABNODE_NAME;
    p->fetch_ext.highest  = PROCPS_SLABNODE_SIZE;
#endif

    p->refcount = 1;

    /* do a priming read here for the following potential benefits:
         1) see if the caller's permissions are sufficient (root)
         2) make delta results potentially useful the first time  */
    if ((rc = read_slabinfo_failed(p))) {
        procps_slabinfo_unref(&p);
        return rc;
    }

    *info = p;
    return 0;
} // end: procps_slabinfo_new


PROCPS_EXPORT int procps_slabinfo_ref (
        struct procps_slabinfo *info)
{
    if (info == NULL)
        return -EINVAL;

    info->refcount++;
    return info->refcount;
} // end: procps_slabinfo_ref


PROCPS_EXPORT int procps_slabinfo_unref (
        struct procps_slabinfo **info)
{
    if (info == NULL || *info == NULL)
        return -EINVAL;

    (*info)->refcount--;
    if ((*info)->refcount == 0) {
        if ((*info)->slabinfo_fp) {
            fclose((*info)->slabinfo_fp);
            (*info)->slabinfo_fp = NULL;
        }
        if ((*info)->fetch.anchor)
            free((*info)->fetch.anchor);
        if ((*info)->fetch.results.stacks)
            free((*info)->fetch.results.stacks);

        if ((*info)->select_ext.extents)
            extents_free_all((&(*info)->select_ext));
        if ((*info)->select_ext.items)
            free((*info)->select_ext.items);

        if ((*info)->fetch_ext.extents)
            extents_free_all(&(*info)->fetch_ext);
        if ((*info)->fetch_ext.items)
            free((*info)->fetch_ext.items);

        free((*info)->nodes);

        free(*info);
        *info = NULL;

        return 0;
    }
    return (*info)->refcount;
} // end: procps_slabinfo_unref


PROCPS_EXPORT signed long procps_slabinfo_get (
        struct procps_slabinfo *info,
        enum slabinfo_item item)
{
    static time_t sav_secs;
    time_t cur_secs;
    int rc;

    if (info == NULL)
        return -EINVAL;
    if (item < 0 || item >= PROCPS_SLABINFO_logical_end)
        return -EINVAL;

    /* we will NOT read the slabinfo file with every call - rather, we'll offer
       a granularity of 1 second between reads ... */
    cur_secs = time(NULL);
    if (1 <= cur_secs - sav_secs) {
        if ((rc = read_slabinfo_failed(info)))
            return rc;
        sav_secs = cur_secs;
    }

    return Item_table[item].getsfunc(info);
} // end: procps_slabinfo_get


/* procps_slabinfo_reap():
 *
 * Harvest all the requested SLABNODE information providing the
 * result stacks along with totals via the reap summary.
 *
 * Returns: pointer to a stat_reaped struct on success, NULL on error.
 */
PROCPS_EXPORT struct slabinfo_reap *procps_slabinfo_reap (
        struct procps_slabinfo *info,
        enum slabinfo_item *items,
        int numitems)
{
    if (info == NULL || items == NULL)
        return NULL;

    if (0 > stacks_reconfig_maybe(&info->fetch_ext, items, numitems))
        return NULL;

    if (info->fetch_ext.dirty_stacks)
        cleanup_stacks_all(&info->fetch_ext);

    if (read_slabinfo_failed(info))
        return NULL;
    stacks_fetch(info);
    info->fetch_ext.dirty_stacks = 1;

    return &info->fetch.results;
} // end: procps_slabinfo_reap


/* procps_slabinfo_select():
 *
 * Harvest all the requested MEM and/or SWAP information then return
 * it in a results stack.
 *
 * Returns: pointer to a slabinfo_stack struct on success, NULL on error.
 */
PROCPS_EXPORT struct slabinfo_stack *procps_slabinfo_select (
        struct procps_slabinfo *info,
        enum slabinfo_item *items,
        int numitems)
{
    if (info == NULL || items == NULL)
        return NULL;

    if (0 > stacks_reconfig_maybe(&info->select_ext, items, numitems))
        return NULL;

    if (!info->select_ext.extents
    && !(stacks_alloc(&info->select_ext, 1)))
       return NULL;

    if (info->select_ext.dirty_stacks)
        cleanup_stacks_all(&info->select_ext);

    if (read_slabinfo_failed(info))
        return NULL;
    assign_results(info->select_ext.extents->stacks[0], &info->hist, &info->nul_node);
    info->select_ext.dirty_stacks = 1;

    return info->select_ext.extents->stacks[0];
} // end: procps_slabinfo_select


/*
 * procps_slabinfo_sort():
 *
 * Sort stacks anchored as 'heads' in the passed slabinfo_stack pointers
 * array based on the designated sort enumerator.
 *
 * Returns those same addresses sorted.
 *
 * Note: all of the stacks must be homogeneous (of equal length and content).
 */
PROCPS_EXPORT struct slabinfo_stack **procps_slabinfo_sort (
        struct procps_slabinfo *info,
        struct slabinfo_stack *stacks[],
        int numstacked,
        enum slabinfo_item sortitem,
        enum slabinfo_sort_order order)
{
    struct slabinfo_result *p;
    struct sort_parms parms;
    int offset;

    if (info == NULL || stacks == NULL)
        return NULL;

    // a slabinfo_item is currently unsigned, but we'll protect our future
    if (sortitem < 0 || sortitem >= PROCPS_SLABINFO_logical_end)
        return NULL;
    if (order != PROCPS_SLABINFO_ASCEND && order != PROCPS_SLABINFO_DESCEND)
        return NULL;
    if (numstacked < 2)
        return stacks;

    offset = 0;
    p = stacks[0]->head;
    for (;;) {
        if (p->item == sortitem)
            break;
        ++offset;
        if (p->item == PROCPS_SLABINFO_logical_end)
            return NULL;
        ++p;
    }
    parms.offset = offset;
    parms.order = order;

    qsort_r(stacks, numstacked, sizeof(void *), (QSR_t)Item_table[p->item].sortfunc, &parms);
    return stacks;
 #undef QSORT_r
} // end: procps_slabinfo_sort