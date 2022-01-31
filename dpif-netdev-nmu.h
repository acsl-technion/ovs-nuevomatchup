#ifndef DPIF_NETDEV_NMU
#define DPIF_NETDEV_NMU

#include <stdbool.h>

#include "dpif-netdev-private.h"
#include "flow.h"
#include "openvswitch/dynamic-string.h"
#include "util.h"

struct nmucls;
struct nmu_config;

struct nmu_config * nmu_config_init(void);
void nmu_config_destroy(struct nmu_config *);

/* Returns 1 iff nmu state has changed */
int nmu_config_read(struct nmu_config *cfg,
                    const struct smap *other_config);

bool nmu_config_enabled(struct nmu_config *cfg);

bool nmucls_enabled(struct nmucls *nmucls);

bool nmucls_cmpflow_enabled(struct nmucls *nmucls);

struct nmucls *nmucls_init(struct nmu_config *cfg,
                           struct dp_netdev_pmd_thread *pmd);

void nmucls_run(struct nmucls *nmucls);

void nmucls_destroy(struct nmucls *nmucls);

void nmucls_insert(struct nmucls *,
                   const struct flow*,
                   struct flow_wildcards*,
                   struct dpcls_rule*,
                   struct cmpflow_iterator *);
int nmucls_remove(struct nmucls *,
                  struct dpcls *,
                  struct dp_netdev_flow *);

void nmucls_lookup(struct nmucls *nmucls,
                   const struct netdev_flow_key **keys,
                   uint32_t *keys_map,
                   size_t cnt,
                   struct dpcls_rule **rules);

void nmucls_print_rule_with_key(const struct nmucls *,
                                const struct netdev_flow_key *key);

void nmucls_print_stats(struct ds *reply,
                        struct nmucls *nmucls,
                        const char *sep);
void nmucls_clear_stats(struct nmucls *nmucls);

void nmucls_rule_lock(struct nmucls *, struct dpcls_rule *);
void nmucls_rule_unlock(struct nmucls *, struct dpcls_rule *);

bool nmucls_rule_is_cmpflow(const struct dpcls_rule *);

#endif
