#include <linux/kernel.h>
#include <linux/blk_types.h>
#include <linux/backing-dev.h>
#include <linux/module.h>
#include <linux/timer.h>
#include <linux/memcontrol.h>
#include <linux/sched/loadavg.h>
#include <linux/sched/signal.h>
#include <trace/events/block.h>
#include <linux/blk-mq.h>
#include "blk-rq-qos.h"
#include "blk-stat.h"


static struct blkcg_policy blkcg_policy_iotrack;

struct blk_iotrack {
	struct rq_qos rqos;
};


#define LAT_BUCKET_NR 8
/* default latency bucket(ns) */
uint64_t def_latb_thresh[LAT_BUCKET_NR] = {
	50000,		/* 50 us */
	100000,		/* 100 us */
	200000,		/* 200 us */
	400000,		/* 400 us */
	1000000,	/* 1 ms */
	2000000,	/* 2 ms */
	4000000,	/* 4 ms */
	8000000,	/* 8 ms */
};

enum {
	IOT_READ,
	IOT_WRITE,
	IOT_OTHER,
	IOT_NR,
};

struct iotrack_stat {
	struct blk_rq_stat  rqs;
	uint64_t ios[IOT_NR];
	uint64_t sts[IOT_NR];
	uint64_t tms[IOT_NR];
	uint64_t dtms[IOT_NR];
	uint64_t hit[IOT_NR][LAT_BUCKET_NR];
};

struct iotrack_grp {
	struct blkg_policy_data pd;
	struct iotrack_stat __percpu *stat_pcpu;
	uint64_t thresh_ns[LAT_BUCKET_NR];
	struct iotrack_stat stat;
};

static inline struct blk_iotrack *BLKIOTIME(struct rq_qos *rqos)
{
	return container_of(rqos, struct blk_iotrack, rqos);
}

static inline struct iotrack_grp *pd_to_iot(struct blkg_policy_data *pd)
{
	return pd ? container_of(pd, struct iotrack_grp, pd) : NULL;
}

static inline struct iotrack_grp *blkg_to_iot(struct blkcg_gq *blkg)
{
	return pd_to_iot(blkg_to_pd(blkg, &blkcg_policy_iotrack));
}

static inline struct blkcg_gq *iot_to_blkg(struct iotrack_grp *iot)
{
	return pd_to_blkg(&iot->pd);
}

static struct blkg_policy_data *iotrack_pd_alloc(gfp_t gfp,
			struct request_queue *q, struct blkcg *blkcg)
{
	struct iotrack_grp *iot;

	iot = kzalloc_node(sizeof(*iot), gfp, q->node);
	if (!iot)
		return NULL;

	iot->stat_pcpu = __alloc_percpu_gfp(sizeof(struct iotrack_stat),
				__alignof__(struct iotrack_stat), gfp);
	if (!iot->stat_pcpu) {
		kfree(iot);
		return NULL;
	}

	return &iot->pd;
}

static void iotrack_pd_init(struct blkg_policy_data *pd)
{
	struct iotrack_grp *iot = pd_to_iot(pd);
	int i, j, cpu;

	for_each_possible_cpu(cpu) {
		struct iotrack_stat *stat;
		stat = per_cpu_ptr(iot->stat_pcpu, cpu);
		blk_rq_stat_init(&stat->rqs);
		for (i = 0; i < IOT_NR; i++) {
			stat->ios[i] = stat->sts[i] = 0;
			stat->tms[i] = stat->dtms[i] = 0;
			for (j = 0; j < LAT_BUCKET_NR; j++)
				stat->hit[i][j] = 0;
		}
	}

	blk_rq_stat_init(&iot->stat.rqs);
	for (i = 0; i < IOT_NR; i++) {
		iot->stat.ios[i] = iot->stat.sts[i] = 0;
		iot->stat.tms[i] = iot->stat.dtms[i] = 0;
		for (j = 0; j < LAT_BUCKET_NR; j++)
			iot->stat.hit[i][j] = 0;
	}

	for (i = 0; i < LAT_BUCKET_NR; i++)
		iot->thresh_ns[i] = def_latb_thresh[i];
}

static void iotrack_pd_offline(struct blkg_policy_data *pd)
{
}

static void iotrack_pd_free(struct blkg_policy_data *pd)
{
	struct iotrack_grp *iot = pd_to_iot(pd);

	free_percpu(iot->stat_pcpu);
	kfree(iot);
}

static u64 iotrack_prfill_stat(struct seq_file *sf, struct blkg_policy_data *pd,
			       int off)
{
	struct iotrack_grp *iot = pd_to_iot(pd);
	struct iotrack_stat *stat = &iot->stat;
	struct blk_rq_stat *rqs = &stat->rqs;
	const char *dname = blkg_dev_name(pd->blkg);
	int cpu, i, j;

	if (!dname)
		return 0;

	/* collect per cpu data */
	preempt_disable();
	for_each_online_cpu(cpu) {
		struct iotrack_stat* s;
		s = per_cpu_ptr(iot->stat_pcpu, cpu);
		blk_rq_stat_sum(rqs, &s->rqs);
		blk_rq_stat_init(&s->rqs);
		for (i = 0; i < IOT_NR; i++) {
			stat->ios[i] += s->ios[i];
			stat->sts[i] += s->sts[i];
			stat->tms[i] += s->tms[i];
			stat->dtms[i] += s->dtms[i];
			s->ios[i] = 0;
			s->sts[i] = 0;
			s->tms[i] = 0;
			s->dtms[i] = 0;
			for (j = 0; j < LAT_BUCKET_NR; j++) {
				stat->hit[i][j] += s->hit[i][j];
				s->hit[i][j] = 0;
			}
		}
	}
	preempt_enable();

	seq_printf(sf, "%s mean: %llu min: %llu max: %llu sum: %llu "
			"rios: %llu wios: %llu oios:%llu "
			"rsts: %llu wsts: %llu osts: %llu "
			"rtms: %llu wtms: %llu otms: %llu "
			"rdtms: %llu wdtms: %llu odtms: %llu",
		dname, rqs->mean, rqs->min, rqs->max, rqs->batch,
		stat->ios[IOT_READ], stat->ios[IOT_WRITE], stat->ios[IOT_OTHER],
		stat->sts[IOT_READ], stat->sts[IOT_WRITE], stat->sts[IOT_OTHER],
		stat->tms[IOT_READ], stat->tms[IOT_WRITE], stat->tms[IOT_OTHER],
		stat->dtms[IOT_READ], stat->dtms[IOT_WRITE], stat->dtms[IOT_OTHER]);

	/* read hit */
	seq_printf(sf, " rhit:");
	for (i = 0; i < LAT_BUCKET_NR; i++)
		seq_printf(sf, " %llu",  stat->hit[IOT_READ][i]);

	/* write hit */
	seq_printf(sf, " whit:");
	for (i = 0; i < LAT_BUCKET_NR; i++)
		seq_printf(sf, " %llu",  stat->hit[IOT_WRITE][i]);

	/* other hit */
	seq_printf(sf, " ohit:");
	for (i = 0; i < LAT_BUCKET_NR; i++)
		seq_printf(sf, " %llu",  stat->hit[IOT_OTHER][i]);

	seq_printf(sf, "\n");

	return 0;
}

static int iotrack_print_stat(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)), iotrack_prfill_stat,
			  &blkcg_policy_iotrack, seq_cft(sf)->private, false);
	return 0;
}

static u64 iotrack_prfill_lat_thresh(struct seq_file *sf,
			struct blkg_policy_data *pd, int off)
{
	struct iotrack_grp *iot = pd_to_iot(pd);
	const char *dname = blkg_dev_name(pd->blkg);
	int i;

	if (!dname)
		return 0;

	seq_printf(sf, "%s", dname);
	for (i = 0; i < LAT_BUCKET_NR; i++)
		seq_printf(sf, " %llu",  iot->thresh_ns[i]);

	seq_printf(sf, "\n");

	return 0;
}

static int iotrack_print_lat_thresh(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)),
		iotrack_prfill_lat_thresh, &blkcg_policy_iotrack,
		seq_cft(sf)->private, false);
	return 0;
}

static ssize_t iotrack_set_lat_thresh(struct kernfs_open_file *of, char *buf,
			     size_t nbytes, loff_t off)
{
	struct blkcg *blkcg = css_to_blkcg(of_css(of));
	struct blkg_conf_ctx ctx;
	struct iotrack_grp *iot;
	uint64_t tmp[LAT_BUCKET_NR];
	int i, ret;
	char *p;

	ret = blkg_conf_prep(blkcg, &blkcg_policy_iotrack, buf, &ctx);
	if (ret)
		return ret;

	iot = blkg_to_iot(ctx.blkg);
	p = ctx.body;

	ret = -EINVAL;
	if (LAT_BUCKET_NR != sscanf(p, "%llu %llu %llu %llu %llu %llu %llu %llu",
			&tmp[0], &tmp[1], &tmp[2], &tmp[3],
			&tmp[4], &tmp[5], &tmp[6], &tmp[7]))
		goto out;

	/* make sure threshold in order */
	for (i = 0; i < LAT_BUCKET_NR - 1; i++) {
		if (tmp[i] >= tmp[i + 1])
			goto out;
	}

	/* update threshold for each bucket */
	for (i = 0; i < LAT_BUCKET_NR; i++)
		iot->thresh_ns[i] = tmp[i];

	ret = 0;
out:
	blkg_conf_finish(&ctx);
	return ret ?: nbytes;
}

static struct cftype iotrack_files[] = {
	{
		.name = "iotrack.stat",
		.seq_show = iotrack_print_stat,
	},
	{
		.name = "iotrack.lat_thresh",
		.seq_show = iotrack_print_lat_thresh,
		.write = iotrack_set_lat_thresh,
	},
	{}
};

static struct cftype iotrack_def_files[] = {
	{
		.name = "iotrack.stat",
		.seq_show = iotrack_print_stat,
	},
	{
		.name = "iotrack.lat_thresh",
		.seq_show = iotrack_print_lat_thresh,
		.write = iotrack_set_lat_thresh,
	},
	{}
};

static struct blkcg_policy blkcg_policy_iotrack = {
	.dfl_cftypes	= iotrack_def_files,
	.legacy_cftypes = iotrack_files,
	.pd_alloc_fn	= iotrack_pd_alloc,
	.pd_init_fn	= iotrack_pd_init,
	.pd_offline_fn	= iotrack_pd_offline,
	.pd_free_fn	= iotrack_pd_free,
};

static void iotrack_account_bio(struct iotrack_grp *iot, struct bio *bio,
		u64 now)
{
	u64 delta, start = bio_issue_time(&bio->bi_issue);
	u64 delta_disk, start_disk = bio_start_time(&bio->bi_start);
	struct iotrack_stat *stat;
	int i, t;

	now = __bio_issue_time(now);

	if (now <= start)
		return;

	switch (bio_op(bio)) {
	case REQ_OP_READ:
		t = IOT_READ;
		break;
	case REQ_OP_WRITE:
		t = IOT_WRITE;
		break;
	default:
		t = IOT_OTHER;
		break;
	}

	delta = now - start;
	stat = get_cpu_ptr(iot->stat_pcpu);
	blk_rq_stat_add(&stat->rqs, delta);
	stat->ios[t]++;
	stat->sts[t] += (bio_issue_size(&bio->bi_issue));
	stat->tms[t] += delta;
	if (start_disk && (start_disk > start) && (now > start_disk))
		delta_disk = now - start_disk;
	else
		delta_disk = 0;
	stat->dtms[t] += delta_disk;
	for (i = 0; i < LAT_BUCKET_NR; i++) {
		if (delta < iot->thresh_ns[i])
			stat->hit[t][i]++;
	}
	put_cpu_ptr(stat);
}

static void blkcg_iotrack_done_bio(struct rq_qos *rqos, struct bio *bio)
{
	struct blkcg_gq *blkg;
	struct iotrack_grp *iot;
	u64 now = ktime_to_ns(ktime_get());

	 blkg = bio->bi_blkg;
	if (!blkg)
		return;

	iot = blkg_to_iot(bio->bi_blkg);
	if (!iot)
		return;

	/* account io statistics */
	while (blkg) {
		iot = blkg_to_iot(blkg);
		if (!iot) {
			blkg = blkg->parent;
			continue;
		}

		iotrack_account_bio(iot, bio, now);
		blkg = blkg->parent;
	}
}

static void blkcg_iotrack_exit(struct rq_qos *rqos)
{
	struct blk_iotrack *blkiotrack = BLKIOTIME(rqos);

	blkcg_deactivate_policy(rqos->q, &blkcg_policy_iotrack);
	kfree(blkiotrack);
}

static struct rq_qos_ops blkcg_iotrack_ops = {
	.done_bio = blkcg_iotrack_done_bio,
	.exit = blkcg_iotrack_exit,
};

int blk_iotrack_init(struct request_queue *q)
{
	struct blk_iotrack *blkiotrack;
	struct rq_qos *rqos;
	int ret;

	blkiotrack = kzalloc(sizeof(*blkiotrack), GFP_KERNEL);
	if (!blkiotrack)
		return -ENOMEM;

	rqos = &blkiotrack->rqos;
	rqos->id = RQ_QOS_IOTRACK;
	rqos->ops = &blkcg_iotrack_ops;
	rqos->q = q;

	rq_qos_add(q, rqos);

	ret = blkcg_activate_policy(q, &blkcg_policy_iotrack);
	if (ret) {
		rq_qos_del(q, rqos);
		kfree(blkiotrack);
		return ret;
	}

	return 0;
}

static int __init iotrack_init(void)
{
	return blkcg_policy_register(&blkcg_policy_iotrack);
}

static void __exit iotrack_exit(void)
{
	return blkcg_policy_unregister(&blkcg_policy_iotrack);
}

module_init(iotrack_init);
module_exit(iotrack_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("weiping zhang <zhangweiping@didichuxing.com>");
