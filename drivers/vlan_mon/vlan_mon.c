#include <linux/capability.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <linux/in.h>
#include <linux/init.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/semaphore.h>
#include <linux/version.h>

#include <net/genetlink.h>
#include <net/sock.h>
#include <net/net_namespace.h>
#include <net/netns/generic.h>

#include "vlan_mon.h"
#include "version.h"

#define VLAN_MON_MAGIC 0x639fa78c

#define VLAN_MON_PROTO_IP     0
#define VLAN_MON_PROTO_PPPOE  1
//#define VLAN_MON_PROTO_IP6    2

#define VLAN_MON_NLMSG_SIZE (NLMSG_DEFAULT_SIZE - GENL_HDRLEN - 128)

#ifndef DEFINE_SEMAPHORE
#define DEFINE_SEMAPHORE(name) struct semaphore name = __SEMAPHORE_INITIALIZER(name, 1)
#endif

#ifndef NETIF_F_HW_VLAN_FILTER
#define NETIF_F_HW_VLAN_FILTER NETIF_F_HW_VLAN_CTAG_FILTER
#endif

#ifndef RHEL_MAJOR
#define RHEL_MAJOR 0
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,0,0) || RHEL_MAJOR == 7
#define vlan_tx_tag_present(skb) skb_vlan_tag_present(skb)
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(5,2,0)
#define nla_nest_start_noflag(skb, attr) nla_nest_start(skb, attr)
#endif

struct vlan_dev {
	unsigned int magic;
	int ifindex;
	struct rcu_head rcu_head;
	struct list_head entry;

	spinlock_t lock;
	unsigned long vid[2][4096/8/sizeof(long)];
	unsigned long busy[4096/8/sizeof(long)];
	int proto;
};

struct vlan_notify {
	struct list_head entry;
	int ifindex;
	int vlan_ifindex;
	int vid;
	int proto;
};

static int autoclean = 0;

static LIST_HEAD(vlan_devices);
static LIST_HEAD(vlan_notifies);
static DEFINE_SPINLOCK(vlan_lock);
static struct work_struct vlan_notify_work;

static struct genl_family vlan_mon_nl_family;
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,13,0) && RHEL_MAJOR < 7
static struct genl_multicast_group vlan_mon_nl_mcg;
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(6,4,0)
static DEFINE_SEMAPHORE(vlan_mon_lock);
#else
static DEFINE_SEMAPHORE(vlan_mon_lock,1);
#endif

static inline int vlan_mon_proto(int proto)
{
	if (proto == ETH_P_PPP_DISC)
		return VLAN_MON_PROTO_PPPOE;

	if (proto == ETH_P_IP)
		return VLAN_MON_PROTO_IP;

	return -ENOSYS;
}

static int vlan_pt_recv(struct sk_buff *skb, struct net_device *dev, struct packet_type *prev, struct net_device *orig_dev)
{
	struct vlan_dev *d;
	struct vlan_notify *n;
	int vid;
	int vlan_ifindex = 0;
	int proto;

	if (!dev->ml_priv)
		goto out;

	if (!vlan_tx_tag_present(skb))
		goto out;

	if (skb->protocol == htons(ETH_P_IP) || skb->protocol == htons(ETH_P_ARP))
		proto = VLAN_MON_PROTO_IP;
	//else if (skb->protocol == htons(ETH_P_IPV6))
	//	proto = VLAN_MON_PROTO_IP6;
	else if (skb->protocol == htons(ETH_P_PPP_DISC))
		proto = VLAN_MON_PROTO_PPPOE;
	else
		goto out;

	rcu_read_lock();

	d = rcu_dereference(dev->ml_priv);
	if (!d || d->magic != VLAN_MON_MAGIC || d->ifindex != dev->ifindex || (d->proto & (1 << proto)) == 0) {
		rcu_read_unlock();
		goto out;
	}

	vid = skb->vlan_tci & VLAN_VID_MASK;

	if (likely(d->busy[vid / (8*sizeof(long))] & (1lu << (vid % (8*sizeof(long))))))
		vid = -1;
	else if (likely(!(d->vid[proto][vid / (8*sizeof(long))] & (1lu << (vid % (8*sizeof(long))))))) {
		spin_lock(&d->lock);
		d->busy[vid / (8*sizeof(long))] |= 1lu << (vid % (8*sizeof(long)));
		d->vid[proto][vid / (8*sizeof(long))] |= 1lu << (vid % (8*sizeof(long)));
		spin_unlock(&d->lock);
	} else
		vid = -1;

	if (vid > 0) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,10,0)
		struct net_device *vd = __vlan_find_dev_deep(dev, vid);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(3,16,0) && RHEL_MAJOR < 7
		struct net_device *vd = __vlan_find_dev_deep(dev, skb->vlan_proto, vid);
#else
		struct net_device *vd = __vlan_find_dev_deep_rcu(dev, skb->vlan_proto, vid);
#endif
		if (vd)
			vlan_ifindex = vd->ifindex;
	}

	rcu_read_unlock();

	if (vid == -1)
		goto out;

	//pr_info("queue %i %i %04x\n", dev->ifindex, vid, skb->protocol);

	n = kmalloc(sizeof(*n), GFP_ATOMIC);
	if (!n)
		goto out;

	n->ifindex = dev->ifindex;
	n->vlan_ifindex = vlan_ifindex;
	n->vid = vid;
	n->proto = ntohs(skb->protocol);

	spin_lock(&vlan_lock);
	list_add_tail(&n->entry, &vlan_notifies);
	spin_unlock(&vlan_lock);

	schedule_work(&vlan_notify_work);

out:
	kfree_skb(skb);
	return 0;
}

static void vlan_do_notify(struct work_struct *w)
{
	struct vlan_notify *n;
	struct sk_buff *report_skb = NULL;
	void *header = NULL;
	struct nlattr *ns;
	int id = 1;

	//pr_info("vlan_do_notify\n");

	while (1) {
		spin_lock_bh(&vlan_lock);
		if (list_empty(&vlan_notifies))
			n = NULL;
		else {
			n = list_first_entry(&vlan_notifies, typeof(*n), entry);
			list_del(&n->entry);
		}
		spin_unlock_bh(&vlan_lock);

		if (!n)
			break;

		if (!report_skb) {
			report_skb = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,13,0) && RHEL_MAJOR < 7
			header = genlmsg_put(report_skb, 0, vlan_mon_nl_mcg.id, &vlan_mon_nl_family, 0, VLAN_MON_NOTIFY);
#else
			header = genlmsg_put(report_skb, 0, 0, &vlan_mon_nl_family, 0, VLAN_MON_NOTIFY);
#endif
		}

		//pr_info("notify %i vlan %i\n", id, n->vid);

		ns = nla_nest_start_noflag(report_skb, id++);
		if (!ns)
			goto nl_err;

		if (nla_put_u32(report_skb, VLAN_MON_ATTR_IFINDEX, n->ifindex))
			goto nl_err;

		if (n->vlan_ifindex && nla_put_u32(report_skb, VLAN_MON_ATTR_VLAN_IFINDEX, n->vlan_ifindex))
			goto nl_err;

		if (nla_put_u16(report_skb, VLAN_MON_ATTR_VID, n->vid))
			goto nl_err;

		if (nla_put_u16(report_skb, VLAN_MON_ATTR_PROTO, n->proto))
			goto nl_err;

		if (nla_nest_end(report_skb, ns) >= VLAN_MON_NLMSG_SIZE || id == 255) {
			genlmsg_end(report_skb, header);
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,13,0) && RHEL_MAJOR < 7
			genlmsg_multicast(report_skb, 0, vlan_mon_nl_mcg.id, GFP_KERNEL);
#else
			genlmsg_multicast(&vlan_mon_nl_family, report_skb, 0, 0, GFP_KERNEL);
#endif
			report_skb = NULL;
			id = 1;
		}

		kfree(n);
		continue;

nl_err:
		nlmsg_free(report_skb);
		report_skb = NULL;
	}

	if (report_skb) {
		genlmsg_end(report_skb, header);
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,13,0) && RHEL_MAJOR < 7
		genlmsg_multicast(report_skb, 0, vlan_mon_nl_mcg.id, GFP_KERNEL);
#else
		genlmsg_multicast(&vlan_mon_nl_family, report_skb, 0, 0, GFP_KERNEL);
#endif
	}
}

static int vlan_mon_nl_cmd_noop(struct sk_buff *skb, struct genl_info *info)
{
	struct sk_buff *msg;
	void *hdr;
	int ret = -ENOBUFS;

	msg = nlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
	if (!msg) {
		ret = -ENOMEM;
		goto out;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,7,0)
	hdr = genlmsg_put(msg, info->snd_pid, info->snd_seq, &vlan_mon_nl_family, 0, VLAN_MON_CMD_NOOP);
#else
	hdr = genlmsg_put(msg, info->snd_portid, info->snd_seq, &vlan_mon_nl_family, 0, VLAN_MON_CMD_NOOP);
#endif

	if (IS_ERR(hdr)) {
		ret = PTR_ERR(hdr);
		goto err_out;
	}

	genlmsg_end(msg, hdr);

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,7,0)
	return genlmsg_unicast(genl_info_net(info), msg, info->snd_pid);
#else
	return genlmsg_unicast(genl_info_net(info), msg, info->snd_portid);
#endif

err_out:
	nlmsg_free(msg);

out:
	return ret;
}

static int vlan_mon_nl_cmd_add_vlan_mon(struct sk_buff *skb, struct genl_info *info)
{
	struct vlan_dev *d;
	struct net_device *dev;
	int ifindex, i, proto;

	if (!info->attrs[VLAN_MON_ATTR_IFINDEX])
		return -EINVAL;

	if (!info->attrs[VLAN_MON_ATTR_PROTO])
		return -EINVAL;

	proto = nla_get_u16(info->attrs[VLAN_MON_ATTR_PROTO]);

	proto = vlan_mon_proto(proto);
	if (proto < 0)
		return proto;

	ifindex = nla_get_u32(info->attrs[VLAN_MON_ATTR_IFINDEX]);

	dev = dev_get_by_index(&init_net, ifindex);

	if (!dev)
		return -ENODEV;

	down(&vlan_mon_lock);
	if (dev->ml_priv) {
		d = (struct vlan_dev *)dev->ml_priv;
		if (d->magic != VLAN_MON_MAGIC || (d->proto & (1 << proto))) {
			up(&vlan_mon_lock);
			dev_put(dev);
			return -EBUSY;
		}
	} else {
		d = kzalloc(sizeof(*d), GFP_KERNEL);
		if (!d) {
			up(&vlan_mon_lock);
			dev_put(dev);
			return -ENOMEM;
		}

		spin_lock_init(&d->lock);
		d->magic = VLAN_MON_MAGIC;
		d->ifindex = ifindex;
		d->proto = 0;

		rcu_assign_pointer(dev->ml_priv, d);

		list_add_tail(&d->entry, &vlan_devices);
	}

	d->proto |= 1 << proto;

	if (info->attrs[VLAN_MON_ATTR_VLAN_MASK]) {
		memcpy(d->vid[proto], nla_data(info->attrs[VLAN_MON_ATTR_VLAN_MASK]), min((int)nla_len(info->attrs[VLAN_MON_ATTR_VLAN_MASK]), (int)sizeof(d->vid)));

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,10,0)
		if (dev->features & NETIF_F_HW_VLAN_FILTER) {
			rtnl_lock();
			for (i = 1; i < 4096; i++) {
				if (!(d->vid[proto][i / (8*sizeof(long))] & (1lu << (i % (8*sizeof(long))))))
					dev->netdev_ops->ndo_vlan_rx_add_vid(dev, i);
			}
			rtnl_unlock();
		}
#else
		if (dev->features & NETIF_F_HW_VLAN_CTAG_FILTER) {
			rtnl_lock();
			for (i = 1; i < 4096; i++) {
				if (!(d->vid[proto][i / (8*sizeof(long))] & (1lu << (i % (8*sizeof(long))))))
					dev->netdev_ops->ndo_vlan_rx_add_vid(dev, htons(ETH_P_8021Q), i);
			}
			rtnl_unlock();
		}
#endif
	}

	up(&vlan_mon_lock);

	dev_put(dev);

	return 0;
}

static int vlan_mon_nl_cmd_add_vlan_mon_vid(struct sk_buff *skb, struct genl_info *info)
{
	struct vlan_dev *d;
	int ifindex, vid, proto;
	struct net_device *dev;

	if (!info->attrs[VLAN_MON_ATTR_IFINDEX] || !info->attrs[VLAN_MON_ATTR_VID] || !info->attrs[VLAN_MON_ATTR_PROTO])
		return -EINVAL;

	ifindex = nla_get_u32(info->attrs[VLAN_MON_ATTR_IFINDEX]);
	vid = nla_get_u16(info->attrs[VLAN_MON_ATTR_VID]);
	proto = nla_get_u16(info->attrs[VLAN_MON_ATTR_PROTO]);

	proto = vlan_mon_proto(proto);
	if (proto < 0)
		return proto;

	dev = dev_get_by_index(&init_net, ifindex);

	if (!dev)
		return -ENODEV;

	down(&vlan_mon_lock);

	if (!dev->ml_priv) {
		up(&vlan_mon_lock);
		dev_put(dev);
		return -EINVAL;
	}

	d = dev->ml_priv;

	spin_lock_bh(&d->lock);
	d->vid[proto][vid / (8*sizeof(long))] &= ~(1lu << (vid % (8*sizeof(long))));
	d->busy[vid / (8*sizeof(long))] &= ~(1lu << (vid % (8*sizeof(long))));
	spin_unlock_bh(&d->lock);
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,10,0)
	if (dev->features & NETIF_F_HW_VLAN_FILTER) {
		rtnl_lock();
		dev->netdev_ops->ndo_vlan_rx_add_vid(dev, vid);
		rtnl_unlock();
	}
#else
	if (dev->features & NETIF_F_HW_VLAN_CTAG_FILTER) {
		rtnl_lock();
		dev->netdev_ops->ndo_vlan_rx_add_vid(dev, htons(ETH_P_8021Q), vid);
		rtnl_unlock();
	}
#endif

	up(&vlan_mon_lock);

	dev_put(dev);

	return 0;
}

static int vlan_mon_nl_cmd_del_vlan_mon_vid(struct sk_buff *skb, struct genl_info *info)
{
	struct vlan_dev *d;
	int ifindex, vid, proto;
	struct net_device *dev;

	if (!info->attrs[VLAN_MON_ATTR_IFINDEX] || !info->attrs[VLAN_MON_ATTR_VID] || !info->attrs[VLAN_MON_ATTR_PROTO])
		return -EINVAL;

	ifindex = nla_get_u32(info->attrs[VLAN_MON_ATTR_IFINDEX]);
	vid = nla_get_u16(info->attrs[VLAN_MON_ATTR_VID]);
	proto = nla_get_u16(info->attrs[VLAN_MON_ATTR_PROTO]);

	proto = vlan_mon_proto(proto);
	if (proto < 0)
		return proto;

	down(&vlan_mon_lock);

	rtnl_lock();
	dev = __dev_get_by_index(&init_net, ifindex);
	if (!dev) {
		rtnl_unlock();
		up(&vlan_mon_lock);
		return -ENODEV;
	}

	if (!dev->ml_priv) {
		rtnl_unlock();
		up(&vlan_mon_lock);
		return -EINVAL;
	}

	d = dev->ml_priv;

	spin_lock_bh(&d->lock);
	d->vid[proto][vid / (8*sizeof(long))] |= 1lu << (vid % (8*sizeof(long)));
	d->busy[vid / (8*sizeof(long))] &= ~(1lu << (vid % (8*sizeof(long))));
	spin_unlock_bh(&d->lock);

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,10,0)
	if (dev->features & NETIF_F_HW_VLAN_FILTER)
		dev->netdev_ops->ndo_vlan_rx_add_vid(dev, vid);
#else
	if (dev->features & NETIF_F_HW_VLAN_CTAG_FILTER)
		dev->netdev_ops->ndo_vlan_rx_add_vid(dev, htons(ETH_P_8021Q), vid);
#endif

	rtnl_unlock();

	up(&vlan_mon_lock);

	return 0;
}

static void vlan_dev_clean(struct vlan_dev *d, struct net_device *dev, struct list_head *list)
{
	int i;
	struct net_device *vd;

	for (i = 1; i < 4096; i++) {
		if (d->busy[i / (8*sizeof(long))] & (1lu << (i % (8*sizeof(long))))) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,10,0)
			vd = __vlan_find_dev_deep(dev, i);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(3,16,0) && RHEL_MAJOR < 7
			vd = __vlan_find_dev_deep(dev, htons(ETH_P_8021Q), i);
			if (!vd)
				vd = __vlan_find_dev_deep(dev, htons(ETH_P_8021AD), i);
#else
			vd = __vlan_find_dev_deep_rcu(dev, htons(ETH_P_8021Q), i);
			if (!vd)
				vd = __vlan_find_dev_deep_rcu(dev, htons(ETH_P_8021AD), i);
#endif

			if (vd)
				vd->rtnl_link_ops->dellink(vd, list);
		}
	}
}

static int vlan_mon_nl_cmd_del_vlan_mon(struct sk_buff *skb, struct genl_info *info)
{
	struct vlan_dev *d;
	struct vlan_notify *vn;
	int ifindex, proto = 0xffff;
	unsigned long flags;
	struct list_head *pos, *n;
	struct net_device *dev;
	LIST_HEAD(list_kill);

	if (info->attrs[VLAN_MON_ATTR_PROTO]) {
		proto = nla_get_u16(info->attrs[VLAN_MON_ATTR_PROTO]);

		proto = vlan_mon_proto(proto);
		if (proto < 0)
			return proto;

		proto = 1 << proto;
	}

	if (info->attrs[VLAN_MON_ATTR_IFINDEX])
		ifindex = nla_get_u32(info->attrs[VLAN_MON_ATTR_IFINDEX]);
	else
		ifindex = -1;

	down(&vlan_mon_lock);

	rtnl_lock();
	rcu_read_lock();
	list_for_each_safe(pos, n, &vlan_devices) {
		d = list_entry(pos, typeof(*d), entry);
		if ((ifindex == -1 || d->ifindex == ifindex) && (d->proto & proto)) {
			d->proto &= ~proto;

			dev = __dev_get_by_index(&init_net, d->ifindex);
			if (dev) {
				if (dev->ml_priv == d) {
					if (!d->proto)
						rcu_assign_pointer(dev->ml_priv, NULL);
				}

				if (!d->proto && autoclean)
					vlan_dev_clean(d, dev, &list_kill);
			}

			if (!d->proto) {
				list_del(&d->entry);
				kfree_rcu(d, rcu_head);
			}
		}
	}
	rcu_read_unlock();

	if (!list_empty(&list_kill)) {
		unregister_netdevice_many(&list_kill);
		if (list_kill.next != LIST_POISON1)
			list_del(&list_kill);
	}

	rtnl_unlock();

	up(&vlan_mon_lock);

	synchronize_net();

	spin_lock_irqsave(&vlan_lock, flags);
	list_for_each_safe(pos, n, &vlan_notifies) {
		vn = list_entry(pos, typeof(*vn), entry);
		if ((ifindex == -1 || vn->ifindex == ifindex) && (proto & (1 << vlan_mon_proto(vn->proto)))) {
			list_del(&vn->entry);
			kfree(vn);
		}
	}
	spin_unlock_irqrestore(&vlan_lock, flags);

	return 0;
}

static int vlan_mon_nl_cmd_check_busy(struct sk_buff *skb, struct genl_info *info)
{
	int ifindex, vid;
	struct net_device *dev;
	int ret = 0;

	if (!info->attrs[VLAN_MON_ATTR_IFINDEX] || !info->attrs[VLAN_MON_ATTR_VID])
		return -EINVAL;

	ifindex = nla_get_u32(info->attrs[VLAN_MON_ATTR_IFINDEX]);
	vid = nla_get_u16(info->attrs[VLAN_MON_ATTR_VID]);

	down(&vlan_mon_lock);

	rtnl_lock();
	dev = __dev_get_by_index(&init_net, ifindex);
	if (dev) {
		struct vlan_dev *d = dev->ml_priv;
		if (d) {
			if (d->busy[vid / (8*sizeof(long))] & (1lu << (vid % (8*sizeof(long)))))
				ret = -EBUSY;
		}
	}
	rtnl_unlock();

	up(&vlan_mon_lock);

	return ret;
}

static const struct nla_policy vlan_mon_nl_policy[VLAN_MON_ATTR_MAX + 1] = {
	[VLAN_MON_ATTR_NONE]		    = { .type = NLA_UNSPEC,                     },
	[VLAN_MON_ATTR_VLAN_MASK]	  = { .type = NLA_BINARY, .len = 4096/8       },
	[VLAN_MON_ATTR_PROTO]       = { .type = NLA_U16,                        },
	[VLAN_MON_ATTR_IFINDEX]     = { .type = NLA_U32,                        },
	[VLAN_MON_ATTR_VID]         = { .type = NLA_U16,                        },
};

static const struct genl_ops vlan_mon_nl_ops[] = {
	{
		.cmd = VLAN_MON_CMD_NOOP,
		.doit = vlan_mon_nl_cmd_noop,
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,2,0)
		.policy = vlan_mon_nl_policy,
#endif
		/* can be retrieved by unprivileged users */
	},
	{
		.cmd = VLAN_MON_CMD_ADD,
		.doit = vlan_mon_nl_cmd_add_vlan_mon,
		.flags = GENL_ADMIN_PERM,
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,2,0)
		.policy = vlan_mon_nl_policy,
#endif
	},
	{
		.cmd = VLAN_MON_CMD_ADD_VID,
		.doit = vlan_mon_nl_cmd_add_vlan_mon_vid,
		.flags = GENL_ADMIN_PERM,
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,2,0)
		.policy = vlan_mon_nl_policy,
#endif
	},
	{
		.cmd = VLAN_MON_CMD_DEL,
		.doit = vlan_mon_nl_cmd_del_vlan_mon,
		.flags = GENL_ADMIN_PERM,
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,2,0)
		.policy = vlan_mon_nl_policy,
#endif
	},
	{
		.cmd = VLAN_MON_CMD_CHECK_BUSY,
		.doit = vlan_mon_nl_cmd_check_busy,
		.flags = GENL_ADMIN_PERM,
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,2,0)
		.policy = vlan_mon_nl_policy,
#endif
	},
	{
		.cmd = VLAN_MON_CMD_DEL_VID,
		.doit = vlan_mon_nl_cmd_del_vlan_mon_vid,
		.flags = GENL_ADMIN_PERM,
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,2,0)
		.policy = vlan_mon_nl_policy,
#endif
	},
};

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,13,0) && RHEL_MAJOR < 7
static struct genl_multicast_group vlan_mon_nl_mcg = {
	.name = VLAN_MON_GENL_MCG,
};
#else
static struct genl_multicast_group vlan_mon_nl_mcgs[] = {
	{ .name = VLAN_MON_GENL_MCG, }
};
#endif

static struct genl_family vlan_mon_nl_family = {
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,10,0)
	.id		= GENL_ID_GENERATE,
#endif
	.name		= VLAN_MON_GENL_NAME,
	.version	= VLAN_MON_GENL_VERSION,
	.hdrsize	= 0,
	.maxattr	= VLAN_MON_ATTR_MAX,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,10,0)
	.module = THIS_MODULE,
	.ops = vlan_mon_nl_ops,
	.n_ops = ARRAY_SIZE(vlan_mon_nl_ops),
	.mcgrps = vlan_mon_nl_mcgs,
	.n_mcgrps = ARRAY_SIZE(vlan_mon_nl_mcgs),
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,2,0)
	.policy = vlan_mon_nl_policy,
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,1,0)
	.resv_start_op = CTRL_CMD_GETPOLICY + 1,
#endif
};

static struct packet_type vlan_pt __read_mostly = {
	.type = __constant_htons(ETH_P_ALL),
	.func = vlan_pt_recv,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,15,0)
	.af_packet_net = &init_net,
#endif
};

static int __init vlan_mon_init(void)
{
	int err;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,35)
	int i;
#endif

	printk("vlan-mon driver v%s\n", ACCEL_PPP_VERSION);

	INIT_WORK(&vlan_notify_work, vlan_do_notify);

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,13,0) && RHEL_MAJOR < 7
	err = genl_register_family_with_ops(&vlan_mon_nl_family, vlan_mon_nl_ops, ARRAY_SIZE(vlan_mon_nl_ops));
#elif LINUX_VERSION_CODE < KERNEL_VERSION(4,10,0)
	err = genl_register_family_with_ops_groups(&vlan_mon_nl_family, vlan_mon_nl_ops, vlan_mon_nl_mcgs);
#else
	err = genl_register_family(&vlan_mon_nl_family);
#endif
	if (err < 0) {
		printk(KERN_INFO "vlan_mon: can't register netlink interface\n");
		goto out;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,13,0) && RHEL_MAJOR < 7
	err = genl_register_mc_group(&vlan_mon_nl_family, &vlan_mon_nl_mcg);
	if (err < 0) {
		printk(KERN_INFO "vlan_mon: can't register netlink multicast group\n");
		goto out_unreg;
	}
#endif

	dev_add_pack(&vlan_pt);

	return 0;

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,13,0) && RHEL_MAJOR < 7
out_unreg:
#endif
	genl_unregister_family(&vlan_mon_nl_family);
out:
	return err;
}

static void __exit vlan_mon_fini(void)
{
	struct vlan_dev *d;
	struct vlan_notify *vn;
	struct net_device *dev;
	LIST_HEAD(list_kill);

	dev_remove_pack(&vlan_pt);

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,13,0) && RHEL_MAJOR < 7
	genl_unregister_mc_group(&vlan_mon_nl_family, &vlan_mon_nl_mcg);
#endif
	genl_unregister_family(&vlan_mon_nl_family);

	down(&vlan_mon_lock);
	up(&vlan_mon_lock);

	rtnl_lock();
	rcu_read_lock();
	while (!list_empty(&vlan_devices)) {
		d = list_first_entry(&vlan_devices, typeof(*d), entry);
		dev = __dev_get_by_index(&init_net, d->ifindex);

		if (dev) {
			rcu_assign_pointer(dev->ml_priv, NULL);

			if (autoclean)
				vlan_dev_clean(d, dev, &list_kill);
		}

		list_del(&d->entry);
		kfree_rcu(d, rcu_head);
	}
	rcu_read_unlock();

	if (!list_empty(&list_kill)) {
		unregister_netdevice_many(&list_kill);
		if (list_kill.next != LIST_POISON1)
			list_del(&list_kill);
	}

	rtnl_unlock();

	synchronize_net();

	while (!list_empty(&vlan_notifies)) {
		vn = list_first_entry(&vlan_notifies, typeof(*vn), entry);
		list_del(&vn->entry);
		kfree(vn);
	}

	synchronize_rcu();
}

module_init(vlan_mon_init);
module_exit(vlan_mon_fini);
MODULE_LICENSE("GPL");
module_param(autoclean, int, 0);
//MODULE_PARAM_DESC(autoclean, "automaticaly remove created vlan interfaces on restart");

