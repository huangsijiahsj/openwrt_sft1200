#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/netfilter.h>
#include <linux/rhashtable.h>
#include <linux/netdevice.h>
#include <net/ip.h>
#include <net/ip6_route.h>
#include <net/netfilter/nf_flow_table.h>
#include <net/netfilter/nf_conntrack.h>
#include <net/netfilter/nf_conntrack_core.h>
#include <net/netfilter/nf_conntrack_tuple.h>
#include <net/netfilter/nf_conntrack_acct.h>

struct flow_offload_entry {
	struct flow_offload	flow;
	struct nf_conn		*ct;
	struct rcu_head		rcu_head;
};

static DEFINE_MUTEX(flowtable_lock);
static LIST_HEAD(flowtables);

static void
flow_offload_fill_dir(struct flow_offload *flow, struct nf_conn *ct,
		      struct nf_flow_route *route,
		      enum flow_offload_tuple_dir dir)
{
	struct flow_offload_tuple *ft = &flow->tuplehash[dir].tuple;
	struct nf_conntrack_tuple *ctt = &ct->tuplehash[dir].tuple;
	struct dst_entry *other_dst = route->tuple[!dir].dst;
	struct dst_entry *dst = route->tuple[dir].dst;

	ft->dir = dir;

	switch (ctt->src.l3num) {
	case NFPROTO_IPV4:
		ft->src_v4 = ctt->src.u3.in;
		ft->dst_v4 = ctt->dst.u3.in;
		ft->mtu = ip_dst_mtu_maybe_forward(dst, true);
		break;
	case NFPROTO_IPV6:
		ft->src_v6 = ctt->src.u3.in6;
		ft->dst_v6 = ctt->dst.u3.in6;
		ft->mtu = ip6_dst_mtu_forward(dst);
		break;
	}

	ft->l3proto = ctt->src.l3num;
	ft->l4proto = ctt->dst.protonum;
	ft->src_port = ctt->src.u.tcp.port;
	ft->dst_port = ctt->dst.u.tcp.port;


	ft->iifidx = other_dst->dev->ifindex;
	ft->oifidx = dst->dev->ifindex;
	ft->dst_cache = dst;
}

struct flow_offload *
flow_offload_alloc(struct nf_conn *ct, struct nf_flow_route *route)
{
	struct flow_offload_entry *entry;
	struct flow_offload *flow;

	if (unlikely(nf_ct_is_dying(ct) ||
	    !atomic_inc_not_zero(&ct->ct_general.use)))
		return NULL;

	entry = kzalloc(sizeof(*entry), GFP_ATOMIC);
	if (!entry)
		goto err_ct_refcnt;

	flow = &entry->flow;

	if (!dst_hold_safe(route->tuple[FLOW_OFFLOAD_DIR_ORIGINAL].dst))
		goto err_dst_cache_original;

	if (!dst_hold_safe(route->tuple[FLOW_OFFLOAD_DIR_REPLY].dst))
		goto err_dst_cache_reply;

	entry->ct = ct;

	flow_offload_fill_dir(flow, ct, route, FLOW_OFFLOAD_DIR_ORIGINAL);
	flow_offload_fill_dir(flow, ct, route, FLOW_OFFLOAD_DIR_REPLY);

	WRITE_ONCE(flow->hnat_idx, -1);

	if (ct->status & IPS_SRC_NAT)
		__set_bit(NF_FLOW_SNAT, &flow->flags);
	if (ct->status & IPS_DST_NAT)
		__set_bit(NF_FLOW_DNAT, &flow->flags);

	return flow;

err_dst_cache_reply:
	dst_release(route->tuple[FLOW_OFFLOAD_DIR_ORIGINAL].dst);
err_dst_cache_original:
	kfree(entry);
err_ct_refcnt:
	nf_ct_put(ct);

	return NULL;
}
EXPORT_SYMBOL_GPL(flow_offload_alloc);

static void flow_offload_fixup_tcp(struct ip_ct_tcp *tcp)
{
	tcp->state = TCP_CONNTRACK_ESTABLISHED;
	tcp->seen[0].td_maxwin = 0;
	tcp->seen[1].td_maxwin = 0;
}

static void flow_offload_fixup_ct_timeout(struct nf_conn *ct)
{
	const struct nf_conntrack_l4proto *l4proto;
	struct net *net = nf_ct_net(ct);
	unsigned int *timeouts;
	unsigned int timeout;
	int l4num;

	l4num = nf_ct_protonum(ct);

	l4proto = __nf_ct_l4proto_find(nf_ct_l3num(ct), l4num);
	if (!l4proto)
		return;

	timeouts = l4proto->get_timeouts(net);
	if (!timeouts)
		return;

	if (l4num == IPPROTO_TCP)
		timeout = timeouts[ct->proto.tcp.state];
	else if (l4num == IPPROTO_UDP)
		timeout = timeouts[UDP_CT_REPLIED];
	else
		return;

	ct->timeout = nfct_time_stamp + timeout;
}

static void flow_offload_fixup_ct_state(struct nf_conn *ct)
{
	if (nf_ct_protonum(ct) == IPPROTO_TCP)
		flow_offload_fixup_tcp(&ct->proto.tcp);
}

void flow_offload_free(struct flow_offload *flow)
{
	struct flow_offload_entry *e;

	// printk("free flow 0x%p  flags 0x%x flags 0x%p \n", flow, flow->flags, &flow->flags);
	dst_release(flow->tuplehash[FLOW_OFFLOAD_DIR_ORIGINAL].tuple.dst_cache);
	dst_release(flow->tuplehash[FLOW_OFFLOAD_DIR_REPLY].tuple.dst_cache);
	e = container_of(flow, struct flow_offload_entry, flow);
	nf_ct_put(e->ct);
	kfree_rcu(e, rcu_head);
}
EXPORT_SYMBOL_GPL(flow_offload_free);

void nf_flow_table_acct(struct flow_offload *flow, struct sk_buff *skb, int dir)
{
	struct flow_offload_entry *entry;
	struct nf_conn_acct *acct;

	entry = container_of(flow, struct flow_offload_entry, flow);
	acct = nf_conn_acct_find(entry->ct);
	if (acct) {
		struct nf_conn_counter *counter = acct->counter;

		atomic64_inc(&counter[dir].packets);
		atomic64_add(skb->len, &counter[dir].bytes);
	}
}
EXPORT_SYMBOL_GPL(nf_flow_table_acct);

static u32 flow_offload_hash(const void *data, u32 len, u32 seed)
{
	const struct flow_offload_tuple *tuple = data;

	return jhash(tuple, offsetof(struct flow_offload_tuple, dir), seed);
}

static u32 flow_offload_hash_obj(const void *data, u32 len, u32 seed)
{
	const struct flow_offload_tuple_rhash *tuplehash = data;

	return jhash(&tuplehash->tuple, offsetof(struct flow_offload_tuple, dir), seed);
}

static int flow_offload_hash_cmp(struct rhashtable_compare_arg *arg,
					const void *ptr)
{
	const struct flow_offload_tuple *tuple = arg->key;
	const struct flow_offload_tuple_rhash *x = ptr;
	//RM#9848  get null ptr
	if(x != NULL){
		if (memcmp(&x->tuple, tuple, offsetof(struct flow_offload_tuple, dir)))
		  return 1;
	}

	return 0;
}

static const struct rhashtable_params nf_flow_offload_rhash_params = {
	.head_offset		= offsetof(struct flow_offload_tuple_rhash, node),
	.hashfn			= flow_offload_hash,
	.obj_hashfn		= flow_offload_hash_obj,
	.obj_cmpfn		= flow_offload_hash_cmp,
	.automatic_shrinking	= true,
};

#define	DAY	(86400 * HZ)

/* Set an arbitrary timeout large enough not to ever expire, this save
 * us a check for the IPS_OFFLOAD_BIT from the packet path via
 * nf_ct_is_expired().
 */
static void nf_ct_offload_timeout(struct flow_offload *flow)
{
	struct flow_offload_entry *entry;
	struct nf_conn *ct;

	entry = container_of(flow, struct flow_offload_entry, flow);
	ct = entry->ct;

	if (nf_ct_expires(ct) < DAY / 2)
		ct->timeout = nfct_time_stamp + DAY;
}

volatile bool nf_flow_need_all_aging;
EXPORT_SYMBOL(nf_flow_need_all_aging);

volatile bool nf_flow_need_udp_aging;
EXPORT_SYMBOL(nf_flow_need_udp_aging);

int flow_offload_add(struct nf_flowtable *flow_table, struct flow_offload *flow)
{
	int err;

	flow->timeout = (u32)jiffies + NF_FLOW_TIMEOUT;

	err = rhashtable_insert_fast(&flow_table->rhashtable,
				     &flow->tuplehash[FLOW_OFFLOAD_DIR_ORIGINAL].node,
				     nf_flow_offload_rhash_params);
	if (err < 0)
		return err;

	err = rhashtable_insert_fast(&flow_table->rhashtable,
				     &flow->tuplehash[FLOW_OFFLOAD_DIR_REPLY].node,
				     nf_flow_offload_rhash_params);
	if (err < 0) {
		rhashtable_remove_fast(&flow_table->rhashtable,
				       &flow->tuplehash[FLOW_OFFLOAD_DIR_ORIGINAL].node,
				       nf_flow_offload_rhash_params);
		return err;
	}

	nf_ct_offload_timeout(flow);

	atomic_inc(&(flow_table->nf_count.total_count));
	if(flow->tuplehash[FLOW_OFFLOAD_DIR_ORIGINAL].tuple.l4proto == IPPROTO_UDP)
	  atomic_inc(&(flow_table->nf_count.udp_count));
	else
	  atomic_inc(&(flow_table->nf_count.tcp_count));

	return 0;
}
EXPORT_SYMBOL_GPL(flow_offload_add);

static void flow_offload_del(struct nf_flowtable *flow_table,
			     struct flow_offload *flow)
{
	struct flow_offload_entry *e;

// XC ADD
#if  1
	atomic_dec(&(flow_table->nf_count.total_count));
	if(flow->tuplehash[FLOW_OFFLOAD_DIR_ORIGINAL].tuple.l4proto == IPPROTO_UDP)
	  atomic_dec(&( flow_table->nf_count.udp_count));
	else
	  atomic_dec(&(flow_table->nf_count.tcp_count));
#endif

	rhashtable_remove_fast(&flow_table->rhashtable,
			       &flow->tuplehash[FLOW_OFFLOAD_DIR_ORIGINAL].node,
			       nf_flow_offload_rhash_params);
	rhashtable_remove_fast(&flow_table->rhashtable,
			       &flow->tuplehash[FLOW_OFFLOAD_DIR_REPLY].node,
			       nf_flow_offload_rhash_params);

	e = container_of(flow, struct flow_offload_entry, flow);
	clear_bit(IPS_OFFLOAD_BIT, &e->ct->status);

	flow_offload_fixup_ct_timeout(e->ct);

	flow_offload_free(flow);
}

void flow_offload_teardown(struct flow_offload *flow)
{
	struct flow_offload_entry *e;

	set_bit(NF_FLOW_TEARDOWN, &flow->flags);

	e = container_of(flow, struct flow_offload_entry, flow);
	flow_offload_fixup_ct_state(e->ct);
}
EXPORT_SYMBOL_GPL(flow_offload_teardown);

struct flow_offload_tuple_rhash *
flow_offload_lookup(struct nf_flowtable *flow_table,
		    struct flow_offload_tuple *tuple)
{
	struct flow_offload_tuple_rhash *tuplehash;
	struct flow_offload_entry *e;
	struct flow_offload *flow;
	int dir;

	tuplehash = rhashtable_lookup_fast(&flow_table->rhashtable, tuple,
					   nf_flow_offload_rhash_params);
	if (!tuplehash)
		return NULL;

	dir = tuplehash->tuple.dir;
	flow = container_of(tuplehash, struct flow_offload, tuplehash[dir]);
	if (test_bit(NF_FLOW_TEARDOWN, &flow->flags))
		return NULL;

	e = container_of(flow, struct flow_offload_entry, flow);
	if (nf_ct_is_dying(e->ct))
		return NULL;

	return tuplehash;
}
EXPORT_SYMBOL_GPL(flow_offload_lookup);

int nf_flow_table_iterate(struct nf_flowtable *flow_table,
			  void (*iter)(struct flow_offload *flow, void *data),
			  void *data)
{
	struct flow_offload_tuple_rhash *tuplehash;
	struct rhashtable_iter hti;
	struct flow_offload *flow;
	int err;

	err = rhashtable_walk_init(&flow_table->rhashtable, &hti, GFP_KERNEL);
	if (err)
		return err;

	rhashtable_walk_start(&hti);

	while ((tuplehash = rhashtable_walk_next(&hti))) {
		if (IS_ERR(tuplehash)) {
			err = PTR_ERR(tuplehash);
			if (err != -EAGAIN)
				goto out;

			continue;
		}
		if (tuplehash->tuple.dir)
			continue;

		flow = container_of(tuplehash, struct flow_offload, tuplehash[0]);

		iter(flow, data);
	}
out:
	rhashtable_walk_stop(&hti);
	rhashtable_walk_exit(&hti);

	return err;
}
EXPORT_SYMBOL_GPL(nf_flow_table_iterate);

int nf_flow_table_iterate_ret(struct nf_flowtable *flow_table,
			  int (*iter)(struct flow_offload *flow, void *data),
			  void *data)
{
	struct flow_offload_tuple_rhash *tuplehash;
	struct rhashtable_iter hti;
	struct flow_offload *flow;
	int err;

	err = rhashtable_walk_init(&flow_table->rhashtable, &hti, GFP_KERNEL);
	if (err)
		return err;

	rhashtable_walk_start(&hti);

	while ((tuplehash = rhashtable_walk_next(&hti))) {
		if (IS_ERR(tuplehash)) {
			err = PTR_ERR(tuplehash);
			if (err != -EAGAIN)
				goto out;

			continue;
		}
		if (tuplehash->tuple.dir)
			continue;

		flow = container_of(tuplehash, struct flow_offload, tuplehash[0]);

		if(iter(flow, data) == 0)
		  goto out;
	}
out:
	rhashtable_walk_stop(&hti);
	rhashtable_walk_exit(&hti);

	return err;
}
EXPORT_SYMBOL_GPL(nf_flow_table_iterate_ret);

static inline bool nf_flow_has_expired(const struct flow_offload *flow)
{
	return (__s32)(flow->timeout - (u32)jiffies) <= 0;
}

static int nf_flow_offload_gc_step(struct nf_flowtable *flow_table)
{
	struct net *net = read_pnet(&flow_table->ft_net);
	bool need_all_aging = nf_flow_need_all_aging;
	bool need_udp_aging = nf_flow_need_udp_aging;
	struct flow_offload_tuple_rhash *tuplehash;
	struct flow_offload_entry *e;
	struct rhashtable_iter hti;
	struct flow_offload *flow;
	unsigned int cycle = jiffies % 2;
	int err;

	err = rhashtable_walk_init(&flow_table->rhashtable, &hti, GFP_KERNEL);
	if (err)
		return 0;

	rhashtable_walk_start(&hti);

	while ((tuplehash = rhashtable_walk_next(&hti))) {
		cycle++;
		if (IS_ERR(tuplehash)) {
			err = PTR_ERR(tuplehash);
			if (err != -EAGAIN)
			  goto out;

			continue;
		}
		if (tuplehash->tuple.dir)
		  continue;

		flow = container_of(tuplehash, struct flow_offload, tuplehash[0]);
		e = container_of(flow, struct flow_offload_entry, flow);

		if (nf_flow_has_expired(flow) || nf_ct_is_dying(e->ct))
			set_bit(NF_FLOW_TEARDOWN, &flow->flags);

		if (test_bit(NF_FLOW_TEARDOWN, &flow->flags)) {
			if (test_bit(NF_FLOW_HW, &flow->flags)) {
				if (test_bit(NF_FLOW_HW_PENDING, &flow->flags))
					continue;
				else if (!test_bit(NF_FLOW_HW_DYING, &flow->flags))
					nf_flow_offload_hw_del(net, flow, &flow_table->nf_count);
				else if (test_bit(NF_FLOW_HW_DEAD, &flow->flags))
					flow_offload_del(flow_table, flow);
			} else {
				flow_offload_del(flow_table, flow);
			}
		} else {
			nf_ct_offload_timeout(flow);

			if ((test_bit(NF_FLOW_HW, &flow->flags) &&
			    !test_bit(NF_FLOW_HW_DEAD, &flow->flags))){
				if (need_all_aging && (cycle%2)) {
						flow_offload_teardown(flow);
				}else if (need_udp_aging && (cycle%2)) {
					if (flow->tuplehash[FLOW_OFFLOAD_DIR_ORIGINAL].tuple.l4proto == IPPROTO_UDP) {
						flow_offload_teardown(flow);
					}
				}else {
						flow->timeout = (u32)jiffies + NF_FLOW_TIMEOUT;
				}
			}
		}
	}

out:
	rhashtable_walk_stop(&hti);
	rhashtable_walk_exit(&hti);

#if 1
	if(need_all_aging){
		atomic_inc(&(flow_table->nf_count.full_age_count));
		// printk("del %d cycle %d  age count %d\n",do_del, cycle,  age_count);
	}
	if(need_udp_aging){
		atomic_inc(&(flow_table->nf_count.udp_age_count));
		// printk("cycle %d   age count %d\n",do_del,cycle, age_count);
	}
#endif
	return 1;
}

void nf_flow_offload_work_gc(struct work_struct *work)
{
	struct nf_flowtable *flow_table;

	flow_table = container_of(work, struct nf_flowtable, gc_work.work);
	nf_flow_offload_gc_step(flow_table);
	queue_delayed_work(system_power_efficient_wq, &flow_table->gc_work, HZ);
}
EXPORT_SYMBOL(nf_flow_offload_work_gc);

static int nf_flow_nat_port_tcp(struct sk_buff *skb, unsigned int thoff,
			__be16 port, __be16 new_port)
{
	struct tcphdr *tcph;

	if (!pskb_may_pull(skb, thoff + sizeof(*tcph)) ||
				skb_try_make_writable(skb, thoff + sizeof(*tcph)))
	  return -1;

	tcph = (void *)(skb_network_header(skb) + thoff);
	inet_proto_csum_replace2(&tcph->check, skb, port, new_port, true);

	return 0;
}

static int nf_flow_nat_port_udp(struct sk_buff *skb, unsigned int thoff,
			__be16 port, __be16 new_port)
{
	struct udphdr *udph;

	if (!pskb_may_pull(skb, thoff + sizeof(*udph)) ||
				skb_try_make_writable(skb, thoff + sizeof(*udph)))
	  return -1;

	udph = (void *)(skb_network_header(skb) + thoff);
	if (udph->check || skb->ip_summed == CHECKSUM_PARTIAL) {
		inet_proto_csum_replace2(&udph->check, skb, port,
					new_port, true);
		if (!udph->check)
		  udph->check = CSUM_MANGLED_0;
	}

	return 0;
}

static int nf_flow_nat_port(struct sk_buff *skb, unsigned int thoff,
			u8 protocol, __be16 port, __be16 new_port)
{
	switch (protocol) {
		case IPPROTO_TCP:
			if (nf_flow_nat_port_tcp(skb, thoff, port, new_port) < 0)
			  return NF_DROP;
			break;
		case IPPROTO_UDP:
			if (nf_flow_nat_port_udp(skb, thoff, port, new_port) < 0)
			  return NF_DROP;
			break;
	}

	return 0;
}

int nf_flow_snat_port(const struct flow_offload *flow,
		      struct sk_buff *skb, unsigned int thoff,
		      u8 protocol, enum flow_offload_tuple_dir dir)
{
	struct flow_ports *hdr;
	__be16 port, new_port;

	if (!pskb_may_pull(skb, thoff + sizeof(*hdr)) ||
	    skb_try_make_writable(skb, thoff + sizeof(*hdr)))
		return -1;

	hdr = (void *)(skb_network_header(skb) + thoff);

	switch (dir) {
	case FLOW_OFFLOAD_DIR_ORIGINAL:
		port = hdr->source;
		new_port = flow->tuplehash[FLOW_OFFLOAD_DIR_REPLY].tuple.dst_port;
		hdr->source = new_port;
		break;
	case FLOW_OFFLOAD_DIR_REPLY:
		port = hdr->dest;
		new_port = flow->tuplehash[FLOW_OFFLOAD_DIR_ORIGINAL].tuple.src_port;
		hdr->dest = new_port;
		break;
	default:
		return -1;
	}

	return nf_flow_nat_port(skb, thoff, protocol, port, new_port);
}
EXPORT_SYMBOL_GPL(nf_flow_snat_port);

int nf_flow_dnat_port(const struct flow_offload *flow,
		      struct sk_buff *skb, unsigned int thoff,
		      u8 protocol, enum flow_offload_tuple_dir dir)
{
	struct flow_ports *hdr;
	__be16 port, new_port;

	if (!pskb_may_pull(skb, thoff + sizeof(*hdr)) ||
	    skb_try_make_writable(skb, thoff + sizeof(*hdr)))
		return -1;

	hdr = (void *)(skb_network_header(skb) + thoff);

	switch (dir) {
	case FLOW_OFFLOAD_DIR_ORIGINAL:
		port = hdr->dest;
		new_port = flow->tuplehash[FLOW_OFFLOAD_DIR_REPLY].tuple.src_port;
		hdr->dest = new_port;
		break;
	case FLOW_OFFLOAD_DIR_REPLY:
		port = hdr->source;
		new_port = flow->tuplehash[FLOW_OFFLOAD_DIR_ORIGINAL].tuple.dst_port;
		hdr->source = new_port;
		break;
	default:
		return -1;
	}

	return nf_flow_nat_port(skb, thoff, protocol, port, new_port);
}
EXPORT_SYMBOL_GPL(nf_flow_dnat_port);

static const struct nf_flow_table_hw __rcu *nf_flow_table_hw_hook __read_mostly;

static int nf_flow_offload_hw_init(struct nf_flowtable *flow_table)
{
	const struct nf_flow_table_hw *offload;

	if (!rcu_access_pointer(nf_flow_table_hw_hook))
		request_module("nf-flow-table-hw");

	rcu_read_lock();
	offload = rcu_dereference(nf_flow_table_hw_hook);
	if (!offload)
		goto err_no_hw_offload;

	if (!try_module_get(offload->owner))
		goto err_no_hw_offload;

	rcu_read_unlock();

	return 0;

err_no_hw_offload:
	rcu_read_unlock();

	return -EOPNOTSUPP;
}

int nf_flow_table_init(struct nf_flowtable *flowtable)
{
	int err;

	if (flowtable->flags & NF_FLOWTABLE_F_HW) {
		err = nf_flow_offload_hw_init(flowtable);
		if (err)
			return err;
	}

	INIT_DEFERRABLE_WORK(&flowtable->gc_work, nf_flow_offload_work_gc);

	err = rhashtable_init(&flowtable->rhashtable,
			      &nf_flow_offload_rhash_params);
	if (err < 0)
		return err;

	queue_delayed_work(system_power_efficient_wq,
			   &flowtable->gc_work, HZ);

	mutex_lock(&flowtable_lock);
	list_add(&flowtable->list, &flowtables);
	mutex_unlock(&flowtable_lock);
// XC ADD:
	memset(&flowtable->nf_count, 0, sizeof(struct nf_flowtable_count));
	return 0;
}
EXPORT_SYMBOL_GPL(nf_flow_table_init);

static void nf_flow_table_do_cleanup(struct flow_offload *flow, void *data)
{
	struct net_device *dev = data;

	if (!dev) {
		flow_offload_teardown(flow);
		return;
	}

	if (flow->tuplehash[0].tuple.iifidx == dev->ifindex ||
	    flow->tuplehash[1].tuple.iifidx == dev->ifindex)
		flow_offload_teardown(flow);
}

static void nf_flow_table_iterate_cleanup(struct nf_flowtable *flowtable,
					  struct net_device *dev)
{
	nf_flow_table_iterate(flowtable, nf_flow_table_do_cleanup, dev);
	flush_delayed_work(&flowtable->gc_work);
	if (flowtable->flags & NF_FLOWTABLE_F_HW)
		flush_work(&nf_flow_offload_hw_work);
}

void nf_flow_table_cleanup(struct net *net, struct net_device *dev)
{
	struct nf_flowtable *flowtable;

	mutex_lock(&flowtable_lock);
	list_for_each_entry(flowtable, &flowtables, list)
		nf_flow_table_iterate_cleanup(flowtable, dev);
	mutex_unlock(&flowtable_lock);
}
EXPORT_SYMBOL_GPL(nf_flow_table_cleanup);

struct work_struct nf_flow_offload_hw_work;
EXPORT_SYMBOL_GPL(nf_flow_offload_hw_work);

/* Give the hardware workqueue the chance to remove entries from hardware.*/
static void nf_flow_offload_hw_free(struct nf_flowtable *flowtable)
{
	const struct nf_flow_table_hw *offload;

	flush_work(&nf_flow_offload_hw_work);

	rcu_read_lock();
	offload = rcu_dereference(nf_flow_table_hw_hook);
	if (!offload) {
		rcu_read_unlock();
		return;
	}
	module_put(offload->owner);
	rcu_read_unlock();
}

void nf_flow_table_free(struct nf_flowtable *flow_table)
{
	mutex_lock(&flowtable_lock);
	list_del(&flow_table->list);
	mutex_unlock(&flowtable_lock);
	cancel_delayed_work_sync(&flow_table->gc_work);
	nf_flow_table_iterate(flow_table, nf_flow_table_do_cleanup, NULL);
	WARN_ON(!nf_flow_offload_gc_step(flow_table));
	rhashtable_destroy(&flow_table->rhashtable);
	if (flow_table->flags & NF_FLOWTABLE_F_HW)
		nf_flow_offload_hw_free(flow_table);
}
EXPORT_SYMBOL_GPL(nf_flow_table_free);

/* Must be called from user context. */
void nf_flow_offload_hw_add(struct net *net, struct flow_offload *flow,
			    struct nf_conn *ct, struct nf_flowtable_count * nf_count)
{
	const struct nf_flow_table_hw *offload;

	rcu_read_lock();
	offload = rcu_dereference(nf_flow_table_hw_hook);
	if (offload) {
		set_bit(NF_FLOW_HW, &flow->flags);
		offload->add(net, flow, ct, nf_count);
	}
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(nf_flow_offload_hw_add);

/* Must be called from user context. */
void nf_flow_offload_hw_del(struct net *net, struct flow_offload *flow, struct nf_flowtable_count * nf_count)
{
	const struct nf_flow_table_hw *offload;

	rcu_read_lock();
	offload = rcu_dereference(nf_flow_table_hw_hook);
	if (offload)
		offload->del(net, flow, nf_count);
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(nf_flow_offload_hw_del);

int nf_flow_table_hw_register(const struct nf_flow_table_hw *offload)
{
	if (rcu_access_pointer(nf_flow_table_hw_hook))
		return -EBUSY;

	rcu_assign_pointer(nf_flow_table_hw_hook, offload);

	return 0;
}
EXPORT_SYMBOL_GPL(nf_flow_table_hw_register);

void nf_flow_table_hw_unregister(const struct nf_flow_table_hw *offload)
{
	WARN_ON(rcu_access_pointer(nf_flow_table_hw_hook) != offload);
	rcu_assign_pointer(nf_flow_table_hw_hook, NULL);

	synchronize_rcu();
}
EXPORT_SYMBOL_GPL(nf_flow_table_hw_unregister);

static int nf_flow_table_netdev_event(struct notifier_block *this,
				      unsigned long event, void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);

	if (event != NETDEV_DOWN)
		return NOTIFY_DONE;

	nf_flow_table_cleanup(dev_net(dev), dev);

	return NOTIFY_DONE;
}

static struct notifier_block flow_offload_netdev_notifier = {
	.notifier_call	= nf_flow_table_netdev_event,
};

static int __init nf_flow_table_module_init(void)
{
	return register_netdevice_notifier(&flow_offload_netdev_notifier);
}

static void __exit nf_flow_table_module_exit(void)
{
	unregister_netdevice_notifier(&flow_offload_netdev_notifier);
}

module_init(nf_flow_table_module_init);
module_exit(nf_flow_table_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Pablo Neira Ayuso <pablo@netfilter.org>");
