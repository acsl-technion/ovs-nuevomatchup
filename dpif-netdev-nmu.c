#include <byteswap.h>
#include <config.h>
#include <float.h>
#ifdef HAVE_NUEVOMATCHUP
#include <libnuevomatchup.h>
#endif
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "bitmap.h"
#include "byte-order.h"
#include "cmap.h"
#include "dpif.h"
#include "dpif-netdev-nmu.h"
#include "dpif-netdev-perf.h"
#include "dpif-netdev-private.h"
#include "flow.h"
#include "hash.h"
#include "odp-util.h"
#include "openvswitch/dynamic-string.h"
#include "openvswitch/list.h"
#include "openvswitch/match.h"
#include "openvswitch/thread.h"
#include "openvswitch/vlog.h"
#include "openvswitch/util.h"
#include "ovs-atomic.h"
#include "ovs-thread.h"
#include "simd.h"
#include "smap.h"
#include "util.h"
#include "uuid.h"

#define NUMBER_OF_FIELDS 5

/* Ignore unused warnings in GCC when libnuevomatchup is missing */
#ifndef __always_unused 
#define __always_unused __attribute__((unused))
#endif

/* Interval between adjacent cmpflow synchronization with slow path (ms) */
#define CMPFLOW_SYNC_INTERVAL 10

/* Converts integers from network (big-endian) to host. */
#define NMUCLS_NTHBE_8(X) X
#define NMUCLS_NTHBE_16(X) ntohs(X)
#define NMUCLS_NTHBE_32(X) ntohl(X)

/* Bit masks */
#define NMUCLS_BITMASK_8  0xff
#define NMUCLS_BITMASK_16 0xffff
#define NMUCLS_BITMASK_32 0xffffffff

/* Helpers to measure time */
#define TIMESPAN_GET_NS(DEST, START, END)          \
        DEST=-(START.tv_sec * 1e9 + START.tv_nsec) \
             +(END.tv_sec * 1e9 + END.tv_nsec);

#define TIMESPAN_MEASURE(NAME) \
        clock_gettime(CLOCK_MONOTONIC, &NAME);


/* Extract 'FIELD' from 'FLOW_P' and 'WILDCARD_P', then populate 'FIELD'
 * of 'CMPFLOW' with values of 'SIZE' bits */
#define NMUCLS_EXTRACT_FIELD(FIELD, FLOW_P, WILDCARD_P, CMPFLOW, IDX, SIZE)  \
do {                                                                         \
    uint##SIZE##_t mask = NMUCLS_NTHBE_##SIZE(WILDCARD_P->masks.FIELD);      \
    CMPFLOW.fields[IDX].low = NMUCLS_NTHBE_##SIZE(FLOW_P->FIELD) & mask;     \
    CMPFLOW.fields[IDX].high = (CMPFLOW.fields[IDX].low | ~mask) &           \
                               NMUCLS_BITMASK_##SIZE;                        \
} while (0);

VLOG_DEFINE_THIS_MODULE(dpif_netdev_nmu);

struct nmu_trainer;
struct rule_info;

struct nmu_config {
    const char *cmpflow_bridge_name;
    int cool_down_time_ms;
    int garbage_collection_ms;
    int error_threshold;
    int max_collision;
    int minimal_coverage;
    int train_threshold;
    int max_retrain_sessions;
    int samples_per_session;
    bool nmu_enable;
    bool use_batching;
    bool use_cmpflows;
    bool instant_remainder;
};

struct nmu_statistics {
    double hit_count;
    double hitrate;
    double total_packets;
    double parsing_ns;
    double remainder_ns;
    double inference_ns;
    double search_ns;
    double validation_ns;
    double avg_matches;
    double num_validations;
};

struct nmucls {
    struct nmu_config *cfg;
    struct dp_netdev_pmd_thread *pmd;
    struct nmu_trainer *nmt;
    struct cmap cmpflow_table;
    long long int cmpflow_sync_time;
    long long int garbage_time;
    bool thread_running;
    pthread_t manager_thread;
    atomic_bool enabled;
};

/* Static method declaration */
#ifdef HAVE_NUEVOMATCHUP

typedef uint64_t
(*rule_match_func)(const struct iset_match* iset_match,
                   const struct netdev_flow_key *key);


enum rule_type {
    REMAINDER_TO_ISET = 0,
    ISET_TO_REMAINDER = 1,
    DELETED_RULES = 2
};

static inline struct nmucls *
nmucls_init__(struct nmu_config __always_unused *cfg,
              struct dp_netdev_pmd_thread __always_unused *pmd);

static inline void nmucls_run__(struct nmucls *nmucls);

static inline void
nmucls_insert__(struct nmucls *nmucls,
                const struct flow*,
                struct flow_wildcards*,
                struct dpcls_rule *rule,
                struct cmpflow_iterator *it);

static inline int
nmucls_remove__(struct nmucls *nmucls,
                struct dpcls *cls,
                struct dp_netdev_flow *flow);

static inline void
nmucls_print_rule_with_key__(const struct nmucls *nmucls,
                             const struct netdev_flow_key *key);

static void *nmucls_thread_main(void*);
static void nmu_move_rules_from_remainder(struct nmucls *nmucls);
static void nmu_move_rules_from_isets(struct nmucls *nmucls);
static inline enum lnmu_inclusion_policy
nmucls_set_inclusion_policy(struct nmucls *nmucls);

static void nmu_unref_deleted_flows(struct nmucls *nmucls);

static void nmu_insert(struct nmucls *,
                       const struct flow *,
                       const struct flow_wildcards *,
                       struct dpcls_rule *,
                       struct cmpflow *);

static void nmu_remove(struct nmucls *, struct dpcls_rule *rule);

static inline void nmu_lookup(struct nmucls *,
                              const struct netdev_flow_key **keys,
                              uint32_t *keys_map,
                              const int count,
                              struct dpcls_rule **results);

static void nmu_init(struct nmucls *);
static void nmu_destroy(struct nmucls *);

static inline void nmu_print_stats(struct ds *reply,
                                   struct nmucls *nmucls,
                                   const char *sep);
static void nmu_rule_get_status(struct nmucls *,
                                const struct dpcls_rule *rule,
                                bool *in_nmu,
                                bool *removed,
                                uint32_t *lib_id);
static inline void nmu_debug_print(const struct nmucls *nmucls,
                                   const struct netdev_flow_key *key,
                                   struct dpcls_rule **result);
static inline uint32_t min_uint(uint32_t a, uint32_t b);
static inline uint32_t max_uint(uint32_t a, uint32_t b);
static inline uint32_t max_int(int a, int b);

static int iset_entry_precedence_compare(const void *a, const void *b);
static inline struct iset_match_impl *
iset_match_get_impl(const struct iset_match *);
static inline struct iset_impl * iset_get_impl(const struct iset *iset);

static void ptr_list_destroy(struct ovs_list *lst);
static void ptr_list_push_ptr(struct ovs_list *lst, void *ptr);
static void free_list_destroy(struct ovs_list *lst);
static inline void rule_info_unlock(struct rule_info *info);
static inline void rule_info_lock(struct rule_info *info);
static struct rule_info* rule_info_lookup(const struct nmucls *,
                                          const struct dpcls_rule *);
static void rule_to_string(const struct nmucls *,
                           struct rule_info *,
                           struct ds *);

static void nmu_rule_lock(struct nmucls *, const struct dpcls_rule *);
static void nmu_rule_unlock(struct nmucls *, const struct dpcls_rule *);
static int nmu_classifier_remove(struct nuevomatchup *nuevomatch,
                                 const struct rule_info *rule_info);
static void nmu_preprocess_rules(struct nmucls *nmucls);
static void nmu_postprocess_rules(struct nmucls *nmucls);
static void nmu_garbage_collect_rules(struct nmucls *nmucls);
static void nmu_train(struct nmucls *nmucls, bool *new_classifier);
static void nmu_get_rules(struct nmucls *,
                          enum rule_type type,
                          struct dpcls_rule ***rules,
                          size_t* size);
static void nmu_switch(struct nmucls *nmucls);
static size_t nmu_wait(struct nmucls *nmucls);
static inline bool nmu_cmpflow_enabled(const struct nmucls *);

static inline void ALWAYS_INLINE
nmu_extract_flow_fields(const struct netdev_flow_key** keys,
                        const size_t count,
                        uint32_t *header_p);

static struct cmpflow_item* cmpflow_lookup(struct nmucls *, uint64_t id);

static void trainer_thread_ref(void);
static int trainer_thread_unref(void);
static bool trainer_thread_produce(struct nmucls *nmucls);

#endif

bool
nmucls_enabled(struct nmucls *nmucls)
{
    return nmucls && nmucls->enabled;
}

bool
nmu_config_enabled(struct nmu_config *cfg)
{
    return cfg && cfg->nmu_enable;
}

bool
nmucls_cmpflow_enabled(struct nmucls *nmucls)
{
#ifndef HAVE_NUEVOMATCHUP
    return false;
#else
    return nmucls_enabled(nmucls) && nmu_cmpflow_enabled(nmucls);
#endif
}

struct nmu_config *
nmu_config_init(void)
{
    struct nmu_config *cfg;
    cfg = xmalloc(sizeof(*cfg));
    memset(cfg, 0, sizeof(*cfg));
    return cfg;
}

void
nmu_config_destroy(struct nmu_config *cfg)
{
    free(cfg);
}

int
nmu_config_read(struct nmu_config *cfg,
                const struct smap *other_config)
{
    bool old_state;

    atomic_read_relaxed(&cfg->nmu_enable, &old_state);
    cfg->nmu_enable = smap_get_bool(other_config, "nmu-enable", false);

#ifndef HAVE_NUEVOMATCHUP
    cfg->nmu_enable = false;
#endif

    if (!cfg->nmu_enable) {
        VLOG_INFO("NuevoMatchUp is disabled");
        goto exit;
    }

    cfg->cool_down_time_ms =
        smap_get_int(other_config, "nmu-cool-down-time-ms", 2000);

    cfg->error_threshold =
        smap_get_int(other_config, "nmu-error-threshold", 128);

    cfg->max_collision = smap_get_int(other_config, "nmu-max-collision", 16);

    cfg->minimal_coverage = 
        smap_get_int(other_config, "num-minimal-coverage", 25);

    cfg->train_threshold = 
        smap_get_int(other_config, "nmu-train-threshold", 90);
    
    cfg->garbage_collection_ms =
        smap_get_int(other_config, "nmu-garbage-collection-ms", 0);

    cfg->max_retrain_sessions =
        (uint8_t)smap_get_int(other_config, "nmu-sessions", 6);

    cfg->samples_per_session =
        (uint16_t)smap_get_int(other_config, "nmu-samples", 4000);

    cfg->use_batching =
            smap_get_bool(other_config, "nmu-batching", false);

    cfg->use_cmpflows =
            smap_get_bool(other_config, "nmu-use-cmpflows", false);

    cfg->cmpflow_bridge_name =
            smap_get_def(other_config, "nmu-cmpflows-bridge-name", "");

    cfg->instant_remainder =
            smap_get_bool(other_config, "nmu-instant-remainder", false);

    VLOG_INFO("NuevoMatchUp is enabled with the following:"
              "max-collision: %d, error-threshold: %d, "
              "min-coverage: %d, cool-down: %d, "
              "train-threshold: %d, "
              "garbage-collection: %d, allow-cmpflows: %d, "
              "instant-remainder: %d, "
              "sessions: %d, samples: %d, batching: %d ",
              cfg->max_collision,
              cfg->error_threshold,
              cfg->minimal_coverage,
              cfg->cool_down_time_ms,
              cfg->train_threshold,
              cfg->garbage_collection_ms,
              cfg->use_cmpflows,
              cfg->instant_remainder,
              cfg->max_retrain_sessions,
              cfg->samples_per_session,
              cfg->use_batching);

exit:
    return old_state != cfg->nmu_enable;
}

struct nmucls *
nmucls_init(struct nmu_config __always_unused *cfg,
            struct dp_netdev_pmd_thread __always_unused *pmd)
{
#ifdef HAVE_NUEVOMATCHUP
    return nmucls_init__(cfg, pmd);
#else
    return NULL;
#endif
}

void
nmucls_run(struct nmucls *nmucls)
{
#ifdef HAVE_NUEVOMATCHUP
    nmucls_run__(nmucls);
#endif

}

void
nmucls_destroy(struct nmucls *nmucls)
{
    if (!nmucls_enabled(nmucls)) {
        return;
    }
#ifdef HAVE_NUEVOMATCHUP
    atomic_store(&nmucls->enabled, false);
    xpthread_join(nmucls->manager_thread, NULL);
    nmu_destroy(nmucls);
    cmap_destroy(&nmucls->cmpflow_table);
    free(nmucls->nmt);
    free(nmucls);
#endif
}

void
nmucls_insert(struct nmucls __always_unused *nmucls,
              const struct flow __always_unused *flow,
              struct flow_wildcards __always_unused *wc,
              struct dpcls_rule __always_unused *rule,
              struct cmpflow_iterator __always_unused *it)
{
#ifdef HAVE_NUEVOMATCHUP
    nmucls_insert__(nmucls, flow, wc, rule, it);
#endif
}

int
nmucls_remove(struct nmucls __always_unused *nmucls,
              struct dpcls __always_unused *cls,
              struct dp_netdev_flow __always_unused *flow)
{
#ifdef HAVE_NUEVOMATCHUP
    return nmucls_remove__(nmucls, cls, flow);
#else
    return 0;
#endif
}

void nmucls_lookup(struct nmucls __always_unused *nmucls,
                   const struct netdev_flow_key __always_unused **keys,
                   uint32_t __always_unused *keys_map,
                   size_t __always_unused cnt,
                   struct dpcls_rule __always_unused **rules)
{
#ifdef HAVE_NUEVOMATCHUP
    return nmu_lookup(nmucls, keys, keys_map, cnt, rules);
#endif
}

void
nmucls_print_rule_with_key(const struct nmucls __always_unused *nmucls,
                           const struct netdev_flow_key __always_unused *key)
{
#ifdef HAVE_NUEVOMATCHUP
    nmucls_print_rule_with_key__(nmucls, key);
#endif
}

void
nmucls_print_stats(struct ds *reply,
                   struct nmucls *nmucls,
                   const char *sep)
{
#ifdef HAVE_NUEVOMATCHUP
    nmu_print_stats(reply, nmucls, sep);
#endif
}

bool
nmucls_rule_is_cmpflow(const struct dpcls_rule *rule)
{
    return rule->is_cmpflow;
}

void
nmucls_rule_lock(struct nmucls __always_unused *nmucls,
                 struct dpcls_rule __always_unused *dpcls_rule)
{
#ifdef HAVE_NUEVOMATCHUP
    nmu_rule_lock(nmucls, dpcls_rule);
#endif
}

void
nmucls_rule_unlock(struct nmucls __always_unused *nmucls,
                   struct dpcls_rule __always_unused *dpcls_rule)
{
#ifdef HAVE_NUEVOMATCHUP
    nmu_rule_unlock(nmucls, dpcls_rule);
#endif

}


/* OVS - NuevoMatchUp internals */

#ifdef HAVE_NUEVOMATCHUP

/* Additional information for iSet rules */
struct iset_match_impl {
    void *match_func; /* Match function */
    void *mask;
    void *flow;
    uint64_t *mf_masks;
    const uint64_t *mf_values;
    int priority;
    uint8_t mf_bits_set_unit0;
    uint8_t mf_bits_set_unit1;
};

struct nmu_trainer {
    struct nmucls *nmucls;            /* Parent struct */
    struct nmu_statistics stats;      /* Statistics */

    struct cmap rule_map;             /* holds all rules */
    struct ovs_spin rule_map_lock;    /* guards "rule_map" */
    struct ovs_spin remainder_lock;   /* prevents concurrent reads/writes */

    struct timespec last_update;      /* for measuring time between updates */
    struct classifier_version *active;   /* active classifier */
    struct classifier_version *shadow;   /* classifier in training */
    struct classifier_version *obsolete; /* destroyed in the next switch */

    int shadow_valid;        /* whether "shadow->nm" is valid */
    int version;             /* version of active classifier */
    bool updates;            /* updates since last training */
    size_t size;             /* size of nuevomatchup in bytes */
    bool instant_remainder;  /* instant remainder insertions w/ cmpflows */

    size_t num_of_isets;

    double triaing_time_ns;         /* last training time */
    double cool_down_time_ms;       /* time between adjacent training */
    double train_threshold;         /* minimal coverage for training */

    size_t num_of_remainder_rules;  /* used for measuring coverage */
    size_t num_of_iset_rules;       /* used for measuring coverage */
    size_t num_of_rules_inserted;   /* used for measuring coverage */

    size_t num_of_iset_rules_out;   /* used for measuring coverage */
    size_t num_of_total_rules_out;  /* used for measuring coverage */

    struct ovs_list free_list;     /* rules in "rule_map" can be in either   */
    struct ovs_list rem_to_iset;   /* one of these lists, mutually exclusive */
    struct ovs_list iset_to_rem;   /* these lists are used to update dpcls   */
    struct ovs_list deleted_rules; /* and to perform garbage collection      */

    struct lnmu_trainer trainer;   /* libnuevomatchup trainer */
};

/* Additional private information of iSets */
struct iset_impl {
    struct cmap map;
    int valid;
    bool remainder_enabled;
};

/* Reflection on iSet rules */
struct iset_rule_info {
    struct cmap_node node;       /* Within "iset_impl"*/
    int entry_idx;               /* Fast access to iSet entry */
    void *rule_p;                /* Pointer to dpcls rule */
    struct rule_info *rule_info; /* General information on this rule */
    uint32_t hash;
};

/* Used for sorting iSet entry rules by priority */
struct iset_entry_precedence {
    int index;
    int priority;
};

/* Linked list of pointers */
struct ptr_list {
    struct ovs_list node;        /* Within "ovs_list" */
    void *ptr;
};

/* Used for fast switching between classifier versions using pointers */
struct classifier_version {
    struct nuevomatchup *nmu; /* NMU classifier */
    void *rem;                /* Remainder, using cmpflows */
};

/* Necessary information and status on rules and flows */
struct rule_info {
    struct cmap_node node;            /* Within nmu_trainer */

    struct ovs_spin lock;             /* Prevent multiple writers */
    struct cmpflow *cmpflow;          /* Pointer to cmpflow from netdev */
    void *rule_p;                     /* Pointer to unique dpcls rule */

    bool in_lib;                      /* Held by libnuevomatchup */
    bool removed;                     /* Removed by a revalidator */
    bool delete_me;                   /* Marked for garbage collection */
    enum lnmu_inclusion_policy flags; /* Allow in iSet/Remainder/Both */

    uint32_t hash;                    /* Hash for "rule_map" */
    uint32_t lib_unique_id;           /* Unique id in libnuevomatchup */
    int version;                      /* Classifier version */
    int subset_idx;                   /* iSet index or -1 for remainder */

    struct lnmu_flow flow;            /* 5-tuple flow for libnuevomatchup */
};

/* Items in nmucls->cmpflow_table */
struct cmpflow_item {
    struct cmap_node node;
    struct dp_netdev_flow *flow;
};

static struct vlog_rate_limit cmpflow_rl = VLOG_RATE_LIMIT_INIT(600, 600);

static inline uint32_t
min_uint(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

static inline uint32_t
max_uint(uint32_t a, uint32_t b)
{
    return a > b ? a : b;
}

static inline uint32_t
max_int(int a, int b)
{
    return a > b ? a : b;
}

static int
iset_entry_precedence_compare(const void *a, const void *b)
{
    struct iset_entry_precedence *first, *second;
    first = (struct iset_entry_precedence*)a;
    second = (struct iset_entry_precedence*)b;
    if (first->priority > second->priority) {
        return -1;
    } else if (first->priority < second->priority) {
        return 1;
    } else {
        return 0;
    }
}

static void
ptr_list_destroy(struct ovs_list *lst)
{
    struct ptr_list *item;
    LIST_FOR_EACH_POP(item, node, lst) {
        ovsrcu_postpone(free, item);
    }
    ovs_list_init(lst);
}

static void
ptr_list_push_ptr(struct ovs_list *lst, void *ptr)
{
    struct ptr_list *item;
    item = xmalloc(sizeof(struct ptr_list));
    item->ptr = ptr;
    ovs_list_push_back(lst, &item->node);
}

static void
free_list_destroy(struct ovs_list *lst)
{
    struct ptr_list *item;
    LIST_FOR_EACH_POP(item, node, lst) {
        ovsrcu_postpone(free, (char*)item->ptr);
    }
    ptr_list_destroy(lst);
}

static inline void
netdev_flow_key_flatten_unit(const uint64_t *pkt_blocks,
                             const uint64_t *tbl_blocks,
                             const uint64_t *mf_masks,
                             uint64_t *blocks_scratch,
                             const uint64_t pkt_mf_bits,
                             const uint32_t count)
{
    uint32_t i;

    for (i = 0; i < count; i++) {
        uint64_t mf_mask = mf_masks[i];
        /* Calculate the block index for the packet metadata. */
        uint64_t idx_bits = mf_mask & pkt_mf_bits;
        const uint32_t pkt_idx = count_1bits(idx_bits);

        /* Check if the packet has the subtable miniflow bit set. If yes, the
         * block at the above pkt_idx will be stored, otherwise it is masked
         * out to be zero.
         */
        uint64_t pkt_has_mf_bit = (mf_mask + 1) & pkt_mf_bits;
        uint64_t no_bit = ((!pkt_has_mf_bit) > 0) - 1;

        /* Mask packet block by table block, and mask to zero if packet
         * doesn't actually contain this block of metadata.
         */
        blocks_scratch[i] = pkt_blocks[pkt_idx] & tbl_blocks[i] & no_bit;
    }
}

/* This function takes a packet, and subtable and writes an array of uint64_t
 * blocks. The blocks contain the metadata that the subtable matches on, in
 * the same order as the subtable, allowing linear iteration over the blocks.
 *
 * To calculate the blocks contents, the netdev_flow_key_flatten_unit function
 * is called twice, once for each "unit" of the miniflow. This call can be
 * inlined by the compiler for performance.
 *
 * Note that the u0_count and u1_count variables can be compile-time constants,
 * allowing the loop in the inlined flatten_unit() function to be compile-time
 * unrolled, or possibly removed totally by unrolling by the loop iterations.
 * The compile time optimizations enabled by this design improves performance.
 */
static inline void
netdev_flow_key_flatten(const struct netdev_flow_key *key,
                        const struct netdev_flow_key *mask,
                        const uint64_t *mf_masks,
                        uint64_t *blocks_scratch,
                        const uint32_t u0_count,
                        const uint32_t u1_count)
{
    /* Load mask from subtable, mask with packet mf, popcount to get idx. */
    const uint64_t *pkt_blocks = miniflow_get_values(&key->mf);
    const uint64_t *tbl_blocks = miniflow_get_values(&mask->mf);

    /* Packet miniflow bits to be masked by pre-calculated mf_masks. */
    const uint64_t pkt_bits_u0 = key->mf.map.bits[0];
    const uint32_t pkt_bits_u0_pop = count_1bits(pkt_bits_u0);
    const uint64_t pkt_bits_u1 = key->mf.map.bits[1];

    /* Unit 0 flattening */
    netdev_flow_key_flatten_unit(&pkt_blocks[0],
                                 &tbl_blocks[0],
                                 &mf_masks[0],
                                 &blocks_scratch[0],
                                 pkt_bits_u0,
                                 u0_count);

    /* Unit 1 flattening:
     * Move the pointers forward in the arrays based on u0 offsets, NOTE:
     * 1) pkt blocks indexed by actual popcount of u0, which is NOT always
     *    the same as the amount of bits set in the subtable.
     * 2) mf_masks, tbl_block and blocks_scratch are all "flat" arrays, so
     *    the index is always u0_count.
     */
    netdev_flow_key_flatten_unit(&pkt_blocks[pkt_bits_u0_pop],
                                 &tbl_blocks[u0_count],
                                 &mf_masks[u0_count],
                                 &blocks_scratch[u0_count],
                                 pkt_bits_u1,
                                 u1_count);
}

static inline uint64_t ALWAYS_INLINE
rule_match_impl(const struct iset_match* iset_match,
                const struct netdev_flow_key *key,
                const uint32_t bit_count_u0,
                const uint32_t bit_count_u1)
{
    if (!iset_match->match) {
        return 0;
    }

    struct iset_match_impl *impl = iset_match_get_impl(iset_match);
    uint32_t bit_count_total = bit_count_u0 + bit_count_u1;
    struct netdev_flow_key *mask;
    uint64_t blocks_scratch[bit_count_total];
    uint64_t not_match;

    not_match = 0;
    mask = (struct netdev_flow_key*)impl->mask;

    netdev_flow_key_flatten(key,
                mask,
                impl->mf_masks,
                blocks_scratch,
                bit_count_u0,
                bit_count_u1);

    const uint64_t *keyp = impl->mf_values;
    const uint64_t *maskp = miniflow_get_values(&mask->mf);

    for (int i = 0; i < bit_count_total; i++) {
        not_match |= (blocks_scratch[i] & maskp[i]) != keyp[i];
    }

    /* Invert result to show match as 1. */
    return !not_match;
}

static uint64_t
rule_match_generic(const struct iset_match* iset_match,
                   const struct netdev_flow_key *key)
{
    struct iset_match_impl *impl = iset_match_get_impl(iset_match);
    return rule_match_impl(iset_match,
                           key,
                           impl->mf_bits_set_unit0,
                           impl->mf_bits_set_unit1);
}

/* Expand out specialized functions with U0 and U1 bit attributes. */
#define DECLARE_OPTIMIZED_LOOKUP_FUNCTION(U0, U1)                             \
    static uint64_t                                                           \
    rule_match__mf_u0w##U0##_u1w##U1(                                         \
                                         const struct iset_match *iset_match, \
                                         const struct netdev_flow_key *key)   \
    {                                                                         \
        return rule_match_impl(iset_match, key, U0, U1);                      \
    }                                                                         \

DECLARE_OPTIMIZED_LOOKUP_FUNCTION(5, 1);
DECLARE_OPTIMIZED_LOOKUP_FUNCTION(4, 1);
DECLARE_OPTIMIZED_LOOKUP_FUNCTION(4, 0);

/* Check if a specialized function is valid for the required subtable. */
#define CHECK_LOOKUP_FUNCTION(U0, U1)                                          \
    if (!f && u0_bits == U0 && u1_bits == U1) {                                \
        f = rule_match__mf_u0w##U0##_u1w##U1;                                  \
    }

static rule_match_func
rule_match_probe(uint8_t u0_bits, uint8_t u1_bits)
{
    rule_match_func f = NULL;
    CHECK_LOOKUP_FUNCTION(5, 1);
    CHECK_LOOKUP_FUNCTION(4, 1);
    CHECK_LOOKUP_FUNCTION(4, 0);

    if (!f) {
        f = rule_match_generic;
    }

    return f;
}

static bool
submodel_valid(const struct submodel *smodel)
{
    float *c;
    for (c=(float*)smodel; (char*)c < (char*)smodel+sizeof(smodel); ++c) {
        if (isnan(*c)) {
            return false;
        }
    }
    return true;
}

static inline float ALWAYS_INLINE
submodel_feed_forward(const struct submodel *smodel,
                      float input)
{
#ifdef NO_SIMD
    /* A simple implementation without intrinsics */
    float result = 0;
    for (int i=0; i<SUBMODEL_WIDTH; ++i) {
        result += max_float(0.0f, input*smodel->w1[i]+smodel->b1[i]) *
                  smodel->w2[i];
    }
    result += smodel->b2;
    return result;
#endif

    float output;
    PS_REG zeros = SIMD_ZEROS_PS;
    PS_REG result = SIMD_SET1_PS(input);

    PS_REG w1 = SIMD_LOADU_PS(smodel->w1);
    PS_REG b1 = SIMD_LOADU_PS(smodel->b1);
    PS_REG w2 = SIMD_LOADU_PS(smodel->w2);

    /* AVX and AVX512 are fine with that */
    SIMD_FMA_PS(result, w1, b1);
    SIMD_MAX_PS(result, result, zeros);
    result = SIMD_MUL_PS(result, w2);
    SIMD_REDUCE_SUM_PS(output, result);

    /* SSE Should perform more calculations */
#if (__SSE__) && (!__AVX__)
    PS_REG result_4 = SIMD_SET1_PS(input);
    PS_REG w1_4 = SIMD_LOADU_PS(smodel->w1+4);
    PS_REG b1_4 = SIMD_LOADU_PS(smodel->b1+4);
    PS_REG w2_4 = SIMD_LOADU_PS(smodel->w2+4);

    float output_4;
    SIMD_FMA_PS(result_4, w1_4, b1_4);
    SIMD_MAX_PS(result_4, result_4, zeros);
    result_4 = SIMD_MUL_PS(result_4, w2_4);
    SIMD_REDUCE_SUM_PS(output_4, result_4);
    output += output_4;
#endif

    output += smodel->b2;
    return output;
}

static bool
rqrmi_valid(const struct rqrmi *rqrmi)
{
    int num;

    num = 0;
    for (int i=0; i<rqrmi->num_of_stages; ++i) {
        num += rqrmi->widths[i];
    }

    for (int i=0; i<num; ++i) {
        if (!submodel_valid(&rqrmi->submodels[i])) {
            return false;
        }
    }

    return true;
}

static inline void ALWAYS_INLINE
rqrmi_inference(const struct rqrmi *rqrmi,
                const simd_vector_t *input,
                simd_vector_t *output,
                simd_vector_t *errors,
                const int batch_size)
{
    int base_idx;
    struct submodel *smodel;
    float current_input;
    int submodel_indices[batch_size];

    /* Set ones and zeros */
    PS_REG zeros = SIMD_ZEROS_PS;
    PS_REG ones = SIMD_SET1_PS(1-FLT_EPSILON);

    /* Intermediate results. Starts with 0 */
    for (int j=0; j<batch_size; ++j) {
        output->scalars[j] = 0;
    }

    base_idx = 0;

    /* Go over all stages */
    for (size_t i=0; i<rqrmi->num_of_stages; ++i) {
        /* For each input in batch */
        for (int j=0; j<batch_size; ++j) {
            /* Get the submodel */
            submodel_indices[j] = floor(rqrmi->widths[i] * output->scalars[j]);
            smodel = &rqrmi->submodels[base_idx + submodel_indices[j]];
            /* Perform input normalization */
            current_input = (input->scalars[j] - smodel->in_mean) /
                            smodel->in_stddev;
            /* Perform inference */
            output->scalars[j] = submodel_feed_forward(smodel, current_input);
        }
        /* Perform saturation over all outputs */
        PS_REG imm = SIMD_LOADU_PS(output->scalars);
        SIMD_MIN_PS(imm, imm, ones);
        SIMD_MAX_PS(imm, imm, zeros);
        SIMD_STORE_PS(output->scalars, imm);
        /* Update base index */
        base_idx += rqrmi->widths[i];
    }
    /* Set the errors */
    for (int i=0; i<batch_size; ++i) {
        errors->integers[i] = rqrmi->errors[submodel_indices[i]];
    }
}

static inline struct iset_match_impl *
iset_match_get_impl(const struct iset_match *iset_match)
{
    return CONST_CAST(struct iset_match_impl*, iset_match->data);
}

static inline struct iset_impl *
iset_get_impl(const struct iset *iset)
{
    return CONST_CAST(struct iset_impl*, iset->args);
}

static void
iset_match_invalidate(struct iset *iset, int entry_idx, int rule_idx)
{
    struct iset_match *iset_match;
    size_t db_entry, val_entry;
    uint32_t *val_matrix;
    int *entry_rules;
    int offset;

    db_entry = iset->row_size * entry_idx;
    val_entry = db_entry*2*NUMBER_OF_FIELDS;

    iset_match = &iset->match_db[db_entry];
    entry_rules = &iset->entry_rules[entry_idx];
    val_matrix = &iset->validation_db[val_entry];

    /* Switch the current rule with the last rule */
    (*entry_rules)--;

    /* Invalidate rule */
    iset_match[rule_idx].match = NULL;
    iset_match[rule_idx].args = NULL;
    free(iset_match[rule_idx].data);
    iset_match[rule_idx].data = NULL;

    for (int k=0; k<NUMBER_OF_FIELDS; ++k) {
        offset = k*iset->row_size;
        val_matrix[rule_idx+offset] = 0xFFFFFFFF;
        val_matrix[rule_idx+offset+iset->hi_values] = 0;
    }
}

static void
iset_entry_sort_by_priority(struct iset *iset, int entry_idx)
{
    struct iset_match_impl *iset_match_impl;
    struct iset_entry_precedence *elements;
    struct iset_match *iset_match, *iset_match_cpy;
    uint32_t *val_matrix, *val_matrix_cpy;
    size_t db_entry, val_entry;
    int idx, offset;

    const int mat_rows = iset->row_size;
    const int mat_cols = 2*NUMBER_OF_FIELDS;

    db_entry = mat_rows*entry_idx;
    val_entry = mat_rows*mat_cols*entry_idx;

    iset_match = &iset->match_db[db_entry];
    val_matrix = &iset->validation_db[val_entry];

    /* Copy the iset match and the validation matrix */
    iset_match_cpy = xmemdup(iset_match, sizeof(*iset_match)*mat_rows);
    val_matrix_cpy = xmemdup(val_matrix, sizeof(*val_matrix)*mat_rows*mat_cols);

    /* Sort rules by priority (largest to smallest) */
    elements = xmalloc(sizeof(*elements)*mat_rows);
    for (int i=0; i<iset->row_size; ++i) {
        iset_match_impl = iset_match_get_impl(&iset_match[i]);
        elements[i].index = i;
        elements[i].priority = iset_match_impl ?
                               iset_match_impl->priority :
                               -1;
    }
    qsort(elements, mat_rows, sizeof(*elements),
          iset_entry_precedence_compare);

    /* Re-order elements from the memory copy */
    for (int i=0; i<mat_rows; ++i) {
        idx = elements[i].index;
        iset_match[i] = iset_match_cpy[idx];
        for (int k=0; k<mat_cols; ++k) {
            offset = k*mat_rows;
            val_matrix[i+offset] = val_matrix_cpy[idx+offset];
        }
    }

    free(elements);
    free(iset_match_cpy);
    free(val_matrix_cpy);
}


static inline void
iset_init(struct iset *iset)
{
    struct iset_match_impl *iset_match_impl;
    struct iset_rule_info *iset_rule_info;
    struct rule_info *rule_info;
    struct iset_match *iset_match;
    struct dpcls_rule *rule_p;
    struct cmpflow *cmpflow;

    iset->args = xmalloc(sizeof(struct iset_impl));
    iset_get_impl(iset)->valid = 1;
    cmap_init(&iset_get_impl(iset)->map);

    for (size_t i=0; i<iset->num_of_entries; ++i) {
        for (int j=0; j<iset->entry_rules[i]; ++j) {
            iset_match = &iset->match_db[i*iset->row_size+j];
            iset_match->data = NULL;

            /* Skip NULL matches */
            if (!iset_match->match) {
                continue;
            }

            /* Lock on rule info */
            rule_info = (struct rule_info*)iset_match->args;
            rule_info_lock(rule_info);

            /* Check that the rule was not removed */
            if (rule_info->removed) {
                iset_match->match = NULL;
                free(iset_match->data);
                rule_info_unlock(rule_info);
                continue;
            }

            /* Assign iset match additional data */
            iset_match_impl = xmalloc(sizeof(struct iset_match_impl));
            iset_match->data = iset_match_impl;

            /* Set the iSet match */
            rule_p = (struct dpcls_rule*)iset_match->match;

            cmpflow = rule_info->cmpflow;
            iset_match_impl->mf_bits_set_unit0 = cmpflow->mf_bits_set_unit0;
            iset_match_impl->mf_bits_set_unit1 = cmpflow->mf_bits_set_unit1;
            iset_match_impl->flow = cmpflow->flow;
            iset_match_impl->mask = cmpflow->mask;
            iset_match_impl->mf_masks = cmpflow->mf_masks;
            iset_match_impl->mf_values = cmpflow->mf_values;
            iset_match_impl->priority = cmpflow->priority;
            iset_match_impl->match_func =
                    (void*)rule_match_probe(iset_match_impl->mf_bits_set_unit0,
                                            iset_match_impl->mf_bits_set_unit1);

            /* Set the iSet match map */
            iset_rule_info = xmalloc(sizeof *iset_rule_info);
            iset_rule_info->rule_p = rule_p;
            iset_rule_info->entry_idx = i;
            iset_rule_info->hash = hash_pointer(rule_info, 0);
            iset_rule_info->rule_info = rule_info;
            cmap_insert(&iset_get_impl(iset)->map, &iset_rule_info->node,
                        iset_rule_info->hash);

            rule_info_unlock(rule_info);
        }
    }

    /* Swap invalid matches */
    for (size_t entry_idx=0; entry_idx<iset->num_of_entries; ++entry_idx) {
        for (int rule_idx=0; rule_idx<iset->entry_rules[entry_idx];) {
            iset_match = &iset->match_db[entry_idx*iset->row_size+rule_idx];
            if (!iset_match->match) {
                iset_match_invalidate(iset, entry_idx, rule_idx);
            }
            if (iset_match->match) {
                ++rule_idx;
            }
        }
    }

    /* Sort entry rules by priority (largest to smallest) */
    for (size_t entry_idx=0; entry_idx<iset->num_of_entries; ++entry_idx) {
        iset_entry_sort_by_priority(iset, entry_idx);
    }
}

static inline void
iset_destroy(struct nmucls *nmucls, struct iset *iset)
{
    struct iset_rule_info *item;

    if (!iset || !iset_get_impl(iset)->valid) {
        return;
    }

    iset_get_impl(iset)->valid = 0;

    /* Remove the iSet's rule information */
    CMAP_FOR_EACH (item, node, &iset_get_impl(iset)->map) {
        if (item->entry_idx == -2) {
            continue;
        }
        item->entry_idx = -2;
        cmap_remove(&iset_get_impl(iset)->map, &item->node, item->hash);
        ptr_list_push_ptr(&nmucls->nmt->free_list, item);
    }

    cmap_destroy(&iset_get_impl(iset)->map);
    ptr_list_push_ptr(&nmucls->nmt->free_list, iset_get_impl(iset));
}

static inline void ALWAYS_INLINE
iset_secondary_search(struct iset *iset,
                      const simd_vector_t *rqrmi_inputs,
                      simd_vector_t *rqrmi_outputs,
                      simd_vector_t *rqrmi_errors,
                      simd_vector_t *results,
                      uint32_t *keys_map,
                      int packet_idx,
                      const int batch_size)
{
    uint32_t position[batch_size];
    uint32_t u_bound[batch_size];
    uint32_t l_bound[batch_size];
    bool low_bound_valid[batch_size];
    bool high_bound_valid[batch_size];
    uint32_t max_error;
    size_t size;

    size = iset->num_of_entries;
    max_error = 0;

    /* Initiate bounds for each input */
    for (int k=0; k<batch_size; ++k) {
        /* Skip resolved packets */
        if (!ULLONG_GET(*keys_map, packet_idx+k)) {
            continue;
        }
        position[k] = rqrmi_outputs->scalars[k] * size;
        u_bound[k] = min_uint(size-1, position[k]+rqrmi_errors->integers[k]);
        l_bound[k] = max_int(0, (int)position[k]-rqrmi_errors->integers[k]);
        max_error = max_uint(rqrmi_errors->integers[k], max_error);
    }

    /* Binary search, use memory parallelization over batch size */
    while (max_error > 0) {
        /* Fetch index database information from memory */
        /* Exploit memory access parallelism in value_db */
        for (int k=0; k<batch_size; ++k) {
            /* Skip resolved packets */
            if (!ULLONG_GET(*keys_map, packet_idx+k)) {
                continue;
            }
            low_bound_valid[k] = iset->value_db[position[k]] <=
                                 rqrmi_inputs->scalars[k];
            high_bound_valid[k] = iset->value_db[position[k]+1] >
                                  rqrmi_inputs->scalars[k];
        }
        /* Calculate the next position per packet in batch */
        for (int k=0; k<batch_size; ++k) {
            /* Skip resolved packets */
            if (!ULLONG_GET(*keys_map, packet_idx+k)) {
                continue;
            }
            else if (low_bound_valid[k] & high_bound_valid[k]) {
                /* Do nothing */
            } else if (low_bound_valid[k]) {
                l_bound[k] = position[k];
                position[k]=(l_bound[k]+u_bound[k]);
                position[k]=(position[k]>>1)+(position[k]&0x1); /* Ceil */
            } else {
                u_bound[k] = position[k];
                position[k]=(l_bound[k]+u_bound[k])>>1; /* Floor */
            }
        }

        /* Update error */
        max_error >>= 1;
    }

    for (int k=0; k<batch_size; ++k) {
        results->integers[k] = position[k];
    }
}

static inline int ALWAYS_INLINE
iset_entry_validation(const struct netdev_flow_key *key,
                      struct dpcls_rule **result,
                      int *current_priority,
                      int entry_index,
                      const uint32_t *header_p,
                      struct iset *iset)

{
    const struct iset_match *iset_match;
    const struct iset_match *current_match;
    const struct iset_match_impl *iset_match_impl;
    size_t db_entry, val_entry;
    const uint32_t *val_matrix;
    rule_match_func match_func;
    int validation_count;
    int collision_position;
    int entry_offset;
    const uint32_t *cursor_lo, *cursor_hi;
    uint32_t element;

    db_entry = iset->row_size * entry_index;
    val_entry = db_entry * (2 * NUMBER_OF_FIELDS);
    validation_count = 0;
    collision_position = -SIMD_WIDTH;

    /* Get pointers to all arrays */
    iset_match = &iset->match_db[db_entry];
    val_matrix = &iset->validation_db[val_entry];

    /* Get the first and only rule that matches the iSet field */
    for (int i=0; i<iset->iterations; ++i) {
        /* Update the base collision position */
        collision_position += SIMD_WIDTH;

        EPU_REG indices = SIMD_SET_EPI32(8,7,6,5,4,3,2,1);
        PS_REG indices_ps = SIMD_CASTSI_PS(indices);

        /* Calibrate base pointers to point to fields low & high values */
        cursor_lo = &val_matrix[collision_position];
        cursor_hi = &val_matrix[collision_position + iset->hi_values];

        /* For each field (row in validation matrix) */
        for (int f=0; f<NUMBER_OF_FIELDS; ++f) {
            EPU_REG header = SIMD_SET1_EPI(header_p[f]);
            EPU_REG vector_lo = SIMD_LOADU_SI(cursor_lo);
            EPU_REG vector_hi = SIMD_LOADU_SI(cursor_hi);
            /* Which one is higher? */
            EPU_REG result_lo, result_hi;
            SIMD_MAX_EPU32(result_lo, header, vector_lo);
            SIMD_MAX_EPU32(result_hi, header, vector_hi);
            /* Low bound we wish header >= vector_lo
             * High bound we wish vector_hi >= header */
            SIMD_CMPEQ_EPI32(result_lo, result_lo, header);
            SIMD_CMPEQ_EPI32(result_hi, result_hi, vector_hi);
            /* Result now hold scalars which are 0x00000000 (not good)
             * or 0xffffffff (good). We wish results that are valid
             * in both low and high */
            PS_REG result_lo_ps = SIMD_CASTSI_PS(result_lo);
            PS_REG result_hi_ps = SIMD_CASTSI_PS(result_hi);
            PS_REG result_both_ps = SIMD_AND_PS(result_lo_ps, result_hi_ps);
            /* Perform AND between the current results and the indices */
            indices_ps = SIMD_AND_PS(indices_ps, result_both_ps);
            /* Update to next field */
            cursor_lo += iset->row_size;
            cursor_hi += iset->row_size;
        }

        /* Go over all integers in "indices_ps" */
        indices = SIMD_CASTPS_SI(indices_ps);
        SIMD_FOREACH_EPU32(indices, element) {
            if (!element) {
                continue;
            }

            entry_offset = element + collision_position - 1;
            current_match = &iset_match[entry_offset];
            iset_match_impl = iset_match_get_impl(current_match);
            validation_count++;

            /* The rule was removed */
            if (!current_match->match) {
                continue;
            }

            /* We can't possibly find another match, as priorities descend */
            if (*current_priority > iset_match_impl->priority) {
                goto finish;
            }

            match_func = (rule_match_func)iset_match_impl->match_func;

            /* Validate that the rule matches */
            if (match_func(current_match, key)) {
                *result = current_match->match;
                *current_priority = iset_match_impl->priority;
                /* We stop on the first rule we encounter, either if
                 * they do not overlap (megaflows) or they do overlap with
                 * priorities (ordered by priority) */
                goto finish;
            }
        }
    }

finish:
    return validation_count;
}

static inline void ALWAYS_INLINE
iset_validation_phase(const struct netdev_flow_key **keys,
                      const int packet_idx,
                      struct iset *iset,
                      const uint32_t *header_p,
                      const simd_vector_t *search_results,
                      struct dpcls_rule **results,
                      int *priorities,
                      double *avg_matches_p,
                      double *num_validations_p,
                      uint32_t *keys_map,
                      const int batch_size)
{
    int entry_index;
    int validation_count;
    int base;

    base = 0;
    for (int k=0; k<batch_size; ++k) {
        /* Skip resolved/invalid packets */
        if (!ULLONG_GET(*keys_map, packet_idx+k)) {
            continue;
        }

        entry_index = search_results->integers[k];


        validation_count = iset_entry_validation(keys[packet_idx+k],
                                                 &results[packet_idx+k],
                                                 &priorities[packet_idx+k],
                                                 entry_index,
                                                 &header_p[base],
                                                 iset);
        /* Used for statistics */
        *num_validations_p += validation_count;
        base += NUMBER_OF_FIELDS;

        /* Updates statistics, mark packet as resolved */
        if (results[packet_idx+k]) {
            ULLONG_SET0(*keys_map, packet_idx+k);
            ++(*avg_matches_p);
        }
    }
}

static struct iset_rule_info*
iset_find_item(struct iset *iset,
               const struct rule_info *rule_info)
{
    struct iset_rule_info *item_node;
    uint32_t hash;

    /* Validate that the iSet contains the priority */
    hash = hash_pointer(rule_info, 0);
    CMAP_FOR_EACH_WITH_HASH(item_node, node, hash, &iset_get_impl(iset)->map)
    {
        if (item_node->rule_info == rule_info) {
            return item_node;
        }
    }
    return NULL;
}

static int
iset_invalidate(struct iset *iset,
                const struct rule_info *rule_info)
{
    struct iset_rule_info *iset_rule_info;
    struct iset_match *iset_match;
    int entry_idx;
    int *entry_rules;
    size_t db_entry;
    int result;

    iset_rule_info = iset_find_item(iset, rule_info);
    if (!iset_rule_info) {
        return 0;
    }

    result = 0;
    entry_idx = iset_rule_info->entry_idx;
    db_entry = iset->row_size * entry_idx;
    iset_match = &iset->match_db[db_entry];
    entry_rules = &iset->entry_rules[entry_idx];

    /* Go over all items, search for the current priority */
    for (size_t rule_idx =0; rule_idx<*entry_rules; ++rule_idx) {
        if (iset_match[rule_idx].match == rule_info->rule_p) {

            iset_match_invalidate(iset, entry_idx, rule_idx);

            /* Mark cmap node as invalid */
            iset_rule_info->rule_info = NULL;
            iset_rule_info->rule_p = NULL;
            iset_rule_info->entry_idx = -1;

            result = 1;
            break;
        }
    }

    return result;
}

static inline void ALWAYS_INLINE
iset_lookup_batch__(struct nmu_trainer *nmt,
                    const struct netdev_flow_key **keys,
                    uint32_t *keys_map,
                    const int packet_idx,
                    struct iset *iset,
                    struct rqrmi *rqrmi,
                    const uint32_t *header_p,
                    struct dpcls_rule **results,
                    int *priorities,
                    simd_vector_t *rqrmi_inputs,
                    simd_vector_t *rqrmi_outputs,
                    simd_vector_t *rqrmi_errors,
                    simd_vector_t *search_results,
                    const int batch_size)
{

    int base = 0;

    /* Set inputs */
    for (int i=0; i<batch_size; ++i) {
        rqrmi_inputs->scalars[i] = header_p[base + iset->field_idx];
        base += NUMBER_OF_FIELDS;
    }

    /* 3 Phases */
    PERF_START(inference_timer_ns);
    rqrmi_inference(rqrmi, rqrmi_inputs, rqrmi_outputs,
                    rqrmi_errors, batch_size);
    PERF_END(inference_timer_ns);

    PERF_START(search_timer_ns);
    iset_secondary_search(iset, rqrmi_inputs, rqrmi_outputs,
                          rqrmi_errors, search_results,
                          keys_map, packet_idx, batch_size);
    PERF_END(search_timer_ns);

    PERF_START(validation_timer_ns);
    iset_validation_phase(keys, packet_idx,
            iset, header_p, search_results, results,
            priorities,
            &nmt->stats.avg_matches,
            &nmt->stats.num_validations,
            keys_map,
            batch_size);

    PERF_END(validation_timer_ns);

    nmt->stats.inference_ns += inference_timer_ns;
    nmt->stats.search_ns += search_timer_ns;
    nmt->stats.validation_ns += validation_timer_ns;
}

static void
iset_lookup_batch(struct nmu_trainer *nmt,
                  const struct netdev_flow_key **keys,
                  uint32_t *keys_map,
                  const int packet_idx,
                  struct iset *iset,
                  struct rqrmi *rqrmi,
                  const uint32_t *header_p,
                  struct dpcls_rule **results,
                  int *priorities,
                  const int count)
{
    simd_vector_t rqrmi_inputs, rqrmi_outputs, rqrmi_errors, search_results;
    iset_lookup_batch__(nmt, keys, keys_map, packet_idx, iset,
            rqrmi, header_p, results, priorities, &rqrmi_inputs, &rqrmi_outputs,
            &rqrmi_errors, &search_results, count);
}

static void
iset_lookup_debug_print(const struct nmucls *nmucls,
                        const struct netdev_flow_key *key,
                        const uint32_t *header_p,
                        int iset_idx)

{
    simd_vector_t rqrmi_inputs, rqrmi_outputs, rqrmi_errors, search_results;
    struct dpcls_rule *result;
    int priority;
    int entry_index;
    float rqrmi_input;
    uint32_t entry_left, entry_right;
    bool search_found;
    bool bound_found;
    int validation_count;
    uint32_t keys_map;
    struct iset *iset;
    struct rqrmi *rqrmi;
    struct rule_info *info;
    struct ds str;

    iset = nmucls->nmt->active->nmu->iset[iset_idx];
    rqrmi = nmucls->nmt->active->nmu->rqrmi[iset_idx];

    result = NULL;
    priority = 0;
    keys_map = 1;

    iset_lookup_batch__(nmucls->nmt, &key, &keys_map, 0, iset,
            rqrmi, header_p, &result, &priority, &rqrmi_inputs, &rqrmi_outputs,
            &rqrmi_errors, &search_results, 1);

    entry_index = search_results.integers[0];
    rqrmi_input = rqrmi_inputs.scalars[0];
    entry_left = iset->value_db[entry_index];
    entry_right = (entry_index == iset->num_of_entries -1) ?
                   0xffffffff : iset->value_db[entry_index+1];
    search_found = (rqrmi_input >= entry_left) &&
                   (rqrmi_input <= entry_right);

    validation_count = iset_entry_validation(key,
                                             &result,
                                             &priority,
                                             entry_index,
                                             header_p,
                                             iset);

    bound_found = validation_count>0;

    VLOG_INFO("iSet %d lookup info: search: %u, "
              "bound-found: %d, match-found: %d, header-field: %u, "
              "version: %d",
              iset_idx, search_found, bound_found,
              (result != NULL), header_p[iset->field_idx],
              nmucls->nmt->active->nmu->version);

    ds_init(&str);

    if (result) {
        info = rule_info_lookup(nmucls, result);
        rule_to_string(nmucls, info, &str);
    } else {
        ds_put_format(&str,
                      "no rule. field-index: %d, "
                      "entry-start: %u, rqrmi-error: %d "
                      "entry-index: %d, entry-bounds: [%u,%u], ",
                      iset->field_idx,
                      (uint32_t)(rqrmi_outputs.scalars[0] *
                                 iset->num_of_entries),
                      rqrmi_errors.integers[0],
                      entry_index,
                      entry_left,
                      entry_right);
    }

    VLOG_INFO("Candidate info: %s", ds_cstr(&str));
    ds_destroy(&str);
}


static void
rule_map_lock(struct nmucls *nmucls)
{
    ovs_spin_lock(&nmucls->nmt->rule_map_lock);
}

static void
rule_map_unlock(struct nmucls *nmucls)
{
    ovs_spin_unlock(&nmucls->nmt->rule_map_lock);
}

static void
rule_map_insert(struct nmucls *nmucls, struct rule_info *info)
{
    rule_map_lock(nmucls);
    cmap_insert(&nmucls->nmt->rule_map, &info->node, info->hash);
    rule_map_unlock(nmucls);
}

static void
rule_map_remove(struct nmucls *nmucls, struct rule_info *info)
{
    rule_map_lock(nmucls);
    cmap_remove(&nmucls->nmt->rule_map, &info->node, info->hash);
    rule_map_unlock(nmucls);
}

static inline void
rule_info_lock(struct rule_info *info)
{
    ovs_spin_lock(&info->lock);
}

static inline void
rule_info_unlock(struct rule_info *info)
{
    ovs_spin_unlock(&info->lock);
}

static void
rule_info_remove(struct nmucls *nmucls, struct rule_info *info)
{
    struct nuevomatchup *active;
    struct nuevomatchup *shadow;
    int shadow_valid;
    struct nmu_trainer *nmt;

    nmt = nmucls->nmt;
    active = nmt->active->nmu;
    shadow = nmt->shadow->nmu;
    shadow_valid = nmt->shadow_valid;

    /* Rule should be locked */
    /* Mark as removed */
    info->removed = true;
    nmt->updates = true;
    /* Remove from both active and shadow classifiers, only if valid */
    nmu_classifier_remove(active, info);
    if ((shadow) && (shadow_valid) && (active != shadow)) {
        nmu_classifier_remove(shadow, info);
    }
}

static struct rule_info*
rule_info_lookup(const struct nmucls *nmucls, const struct dpcls_rule *rule)
{
    struct rule_info *info;
    uint32_t hash;
    struct nmu_trainer *nmt;

    nmt = nmucls->nmt;
    hash = hash_pointer(rule, 0);
    CMAP_FOR_EACH_WITH_HASH(info, node, hash, &nmt->rule_map) {
        if (info->rule_p != rule) {
            continue;
        }
        return info;
    }
    return NULL;
}


static int
remainder_classify(struct nmucls *nmucls,
                   const struct netdev_flow_key* key,
                   uint32_t *header,
                   int *priority,
                   struct dpcls_rule **result)
{
    struct iset_match_impl iset_match_impl;
    struct iset_match iset_match;
    struct rule_info *rule_info;
    struct lnmu_trainer *lib;
    struct cmpflow *cmpflow;
    void *rule_p;
    int error;

    rule_info = NULL;
    rule_p = *result;
    lib = &nmucls->nmt->trainer;
    ovs_spin_lock(&nmucls->nmt->remainder_lock);
    error = lnmu_remainder_classify(lib, nmucls->nmt->active->rem,
                                             header, priority,
                                             &rule_p, (void**)&rule_info);
    ovs_spin_unlock(&nmucls->nmt->remainder_lock);

    if (error) {
        VLOG_WARN("%s", lib->error);
    }
    if (rule_p == *result) {
        return 0;
    }

    cmpflow = rule_info->cmpflow;
    iset_match.match = rule_p;
    iset_match.data = (void*)&iset_match_impl;
    iset_match_impl.mask = cmpflow->mask;
    iset_match_impl.mf_masks = cmpflow->mf_masks;
    iset_match_impl.mf_values = cmpflow->mf_values;
    iset_match_impl.mf_bits_set_unit0 = cmpflow->mf_bits_set_unit0;
    iset_match_impl.mf_bits_set_unit1 = cmpflow->mf_bits_set_unit1;
    if (rule_match_generic(&iset_match, key)) {
        *result = rule_p;

        return 1;
    }
    return 0;
}




static bool
nmu_classifier_init(struct nuevomatchup *nuevomatch)
{
    if (!nuevomatch) {
        return true;
    }

    for (size_t i=0; i<nuevomatch->num_of_isets; ++i) {
        /* Initiate iSets */
        iset_init(nuevomatch->iset[i]);

        /* Make sure RQ-RMI is valid */
        if (!rqrmi_valid(nuevomatch->rqrmi[i])) {
            return false;
        }
    }
    return true;
}

static void
nmu_classifier_destroy(struct nmucls *nmucls,
                       struct nuevomatchup **nuevomatch)
{
    /* Must be called in RCU grace period */
    if (!nuevomatch || !*nuevomatch) {
        return;
    }

    for (size_t i=0; i<(*nuevomatch)->num_of_isets; ++i) {
        iset_destroy(nmucls, (*nuevomatch)->iset[i]);
    }
    /* The allocated memory is contiguous, so only 1 free required */
    ptr_list_push_ptr(&nmucls->nmt->free_list, *nuevomatch);
    nuevomatch = NULL;

    /* Clear free list */
    free_list_destroy(&nmucls->nmt->free_list);
}

static int
nmu_classifier_remove(struct nuevomatchup *nuevomatch,
                      const struct rule_info *rule_info)
{
    int result;
    if (!nuevomatch) {
        return 0;
    }
    result = 0;
    for (size_t i=0; i<nuevomatch->num_of_isets; ++i) {
        result |= iset_invalidate(nuevomatch->iset[i], rule_info);
    }
    return result;
}

static void
cmpflow_to_string(struct lnmu_flow *flow, struct ds *str)
{
    for (int i=0; i<5; ++i) {
        ds_put_format(str, "[%u,%u] ",
                      flow->fields[i].low,
                      flow->fields[i].high);
    }
}

static void
rule_to_string(const struct nmucls *nmucls,
               struct rule_info *info,
               struct ds *str)
{
    struct iset_rule_info *iset_rule_info;
    struct nuevomatchup *nuevomatch;
    struct iset_match *iset_match;
    struct cmap *iset_map;
    struct iset *iset;
    int subset_idx;
    int row_size;
    int entry_idx, entry_pos;
    struct nmu_trainer *nmt;
    uint32_t *match_column;
    uint32_t entry_left, entry_right;
    size_t db_entry, val_entry;
    int entry_rules;
    uint32_t *mat;

    if (!info) {
        ds_put_format(str, "Cannot find rule");
        return;
    }

    /* Should lock on rule */
    nmt = nmucls->nmt;
    nuevomatch = nmt->active->nmu;
    subset_idx = -1;

    subset_idx = lnmu_get_subset_idx(&nmt->trainer,
                                     info->lib_unique_id);

    ds_put_format(str,
                  "rule: %p, id: %u, version: %d, in-lib: %d, "
                  "removed: %d, cached-subset-idx: %d, "
                  "delete-me: %d, flags: %d, "
                  "lib-subset-idx: %d",
                  info->rule_p,
                  info->lib_unique_id,
                  info->version,
                  info->in_lib,
                  info->removed,
                  info->flags,
                  info->subset_idx,
                  info->delete_me,
                  subset_idx);

    if (subset_idx < 0) {
        return;
    }

    /* Get additional data from the iSet */
    iset = nuevomatch->iset[subset_idx];
    iset_map = &iset_get_impl(iset)->map;
    row_size = iset->row_size;

    CMAP_FOR_EACH(iset_rule_info, node, iset_map) {
        if (iset_rule_info->rule_p != info->rule_p) {
            continue;
        }

        entry_idx = iset_rule_info->entry_idx;
        entry_pos = -1;
        db_entry = iset->row_size * entry_idx;
        val_entry = db_entry * 2 * NUMBER_OF_FIELDS;
        mat = &iset->validation_db[val_entry];
        entry_left = iset->value_db[entry_idx];
        entry_rules = iset->entry_rules[entry_idx];
        entry_right = (entry_idx == iset->num_of_entries -1) ?
                0xffffffff : iset->value_db[entry_idx+1];

        for (int j=0; j<row_size; ++j) {
            iset_match = &iset->match_db[row_size*entry_idx+j];
            if (iset_match->match == info->rule_p) {
                match_column = &mat[j];
                entry_pos = j;
                break;
            }
        }

        ds_put_format(str,
                      ", version: %d, field-idx: %d, "
                      "entry-index: %d, entry-pos: %d, "
                      "entry-rules: %d, ",
                      nuevomatch->version, iset->field_idx,
                      entry_idx, entry_pos, entry_rules);

        if (entry_pos == -1) {
            ds_put_format(str, "entry-boundaries: [%u, %u]",
                          entry_left, entry_right);
        } else {
            ds_put_cstr(str, "match-boundaries: [");
            for (int f=0; f<NUMBER_OF_FIELDS*2; ++f) {
                ds_put_format(str, "%u%s",
                              match_column[f*iset->row_size],
                              (f < NUMBER_OF_FIELDS*2-1) ? ", " : "]");
            }
        }
        break;
    }
}

static inline double
nmu_get_coverage(struct nmucls *nmucls)
{
    struct nmu_trainer *nmt = nmucls->nmt;
    double iset_rules = nmt->num_of_iset_rules_out;
    double total_rules = nmt->num_of_total_rules_out;
    return total_rules == 0 ? 0 : 100.0 * iset_rules / total_rules;
}

static inline double
nmu_get_training_time_ns(struct nmucls *nmucls)
{
    struct nmu_trainer *nmt = nmucls->nmt;
    return nmt->triaing_time_ns;
}

static inline size_t
nmu_get_num_of_remainder_rules(struct nmucls *nmucls)
{
    struct nmu_trainer *nmt = nmucls->nmt;
    return nmt->num_of_remainder_rules +
           nmt->num_of_rules_inserted;
}

static inline size_t
nmu_get_num_of_iset_rules(const struct nmucls *nmucls)
{
    struct nmu_trainer *nmt = nmucls->nmt;
    return nmt->num_of_iset_rules;
}

static inline size_t
nmu_get_num_of_isets(const struct nmucls *nmucls)
{
    struct nmu_trainer *nmt = nmucls->nmt;
    return nmt->num_of_isets;
}

static inline size_t
nmu_get_size(const struct nmucls *nmucls)
{
    struct nmu_trainer *nmt = nmucls->nmt;
    return nmt->size;
}

static inline int
nmu_get_version(const struct nmucls *nmucls)
{
    struct nmu_trainer *nmt = nmucls->nmt;
    return nmt->version;
}

static inline bool
nmu_cmpflow_enabled(const struct nmucls *nmucls)
{
    return nmucls->cfg->use_cmpflows;
}

static inline double
nmu_get_hitrate(const struct nmucls *nmucls)
{
    return nmucls->nmt->stats.hitrate;
}

static inline void
nmucls_print_rule_with_key__(const struct nmucls *nmucls,
                             const struct netdev_flow_key *key)
{
    struct dpcls_rule *rule;
    
    rule = NULL;
    nmu_debug_print(nmucls, key, &rule);
    if (rule) {
        VLOG_INFO("Rule %p is marked in NuevoMatch: %d, version: %lu, "
                  "cmpflow: %d, dead: %d",
                  rule,
                  rule->in_nmu,
                  rule->nmu_version,
                  rule->is_cmpflow,
                  dp_netdev_flow_is_dead(dp_netdev_flow_cast(rule)));
    }
}

static inline void
nmu_debug_print(const struct nmucls *nmucls,
                const struct netdev_flow_key *key,
                struct dpcls_rule **result)
{
    struct dpcls_rule *rule;
    struct rule_info *info;
    struct iset_match iset_match;
    struct iset_match_impl iset_match_impl;
    uint32_t header_fields[NUMBER_OF_FIELDS];
    struct nuevomatchup *nuevomatch;
    struct nmu_trainer *nmt;
    struct ds str;
    bool found;

    found = false;
    ds_init(&str);
    nmt = nmucls->nmt;

    /* Perform manual lookup */
    CMAP_FOR_EACH(info, node, &nmt->rule_map) {

        rule_info_lock(info);

        if (info->delete_me || !info->rule_p) {
            rule_info_unlock(info);
            continue;
        }

        rule = (struct dpcls_rule *)info->rule_p;
        iset_match_impl.mf_bits_set_unit0 = rule->cmpflow->mf_bits_set_unit0;
        iset_match_impl.mf_bits_set_unit1 = rule->cmpflow->mf_bits_set_unit1;
        iset_match_impl.mf_masks = rule->cmpflow->mf_masks;
        iset_match_impl.mf_values = rule->cmpflow->mf_values;
        iset_match_impl.flow = &rule->flow;
        iset_match_impl.mask = rule->cmpflow->mask;
        iset_match.data = (void*)&iset_match_impl;

        if (info->removed || !rule_match_generic(&iset_match, key)) {
            rule_info_unlock(info);
            continue;
        }

        ds_clear(&str);
        rule_to_string(nmucls, info, &str);
        VLOG_INFO("Slow lookup result (version: %d): %s",
                  info->version,
                  ds_cstr(&str));
        /* Show the extracted field values for the current rule */
        ds_clear(&str);
        cmpflow_to_string(&info->flow, &str);
        VLOG_INFO("Extracted field values: %s", ds_cstr(&str));
        /* Set the result */
        if (result) {
            *result = rule;
        }
        found = true;
        break;
    }

    if (!found) {
        VLOG_INFO("Slow lookup has no results!");
    }

    VLOG_INFO("Fast lookup result:");
    nuevomatch = nmt->active->nmu;
    if (nuevomatch) {
        for (size_t i=0; i<nuevomatch->num_of_isets; ++i) {
            nmu_extract_flow_fields(&key, 1, header_fields);
            ds_clear(&str);
            ds_put_cstr(&str, "[");
            for (int f=0; f<NUMBER_OF_FIELDS; ++f) {
                ds_put_format(&str, "%u%s",
                              header_fields[f],
                              (f==NUMBER_OF_FIELDS-1) ? "]" : ", ");
            }
            VLOG_INFO("iSet %ld extracted fields: %s", i, ds_cstr(&str));
            iset_lookup_debug_print(nmucls, key, header_fields, i);
        }
    }

    if (found) {
        rule_info_unlock(info);
    }

    ds_destroy(&str);
}

static inline void ALWAYS_INLINE
nmu_extract_flow_fields(const struct netdev_flow_key** keys,
                        const size_t count,
                        uint32_t *header_p)
{
    const struct miniflow *mf;
    int base;

    for (int i=0; i<count; ++i) {
        mf = &keys[i]->mf;
        base = i * NUMBER_OF_FIELDS;
        header_p[base+0] = NMUCLS_NTHBE_8(MINIFLOW_GET_U8(mf, nw_proto));
        header_p[base+1] = NMUCLS_NTHBE_32(MINIFLOW_GET_U32(mf, nw_src));
        header_p[base+2] = NMUCLS_NTHBE_32(MINIFLOW_GET_U32(mf, nw_dst));
        header_p[base+3] = NMUCLS_NTHBE_32(miniflow_get_ports(mf))
                           >> 16 & NMUCLS_BITMASK_16;
        header_p[base+4] = NMUCLS_NTHBE_32(miniflow_get_ports(mf))
                           & NMUCLS_BITMASK_16;
    }
}

static inline void
nmu_lookup(struct nmucls *nmucls,
           const struct netdev_flow_key **keys,
           uint32_t *keys_map,
           const int count,
           struct dpcls_rule **results)
{
    uint32_t header_fields[count*NUMBER_OF_FIELDS];
    int priorities[count];
    struct nuevomatchup *nuevomatch;
    struct nmu_trainer *nmt;
    uint32_t found_map;
    uint32_t iset_key_map;
    uint32_t batch_size;
    size_t i;
    int res;

    if (!nmucls_enabled(nmucls)) {
        return;
    }
    
    nmt = nmucls->nmt;
    memset(priorities, 0xFF, sizeof(priorities)); /* Initiate all as -1 */

    /* Optimization for single packet */
    batch_size = (count == 1) ? 1 : SIMD_WIDTH;
    iset_key_map = *keys_map;
    found_map = iset_key_map;

    /* Extract header field for all MF */
    PERF_START(parsing_timer_ns);
    nmu_extract_flow_fields(keys, count, header_fields);
    PERF_END(parsing_timer_ns);
    nmt->stats.parsing_ns += parsing_timer_ns;

    nuevomatch = nmt->active->nmu;
    if (!nuevomatch) {
        goto after_isets;
    }

    for (i=0; i<nuevomatch->num_of_isets; ++i) {
        for (size_t p=0; p<count; p+=SIMD_WIDTH) {
            iset_lookup_batch(nmt,
                    keys,
                    &iset_key_map,
                    p,
                    nuevomatch->iset[i],
                    nuevomatch->rqrmi[i],
                    &header_fields[p*NUMBER_OF_FIELDS],
                    results,
                    priorities,
                    batch_size);
        }

        found_map &= iset_key_map;
        /* overlapping rules require iterating over all iSets (matches are not
         * enough) */
        if (nmu_cmpflow_enabled(nmucls)) {
            iset_key_map = *keys_map;
        }
    }

after_isets:

    /* overlapping also enables the remainder classifier */
    if (nmu_cmpflow_enabled(nmucls)) {
        PERF_START(remainder_time_ns);
        ULLONG_FOR_EACH_1(i, iset_key_map) {
            res = remainder_classify(nmucls,
                                     keys[i],
                                     &header_fields[i*NUMBER_OF_FIELDS],
                                     &priorities[i],
                                     &results[i]);
            if (res) {
                ULLONG_SET0(iset_key_map, i);
            }
        }
        found_map &= iset_key_map;
        PERF_END(remainder_time_ns);
        nmt->stats.remainder_ns += remainder_time_ns;
    }

    nmt->stats.hit_count += count_1bits(found_map  ^ *keys_map);
    nmt->stats.total_packets += count;
    *keys_map = found_map;
}

static void
nmu_init(struct nmucls *nmucls)
{
    struct lnmu_trainer_configuration libconfig;
    struct nmu_trainer *nmt;
    struct nmu_config *config;

    config = nmucls->cfg;
    nmt = nmucls->nmt;

    /* The order and types of "libconfig" members differ from
     * these of "config" */
    libconfig.max_collision = config->max_collision;
    libconfig.max_retraining_sessions = config->max_retrain_sessions;
    libconfig.error_threshold = config->error_threshold;
    libconfig.samples_per_session = config->samples_per_session;
    libconfig.minimal_coverage = config->minimal_coverage;
    libconfig.use_batching = config->use_batching;

    if (lnmu_init(&nmt->trainer, &libconfig)) {
        VLOG_ERR("%s", nmt->trainer.error);
    }

    cmap_init(&nmt->rule_map);

    nmt->cool_down_time_ms = config->cool_down_time_ms;
    nmt->train_threshold = config->train_threshold;

    nmt->shadow = xmalloc(sizeof(struct classifier_version));
    nmt->active = xmalloc(sizeof(struct classifier_version));
    nmt->obsolete = xmalloc(sizeof(struct classifier_version));
    memset(nmt->shadow, 0, sizeof(struct classifier_version));
    memset(nmt->active, 0, sizeof(struct classifier_version));
    memset(nmt->obsolete, 0, sizeof(struct classifier_version));

    nmt->updates = false;
    nmt->instant_remainder = config->instant_remainder;
    nmt->shadow_valid = 0;
    nmt->num_of_iset_rules = 0;
    nmt->num_of_remainder_rules = 0;
    nmt->version = 0;

    /* Just in case, for cmpflows */
    nmt->active->rem = lnmu_remainder_init(&nmt->trainer);
    nmt->shadow->rem = lnmu_remainder_init(&nmt->trainer);
    nmt->obsolete->rem = lnmu_remainder_init(&nmt->trainer);

    ovs_spin_init(&nmt->rule_map_lock);
    ovs_spin_init(&nmt->remainder_lock);

    /* Reset statistics */
    memset(&nmt->stats, 0, sizeof(nmt->stats));

    ovs_list_init(&nmt->rem_to_iset);
    ovs_list_init(&nmt->iset_to_rem);
    ovs_list_init(&nmt->deleted_rules);
    ovs_list_init(&nmt->free_list);

    TIMESPAN_MEASURE(nmt->last_update);
}

static void
nmu_destroy(struct nmucls *nmucls)
{
    struct rule_info *info;
    struct nmu_trainer *nmt;
    
    nmt = nmucls->nmt;
    lnmu_destroy(&nmt->trainer);

    CMAP_FOR_EACH(info, node, &nmt->rule_map) {
        cmap_remove(&nmt->rule_map, &info->node, info->hash);
        ptr_list_push_ptr(&nmt->free_list, info);
    }

    cmap_destroy(&nmt->rule_map);
    ovs_spin_destroy(&nmt->rule_map_lock);
    ovs_spin_destroy(&nmt->remainder_lock);

    nmu_classifier_destroy(nmucls, &nmt->obsolete->nmu);
    nmu_classifier_destroy(nmucls, &nmt->active->nmu);
    nmu_classifier_destroy(nmucls, &nmt->shadow->nmu);

    lnmu_remainder_destroy(nmt->obsolete->rem);
    lnmu_remainder_destroy(nmt->active->rem);
    lnmu_remainder_destroy(nmt->shadow->rem);

    free(nmt->obsolete);
    free(nmt->active);
    free(nmt->shadow);

    ptr_list_destroy(&nmt->rem_to_iset);
    ptr_list_destroy(&nmt->iset_to_rem);
    ptr_list_destroy(&nmt->deleted_rules);
    free_list_destroy(&nmt->free_list);
}

static void
nmu_insert(struct nmucls *nmucls,
           const struct flow *flow,
           const struct flow_wildcards *wc,
           struct dpcls_rule *rule,
           struct cmpflow *cmpflow)
{
    struct rule_info *info, *node;
    struct nmu_trainer *nmt;

    info = xmalloc(sizeof *info);
    info->removed = false;
    info->in_lib = false;
    info->delete_me = false;
    info->subset_idx = -1;
    info->hash = hash_pointer(rule, 0);
    info->rule_p = rule;
    info->flags = LNMU_INCLUSION_ALLOW_IN_ISET;
    info->version = 0;
    info->cmpflow = cmpflow;

    ovs_spin_init(&info->lock);

    /* Extract the 5-tuple from input, convert to NuevoMatch flow */
    NMUCLS_EXTRACT_FIELD(nw_proto, flow, wc, info->flow, 0,8);
    NMUCLS_EXTRACT_FIELD(nw_src, flow, wc, info->flow, 1 ,32);
    NMUCLS_EXTRACT_FIELD(nw_dst, flow, wc, info->flow, 2, 32);
    NMUCLS_EXTRACT_FIELD(tp_src, flow, wc, info->flow, 3, 16);
    NMUCLS_EXTRACT_FIELD(tp_dst, flow, wc, info->flow, 4, 16);
    info->flow.match = rule;
    info->flow.priority = cmpflow->priority;
    info->flow.args = info;

    /* Sanity check */
    nmt = nmucls->nmt;
    CMAP_FOR_EACH_WITH_HASH(node, node, info->hash, &nmt->rule_map) {
        ovs_assert(node->rule_p != info->rule_p);
    }

    nmt->num_of_rules_inserted++;
    nmt->num_of_total_rules_out++;
    nmt->updates = true;

    /* In case of cmpflows, insert the rule now to active remainder */
    if (nmt->instant_remainder && nmu_cmpflow_enabled(nmucls)) {
        ovs_spin_lock(&nmt->remainder_lock);
        int error = lnmu_insert(&nmt->trainer, &info->flow,
                                         nmt->active->rem,
                                         &info->lib_unique_id);
        ovs_spin_unlock(&nmt->remainder_lock);

        if (error) {
            VLOG_WARN("%s", nmt->trainer.error);
        }
        info->in_lib = true;
        info->flags = LNMU_INCLUSION_ALLOW_IN_ISET |
                      LNMU_INCLUSION_ALLOW_IN_REMAINDER;
        nmt->updates = true;
    }

    rule_map_insert(nmucls, info);
}

static void
nmu_remove(struct nmucls *nmucls, struct dpcls_rule *rule)
{
    struct rule_info *info;
    struct nmu_trainer *nmt;
    
    nmt = nmucls->nmt;
    info = rule_info_lookup(nmucls, rule);
    if (!info) {
        return;
    }

    /* In case of cmpflows, remove the rule now from active remainder */
    if (nmt->instant_remainder &&
        nmu_cmpflow_enabled(nmucls) &&
        (info->in_lib)) {
        lnmu_remainder_mark_delete(&nmt->trainer,
                                   nmt->active->rem,
                                   info->lib_unique_id);
    }

    rule_info_remove(nmucls, info);
}

static void
nmu_rule_lock(struct nmucls *nmucls, const struct dpcls_rule *rule)
{
    struct rule_info *info;

    if (!nmucls_enabled(nmucls) || !rule) {
        return;
    }

    info = rule_info_lookup(nmucls, rule);
    if (info) {
        rule_info_lock(info);
    }
}

static void
nmu_rule_unlock(struct nmucls *nmucls, const struct dpcls_rule *rule)
{
    struct rule_info *info;

    if (!nmucls_enabled(nmucls) || !rule) {
        return;
    }

    info = rule_info_lookup(nmucls, rule);
    if (info) {
        rule_info_unlock(info);
    }
}

static void
nmu_rule_get_status(struct nmucls *nmucls,
                    const struct dpcls_rule *rule,
                    bool *in_nmu,
                    bool *removed,
                    uint32_t *lib_id)
{
    struct rule_info *info;

    if (!nmucls_enabled(nmucls) || !rule) {
        return;
    }

    info = rule_info_lookup(nmucls, rule);
    if (info) {
        if (in_nmu) {
            *in_nmu = info->subset_idx >= 0;
        }
        if (removed) {
            *removed = info->removed;
        }
        if (lib_id) {
            *lib_id = info->lib_unique_id;
        }
    }
}

static void
nmu_preprocess_rules(struct nmucls *nmucls)
{
    struct lnmu_trainer *lnm;
    struct nmu_trainer *nmt;
    struct rule_info *info;
    int error;
    int version;

    nmt = nmucls->nmt;
    lnm = &nmt->trainer;
    version = ++nmt->version;
    info = NULL;

    /* The rule_map is iterated w/o locks, meaning nodes may be skipped
     * or iterated twice. We use the node's version to make sure not to
     * preprocess the same node twice */

    /* Update rules status */
    CMAP_FOR_EACH(info, node, &nmt->rule_map) {
        /* In case the rule was already processed by this version */
        if (info->version >= version){
            continue;
        }

        rule_info_lock(info);

        /* Mark the rule for the current version, set as unprocessed */
        info->version = version;

        /* In case the rule is waiting to be deleted */
        if (info->delete_me) {
            goto next_rule;
        }
        /* In case the rule is not active and was removed */
        else if ( (!info->in_lib) && (info->removed) ) {
            info->delete_me = true; /* Mark for deletion */
            ptr_list_push_ptr(&nmt->deleted_rules, info);
            goto next_rule;
        }
        /* In case the rule is in library and was removed, remove it */
        else if ( (info->in_lib) && (info->removed) ) {
            lnmu_remove(lnm, info->lib_unique_id);
            ovs_assert(!lnmu_contains(lnm, info->lib_unique_id));
            info->in_lib = false;
            nmt->updates = true;
            goto next_rule;
        }
        /* The rule was not removed */

        /* In case the rule is not in the library */
        if (!info->in_lib) {
            error = lnmu_insert(lnm, &info->flow, NULL,
                                         &info->lib_unique_id);
            if (error) {
                VLOG_WARN("%s", lnm->error);
            }
            info->in_lib = true;
            nmt->updates = true;
        }

        /* Update the rule's bypass mode */
        info->flags = nmucls_set_inclusion_policy(nmucls);

        lnmu_update_rule(lnm,
                         info->lib_unique_id,
                         info->flow.priority,
                         info->flags);
        next_rule:
        rule_info_unlock(info);
    }

    /* Reset update statistics */
    nmt->num_of_rules_inserted = 0;
}

static void
nmu_postprocess_rules(struct nmucls *nmucls)
{
    struct lnmu_trainer *lnm;
    struct nmu_trainer *nmt;
    struct rule_info *info;
    size_t iset_size;
    size_t remainder_size;
    size_t size;
    int subset_idx;
    void **rule_info;

    nmt = nmucls->nmt;
    lnm = &nmt->trainer;

    /* Should be called before switching between active and shadow versions.
     * Should be called regardless the state of the shadow classifier. */

    ptr_list_destroy(&nmt->rem_to_iset);
    ptr_list_destroy(&nmt->iset_to_rem);

    /* Go over all rules in library w/o accessing CMAP - no locks required */
    ovs_spin_lock(&nmt->remainder_lock);
    iset_size = lnmu_num_of_iset_rules(lnm);
    remainder_size = lnmu_num_of_remainder_rules(lnm);
    size = iset_size + remainder_size;
    rule_info = xmalloc(sizeof(void*) * size);
    memset(rule_info, 0, sizeof(void*) * size);
    lnmu_get_rule_args(lnm, (void**)rule_info,
                                iset_size, remainder_size);
    ovs_spin_unlock(&nmt->remainder_lock);

    for (size_t i=0; i<size; ++i) {
        info = rule_info[i];
        if (!info) {
            continue;
        }
        subset_idx = lnmu_get_subset_idx(lnm, info->lib_unique_id);

        /* Update the status of all rules (in iSet/in remainder) */
        if ((info->subset_idx < 0) && (subset_idx >= 0)) {
            ptr_list_push_ptr(&nmt->rem_to_iset, info);
        } else if ((info->subset_idx >= 0 ) && (subset_idx < 0)) {
            ptr_list_push_ptr(&nmt->iset_to_rem, info);
        }

        /* Remove stale rules */
        if (info->removed) {
            if (nmu_classifier_remove(nmt->shadow->nmu, info)) {
                nmt->num_of_iset_rules--;
            }
        }

        /* Update info */
        info->subset_idx = subset_idx;
    }

    free(rule_info);
}

static void
nmu_garbage_collect_rules(struct nmucls *nmucls)
{
    struct nmu_trainer *nmt;
    struct rule_info *info;
    struct ptr_list *list;
    long long int time_ms;

    time_ms = time_msec();
    if (time_ms - nmucls->garbage_time < nmucls->cfg->garbage_collection_ms) {
        return;
    }
    nmucls->garbage_time = time_ms;

    nmt = nmucls->nmt;
    nmu_unref_deleted_flows(nmucls);

    LIST_FOR_EACH(list, node, &nmt->deleted_rules) {
        info = (struct rule_info *)list->ptr;

        if (!info->delete_me) {
            continue;
        }

        rule_info_lock(info);

        /* Remove from rule map */
        rule_map_remove(nmucls, info);

        ptr_list_push_ptr(&nmt->free_list, info);

        /* Mark info as invalid */
        info->rule_p = NULL;
        info->version = -1;
        rule_info_unlock(info);
    }

    ptr_list_destroy(&nmt->deleted_rules);
    free_list_destroy(&nmt->free_list);
}

/* Returns number of nanosecs to wait until the next try */
static size_t
nmu_wait(struct nmucls *nmucls)
{
    struct lnmu_trainer *lnm;
    struct timespec start;
    double elapsed_ns;
    double cool_down_ns;

    lnm = &nmucls->nmt->trainer;
    cool_down_ns = nmucls->nmt->cool_down_time_ms * 1e6;

    /* In case there is no active classifier, train as fast as possible */
    if (!nmucls->nmt->active->nmu || nmucls->nmt->version < 2) {
        cool_down_ns = 0;
    }

    /* Make sure enough time has passed since last update */
    TIMESPAN_MEASURE(start);
    TIMESPAN_GET_NS(elapsed_ns, nmucls->nmt->last_update, start);
    if (cool_down_ns > elapsed_ns) {
        return cool_down_ns - elapsed_ns;
    }

    /* In case of updates - the coverage may not be correct */
    if (nmucls->nmt->updates) {
        return 0;
    }

    /* Make sure the coverage is lower than threshold */
    if (lnmu_coverage(lnm) > nmucls->nmt->train_threshold) {
        TIMESPAN_MEASURE(nmucls->nmt->last_update);
        return cool_down_ns + 1; /* Force penalty */
    }

    /* In case no updates since last training */
    if (!nmucls->nmt->updates) {
        TIMESPAN_MEASURE(nmucls->nmt->last_update);
        return cool_down_ns + 1; /* Force penalty */
    }
    return 0;
}

static void
nmu_train(struct nmucls *nmucls, bool *new_classifier)
{
    struct timespec start, end;
    struct lnmu_trainer *lnm;
    struct nmu_trainer *nmt;
    double elapsed_ns;
    int error;
    size_t bytes;

    nmt = nmucls->nmt;
    lnm = &nmt->trainer;
    *new_classifier = false;
    bytes = 0;
    error = 0;

    TIMESPAN_MEASURE(start);

    /* Train a new version for nuevomatch, update shadow version */
    error = lnmu_update(lnm, &bytes);
    if (error) {
        VLOG_WARN("%s", lnm->error);
        goto exit;
    }

    /* Current classifier is invalid */
    if (lnmu_iset_num(lnm) == 0) {
        TIMESPAN_MEASURE(nmt->last_update);
        goto exit;
    }

    nmt->shadow_valid = 0;
    nmt->shadow->nmu = xmalloc(bytes);
    nmt->shadow->nmu->version = nmt->version;

    error = lnmu_export(lnm, nmt->shadow->nmu);
    if (error) {
        VLOG_WARN("%s", lnm->error);
        free(nmt->shadow->nmu);
        nmt->shadow->nmu = NULL;
        goto exit;
    }

    if (!nmu_classifier_init(nmt->shadow->nmu)) {
        /* Error initializing classifier */
        if (lnmu_clear(lnm)) {
            VLOG_WARN("%s", lnm->error);
        }
        free(nmt->shadow->nmu);
        nmt->shadow->nmu = NULL;
        goto exit;
    }

    nmt->shadow_valid = 1;
    *new_classifier = true;

    exit:

    /* Train a partial remainder classifier in case of overlaps and a valid
     * of a valid nuevomatch classifier */
    if (nmucls->cfg->use_cmpflows) {
        lnmu_remainder_build(lnm, nmt->shadow->rem);
    }

    TIMESPAN_MEASURE(end);
    TIMESPAN_GET_NS(elapsed_ns, start, end);

    /* Set the number of rules in iSet and remainder */
    nmt->num_of_iset_rules =
            lnmu_num_of_iset_rules(lnm);
    nmt->num_of_remainder_rules =
            lnmu_num_of_remainder_rules(lnm);

    nmt->num_of_isets = lnmu_iset_num(lnm);
    nmt->triaing_time_ns = elapsed_ns;
    nmt->updates = false;
    nmt->size = bytes;

    TIMESPAN_MEASURE(nmt->last_update);

    if (error) { 
        VLOG_INFO("Error training NuevoMatch - skipping version");
    }
}

static void
nmu_switch(struct nmucls *nmucls)
{
    struct classifier_version *active;
    struct classifier_version *shadow;
    struct classifier_version *obsolete;
    struct nmu_trainer *nmt;

    nmt = nmucls->nmt;
    active = nmt->active;
    shadow = nmt->shadow;
    obsolete = nmt->obsolete;

    /* New version of classifier available */
    if (nmt->shadow->nmu) {
        shadow->nmu->version = nmt->version;
    }

    /* Destroy obsolete classifiers */
    nmu_classifier_destroy(nmucls, &obsolete->nmu);

    /* Switch between active and shadow version */
    nmt->obsolete = active;
    nmt->active = shadow;
    nmt->shadow = obsolete;

    /* For coverage */
    nmt->num_of_iset_rules_out = nmt->num_of_iset_rules;
    nmt->num_of_total_rules_out = nmt->num_of_remainder_rules +
                                   nmt->num_of_rules_inserted +
                                   nmt->num_of_iset_rules;

    /* Mark current shadow as invalid */
    nmt->shadow_valid = 0;
    nmt->shadow->nmu = NULL;
}

static void
nmu_get_rules(struct nmucls *nmucls,
              enum rule_type type,
              struct dpcls_rule ***rules,
              size_t* size)
{
    struct ovs_list *list;
    struct ptr_list *item;
    struct rule_info *info;
    size_t counter;

    if (type == REMAINDER_TO_ISET) {
        list = &nmucls->nmt->rem_to_iset;
    } else if (type == ISET_TO_REMAINDER) {
        list = &nmucls->nmt->iset_to_rem;
    } else if (type == DELETED_RULES) {
        list = &nmucls->nmt->deleted_rules;
    } else {
        *size = 0;
        return;
    }

    *size = ovs_list_size(list);
    *rules = (struct dpcls_rule**)xmalloc(*size * sizeof(void*));
    counter = 0;

    LIST_FOR_EACH(item, node, list) {
        info = (struct rule_info*)item->ptr;
        (*rules)[counter++] = info->rule_p;
    }
}



static inline void
nmucls_insert__(struct nmucls *nmucls,
                const struct flow *flow,
                struct flow_wildcards *wc,
                struct dpcls_rule *rule,
                struct cmpflow_iterator *it)
{
    struct dpcls_subtable *subtable;
    struct dp_netdev_flow *dp_flow;
    struct cmpflow *cmpflow;
    uint8_t unit0, unit1;

    rule->in_nmu = false;
    rule->nmu_version = 0;

    if (!nmucls_enabled(nmucls)) {
        rule->is_cmpflow = false;
        return;
    }

    /* Subtable should exist by now */
    subtable = CONTAINER_OF(rule->mask, struct dpcls_subtable, mask);

    /* NuevoMatch holds the rule - do not delete flow! */
    dp_flow = dp_netdev_flow_cast(rule);
    dp_netdev_flow_ref(dp_flow);

    cmpflow = xmalloc(sizeof(*cmpflow));

    if (!nmu_cmpflow_enabled(nmucls)) {
        /* Rule is Megaflow */
        rule->is_cmpflow = false;
        cmpflow->mask = &subtable->mask;
        cmpflow->mf_masks = subtable->mf_masks;
        cmpflow->mf_values = miniflow_get_values(&rule->flow.mf);
        cmpflow->flow = &rule->flow;
        cmpflow->mf_bits_set_unit0 = subtable->mf_bits_set_unit0;
        cmpflow->mf_bits_set_unit1 = subtable->mf_bits_set_unit1;
        cmpflow->priority = 0;
        cmpflow->unique_id = 0;
    } else {
        rule->is_cmpflow = true;
        wc = &it->mask;
        flow = &it->flow;
        /* Rule is Computational flow */
        cmpflow->mask = xmalloc(sizeof(struct netdev_flow_key));
        cmpflow->flow = xmalloc(sizeof(struct netdev_flow_key));
        netdev_flow_mask_init_explicit(cmpflow->mask, flow, wc);
        netdev_flow_key_init_masked(cmpflow->flow, flow, cmpflow->mask);
        unit0 = count_1bits(cmpflow->mask->mf.map.bits[0]);
        unit1 = count_1bits(cmpflow->mask->mf.map.bits[1]);
        cmpflow->mf_bits_set_unit0 = unit0;
        cmpflow->mf_bits_set_unit1 = unit1;
        cmpflow->mf_values = miniflow_get_values(&cmpflow->flow->mf);
        cmpflow->mf_masks = xmalloc(sizeof(uint64_t)*(unit0 + unit1));
        cmpflow->priority = it->priority;
        cmpflow->unique_id = it->unique_id;
        rule->mask = cmpflow->mask;
        netdev_flow_key_gen_masks(cmpflow->mask,
                                  cmpflow->mf_masks,
                                  unit0,
                                  unit1);
    }

    /* Initialize the rule with default values */
    rule->cmpflow = cmpflow;
    nmu_insert(nmucls, flow, wc, rule, cmpflow);
}

static inline int
nmucls_remove__(struct nmucls *nmucls,
                struct dpcls *cls,
                struct dp_netdev_flow *flow)
{
    struct dpcls_subtable *subtable;
    struct dpcls_rule *rule;
    int in_nmu;

    rule = dp_netdev_flow_get_rule(flow);
    in_nmu = 0;

    if (!nmucls_enabled(nmucls)) {
        ovs_assert(!rule->in_nmu);
        return 0;
    }

    /* Remove the rule from existing iSet and from being pending to be 
     * inserted to new ones */
    nmu_remove(nmucls, rule);
 
    if (OVS_LIKELY(rule->in_nmu)) {
        subtable = CONTAINER_OF(&(*rule->mask), struct dpcls_subtable, mask);
        dpcls_subtable_unref(cls, subtable);
        /* Change rule status */
        rule->in_nmu = false;
        in_nmu = 1;
    }
 
    ovs_assert(!rule->in_nmu);
    return in_nmu;
}

static inline void
nmu_print_stats(struct ds *reply, struct nmucls *nmucls, const char *sep)
{
   if (!nmucls_enabled(nmucls)) {
       return;
   }
   const struct nmu_statistics *nmu_stats = &nmucls->nmt->stats;

   double total_packets = nmu_stats->total_packets ?
                          nmu_stats->total_packets : 1;
   double validations = nmu_stats->num_validations ?
                        nmu_stats->num_validations : 1;
   double hit_rate = nmu_stats->hit_count / total_packets;
   double parsing_ns = nmu_stats->parsing_ns / total_packets;
   double remainder_ns = nmu_stats->remainder_ns / total_packets;
   double inference_ns = nmu_stats->inference_ns / total_packets;
   double search_ns = nmu_stats->search_ns / total_packets;
   double validation_ns = nmu_stats->validation_ns / total_packets;
   double avg_matches = nmu_stats->avg_matches / validations;

   ds_put_format(reply,
           "NuevoMatchUp avg. hit rate: %.03f%s"
           "NuevoMatchUp avg. parsing time: %.03f ns%s"
           "NuevoMatchUp avg. inference time: %.03f ns%s"
           "NuevoMatchUp avg. search time: %.03f ns%s"
           "NuevoMatchUp avg. validation time: %.03f ns%s"
           "NuevoMatchUp avg. remainder time: %.03f ns%s"
           "NuevoMatchUp avg. matches per entry: %.03f%s",
           hit_rate, sep, parsing_ns, sep, inference_ns, sep,
           search_ns, sep, validation_ns, sep,
           remainder_ns, sep, avg_matches, sep);
}

static struct cmpflow_item*
cmpflow_lookup(struct nmucls *nmucls, uint64_t id)
{
    struct cmpflow_item *item;
    struct cmpflow *cmpflow;
    int hash;

    hash = hash_uint64(id);
    CMAP_FOR_EACH_WITH_HASH(item, node, hash, &nmucls->cmpflow_table) {
        cmpflow = dp_netdev_flow_get_rule(item->flow)->cmpflow;
        if (cmpflow && cmpflow->unique_id == id) {
            return item;
        }
    }
    return NULL;
}

static void
cmpflow_clean(struct nmucls *nmucls, bool force)
{
    struct cmpflow_item *item;
    struct dpcls_rule *rule;
    struct cmpflow *cmpflow;
    long long int time_ms;
    int counter;
    int hash;

    if (!nmu_cmpflow_enabled(nmucls)) {
        return;
    }

    counter = 0;
    time_ms = 0;

    CMAP_FOR_EACH(item, node, &nmucls->cmpflow_table) {
        rule = dp_netdev_flow_get_rule(item->flow);
        cmpflow = rule->cmpflow;
        ovs_assert(rule->is_cmpflow);
        ovs_assert(cmpflow);
        if (force || rule->nmu_version < nmucls->nmt->version) {
            hash = hash_uint64(cmpflow->unique_id);
            cmap_remove(&nmucls->cmpflow_table, &item->node, hash);
            counter++;

            dp_netdev_pmd_lock(nmucls->pmd);
            if (!dp_netdev_flow_is_dead(item->flow)) {
                dp_netdev_pmd_remove_flow(nmucls->pmd, item->flow);
            }
            dp_netdev_pmd_unlock(nmucls->pmd);

            free(item);
        }

        /* RCU quiesce grace period */
        if (time_msec() - time_ms > 50) {
            xnanosleep(1);
            time_ms = time_msec();
        }

    }
    if (counter) {
        VLOG_INFO("Removed %d cmpflows", counter);
    }
}

static inline struct nmucls *
nmucls_init__(struct nmu_config __always_unused *cfg,
              struct dp_netdev_pmd_thread __always_unused *pmd)
{
    struct nmucls *nmucls;

    if (!nmu_config_enabled(cfg)) {
        return NULL;
    }

    /* Allocate memory, initate variables */
    nmucls = xmalloc(sizeof(*nmucls));
    memset(nmucls, 0, sizeof(*nmucls));

    nmucls->cfg = cfg;
    nmucls->pmd = pmd;
    nmucls->thread_running = false;

    atomic_init(&nmucls->enabled, true);
    cmap_init(&nmucls->cmpflow_table);

    nmucls->nmt = xmalloc(sizeof(struct nmu_trainer));
    nmu_init(nmucls);
    nmucls->nmt->nmucls = nmucls;
    nmucls_run__(nmucls);

    return nmucls;
}

static inline void
nmucls_run__(struct nmucls *nmucls)
{
    struct ds ds;

    if (!nmucls_enabled(nmucls) || nmucls->thread_running) {
        return;
    }

    /* Initiate trainer thread */
    ds_init(&ds);
    ds_put_format(&ds, "nmu-manager-pmd");
    nmucls->manager_thread = ovs_thread_create(ds_cstr(&ds),
                                               nmucls_thread_main,
                                               nmucls);
    VLOG_INFO("NuevoMatchUp manager thread for PMD created.");
    nmucls->thread_running = true;
    ds_destroy(&ds);
}

/* Returns 0 in case the rule is not in NuevoMatchUp */
static inline int
nmcls_remove__(struct nmucls *nmucls,
               struct dpcls *cls,
               struct dp_netdev_flow *flow)
{
    struct dpcls_rule *rule;
    struct dpcls_subtable *subtable;
    int in_nmu;

    /* Remove the rule from being pending to NuevoMatch and
     * from existing iSets */
    rule = dp_netdev_flow_get_rule(flow);

    nmu_remove(nmucls, rule);
    in_nmu = 0;

    if (OVS_LIKELY(rule->in_nmu)) {
        subtable = CONTAINER_OF(&(*rule->mask), struct dpcls_subtable, mask);
        dpcls_subtable_unref(cls, subtable);
        rule->in_nmu = false;
        in_nmu = 1;
    }

    ovs_assert(!rule->in_nmu);
    return in_nmu;
}

static void
nmu_move_rules_from_remainder(struct nmucls *nmucls)
{
    struct dpcls_subtable *subtable;
    struct dp_netdev_flow *flow;
    struct dpcls_rule **rules;
    struct dpcls *cls;
    size_t size;
    bool removed;

    nmu_get_rules(nmucls, REMAINDER_TO_ISET, &rules, &size);
    removed = false;

    /* Remove rules from the remainder that were not covered
     *  by the previous classifier, and now are. */
    for (int i=0; i<size; ++i) {
        /* Get all relevant objects */
        flow = dp_netdev_flow_cast(rules[i]);
        cls = dp_netdev_pmd_find_flow_dpcls(nmucls->pmd, flow);
        subtable = CONTAINER_OF(&(*rules[i]->mask),
                                struct dpcls_subtable,
                                mask);

        if (rules[i]->is_cmpflow) {
            continue;
        }

        /* Change the rule status, critical section */
        nmu_rule_lock(nmucls, rules[i]);

        ovs_assert(!rules[i]->in_nmu);

        nmu_rule_get_status(nmucls,
                            rules[i],
                            &rules[i]->in_nmu,
                            &removed,
                            NULL);

        ovs_assert(rules[i]->in_nmu || removed);

        /* Remove from remainder only if the rule is marked valid and in
         * NeuvoMatch */
        if (rules[i]->in_nmu && !removed) {
            /* NuevoMatch requires this subtable */
            dpcls_subtable_ref(subtable);
            /* Remove rule from remainder */
            dpcls_remove(cls, rules[i]);
        }

        rules[i]->nmu_version = nmu_get_version(nmucls);

        /* End of critical section */
        nmu_rule_unlock(nmucls, rules[i]);
    }

    free(rules);
}

static void
nmu_move_rules_from_isets(struct nmucls *nmucls)
{
    struct dp_netdev_flow *flow;
    struct dpcls_subtable *subtable;
    struct dpcls_rule **rules;
    struct dpcls *cls;
    size_t size;
    bool removed;

    nmu_get_rules(nmucls, ISET_TO_REMAINDER, &rules, &size);
    removed = false;

    /* Insert rules that were covered by a previous classifier and now
     * aren't to the remainder */
    for (int i=0; i<size; ++i) {
        /* Get all relevant objects */
        flow = dp_netdev_flow_cast(rules[i]);
        cls = dp_netdev_pmd_find_flow_dpcls(nmucls->pmd, flow);
        subtable = CONTAINER_OF(&(*rules[i]->mask),
                                struct dpcls_subtable,
                                mask);

        if (rules[i]->is_cmpflow) {
            continue;
        }

        /* Change the rule status, critical section */
        nmu_rule_lock(nmucls, rules[i]);

        ovs_assert(rules[i]->in_nmu || dp_netdev_flow_is_dead(flow));

        nmu_rule_get_status(nmucls,
                            rules[i],
                            &rules[i]->in_nmu,
                            &removed,
                            NULL);

        /* The rule must currently be in NuevoMatch or it has been removed */
        ovs_assert(!rules[i]->in_nmu || removed);

        /* Rule is valid and held by the remainder */
        if (!rules[i]->in_nmu && !removed) {
            /* Insert rule to remainder */
            dpcls_insert(cls, rules[i], &subtable->mask);
            /* NuevoMatch has 1 less reference to the subtable */
            dpcls_subtable_unref(cls, subtable);
        }

        rules[i]->nmu_version = nmu_get_version(nmucls);

        /* End of critical section */
        nmu_rule_unlock(nmucls, rules[i]);
    }
    free(rules);
}

static void
nmu_unref_deleted_flows(struct nmucls *nmucls)
{
    struct dp_netdev_flow *flow;
    struct dpcls_rule **rules;
    size_t size;

    /* Should be called after nmu_switch */

    /* Get all deleted rules */
    nmu_get_rules(nmucls, DELETED_RULES, &rules, &size);

    /* Insert rules that were covered by a previous classifier and now
     * aren't to the remainder */
    for (int i=0; i<size; ++i) {
        flow = dp_netdev_flow_cast(rules[i]);
        dp_netdev_flow_unref(flow);
    }
    free(rules);
}

static inline enum lnmu_inclusion_policy
nmucls_set_inclusion_policy(struct nmucls *nmucls)
{
    if (nmucls->cfg->use_cmpflows) {
        return LNMU_INCLUSION_ALLOW_IN_ISET |
               LNMU_INCLUSION_ALLOW_IN_REMAINDER;
    } else {
        return LNMU_INCLUSION_ALLOW_IN_ISET;
    }   
}

static inline void
nmu_update_stats(struct nmucls *nmucls)
{
    struct nmu_statistics *s;
    double p;
    s = &nmucls->nmt->stats;
    p = s->total_packets ? s->total_packets : 1;
    s->hitrate = s->hit_count / p;
}

static inline struct cmpflow_item*
cmpflow_insert(struct nmucls *nmucls,
               struct ofpbuf *add_actions,
               struct match *match,
               struct uuid *ufid,
               struct cmpflow_iterator *it)
{
    struct dp_netdev_flow *dp_flow;
    struct cmpflow_item *item;
    int hash;

    dp_netdev_pmd_lock(nmucls->pmd);
    dp_flow = dp_netdev_flow_add(nmucls->pmd,
                                 match,
                                 (ovs_u128*)ufid,
                                 add_actions->data,
                                 add_actions->size,
                                 it);
    dp_netdev_pmd_unlock(nmucls->pmd);

    /* Add a mapping from the cmpflow id to dp_flow */
    item = xmalloc(sizeof(*item));
    item->flow = dp_flow;
    hash = hash_uint64(it->unique_id);
    cmap_insert(&nmucls->cmpflow_table, &item->node, hash);

    if (OVS_LIKELY(VLOG_DROP_DBG((&cmpflow_rl)))) {
        return item;
    }

    /* Print flow to log */
    struct ds ds = DS_EMPTY_INITIALIZER;
    struct ofpbuf key_buf, mask_buf;
    struct odp_flow_key_parms odp_parms = {
        .flow = &it->flow,
        .mask = &it->mask.masks,
        .support = dp_netdev_support,
    };

    ofpbuf_init(&key_buf, 0);
    ofpbuf_init(&mask_buf, 0);

    odp_flow_key_from_flow(&odp_parms, &key_buf);
    odp_parms.key_buf = &key_buf;
    odp_flow_key_from_mask(&odp_parms, &mask_buf);

    ds_put_format(&ds,
                  "cmpflow-add (id: %lu prio: %d) ",
                  it->unique_id,
                  it->priority);

    odp_format_ufid((ovs_u128*)&ufid, &ds);
    ds_put_cstr(&ds, " ");
    odp_flow_format(key_buf.data, key_buf.size,
                    mask_buf.data, mask_buf.size,
                    NULL, &ds, false);
    ds_put_cstr(&ds, ", actions:");
    format_odp_actions(&ds, add_actions->data,
                       add_actions->size,
                       NULL);

    VLOG_INFO("%s", ds_cstr(&ds));

    ofpbuf_uninit(&key_buf);
    ofpbuf_uninit(&mask_buf);
    return item;
}

static void
cmpflow_sync(struct nmucls *nmucls, odp_port_t port)
{
    uint64_t actions_stub[64], slow_stub[64];
    struct ofpbuf actions, put_actions;
    struct ofpbuf *add_actions;
    struct cmpflow_iterator it;
    struct cmpflow_item *item;
    struct dp_packet packet;
    struct match match;
    struct uuid ufid;
    int counter_add;
    int error;
    long long int time_ms;

    if (!nmucls->cfg->use_cmpflows) {
        return;
    }

    time_ms = time_msec();
    if (time_ms - nmucls->cmpflow_sync_time < CMPFLOW_SYNC_INTERVAL) {
        return;
    }
    nmucls->cmpflow_sync_time = time_ms;

    time_ms = 0;
    ofpbuf_use_stub(&actions, actions_stub, sizeof actions_stub);
    ofpbuf_use_stub(&put_actions, slow_stub, sizeof slow_stub);

    it.iteration_state = CMPFLOW_ITERATE_START;
    counter_add = 0;

    do {

        /* Force RCU quiesce grace period once every 50 ms*/
        if (time_msec() - time_ms > 50) {
            xnanosleep(1);
            time_ms = time_msec();
        }

        memset(&match.flow, 0, sizeof match.flow);
        memset(&match.wc, 0, sizeof match.wc);
        dp_packet_init(&packet, 64); /* Dummy */

        match.flow.in_port.odp_port = port;
        match.wc.masks.in_port.odp_port = ODPP_NONE;
        match.tun_md.valid = false;

        /* Ugly hack for making xlate to process actions set-src-ip and
         * set-dst-ip, all for checking correctness. Can be skipped. */
        match.flow.dl_type = htons(ETH_TYPE_IP);
        match.flow.packet_type = htonl(PT_ETH);
        match.flow.nw_proto = 1;

        ofpbuf_clear(&actions);
        ofpbuf_clear(&put_actions);
        ufid = uuid_random();
        it.unique_id = 0;

        error = dp_netdev_upcall(nmucls->pmd, &packet,
                                 &match.flow, &match.wc,
                                 (ovs_u128*)&ufid,
                                 DPIF_UC_CMPFLOW_SYNC, NULL, &actions,
                                 &put_actions, &it);
        if (OVS_LIKELY(error == ENOSPC)) {
            error = 0;
        } else if (OVS_LIKELY(error == ENODEV)) {
            /* Invalid port */
            break;
        } else if (OVS_UNLIKELY(error)) {
            VLOG_WARN("Error in cmpflow upcall");
        }

        /* Check whether the compflow is already installed on this port */
        item = cmpflow_lookup(nmucls, it.unique_id);

        /* New cmpflow, install on current port */
        if (!item) {
            add_actions = put_actions.size ? &put_actions : &actions;
            item = cmpflow_insert(nmucls, add_actions, &match, &ufid, &it);
            counter_add++;
        }
        
        /* Update cmpflow version, force PMD stats to keep the flow */
        dp_netdev_flow_get_rule(item->flow)->nmu_version = 
            nmu_get_version(nmucls);
        dp_netdev_flow_used(item->flow, 64, 4096, 0,
                            dp_netdev_pmd_now(nmucls->pmd));

    } while (it.iteration_state);

    if (counter_add) {
        VLOG_INFO("Installed %d uqniue cmpflows", counter_add);
    }

    dp_packet_uninit(&packet);
    ofpbuf_uninit(&actions);
    ofpbuf_uninit(&put_actions);
    cmpflow_clean(nmucls, false);
}

static void*
nmucls_thread_main(void* args)
{
    struct nmucls *nmucls;
    bool result;
    bool last_result;
    size_t rules;
    size_t ns_to_wait, ns_penalty;
    odp_port_t port;
    int retval;

    const size_t penalty_factor_ns = 50000000;/* 50 msec */
    const size_t penalty_max_ns = 5000000000; /* 5 secs */

    nmucls = (struct nmucls*)args;

    ns_to_wait = 0;
    ns_penalty = penalty_factor_ns;
    result = false;
    last_result = true;

    retval = dp_netdev_pmd_get_port_by_name(nmucls->pmd,
                                            nmucls->cfg->cmpflow_bridge_name,
                                            &port);
    if (retval) {
        VLOG_WARN("Cannot get bridge '%s'. Disabling cmpflows.",
                  nmucls->cfg->cmpflow_bridge_name);
        nmucls->cfg->use_cmpflows = 0;
    }

    trainer_thread_ref();

    while(nmucls->enabled) {

        xnanosleep(ns_to_wait + ns_penalty);

        cmpflow_sync(nmucls, port);

        ns_to_wait = nmu_wait(nmucls);
        if (ns_to_wait) {
            continue;
        }

        nmu_preprocess_rules(nmucls);

        /* When training occurs here; does not scale to many manager threads */
        /* nmu_train(nmucls, &result); */
        /* Producer-consumer to a single trainer thread */
        result = trainer_thread_produce(nmucls);

        nmu_postprocess_rules(nmucls);
        nmu_move_rules_from_isets(nmucls);
        nmu_switch(nmucls);
        nmu_move_rules_from_remainder(nmucls);
        nmu_garbage_collect_rules(nmucls);
        nmu_update_stats(nmucls);

        /* Valid result */
        if (result || nmu_cmpflow_enabled(nmucls)) {
            /* Reset penalty */
            ns_penalty = penalty_factor_ns;

            /* Calculate and print stats */
            rules = nmu_get_num_of_remainder_rules(nmucls) +
                    nmu_get_num_of_iset_rules(nmucls);

            VLOG_INFO("NuevoMatchUp new classifier: "
                      "version: %d "
                      "rules: %lu "
                      "coverage: %.01f "
                      "iSets: %lu "
                      "size: %.03f KB "
                      "training: %.02f ms "
                      "hit-rate: %.02f ",
                      nmu_get_version(nmucls),
                      rules,
                      nmu_get_coverage(nmucls),
                      nmu_get_num_of_isets(nmucls),
                      nmu_get_size(nmucls) / 1024.0,
                      nmu_get_training_time_ns(nmucls) / 1e6,
                      nmu_get_hitrate(nmucls));
        }
        /* No valid result, try to increase penalty */
        else if (ns_penalty < penalty_max_ns) {
            ns_penalty += penalty_factor_ns;
        }

        if (last_result && !result) {
            VLOG_INFO("NuevoMatch cannot create a classifier within "
                      "coverage constraints.");
        }

        last_result = result;
        result = false;
    }

    cmpflow_clean(nmucls, true);
    trainer_thread_unref();

    VLOG_INFO("NuevoMatch PMD manager exit");
    return NULL;
}


/* Trainer thread */

#define TRAINER_SLEEP_NS 10000

enum { TRAINER_RING_SIZE = 32 };
enum trainer_status {
    TRAINER_STATUS_EMPTY = 0,
    TRAINER_STATUS_FULL,
    TRAINER_STATUS_READY
};

/* Trainer thread ring element */
OVS_ALIGNED_STRUCT(CACHE_LINE_SIZE, trainer_ring_element) {
    struct nmucls *nmucls;
    enum trainer_status status;
    bool result;
};

/* Trainer ring */
static struct trainer_ring_element trainer_ring[TRAINER_RING_SIZE];
static atomic_int trainer_cursor_write;
static struct ovs_mutex trainer_write_lock;

/* Trainer thread */
static atomic_int trainer_thread_client_num = 0;

static pthread_t trainer_thread;

/* Trainer thread main loop */
static void* trainer_thread_main(void *args __always_unused)
{
    struct trainer_ring_element *elem;
    struct trainer_ring_element new;
    int client_no;
    int read_cursor;

    VLOG_INFO("NMU trainer thread start");
    read_cursor = 0;

    while (1) {
        atomic_read_relaxed(&trainer_thread_client_num, &client_no);
        if (!client_no) {
            break;
        }

        xnanosleep(TRAINER_SLEEP_NS);

        elem = &trainer_ring[read_cursor];
        if (elem->status != TRAINER_STATUS_FULL) {
            continue;
        }

        VLOG_INFO("NMU trainer new element");

        new = *elem;
        nmu_train(new.nmucls, &new.result);
        new.status = TRAINER_STATUS_READY;

        /* Write to ring, single cache line */
        *elem = new;
        read_cursor = (read_cursor + 1) & (TRAINER_RING_SIZE-1);
    }

    VLOG_INFO("NMU trainer thread exit");
    return NULL;
}

/* Produce to ring, waits and returns "nmu_train" result */
static bool trainer_thread_produce(struct nmucls *nmucls)
{
    struct trainer_ring_element *elem;
    struct trainer_ring_element new;
    int write_cursor;

    /* Lock on ring, wait for current slot to be empty */
    ovs_mutex_lock(&trainer_write_lock);

    atomic_add_relaxed(&trainer_cursor_write, 1, &write_cursor);
    write_cursor &= (TRAINER_RING_SIZE-1);
    elem = &trainer_ring[write_cursor];

    while (elem->status != TRAINER_STATUS_EMPTY) {
        xnanosleep(TRAINER_RING_SIZE * TRAINER_SLEEP_NS);
    }
    
    /* Write to ring, unlock */
    new.nmucls = nmucls;
    new.status = TRAINER_STATUS_FULL;
    *elem = new;
    ovs_mutex_unlock(&trainer_write_lock);

    /* Block reader untill slot is ready */
    while (elem->status != TRAINER_STATUS_READY) {
        xnanosleep(TRAINER_RING_SIZE * TRAINER_SLEEP_NS);
    }

    /* Update element status to empty */
    elem->status = TRAINER_STATUS_EMPTY;

    return elem->result;
}

/* Start the trainer thread */
static void trainer_thread_ref(void)
{
    int old;

    /* Register as a client */
    atomic_add_relaxed(&trainer_thread_client_num, 1, &old);
    if (old) {
        return;
    }

    /* Initiate ring */
    memset(&trainer_ring, 0, sizeof(trainer_ring));
    atomic_store(&trainer_cursor_write, 0);
    ovs_mutex_init(&trainer_write_lock);

    /* Start trainer thread */
    trainer_thread = ovs_thread_create("nmu-trainer",
                                       trainer_thread_main,
                                       NULL);
}

/* Decrease number of clients */
static int trainer_thread_unref(void)
{
    int old;
    atomic_sub_relaxed(&trainer_thread_client_num, 1, &old);
    
    /* Join with trainer thread */
    if (old == 1) {
        xpthread_join(trainer_thread, NULL);
    }

    return old;
}

#endif
