// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2020 Facebook

#include "linux/fs.h"
#include <linux/debugfs.h>
#include <linux/random.h>
#include <net/netdev_queues.h>

#include "netdevsim.h"

struct nsim_stat_desc {
	char desc[ETH_GSTRING_LEN];
	size_t offset;
};

#define NSIM_STAT_ENTRY(s) { \
	.desc = #s,  \
	.offset = offsetof(struct rtnl_link_stats64, s) }

#define NSIM_MOCK_STAT_ENTRY(s) { \
	.desc = #s,  \
	.offset = offsetof(struct nsim_mock_stats, s) }

static const struct nsim_stat_desc nsim_stats_desc[] = {
	NSIM_STAT_ENTRY(tx_packets),
	NSIM_STAT_ENTRY(rx_packets),
	NSIM_STAT_ENTRY(tx_bytes),
	NSIM_STAT_ENTRY(rx_bytes),
	NSIM_STAT_ENTRY(tx_dropped),
	NSIM_STAT_ENTRY(rx_dropped),
};

#define NSIM_STATS_LEN	ARRAY_SIZE(nsim_stats_desc)

#define NSIM_MOCK_STATS_LEN	ARRAY_SIZE(nsim_mock_stats_desc)

static const struct nsim_stat_desc nsim_mock_stats_desc[] = {
	NSIM_MOCK_STAT_ENTRY(hw_out_of_sequence),
	NSIM_MOCK_STAT_ENTRY(hw_out_of_buffer),
	NSIM_MOCK_STAT_ENTRY(hw_packet_seq_err),
};

static void
nsim_get_pause_stats(struct net_device *dev,
		     struct ethtool_pause_stats *pause_stats)
{
	struct netdevsim *ns = netdev_priv(dev);

	if (ns->ethtool.pauseparam.report_stats_rx)
		pause_stats->rx_pause_frames = 1;
	if (ns->ethtool.pauseparam.report_stats_tx)
		pause_stats->tx_pause_frames = 2;
}

static void
nsim_get_pauseparam(struct net_device *dev, struct ethtool_pauseparam *pause)
{
	struct netdevsim *ns = netdev_priv(dev);

	pause->autoneg = 0; /* We don't support ksettings, so can't pretend */
	pause->rx_pause = ns->ethtool.pauseparam.rx;
	pause->tx_pause = ns->ethtool.pauseparam.tx;
}

static int
nsim_set_pauseparam(struct net_device *dev, struct ethtool_pauseparam *pause)
{
	struct netdevsim *ns = netdev_priv(dev);

	if (pause->autoneg)
		return -EINVAL;

	ns->ethtool.pauseparam.rx = pause->rx_pause;
	ns->ethtool.pauseparam.tx = pause->tx_pause;
	return 0;
}

static int nsim_get_coalesce(struct net_device *dev,
			     struct ethtool_coalesce *coal,
			     struct kernel_ethtool_coalesce *kernel_coal,
			     struct netlink_ext_ack *extack)
{
	struct netdevsim *ns = netdev_priv(dev);

	memcpy(coal, &ns->ethtool.coalesce, sizeof(ns->ethtool.coalesce));
	return 0;
}

static int nsim_set_coalesce(struct net_device *dev,
			     struct ethtool_coalesce *coal,
			     struct kernel_ethtool_coalesce *kernel_coal,
			     struct netlink_ext_ack *extack)
{
	struct netdevsim *ns = netdev_priv(dev);

	memcpy(&ns->ethtool.coalesce, coal, sizeof(ns->ethtool.coalesce));
	return 0;
}

static void nsim_get_ringparam(struct net_device *dev,
			       struct ethtool_ringparam *ring,
			       struct kernel_ethtool_ringparam *kernel_ring,
			       struct netlink_ext_ack *extack)
{
	struct netdevsim *ns = netdev_priv(dev);

	memcpy(ring, &ns->ethtool.ring, sizeof(ns->ethtool.ring));
	kernel_ring->hds_thresh_max = NSIM_HDS_THRESHOLD_MAX;

	if (dev->cfg->hds_config == ETHTOOL_TCP_DATA_SPLIT_UNKNOWN)
		kernel_ring->tcp_data_split = ETHTOOL_TCP_DATA_SPLIT_ENABLED;
}

static int nsim_set_ringparam(struct net_device *dev,
			      struct ethtool_ringparam *ring,
			      struct kernel_ethtool_ringparam *kernel_ring,
			      struct netlink_ext_ack *extack)
{
	struct netdevsim *ns = netdev_priv(dev);

	ns->ethtool.ring.rx_pending = ring->rx_pending;
	ns->ethtool.ring.rx_jumbo_pending = ring->rx_jumbo_pending;
	ns->ethtool.ring.rx_mini_pending = ring->rx_mini_pending;
	ns->ethtool.ring.tx_pending = ring->tx_pending;
	return 0;
}

static void
nsim_get_channels(struct net_device *dev, struct ethtool_channels *ch)
{
	struct netdevsim *ns = netdev_priv(dev);

	ch->max_combined = ns->nsim_bus_dev->num_queues;
	ch->combined_count = ns->ethtool.channels;
}

static void
nsim_wake_queues(struct net_device *dev)
{
	struct netdevsim *ns = netdev_priv(dev);
	struct netdevsim *peer;

	synchronize_net();
	netif_tx_wake_all_queues(dev);

	rcu_read_lock();
	peer = rcu_dereference(ns->peer);
	if (peer)
		netif_tx_wake_all_queues(peer->netdev);
	rcu_read_unlock();
}

static int
nsim_set_channels(struct net_device *dev, struct ethtool_channels *ch)
{
	struct netdevsim *ns = netdev_priv(dev);
	int err;

	err = netif_set_real_num_queues(dev, ch->combined_count,
					ch->combined_count);
	if (err)
		return err;

	ns->ethtool.channels = ch->combined_count;

	/* Only wake up queues if devices are linked */
	if (rcu_access_pointer(ns->peer))
		nsim_wake_queues(dev);

	return 0;
}

static int
nsim_get_fecparam(struct net_device *dev, struct ethtool_fecparam *fecparam)
{
	struct netdevsim *ns = netdev_priv(dev);

	if (ns->ethtool.get_err)
		return -ns->ethtool.get_err;
	memcpy(fecparam, &ns->ethtool.fec, sizeof(ns->ethtool.fec));
	return 0;
}

static int
nsim_set_fecparam(struct net_device *dev, struct ethtool_fecparam *fecparam)
{
	struct netdevsim *ns = netdev_priv(dev);
	u32 fec;

	if (ns->ethtool.set_err)
		return -ns->ethtool.set_err;
	memcpy(&ns->ethtool.fec, fecparam, sizeof(ns->ethtool.fec));
	fec = fecparam->fec;
	if (fec == ETHTOOL_FEC_AUTO)
		fec |= ETHTOOL_FEC_OFF;
	fec |= ETHTOOL_FEC_NONE;
	ns->ethtool.fec.active_fec = 1 << (fls(fec) - 1);
	return 0;
}

static void
nsim_get_fec_stats(struct net_device *dev, struct ethtool_fec_stats *fec_stats)
{
	fec_stats->corrected_blocks.total = 123;
	fec_stats->uncorrectable_blocks.total = 4;
}

static int nsim_get_ts_info(struct net_device *dev,
			    struct kernel_ethtool_ts_info *info)
{
	struct netdevsim *ns = netdev_priv(dev);

	info->phc_index = mock_phc_index(ns->phc);

	return 0;
}

static int nsim_sset_count(struct net_device *dev, int sset)
{
	struct netdevsim *ns = netdev_priv(dev);

	switch (sset) {
	case ETH_SS_STATS:
		return ns->ethtool.mock_stats.enabled ?
			NSIM_STATS_LEN + NSIM_MOCK_STATS_LEN : NSIM_STATS_LEN;
	default:
		return -EOPNOTSUPP;
	}
}

static void nsim_get_strings(struct net_device *dev, u32 sset, u8 *data)
{
	struct netdevsim *ns = netdev_priv(dev);

	int i;

	switch (sset) {
	case ETH_SS_STATS:
		for (i = 0; i < NSIM_STATS_LEN; i++)
			ethtool_puts(&data, nsim_stats_desc[i].desc);
		if (ns->ethtool.mock_stats.enabled)
			for (i = 0; i < NSIM_MOCK_STATS_LEN; i++)
				ethtool_puts(&data,
					     nsim_mock_stats_desc[i].desc);

		break;
	}
}

static void nsim_ethtool_add_mock_stats(struct netdevsim *ns,
					u64 *data)
{
	unsigned int start, i;
	const u8 *stats_base;
	const u64_stats_t *p;
	size_t offset;

	stats_base = (const u8 *)&ns->ethtool.mock_stats;

	data += NSIM_STATS_LEN;

	do {
		start = u64_stats_fetch_begin(&ns->ethtool.mock_stats.syncp);
		for (i = 0; i < NSIM_MOCK_STATS_LEN; i++) {
			offset = nsim_mock_stats_desc[i].offset;

			p = (const u64_stats_t *)(stats_base + offset);
			data[i] = u64_stats_read(p);
		}
	} while (u64_stats_fetch_retry(&ns->ethtool.mock_stats.syncp, start));
}

static void nsim_get_ethtool_stats(struct net_device *dev,
				   struct ethtool_stats *stats,
				   u64 *data)
{
	struct netdevsim *ns;
	struct rtnl_link_stats64 rtstats = {};
	int i;

	dev_get_stats(dev, &rtstats);

	for (i = 0; i < NSIM_STATS_LEN; i++)
		data[i] = *(u64 *)((u8 *)&rtstats + nsim_stats_desc[i].offset);

	ns = netdev_priv(dev);

	if (ns->ethtool.mock_stats.enabled)
		nsim_ethtool_add_mock_stats(ns, data);
}

#define NSIM_MOCK_STATS_INTERVAL_MS 100

static void nsim_mock_stats_traffic_bump(struct nsim_mock_stats *stats)
{
	if (stats->enabled) {
		stats->hw_out_of_buffer += 1;
		stats->hw_out_of_sequence += 1;
		stats->hw_packet_seq_err += 1;
	}
}

static void nsim_mock_stats_traffic_work(struct work_struct *work)
{
	struct nsim_mock_stats *stats;

	stats = container_of(work, struct nsim_mock_stats, traffic_dw.work);
	nsim_mock_stats_traffic_bump(stats);

	schedule_delayed_work(&stats->traffic_dw,
			      msecs_to_jiffies(NSIM_MOCK_STATS_INTERVAL_MS));
}

static const struct ethtool_ops nsim_ethtool_ops = {
	.supported_coalesce_params	= ETHTOOL_COALESCE_ALL_PARAMS,
	.supported_ring_params		= ETHTOOL_RING_USE_TCP_DATA_SPLIT |
					  ETHTOOL_RING_USE_HDS_THRS,
	.get_pause_stats	        = nsim_get_pause_stats,
	.get_pauseparam		        = nsim_get_pauseparam,
	.set_pauseparam		        = nsim_set_pauseparam,
	.set_coalesce			= nsim_set_coalesce,
	.get_coalesce			= nsim_get_coalesce,
	.get_ringparam			= nsim_get_ringparam,
	.set_ringparam			= nsim_set_ringparam,
	.get_channels			= nsim_get_channels,
	.set_channels			= nsim_set_channels,
	.get_fecparam			= nsim_get_fecparam,
	.set_fecparam			= nsim_set_fecparam,
	.get_fec_stats			= nsim_get_fec_stats,
	.get_ts_info			= nsim_get_ts_info,
	.get_sset_count			= nsim_sset_count,
	.get_strings			= nsim_get_strings,
	.get_ethtool_stats		= nsim_get_ethtool_stats,
};

static void nsim_ethtool_ring_init(struct netdevsim *ns)
{
	ns->ethtool.ring.rx_pending = 512;
	ns->ethtool.ring.rx_max_pending = 4096;
	ns->ethtool.ring.rx_jumbo_max_pending = 4096;
	ns->ethtool.ring.rx_mini_max_pending = 4096;
	ns->ethtool.ring.tx_pending = 512;
	ns->ethtool.ring.tx_max_pending = 4096;
}

static void mock_stats_reset(struct nsim_mock_stats *mock_stats)
{
	mock_stats->hw_out_of_buffer = 0;
	mock_stats->hw_out_of_sequence = 0;
	mock_stats->hw_packet_seq_err = 0;
}

static ssize_t mock_stats_enabled_write(struct file *filp,
					const char __user *ubuf,
					size_t count,
					loff_t *offp)
{
	bool enabled;
	int r;
	struct nsim_mock_stats *mock_stats = filp->private_data;
	struct dentry *dentry = filp->f_path.dentry;

	r = kstrtobool_from_user(ubuf, count, &enabled);
	if (!r) {
		r = debugfs_file_get(dentry);
		if (unlikely(r))
			return r;
		mock_stats->enabled = enabled;
		if (!enabled) {
			mock_stats_reset(mock_stats);
		}
		debugfs_file_put(dentry);
	}

	return count;	
}

static struct debugfs_short_fops mock_stats_fops = {
	.write = mock_stats_enabled_write,
	.llseek = generic_file_llseek
};

void nsim_ethtool_init(struct netdevsim *ns)
{
	struct dentry *ethtool, *dir;

	ns->netdev->ethtool_ops = &nsim_ethtool_ops;

	nsim_ethtool_ring_init(ns);

	ns->ethtool.pauseparam.report_stats_rx = true;
	ns->ethtool.pauseparam.report_stats_tx = true;

	ns->ethtool.fec.fec = ETHTOOL_FEC_NONE;
	ns->ethtool.fec.active_fec = ETHTOOL_FEC_NONE;

	ns->ethtool.channels = ns->nsim_bus_dev->num_queues;

	ethtool = debugfs_create_dir("ethtool", ns->nsim_dev_port->ddir);

	debugfs_create_u32("get_err", 0600, ethtool, &ns->ethtool.get_err);
	debugfs_create_u32("set_err", 0600, ethtool, &ns->ethtool.set_err);

	dir = debugfs_create_dir("pause", ethtool);
	debugfs_create_bool("report_stats_rx", 0600, dir,
			    &ns->ethtool.pauseparam.report_stats_rx);
	debugfs_create_bool("report_stats_tx", 0600, dir,
			    &ns->ethtool.pauseparam.report_stats_tx);

	dir = debugfs_create_dir("ring", ethtool);
	debugfs_create_u32("rx_max_pending", 0600, dir,
			   &ns->ethtool.ring.rx_max_pending);
	debugfs_create_u32("rx_jumbo_max_pending", 0600, dir,
			   &ns->ethtool.ring.rx_jumbo_max_pending);
	debugfs_create_u32("rx_mini_max_pending", 0600, dir,
			   &ns->ethtool.ring.rx_mini_max_pending);
	debugfs_create_u32("tx_max_pending", 0600, dir,
			   &ns->ethtool.ring.tx_max_pending);

	dir = debugfs_create_dir("mock_stats", ethtool);
	debugfs_create_file("enabled", 0600, dir, &ns->ethtool.mock_stats,
			    &mock_stats_fops);

	INIT_DELAYED_WORK(&ns->ethtool.mock_stats.traffic_dw,
			  &nsim_mock_stats_traffic_work);
	schedule_delayed_work(&ns->ethtool.mock_stats.traffic_dw,
			      msecs_to_jiffies(NSIM_MOCK_STATS_INTERVAL_MS));
}

void nsim_ethtool_exit(struct netdevsim *ns)
{
	cancel_delayed_work_sync(&ns->ethtool.mock_stats.traffic_dw);
	
}
