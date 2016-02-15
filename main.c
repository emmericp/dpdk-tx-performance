#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <getopt.h>

#include <rte_config.h>
#include <rte_common.h>
#include <rte_log.h>
#include <rte_memory.h>
#include <rte_eal.h>
#include <rte_cycles.h>
#include <rte_pci.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>


#define MBUF_SIZE			(2048 + sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM)
#define NB_MBUF				8192
#define MEMPOOL_CACHE_SIZE	256

#define RX_PTHRESH 8
#define RX_HTHRESH 8
#define RX_WTHRESH 4

#define TX_PTHRESH 36
#define TX_HTHRESH 0
#define TX_WTHRESH 0

#define TX_DESCS 512
#define RX_DESCS 512
#define BATCH_SIZE 64

static const struct rte_eth_conf port_conf = {
	.rxmode = {
		.split_hdr_size = 0,
		.header_split   = 0,
		.hw_ip_checksum = 0,
		.hw_vlan_filter = 0,
		.jumbo_frame    = 0,
		.hw_strip_crc   = 0,
	},
	.txmode = {
		.mq_mode = ETH_MQ_TX_NONE,
	},
};

static const struct rte_eth_rxconf rx_conf = {
	.rx_thresh = {
		.pthresh = RX_PTHRESH,
		.hthresh = RX_HTHRESH,
		.wthresh = RX_WTHRESH,
	},
};

static struct rte_eth_txconf tx_conf = {
	.tx_thresh = {
		.pthresh = TX_PTHRESH,
		.hthresh = TX_HTHRESH,
		.wthresh = TX_WTHRESH,
	},
	.tx_free_thresh = 0,
	.tx_rs_thresh = 0,
	.txq_flags = 0, // changed during config
};


static struct rte_mempool* make_mempool() {
	static int pool_id = 0;
	char pool_name[32];
	sprintf(pool_name, "pool%d", __sync_fetch_and_add(&pool_id, 1));
	return rte_mempool_create(pool_name, NB_MBUF, MBUF_SIZE, MEMPOOL_CACHE_SIZE,
		sizeof(struct rte_pktmbuf_pool_private),
		rte_pktmbuf_pool_init, NULL,
		rte_pktmbuf_init, NULL,
		rte_socket_id(), 0
	);
}


static bool config_port(uint8_t port, bool simple_tx) {
	int rc = rte_eth_dev_configure(port, 1, 1, &port_conf);
	if (rc) {
		printf("Configure %d failed: %d\n", port, rc);
		return false;
	}
	tx_conf.txq_flags = simple_tx
		? ETH_TXQ_FLAGS_NOMULTSEGS | ETH_TXQ_FLAGS_NOOFFLOADS
		: ETH_TXQ_FLAGS_NOMULTSEGS;
	rc = rte_eth_tx_queue_setup(port, 0, TX_DESCS, rte_socket_id(), &tx_conf);
	if (rc) {
		printf("could not configure tx queue on port %d: %d\n", port, rc);
		return false;
	}
	// dev_start segfaults without a rx queue
	rc = rte_eth_rx_queue_setup(port, 0, RX_DESCS, rte_socket_id(), &rx_conf, make_mempool());
	if (rc) {
		printf("could not configure tx queue on port %d: %d\n", port, rc);
		return false;
	}
	rc = rte_eth_dev_start(port);
	if (rc) {
		printf("failed to start port %d: %d\n", port, rc);
		return false;
	}
	return true;
}

static uint32_t send_pkts(uint8_t port, struct rte_mempool* pool) {
	static uint64_t seq = 0;
	// alloc bufs
	struct rte_mbuf* bufs[BATCH_SIZE];
	uint32_t i;
	for (i = 0; i < BATCH_SIZE; i++) {
		struct rte_mbuf* buf = rte_pktmbuf_alloc(pool);
		rte_pktmbuf_data_len(buf) = 60;
		rte_pktmbuf_pkt_len(buf) = 60;
		bufs[i] = buf;
		// write seq number
		uint64_t* pkt = rte_pktmbuf_mtod(buf, uint64_t*);
		pkt[0] = seq++;
	}
	// send pkts
	uint32_t sent = 0;
	while (1) {
		sent += rte_eth_tx_burst(port, 0, bufs + sent, BATCH_SIZE - sent);
		if (sent >= BATCH_SIZE) {
			return sent;
		}
	}
}

int main(int argc, char **argv) {
	// eal args
	int num_args = rte_eal_init(argc, argv);
	if (num_args < 0)
		rte_exit(EXIT_FAILURE, "init failed");
	argc -= num_args;
	argv += num_args;

	// our args: [-s] port1 port2
	uint8_t port1, port2;
	char opt = getopt(argc, argv, "s");
	bool simple_tx = opt == 's';
	if (simple_tx) {
		printf("Requesting simple tx path\n");
		argc--;
		argv++;
	} else {
		printf("Requesting full-featured tx path\n");
	}
	if (argc != 3) {
		printf("usage: [-s] port1 port2\n");
		return -1;
	}
	port1 = atoi(argv[1]);
	port2 = atoi(argv[2]);
	printf("Using ports %d and %d\n", port1, port2);

	if (!config_port(port1, simple_tx)) return -1;
	if (!config_port(port2, simple_tx)) return -1;

	struct rte_mempool* pool = make_mempool();
	
	uint64_t sent = 0;
	uint64_t next_print = rte_get_tsc_hz();
	uint64_t last_sent = 0;
	while (true) {
		sent += send_pkts(port1, pool);
		sent += send_pkts(port2, pool);
		uint64_t time = rte_rdtsc();
		if (time >= next_print) {
			double elapsed = (time - next_print + rte_get_tsc_hz()) / rte_get_tsc_hz();
			uint64_t pkts = sent - last_sent;
			printf("Packet rate: %.2f Mpps\n", (double) pkts / elapsed / 1000000);
			next_print = time + rte_get_tsc_hz();
			last_sent = sent;
		}
	}

	return 0;
}

