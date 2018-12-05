
#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/jiffies.h>
#include <linux/string.h>
#include <linux/in.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/prefetch.h>
#include <linux/vmalloc.h>
#include <net/netlink.h>
#include <net/pkt_sched.h>
#include <net/sock.h>
#include <net/tcp_states.h>
#include <net/tcp.h>

/*
 * Per flow structure, dynamically allocated
 */
struct gq_bucket {
	struct sk_buff	*head;		/* list of skbs for this flow : first skb */
	struct sk_buff  *tail;		/* last skb in the list */
	int		qlen;		/* number of packets in flow queue */
};

struct curvature_desc {
	unsigned long a;
	u64 c;
	u64 abcI;
	u64 wwI;
};

struct precalc_a_b {
	u64 a;
	u64 b;
};

struct gradient_queue {
	u64                    head_ts;
	u64                    grnlrty;
	u64                    num_of_elements;
	u64                    num_of_buckets;
	u64                    side;
	unsigned long                    h, w, l, s;
	u64                    main_ts, buffer_ts, max_ts, horizon;
	struct gq_bucket       *main_buckets;
	struct gq_bucket       *buffer_buckets;
	struct curvature_desc  *meta1;
	struct curvature_desc  *meta2;
};


struct gq_sched_data {
	u64		       time_next_delayed_wake_up;
	u64		       visited_by_timer;
	struct gradient_queue  *gq;
	struct qdisc_watchdog  watchdog;
};

// Underlying linked list

inline struct sk_buff *gq_bucket_dequeue_head(struct gq_bucket *bucket)
{
	struct sk_buff *skb = bucket->head;

	if (skb) {
		bucket->head = skb->next;
		skb->next = NULL;
		bucket->qlen--;
	}
	return skb;
}

inline void bucket_queue_add(struct gq_bucket *bucket, struct sk_buff *skb)
{
	struct sk_buff *head = bucket->head;
	skb->next = NULL;
	bucket->qlen++;

	if (!head) {
		bucket->head = skb;
		bucket->tail = skb;
		return;
	}

	bucket->tail->next = skb;
	bucket->tail = skb;
}

// circular gradient queue

inline void gq_push (struct gradient_queue *gq, struct sk_buff *skb) {
	unsigned long index = 0;
	struct curvature_desc *meta;
	struct gq_bucket *buckets;

	if (skb->trans_time < gq->buffer_ts) {
		if (skb->trans_time <= gq->main_ts) {
			index = 0;
			skb->trans_time = gq->main_ts;
		} else {
			index = (skb->trans_time - gq->main_ts) / gq->grnlrty;
		}
		meta = gq->meta1;
		buckets = gq->main_buckets;
	} else {
		if (skb->trans_time >= gq->max_ts) {
			index = (gq->max_ts - gq->buffer_ts) / gq->grnlrty - 1;
			skb->trans_time = gq->max_ts;
		} else {
			index = (skb->trans_time - gq->buffer_ts) / gq->grnlrty;
		}
		meta = gq->meta2;
		buckets = gq->buffer_buckets;
	}
	gq->num_of_elements++;

	if (!buckets[index].qlen) {
		int i, done = 0;
		unsigned long parentI = ((gq->s + index - 1) / gq->w);
		unsigned long wI = (gq->s + index - 1) % gq->w;
		for (i = 0; i < gq->l; i++) {
			if (!done) {
				meta[parentI].a |= (1UL << wI);
			}
			meta[parentI].c++;

			if(meta[parentI].c > 1)
				done = 1;

			wI = meta[parentI].wwI;
			parentI = meta[parentI].abcI;
		}
	}
	
	bucket_queue_add(&(buckets[index]), skb);
}

inline unsigned long get_min_index (struct gradient_queue *gq) {
	struct curvature_desc *meta;
	unsigned long I = 0, i = 0;
	if (gq->meta1[0].c) {
		meta = gq->meta1;
	} else if (gq->meta2[0].c) {
		meta = gq->meta2;
	} else {
		return -1;
	}

	if (!meta[0].a)
		return 0;

	I = __ffs(meta[0].a) + 1;
	for (i = 1; i < gq->l; i++) {
		I = gq->w * I + __ffs(meta[I].a) + 1;
	}
	return I - gq->s;
}

inline struct sk_buff *gq_extract(struct gradient_queue *gq, uint64_t now, unsigned long *idx) {
	struct gq_bucket *buckets;
	struct curvature_desc *meta;
	unsigned long index = 0;
	u64 base_ts = 0, skb_ts = 0;
	struct sk_buff *ret_skb;
	
	index = get_min_index(gq);
	*idx = index;
	if(!gq->num_of_elements) {
		gq->main_ts = now;
		gq->buffer_ts = now + gq->horizon;
		gq->max_ts = now + gq->horizon + gq->horizon;
		return NULL;
	}

	
	if (gq->meta1[0].c) {
		base_ts = gq->main_ts;
	} else {
		base_ts = gq->buffer_ts;
	}

	skb_ts = (index * gq->grnlrty) + base_ts;

	if (skb_ts > now) {
		return NULL;
	}
	gq->num_of_elements--;

	if (gq->meta1[0].c) {
		meta = gq->meta1;
		buckets = gq->main_buckets;
	} else {
		meta = gq->meta2;
		buckets = gq->buffer_buckets;
	}

	ret_skb = gq_bucket_dequeue_head(&(buckets[index]));
	if (!buckets[index].qlen) {
		int done = 0, i;
		unsigned long parentI = ((gq->s + index - 1) / gq->w);
		unsigned long wI = (gq->s + index - 1) % gq->w;

		for (i = 0; i < gq->l; i++) {
			if (!done)
				meta[parentI].a &= ~(1UL << wI);
			meta[parentI].c--;

			if(meta[parentI].c > 0)
				done = 1;

			wI = meta[parentI].wwI;
			parentI = meta[parentI].abcI;
		}
	}

	return ret_skb;
}

// qdisc api

static int gq_enqueue(struct sk_buff *skb, struct Qdisc *sch,
		      struct sk_buff **to_free)
{
	struct gq_sched_data *q = qdisc_priv(sch);
	u64 now = ktime_get_ns();

	if (!q->gq->num_of_elements) {
		q->gq->main_ts = now;
		q->gq->buffer_ts = now + q->gq->horizon;
		q->gq->max_ts = now + q->gq->horizon + q->gq->horizon;
	}

	if (unlikely(q->gq->num_of_elements >= sch->limit)) {
		return qdisc_drop(skb, sch, to_free);
	}

	qdisc_qstats_backlog_inc(sch, skb);

	if (skb->sk) {
		if (!skb->sk->sk_time_of_last_sent_pkt || skb->sk->sk_time_of_last_sent_pkt < now)
			skb->sk->sk_time_of_last_sent_pkt = now;
		skb->trans_time = skb->sk->sk_time_of_last_sent_pkt;
	} else {
		skb->trans_time = now;
	}

	gq_push (q->gq, skb);

	sch->q.qlen++;

	return NET_XMIT_SUCCESS;
}

static struct sk_buff *gq_dequeue(struct Qdisc *sch)
{
	struct gq_sched_data *q = qdisc_priv(sch);
	u64 now = ktime_get_ns();
	struct sk_buff *skb;
	u64 time_of_min_pkt;
	unsigned long index = 0; 

	skb = gq_extract(q->gq, now, &index);
	if(!skb) {	
		u64 base_ts = 0;

		if (!(q->gq->num_of_elements)) {
			return NULL;
		}

		if (q->gq->meta1[0].c) {
			base_ts = q->gq->main_ts;
		} else {
			base_ts = q->gq->buffer_ts;
		}

		time_of_min_pkt =  index * q->gq->grnlrty + base_ts;

		if (time_of_min_pkt > q->watchdog.last_expires && time_of_min_pkt < now)
			return NULL;

		qdisc_watchdog_schedule_ns(&q->watchdog, time_of_min_pkt);

		return NULL;
	}

	// ADJUST ROTATION
	if (q->gq->buffer_ts < now && q->gq->meta1[0].c == 0) {
		struct gq_bucket *tmp_buckets = q->gq->buffer_buckets;
		struct curvature_desc *tmp_meta = q->gq->meta2;
		u64 tmp_ts = q->gq->buffer_ts;

		q->gq->buffer_buckets = q->gq->main_buckets;
		q->gq->meta2 = q->gq->meta1;
		q->gq->buffer_ts = q->gq->buffer_ts + q->gq->horizon;
		q->gq->max_ts = q->gq->buffer_ts + q->gq->horizon;

		q->gq->main_buckets = tmp_buckets;
		q->gq->meta1 = tmp_meta;
		q->gq->main_ts = tmp_ts;
	}

	if (skb->sk) {
		u32 rate = skb->sk->sk_pacing_rate;
		if (rate != ~0U) {
			u64 len = ((u64)qdisc_pkt_len(skb)) * NSEC_PER_SEC;

			if (likely(rate))
				do_div(len, rate);
			if (unlikely(len > NSEC_PER_SEC))
				len = NSEC_PER_SEC;
			skb->sk->sk_time_of_last_sent_pkt = skb->sk->sk_time_of_last_sent_pkt + len;
		}
	}

	sch->q.qlen--;

	qdisc_qstats_backlog_dec(sch, skb);
	qdisc_bstats_update(sch, skb);

	
	return skb;
}

static void gq_reset(struct Qdisc *sch)
{
	struct gq_sched_data *q = qdisc_priv(sch);

	sch->q.qlen = 0;
	sch->qstats.backlog = 0;

	memset(q->gq->main_buckets, 0, sizeof(struct gq_bucket) * q->gq->num_of_buckets);
	memset(q->gq->buffer_buckets, 0, sizeof(struct gq_bucket) * q->gq->num_of_buckets);
	memset(q->gq->meta1, 0, sizeof(struct curvature_desc) * q->gq->s);
	memset(q->gq->meta2, 0, sizeof(struct curvature_desc) * q->gq->s);
}

static int gq_change(struct Qdisc *sch, struct nlattr *opt)
{
	return -1;
}

static void gq_destroy(struct Qdisc *sch)
{
	struct gq_sched_data *q = qdisc_priv(sch);
	struct gradient_queue *gq_p = q->gq;

	gq_reset(sch);

	kvfree(gq_p->main_buckets);
	kvfree(gq_p->buffer_buckets);
	kvfree(gq_p->meta1);
	kvfree(gq_p->meta2);
	kvfree(gq_p);

	qdisc_watchdog_cancel(&q->watchdog);
}

// initializer

inline unsigned int log_approx(uint32_t v) {
	const unsigned int b[] = {0x2, 0xC, 0xF0, 0xFF00, 0xFFFF0000};
	const unsigned int S[] = {1, 2, 4, 8, 16};
	int i;

	unsigned int r = 0; // result of log2(v) will go here
	for (i = 4; i >= 0; i--) // unroll for speed...
	{
  		if (v & b[i])
  		{
    			v >>= S[i];
    			r |= S[i];
  		}
	}
	return r;
}

static int gq_init(struct Qdisc *sch, struct nlattr *opt)
{
	struct gq_sched_data *q = qdisc_priv(sch);
	struct gradient_queue *gq_p;
	int i = 0;
	u64 granularity =        10000;
	u64 horizon =      10000000000;
	u32 base = 32;
	u64 now = ktime_get_ns();

	gq_p = kmalloc_node(sizeof(struct gradient_queue),
			GFP_KERNEL | __GFP_REPEAT | __GFP_NOWARN,
			netdev_queue_numa_node_read(sch->dev_queue));


	gq_p->main_ts = now;
	gq_p->buffer_ts = now + horizon;
	gq_p->max_ts = now + horizon + horizon;

	gq_p->horizon = horizon;
	gq_p->head_ts = now / granularity;
	gq_p->grnlrty = granularity;
	gq_p->num_of_buckets = horizon / granularity;
	gq_p->num_of_elements = 0;
	gq_p->w = base;
	gq_p->l = ((log_approx(gq_p->num_of_buckets)
				+ log_approx(gq_p->w) - 1) / log_approx(gq_p->w));
	gq_p->s = 1;
	for (i = 0; i < gq_p->l; i++)
		gq_p->s *= gq_p->w;
	gq_p->s = (gq_p->s - 1) / (gq_p->w - 1);

	gq_p->main_buckets = kmalloc_node(sizeof(struct gq_bucket) * gq_p->num_of_buckets,
			GFP_KERNEL | __GFP_REPEAT | __GFP_NOWARN,
			netdev_queue_numa_node_read(sch->dev_queue));

	if(!gq_p->main_buckets)
		gq_p->main_buckets = vmalloc_node(sizeof(struct gq_bucket) * gq_p->num_of_buckets, netdev_queue_numa_node_read(sch->dev_queue));

	gq_p->buffer_buckets = kmalloc_node(sizeof(struct gq_bucket) * gq_p->num_of_buckets,
		GFP_KERNEL | __GFP_REPEAT | __GFP_NOWARN,
		netdev_queue_numa_node_read(sch->dev_queue));

	if(!gq_p->buffer_buckets)
		gq_p->buffer_buckets = vmalloc_node(sizeof(struct gq_bucket) * gq_p->num_of_buckets, netdev_queue_numa_node_read(sch->dev_queue));

	gq_p->meta1 = kmalloc_node(sizeof(struct curvature_desc) * gq_p->s,
			GFP_KERNEL | __GFP_REPEAT | __GFP_NOWARN,
			netdev_queue_numa_node_read(sch->dev_queue));
	if (!gq_p->meta1)
		gq_p->meta1 = vmalloc_node(sizeof(struct curvature_desc) * gq_p->s, netdev_queue_numa_node_read(sch->dev_queue));


	gq_p->meta2 = kmalloc_node(sizeof(struct curvature_desc) * gq_p->s,
			GFP_KERNEL | __GFP_REPEAT | __GFP_NOWARN,
			netdev_queue_numa_node_read(sch->dev_queue));

	if(!gq_p->meta2)
		gq_p->meta2 = vmalloc_node(sizeof(struct curvature_desc) * gq_p->s, netdev_queue_numa_node_read(sch->dev_queue));
	if(!gq_p->meta1 || !gq_p->meta2 || !gq_p->main_buckets || !gq_p->buffer_buckets)
		return -1;

	for (i =0; i< gq_p->num_of_buckets; i++) {
		gq_p->main_buckets[i].qlen = 0;
		gq_p->main_buckets[i].head = NULL;
		gq_p->main_buckets[i].tail = NULL;
		gq_p->buffer_buckets[i].qlen = 0;
		gq_p->buffer_buckets[i].head = NULL;
		gq_p->buffer_buckets[i].tail = NULL;
	}

	for (i = gq_p->s - 1; i >= 0; i--) {
		gq_p->meta1[i].a = 0;
		gq_p->meta2[i].a = 0;
		gq_p->meta1[i].c = 0;
		gq_p->meta2[i].c = 0;
		gq_p->meta1[i].abcI = ((i - 1) / gq_p->w);
		gq_p->meta1[i].wwI = (i-1) % gq_p->w;
		gq_p->meta2[i].abcI = ((i - 1) / gq_p->w);
		gq_p->meta2[i].wwI = (i-1) % gq_p->w;
	}
	q->gq = gq_p;
	q->visited_by_timer = 0;
	sch->limit		= 80000;
	q->time_next_delayed_wake_up = now;
	qdisc_watchdog_init(&q->watchdog, sch);

	return 0;
}

static int gq_dump(struct Qdisc *sch, struct sk_buff *skb)
{
	return -1;
}

static int gq_dump_stats(struct Qdisc *sch, struct gnet_dump *d)
{
	struct tc_fq_qd_stats st;

	return gnet_stats_copy_app(d, &st, sizeof(st));
}

static struct Qdisc_ops gq_qdisc_ops __read_mostly = {
	.id		=	"gq_ffs",
	.priv_size	=	sizeof(struct gq_sched_data),

	.enqueue	=	gq_enqueue,
	.dequeue	=	gq_dequeue,
	.peek		=	qdisc_peek_dequeued,
	.init		=	gq_init,
	.reset		=	gq_reset,
	.destroy	=	gq_destroy,
	.change		=	gq_change,
	.dump		=	gq_dump,
	.dump_stats	=	gq_dump_stats,
	.owner		=	THIS_MODULE,
};

static int __init gq_module_init(void)
{
	int ret;

	ret = register_qdisc(&gq_qdisc_ops);

	return ret;
}

static void __exit gq_module_exit(void)
{
	unregister_qdisc(&gq_qdisc_ops);
}

module_init(gq_module_init)
module_exit(gq_module_exit)
MODULE_AUTHOR("Ahmed Saeed");
MODULE_LICENSE("GPL");
