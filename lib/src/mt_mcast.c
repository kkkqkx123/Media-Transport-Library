/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "mt_mcast.h"

#include "datapath/mt_queue.h"
#include "mt_log.h"
#include "mt_util.h"

#define MT_MCAST_POOL_INC (32)

static inline struct mt_mcast_impl* get_mcast(struct mtl_main_impl* impl,
                                              enum mtl_port port) {
  return impl->mcast[port];
}

/* A group with any-source members or without source filters is EXCLUDE. */
static inline bool mcast_group_any_source(struct mt_mcast_group_entry* group) {
  return group->any_src_ref_cnt > 0 || group->src_num == 0;
}

static bool mcast_group_has_src(struct mt_mcast_group_entry* group, uint32_t src_addr) {
  struct mt_mcast_src_entry* src;

  TAILQ_FOREACH(src, &group->src_list, entries) {
    if (src->src_ip == src_addr) return true;
  }

  return false;
}

static void mcast_group_free(struct mt_mcast_group_entry* group) {
  struct mt_mcast_src_entry *src, *temp_src;

  src = TAILQ_FIRST(&group->src_list);
  while (src) {
    temp_src = TAILQ_NEXT(src, entries);
    TAILQ_REMOVE(&group->src_list, src, entries);
    mt_rte_free(src);
    src = temp_src;
  }

  mt_rte_free(group);
}

/* a queryer counts as active for three join periods after its last query */
static inline bool mcast_has_external_query(struct mtl_main_impl* impl,
                                            struct mt_mcast_impl* mcast) {
  if (!mcast->last_query_tsc) return false;
  return (mt_get_tsc(impl) - mcast->last_query_tsc) <
         ((uint64_t)IGMP_QUERY_ACTIVE_PERIOD_US * NS_PER_US);
}

/* Computing the Internet Checksum based on rfc1071 */
static uint16_t mcast_msg_checksum(enum mcast_msg_type type, void* msg, size_t msg_len) {
  size_t size = msg_len;

  if (type != MEMBERSHIP_QUERY && type != MEMBERSHIP_REPORT_V3) {
    err("%s, wrong mcast msg type: %d\n", __func__, type);
    return 0;
  }

  return mt_rf1071_check_sum(msg, size, true);
}

/* group record shaping, return record length, refer to RFC3376 - 4.2.4 */
static inline size_t mcast_fill_group_record(uint8_t record_type, uint32_t group_addr,
                                             uint16_t num_sources,
                                             struct mcast_group_record* record) {
  record->record_type = record_type;
  record->aux_data_len = 0;
  record->num_sources = htons(num_sources);
  record->multicast_addr = group_addr;

  return sizeof(*record) + num_sources * sizeof(uint32_t);
}

static size_t mcast_create_group_record_on_query(struct mt_mcast_group_entry* group,
                                                 struct mcast_group_record* record) {
  uint16_t num_sources = 0;

  if (!mcast_group_any_source(group)) {
    struct mt_mcast_src_entry* src;
    TAILQ_FOREACH(src, &group->src_list, entries) {
      record->source_addr[num_sources++] = src->src_ip;
    }
  }

  uint8_t type = num_sources ? MCAST_MODE_IS_INCLUDE : MCAST_MODE_IS_EXCLUDE;
  return mcast_fill_group_record(type, group->group_ip, num_sources, record);
}

/* A change-to-include record carries the remaining source list, if any. */
static size_t mcast_create_group_record_on_action(uint32_t group_addr,
                                                  uint8_t record_type, uint32_t src_addr,
                                                  uint16_t src_num, uint32_t* src_addrs,
                                                  struct mcast_group_record* record) {
  uint16_t num_sources = 0;

  if (record_type == MCAST_ALLOW_NEW_SOURCES || record_type == MCAST_BLOCK_OLD_SOURCES) {
    record->source_addr[0] = src_addr;
    num_sources = 1;
  } else if (record_type == MCAST_CHANGE_TO_INCLUDE_MODE) {
    if (src_addrs) {
      for (uint16_t i = 0; i < src_num; i++) record->source_addr[i] = src_addrs[i];
      num_sources = src_num;
    } else if (src_addr) {
      record->source_addr[0] = src_addr;
      num_sources = 1;
    }
  }

  return mcast_fill_group_record(record_type, group_addr, num_sources, record);
}

/* 224.0.0.22 */
static struct rte_ether_addr const mcast_mac_dst = {{0x01, 0x00, 0x5e, 0x00, 0x00, 0x16}};

static struct rte_ipv4_hdr* mcast_fill_ipv4(struct mtl_main_impl* impl,
                                            enum mtl_port port, struct rte_mbuf* pkt) {
  struct rte_ether_hdr* eth_hdr;
  struct rte_ipv4_hdr* ip_hdr;

  eth_hdr = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr*);
  mt_macaddr_get(impl, port, mt_eth_s_addr(eth_hdr));
  rte_ether_addr_copy(&mcast_mac_dst, mt_eth_d_addr(eth_hdr));
  eth_hdr->ether_type = htons(RTE_ETHER_TYPE_IPV4);

  ip_hdr = rte_pktmbuf_mtod_offset(pkt, struct rte_ipv4_hdr*, sizeof(*eth_hdr));
  ip_hdr->version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);
  ip_hdr->time_to_live = 1;
  ip_hdr->type_of_service = IP_IGMP_DSCP_VALUE;
  ip_hdr->fragment_offset = MT_IP_DONT_FRAGMENT_FLAG;
  ip_hdr->hdr_checksum = 0;
  ip_hdr->total_length = 0;
  ip_hdr->next_proto_id = IPPROTO_IGMP;
  ip_hdr->src_addr = *(uint32_t*)mt_sip_addr(impl, port);
  inet_pton(AF_INET, IGMP_REPORT_IP, &ip_hdr->dst_addr);

  return ip_hdr;
}

#ifdef MCAST_ENABLE_QUERY
/* 224.0.0.1 */
static struct rte_ether_addr const mcast_mac_query = {
    {0x01, 0x00, 0x5e, 0x00, 0x00, 0x01}};

int mcast_membership_general_query(struct mtl_main_impl* impl, enum mtl_port port) {
  struct rte_mbuf* pkt;
  struct rte_ipv4_hdr* ip_hdr;
  struct mcast_mb_query_v3* mb_query;
  size_t hdr_offset = 0;
  size_t mb_query_len = sizeof(struct mcast_mb_query_v3);

  pkt = rte_pktmbuf_alloc(mt_sys_tx_mempool(impl, port));
  if (!pkt) {
    err("%s(%d), report packet alloc failed\n", __func__, port);
    return -ENOMEM;
  }

  ip_hdr = mcast_fill_ipv4(impl, port, pkt);
  hdr_offset += sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr);

  mb_query = rte_pktmbuf_mtod_offset(pkt, struct mcast_mb_query_v3*, hdr_offset);
  mb_query->type = MEMBERSHIP_QUERY;
  mb_query->max_resp_code = 100;
  mb_query->checksum = 0x00;
  mb_query->resv = 0x00;
  mb_query->s = 0x00;
  mb_query->qrv = 0x00;
  mb_query->qqic = 0x08;
  mb_query->group_addr = 0;
  mb_query->num_sources = 0;

  uint16_t checksum = mcast_msg_checksum(MEMBERSHIP_QUERY, mb_query, mb_query_len);
  if (!checksum) {
    err("%s(%d), err checksum %d\n", __func__, checksum, port);
    rte_pktmbuf_free(pkt);
    return -EIO;
  }
  mb_query->checksum = htons(checksum);

  ip_hdr->total_length = htons(sizeof(struct rte_ipv4_hdr) + mb_query_len);
  mt_mbuf_init_ipv4(pkt);
  pkt->pkt_len = pkt->l2_len + pkt->l3_len + mb_query_len;
  pkt->data_len = pkt->pkt_len;

  uint16_t tx = mt_sys_queue_tx_burst(impl, port, &pkt, 1);
  if (tx < 1) {
    err("%s(%d), send pkt fail\n", __func__, port);
    rte_pktmbuf_free(pkt);
    return -EIO;
  }

  return 0;
}
#endif

/* membership report shaping, refer to RFC3376 - 4.2 */
static int mcast_membership_report_on_query(struct mtl_main_impl* impl,
                                            enum mtl_port port, uint32_t group_filter,
                                            uint32_t src_filter) {
  struct mt_mcast_impl* mcast = get_mcast(impl, port);
  struct rte_mbuf* pkt;
  struct rte_ipv4_hdr* ip_hdr;
  struct mcast_mb_report_v3* mb_report;
  size_t hdr_offset = 0;
  size_t mb_report_len = sizeof(struct mcast_mb_report_v3);
  uint16_t record_num = 0;

  if (!mcast) return 0;

  mt_pthread_mutex_lock(&mcast->group_mutex);
  if (mcast->group_num == 0) {
    mt_pthread_mutex_unlock(&mcast->group_mutex);
    dbg("%s(%d), no group to report\n", __func__, port);
    return 0;
  }

  pkt = rte_pktmbuf_alloc(mt_sys_tx_mempool(impl, port));
  if (!pkt) {
    mt_pthread_mutex_unlock(&mcast->group_mutex);
    err("%s(%d), report packet alloc failed\n", __func__, port);
    return -ENOMEM;
  }

  ip_hdr = mcast_fill_ipv4(impl, port, pkt);
  hdr_offset += sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr);

  mb_report = rte_pktmbuf_mtod_offset(pkt, struct mcast_mb_report_v3*, hdr_offset);
  mb_report->type = MEMBERSHIP_REPORT_V3;
  mb_report->reserved_1 = 0x00;
  mb_report->checksum = 0x00;
  mb_report->reserved_2 = 0x00;
  hdr_offset += sizeof(struct mcast_mb_report_v3);
  uint8_t* group_record_addr = rte_pktmbuf_mtod_offset(pkt, uint8_t*, hdr_offset);

  struct mt_mcast_group_entry* group;
  TAILQ_FOREACH(group, &mcast->group_list, entries) {
    if (group_filter && group->group_ip != group_filter) continue;
    if (src_filter && !mcast_group_has_src(group, src_filter)) continue;

    size_t record_len = mcast_create_group_record_on_query(
        group, (struct mcast_group_record*)group_record_addr);
    group_record_addr += record_len;
    mb_report_len += record_len;
    record_num++;
  }
  mt_pthread_mutex_unlock(&mcast->group_mutex);

  if (!record_num) {
    rte_pktmbuf_free(pkt);
    return 0;
  }
  mb_report->num_group_records = htons(record_num);

  uint16_t checksum = mcast_msg_checksum(MEMBERSHIP_REPORT_V3, mb_report, mb_report_len);
  if (!checksum) {
    err("%s(%d), err checksum %d\n", __func__, port, checksum);
    rte_pktmbuf_free(pkt);
    return -EIO;
  }
  mb_report->checksum = htons(checksum);

  ip_hdr->total_length = htons(sizeof(struct rte_ipv4_hdr) + mb_report_len);
  mt_mbuf_init_ipv4(pkt);
  pkt->pkt_len = pkt->l2_len + pkt->l3_len + mb_report_len;
  pkt->data_len = pkt->pkt_len;

#ifdef MCAST_DEBUG
  /* send packet to kernel for capturing */
  if (mt_has_virtio_user(impl, port)) {
    struct mt_interface* inf = mt_if(impl, port);
    rte_eth_tx_burst(inf->virtio_port_id, 0, &pkt, 1);
  }
#endif

  uint16_t tx = mt_sys_queue_tx_burst(impl, port, &pkt, 1);
  if (tx < 1) {
    err("%s(%d), send pkt fail\n", __func__, port);
    rte_pktmbuf_free(pkt);
    return -EIO;
  }

  dbg("%s(%d), send pkt, records %d len %" PRIu64 "\n", __func__, port, record_num,
      mb_report_len);

  return 0;
}

static int mcast_membership_report_on_action(struct mtl_main_impl* impl,
                                             enum mtl_port port, uint32_t group_addr,
                                             uint8_t record_type, uint32_t src_addr,
                                             uint16_t src_num, uint32_t* src_addrs) {
  struct rte_mbuf* pkt;
  struct rte_ipv4_hdr* ip_hdr;
  struct mcast_mb_report_v3* mb_report;
  struct mcast_group_record* group_record;
  size_t hdr_offset = 0;
  size_t mb_report_len = sizeof(struct mcast_mb_report_v3);

  pkt = rte_pktmbuf_alloc(mt_sys_tx_mempool(impl, port));
  if (!pkt) {
    err("%s(%d), report packet alloc failed\n", __func__, port);
    return -ENOMEM;
  }

  ip_hdr = mcast_fill_ipv4(impl, port, pkt);
  hdr_offset += sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr);

  mb_report = rte_pktmbuf_mtod_offset(pkt, struct mcast_mb_report_v3*, hdr_offset);
  mb_report->type = MEMBERSHIP_REPORT_V3;
  mb_report->reserved_1 = 0x00;
  mb_report->checksum = 0x00;
  mb_report->reserved_2 = 0x00;
  mb_report->num_group_records = htons(1);
  hdr_offset += sizeof(struct mcast_mb_report_v3);
  group_record = rte_pktmbuf_mtod_offset(pkt, struct mcast_group_record*, hdr_offset);
  mb_report_len += mcast_create_group_record_on_action(group_addr, record_type, src_addr,
                                                       src_num, src_addrs, group_record);

  uint16_t checksum = mcast_msg_checksum(MEMBERSHIP_REPORT_V3, mb_report, mb_report_len);
  if (!checksum) {
    err("%s(%d), err checksum %d\n", __func__, checksum, port);
    rte_pktmbuf_free(pkt);
    return -EIO;
  }
  mb_report->checksum = htons(checksum);

  ip_hdr->total_length = htons(sizeof(struct rte_ipv4_hdr) + mb_report_len);
  mt_mbuf_init_ipv4(pkt);
  pkt->pkt_len = pkt->l2_len + pkt->l3_len + mb_report_len;
  pkt->data_len = pkt->pkt_len;

#ifdef MCAST_DEBUG
  /* send packet to kernel for capturing */
  if (mt_has_virtio_user(impl, port)) {
    struct mt_interface* inf = mt_if(impl, port);
    rte_eth_tx_burst(inf->virtio_port_id, 0, &pkt, 1);
  }
#endif
  /* send membership report twice */
  struct rte_mbuf* pkt_copy = rte_pktmbuf_copy(pkt, pkt->pool, 0, UINT32_MAX);
  if (!pkt_copy) {
    err("%s(%d), report copy alloc failed\n", __func__, port);
    rte_pktmbuf_free(pkt);
    return -ENOMEM;
  }

  uint16_t tx = mt_sys_queue_tx_burst(impl, port, &pkt, 1);
  if (tx < 1) {
    err("%s(%d), send pkt fail\n", __func__, port);
    rte_pktmbuf_free(pkt);
    rte_pktmbuf_free(pkt_copy);
    return -EIO;
  }

  tx = mt_sys_queue_tx_burst(impl, port, &pkt_copy, 1);
  if (tx < 1) {
    err("%s(%d), send pkt fail\n", __func__, port);
    rte_pktmbuf_free(pkt_copy);
    return -EIO;
  }

  info("%s(%d), send record %u pkt, mb_report_len %" PRIu64 "\n", __func__, port,
       record_type, mb_report_len);

  return 0;
}

static void mcast_membership_report_cb(void* param) {
  struct mtl_main_impl* impl = (struct mtl_main_impl*)param;
  int num_ports = mt_num_ports(impl);

  int ret;

  for (int port = 0; port < num_ports; port++) {
    struct mt_mcast_impl* mcast = get_mcast(impl, port);
    if (!mcast) continue;
    if (!mcast_has_external_query(impl, mcast)) {
      ret = mcast_membership_report_on_query(impl, port, 0, 0);
      if (ret < 0) {
        err("%s(%d), mcast_membership_report fail %d\n", __func__, port, ret);
      }
    }
  }

  ret = rte_eal_alarm_set(IGMP_JOIN_GROUP_PERIOD_US, mcast_membership_report_cb, impl);
  if (ret < 0) err("%s, set igmp alarm fail %d\n", __func__, ret);
}

static int mcast_addr_pool_extend(struct mt_interface* inf) {
  struct rte_ether_addr* mc_list;
  size_t mc_list_size;

  if ((inf->mcast_nb % MT_MCAST_POOL_INC) != 0) {
    inf->mcast_nb++;
    return 0;
  }

  /*
   * [re]allocate a pool with MCAST_POOL_INC more entries.
   * The previous test guarantees that port->mc_addr_nb is a multiple
   * of MCAST_POOL_INC.
   */
  mc_list_size = sizeof(struct rte_ether_addr) * (inf->mcast_nb + MT_MCAST_POOL_INC);
  mc_list = (struct rte_ether_addr*)realloc(inf->mcast_mac_lists, mc_list_size);
  if (mc_list == NULL) {
    return -ENOMEM;
  }

  inf->mcast_mac_lists = mc_list;
  inf->mcast_nb++;
  return 0;
}

static int mcast_addr_pool_append(struct mt_interface* inf,
                                  struct rte_ether_addr* mc_addr) {
  if (mcast_addr_pool_extend(inf) != 0) return -1;
  rte_ether_addr_copy(mc_addr, &inf->mcast_mac_lists[inf->mcast_nb - 1]);
  return 0;
}

static void mcast_addr_pool_remove(struct mt_interface* inf, uint32_t addr_idx) {
  inf->mcast_nb--;
  if (addr_idx == inf->mcast_nb) {
    /* No need to recompose the set of multicast address. */
    if (inf->mcast_nb == 0) {
      /* free the pool of multicast addresses. */
      free(inf->mcast_mac_lists);
      inf->mcast_mac_lists = NULL;
    }
    return;
  }
  memmove(&inf->mcast_mac_lists[addr_idx], &inf->mcast_mac_lists[addr_idx + 1],
          sizeof(struct rte_ether_addr) * (inf->mcast_nb - addr_idx));
}

static int mcast_inf_add_mac(struct mt_interface* inf, struct rte_ether_addr* mcast_mac) {
  uint16_t port_id = inf->port_id;
  uint32_t i;

  /*
   * Check that the added multicast MAC address is not already recorded
   * in the pool of multicast addresses.
   */
  for (i = 0; i < inf->mcast_nb; i++) {
    if (rte_is_same_ether_addr(mcast_mac, &inf->mcast_mac_lists[i])) {
      return 0;
    }
  }

  mcast_addr_pool_append(inf, mcast_mac);
  if (inf->drv_info.flags & MT_DRV_F_USE_MC_ADDR_LIST)
    return rte_eth_dev_set_mc_addr_list(port_id, inf->mcast_mac_lists, inf->mcast_nb);
  else
    return rte_eth_dev_mac_addr_add(port_id, mcast_mac, 0);
}

static int mcast_inf_remove_mac(struct mt_interface* inf,
                                struct rte_ether_addr* mcast_mac) {
  uint32_t i;
  uint16_t port_id = inf->port_id;

  /*
   * Search the pool of multicast MAC addresses for the removed address.
   */
  for (i = 0; i < inf->mcast_nb; i++) {
    if (rte_is_same_ether_addr(mcast_mac, &inf->mcast_mac_lists[i])) break;
  }
  if (i == inf->mcast_nb) {
    return 0;
  }

  mcast_addr_pool_remove(inf, i);
  if (inf->drv_info.flags & MT_DRV_F_USE_MC_ADDR_LIST)
    return rte_eth_dev_set_mc_addr_list(port_id, inf->mcast_mac_lists, inf->mcast_nb);
  else
    return rte_eth_dev_mac_addr_remove(port_id, mcast_mac);
}

/* 224.0.0.1 */
static struct rte_ether_addr mcast_mac_all = {{0x01, 0x00, 0x5e, 0x00, 0x00, 0x01}};

int mt_mcast_init(struct mtl_main_impl* impl) {
  int num_ports = mt_num_ports(impl);
  int socket = mt_socket_id(impl, MTL_PORT_P);
  bool has_mcast = false;

  if (mt_user_no_multicast(impl)) {
    info("%s, skip multicast mgr as MTL_FLAG_NO_MULTICAST\n", __func__);
    return 0;
  }

  for (int i = 0; i < num_ports; i++) {
    if (mt_drv_mcast_in_dp(impl, i)) continue;
    struct mt_mcast_impl* mcast = mt_rte_zmalloc_socket(sizeof(*mcast), socket);
    if (!mcast) {
      err("%s(%d), mcast malloc fail\n", __func__, i);
      mt_mcast_uinit(impl);
      return -ENOMEM;
    }

    mt_pthread_mutex_init(&mcast->group_mutex, NULL);

    TAILQ_INIT(&mcast->group_list);
    mcast->last_query_tsc = 0;

    /* assign mcast instance */
    impl->mcast[i] = mcast;

    if (!mt_drv_use_kernel_ctl(impl, i)) {
      int ret = mcast_inf_add_mac(mt_if(impl, i), &mcast_mac_all);
      if (ret < 0) {
        err("%s(%d), add allsystems mac fail %d\n", __func__, i, ret);
        mt_mcast_uinit(impl);
        return ret;
      }
    }

    has_mcast = true;
  }

  if (has_mcast) {
    int ret =
        rte_eal_alarm_set(IGMP_JOIN_GROUP_PERIOD_US, mcast_membership_report_cb, impl);
    if (ret < 0)
      err("%s, set igmp alarm fail %d\n", __func__, ret);
    else
      info("%s, report every %d seconds\n", __func__, IGMP_JOIN_GROUP_PERIOD_S);
  }

  dbg("%s, succ\n", __func__);
  return 0;
}

static void mcast_group_clear(struct mt_mcast_group_list* group_list) {
  struct mt_mcast_group_entry *group, *temp_group;

  group = TAILQ_FIRST(group_list);
  while (group != NULL) {
    temp_group = TAILQ_NEXT(group, entries);

    TAILQ_REMOVE(group_list, group, entries);
    mcast_group_free(group);
    group = temp_group;
  }
}

int mt_mcast_uinit(struct mtl_main_impl* impl) {
  int num_ports = mt_num_ports(impl);

  bool has_mcast = false;

  for (int i = 0; i < num_ports; i++) {
    struct mt_mcast_impl* mcast = get_mcast(impl, i);
    if (!mcast) continue;

    if (!mt_drv_use_kernel_ctl(impl, i))
      mcast_inf_remove_mac(mt_if(impl, i), &mcast_mac_all);

    /* clear group list */
    mcast_group_clear(&mcast->group_list);

    mt_pthread_mutex_destroy(&mcast->group_mutex);

    /* free the memory */
    mt_rte_free(mcast);
    impl->mcast[i] = NULL;

    has_mcast = true;
  }

  if (has_mcast) {
    int ret = rte_eal_alarm_cancel(mcast_membership_report_cb, impl);
    if (ret < 0) err("%s, alarm cancel fail %d\n", __func__, ret);
  }

  dbg("%s, succ\n", __func__);
  return 0;
}

/* add a group address with/without source address to the group ip list */
int mt_mcast_join(struct mtl_main_impl* impl, uint32_t group_addr, uint32_t source_addr,
                  enum mtl_port port) {
  struct mt_mcast_impl* mcast = get_mcast(impl, port);
  struct mt_interface* inf = mt_if(impl, port);
  uint8_t* ip = (uint8_t*)&group_addr;
  struct rte_ether_addr mcast_mac;

  if (mt_user_no_multicast(impl) || mt_drv_mcast_in_dp(impl, port)) return 0;
  if (!mcast) return -EIO;

  mt_pthread_mutex_lock(&mcast->group_mutex);

  if (mcast->group_num >= MT_MCAST_GROUP_MAX) {
    mt_pthread_mutex_unlock(&mcast->group_mutex);
    err("%s(%d), reach max multicast group number!\n", __func__, port);
    return -EIO;
  }

  struct mt_mcast_group_entry* group = NULL;
  TAILQ_FOREACH(group, &mcast->group_list, entries) {
    if (group->group_ip == group_addr) break;
  }

  bool group_new = false;
  uint16_t src_num_before = 0;
  if (group) {
    group->group_ref_cnt++;
    src_num_before = group->src_num;
  } else {
    group = mt_rte_zmalloc_socket(sizeof(*group), mt_socket_id(impl, port));
    if (!group) {
      err("%s(%d), malloc group fail\n", __func__, port);
      mt_pthread_mutex_unlock(&mcast->group_mutex);
      return -ENOMEM;
    }
    group->group_ip = group_addr;
    group->group_ref_cnt = 1;
    group->any_src_ref_cnt = 0;
    group->src_num = 0;
    TAILQ_INIT(&group->src_list);

    if (!mt_drv_use_kernel_ctl(impl, port)) {
      mt_mcast_ip_to_mac(ip, &mcast_mac);
      int ret = mcast_inf_add_mac(inf, &mcast_mac);
      if (ret < 0) {
        err("%s(%d), add mcast mac fail %d\n", __func__, port, ret);
        mt_rte_free(group);
        mt_pthread_mutex_unlock(&mcast->group_mutex);
        return ret;
      }
    }

    TAILQ_INSERT_TAIL(&mcast->group_list, group, entries);
    mcast->group_num++;
    group_new = true;
  }

  bool source_new = false;
  if (!source_addr) {
    group->any_src_ref_cnt++;
  } else {
    struct mt_mcast_src_entry* src;
    TAILQ_FOREACH(src, &group->src_list, entries) {
      if (src->src_ip == source_addr) {
        src->src_ref_cnt++;
        break;
      }
    }
    if (!src) {
      src = mt_rte_zmalloc_socket(sizeof(*src), mt_socket_id(impl, port));
      if (!src) {
        err("%s(%d), mt_mcast_src malloc fail\n", __func__, port);
        if (group_new) {
          TAILQ_REMOVE(&mcast->group_list, group, entries);
          mcast->group_num--;
          if (!mt_drv_use_kernel_ctl(impl, port)) {
            mt_mcast_ip_to_mac(ip, &mcast_mac);
            mcast_inf_remove_mac(inf, &mcast_mac);
          }
          mcast_group_free(group);
        } else if (group->group_ref_cnt) {
          group->group_ref_cnt--;
        }
        mt_pthread_mutex_unlock(&mcast->group_mutex);
        return -ENOMEM;
      }
      src->src_ip = source_addr;
      src->src_ref_cnt = 1;
      TAILQ_INSERT_TAIL(&group->src_list, src, entries);
      group->src_num++;
      source_new = true;
    }
  }

  uint8_t record_type = 0;
  if (group_new && !source_addr) {
    record_type = MCAST_CHANGE_TO_EXCLUDE_MODE;
  } else if (source_new && group->any_src_ref_cnt == 0) {
    record_type = src_num_before ? MCAST_ALLOW_NEW_SOURCES : MCAST_CHANGE_TO_INCLUDE_MODE;
  }
  mt_pthread_mutex_unlock(&mcast->group_mutex);

  if (record_type) {
    int ret = mcast_membership_report_on_action(impl, port, group_addr, record_type,
                                                source_addr, 0, NULL);
    if (ret < 0) {
      err("%s(%d), send membership report fail\n", __func__, port);
      return ret;
    }
  }

  info("%s(%d), join group %d.%d.%d.%d\n", __func__, port, ip[0], ip[1], ip[2], ip[3]);
  if (source_addr) {
    ip = (uint8_t*)&source_addr;
    info("%s(%d), with source %d.%d.%d.%d\n", __func__, port, ip[0], ip[1], ip[2], ip[3]);
  }

  return 0;
}

/* delete a group address with/without source address from the group ip list */
int mt_mcast_leave(struct mtl_main_impl* impl, uint32_t group_addr, uint32_t source_addr,
                   enum mtl_port port) {
  if (mt_user_no_multicast(impl) || mt_drv_mcast_in_dp(impl, port)) return 0;

  struct mt_mcast_impl* mcast = get_mcast(impl, port);
  struct mt_interface* inf = mt_if(impl, port);
  uint8_t* ip = (uint8_t*)&group_addr;
  struct rte_ether_addr mcast_mac;
  uint32_t* src_addrs = NULL;
  uint16_t src_num = 0;

  if (!mcast) return -EIO;

  mt_pthread_mutex_lock(&mcast->group_mutex);
  /* No groups to proceed*/
  if (mcast->group_num == 0) {
    mt_pthread_mutex_unlock(&mcast->group_mutex);
    return 0;
  }
  /* find existing group */
  struct mt_mcast_group_entry* group = NULL;
  TAILQ_FOREACH(group, &mcast->group_list, entries) {
    if (group->group_ip == group_addr) break;
  }

  if (!group) {
    mt_pthread_mutex_unlock(&mcast->group_mutex);
    warn("%s(%d), group ip not found, nothing to delete\n", __func__, port);
    return 0;
  }

  uint16_t old_any_ref = group->any_src_ref_cnt;
  bool source_deleted = false;
  if (source_addr) {
    struct mt_mcast_src_entry* src;
    TAILQ_FOREACH(src, &group->src_list, entries) {
      if (src->src_ip != source_addr) continue;
      if (src->src_ref_cnt && --src->src_ref_cnt == 0) {
        TAILQ_REMOVE(&group->src_list, src, entries);
        mt_rte_free(src);
        group->src_num--;
        source_deleted = true;
      }
      break;
    }
  } else if (group->any_src_ref_cnt) {
    group->any_src_ref_cnt--;
  }

  bool group_deleted = false;
  if (group->group_ref_cnt && --group->group_ref_cnt == 0) {
    TAILQ_REMOVE(&mcast->group_list, group, entries);
    mcast->group_num--;
    group_deleted = true;

    // If we are not using kernel control, remove multicast MAC from interface
    if (!mt_drv_use_kernel_ctl(impl, port)) {
      mt_mcast_ip_to_mac(ip, &mcast_mac);
      int ret = mcast_inf_remove_mac(inf, &mcast_mac);
      if (ret < 0) err("%s(%d), remove mcast mac fail %d\n", __func__, port, ret);
    }
  }

  uint8_t record_type = 0;
  uint32_t report_src = source_addr;
  if (group_deleted) {
    record_type = MCAST_CHANGE_TO_INCLUDE_MODE;
    report_src = 0;
  } else if (source_deleted && group->any_src_ref_cnt == 0) {
    record_type = group->src_num ? MCAST_BLOCK_OLD_SOURCES : MCAST_CHANGE_TO_INCLUDE_MODE;
    if (!group->src_num) report_src = 0;
  } else if (!source_addr && old_any_ref && group->any_src_ref_cnt == 0 &&
             group->src_num) {
    record_type = MCAST_CHANGE_TO_INCLUDE_MODE;
    report_src = 0;
  }

  if (record_type == MCAST_CHANGE_TO_INCLUDE_MODE && !group_deleted && group->src_num) {
    src_addrs = mt_rte_zmalloc_socket(sizeof(*src_addrs) * group->src_num,
                                      mt_socket_id(impl, port));
    if (!src_addrs) {
      err("%s(%d), source snapshot malloc fail\n", __func__, port);
    } else {
      struct mt_mcast_src_entry* src;
      TAILQ_FOREACH(src, &group->src_list, entries) {
        src_addrs[src_num++] = src->src_ip;
      }
    }
  }
  mt_pthread_mutex_unlock(&mcast->group_mutex);

  if (record_type) {
    int ret = mcast_membership_report_on_action(impl, port, group_addr, record_type,
                                                report_src, src_num, src_addrs);
    if (ret < 0) err("%s(%d), send leave report failed\n", __func__, port);
  }

  info("%s(%d), leave group %d.%d.%d.%d\n", __func__, port, ip[0], ip[1], ip[2], ip[3]);
  if (src_addrs) mt_rte_free(src_addrs);
  if (group_deleted) mcast_group_free(group);

  return 0;
}

int mt_mcast_l2_join(struct mtl_main_impl* impl, struct rte_ether_addr* addr,
                     enum mtl_port port) {
  struct mt_interface* inf = mt_if(impl, port);
  if (mt_drv_use_kernel_ctl(impl, port)) return 0;
  return mcast_inf_add_mac(inf, addr);
}

int mt_mcast_l2_leave(struct mtl_main_impl* impl, struct rte_ether_addr* addr,
                      enum mtl_port port) {
  struct mt_interface* inf = mt_if(impl, port);
  if (mt_drv_use_kernel_ctl(impl, port)) return 0;
  return mcast_inf_remove_mac(inf, addr);
}

int mt_mcast_restore(struct mtl_main_impl* impl, enum mtl_port port) {
  struct mt_interface* inf = mt_if(impl, port);
  uint16_t port_id = inf->port_id;

  if (inf->drv_info.flags & MT_DRV_F_USE_MC_ADDR_LIST) {
    rte_eth_dev_set_mc_addr_list(port_id, inf->mcast_mac_lists, inf->mcast_nb);
  } else {
    for (uint32_t i = 0; i < inf->mcast_nb; i++)
      rte_eth_dev_mac_addr_add(port_id, &inf->mcast_mac_lists[i], 0);
  }
  mcast_membership_report_on_query(impl, port, 0, 0);
  return 0;
}

int mt_mcast_parse(struct mtl_main_impl* impl, struct mcast_mb_query_v3* query,
                   size_t query_len, enum mtl_port port) {
  if (query_len < sizeof(*query)) {
    err("%s(%d), short igmp query %zu\n", __func__, port, query_len);
    return -EIO;
  }

  if (query->type != MEMBERSHIP_QUERY) return 0;

  size_t src_bytes = (size_t)ntohs(query->num_sources) * sizeof(uint32_t);
  if (src_bytes > query_len - sizeof(*query)) {
    err("%s(%d), short igmp source list\n", __func__, port);
    return -EIO;
  }

  uint16_t query_checksum = ntohs(query->checksum);
  query->checksum = 0;
  uint16_t checksum = mcast_msg_checksum(MEMBERSHIP_QUERY, query, query_len);
  query->checksum = query_checksum;
  if (checksum != query_checksum) {
    err("%s(%d), err checksum %d:%d\n", __func__, port, query_checksum, checksum);
    return -EIO;
  }

  struct mt_mcast_impl* mcast = get_mcast(impl, port);
  if (!mcast) return 0;

  if (!mcast_has_external_query(impl, mcast))
    info("%s(%d), received igmp query, stop auto-join\n", __func__, port);
  mcast->last_query_tsc = mt_get_tsc(impl);

  uint32_t group_filter = query->group_addr;
  uint32_t src_filter = 0;
  if (query->s && ntohs(query->num_sources)) src_filter = query->source_addr[0];

  return mcast_membership_report_on_query(impl, port, group_filter, src_filter);
}