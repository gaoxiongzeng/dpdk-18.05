/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2013 Tilera Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Tilera Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <inttypes.h>
#include <stdlib.h>

#include <sys/queue.h>
#include <sys/stat.h>

#include <rte_common.h>
#include <rte_byteorder.h>
#include <rte_log.h>
#include <rte_debug.h>
#include <rte_cycles.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_memzone.h>
#include <rte_launch.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_atomic.h>
#include <rte_branch_prediction.h>
#include <rte_memory.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_interrupts.h>
#include <rte_pci.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_udp.h>
#include <rte_string_fns.h>
#include <rte_timer.h>
#include <rte_malloc.h>

#include "testpmd.h"

#define min(X,Y) ((X) < (Y) ? (X) : (Y))
#define max(X,Y) ((X) > (Y) ? (X) : (Y))

#define ENABLE_AEOLUS 1

#define SERVERNUM 3 // including one warm-up server
static struct   ether_addr eth_addr_array[SERVERNUM];
static uint32_t ip_addr_array[SERVERNUM];

int verbose           = 2; // verbose = 0 guarantees best performance
int total_flow_num    = 6; // total flows for all servers 
int current_server_id = 1;

/* Configuration files to be placed in app/test-pmd/config/ */
/* The first line (server_id=0) is used for warm-up receiver */
static const char ethaddr_filename[] = "app/test-pmd/config/eth_addr_info.txt";
static const char ipaddr_filename[]  = "app/test-pmd/config/ip_addr_info.txt";
/* The first few lines are used for warm-up flows */
static const char flow_filename[]    = "app/test-pmd/config/flow_info_test.txt";

#define DEFAULT_PKT_SIZE 1500
#define L2_LEN sizeof(struct ether_hdr)
#define L3_LEN sizeof(struct ipv4_hdr)
#define L4_LEN sizeof(struct tcp_hdr)
#define HDR_ONLY_SIZE (L2_LEN + L3_LEN + L4_LEN) 

/* Define ip header info */
#define IP_DEFTTL  64   /* from RFC 1340. */
#define IP_VERSION 0x40
#define IP_HDRLEN  0x05 /* default IP header length == five 32-bits words. */
#define IP_VHL_DEF (IP_VERSION | IP_HDRLEN)

/* Homa header info */
#define PT_HOMA_GRANT_REQUEST 0x10
#define PT_HOMA_GRANT 0x11
#define PT_HOMA_DATA 0x12
#define PT_HOMA_RESEND_REQUEST 0x13

/* Redefine TCP header fields for Homa */
#define PKT_TYPE_8BITS tcp_flags
#define FLOW_ID_16BITS rx_win
// Homa grant request header ONLY
#define FLOW_SIZE_LOW_16BITS tcp_urp
#define FLOW_SIZE_HIGH_16BITS cksum
// Homa grant header ONLY
#define PRIORITY_GRANTED_8BITS data_off 
#define SEQ_GRANTED_LOW_16BITS tcp_urp
#define SEQ_GRANTED_HIGH_16BITS cksum
// Homa data header ONLY
#define DATA_LEN_16BITS tcp_urp
// Homa resend request header ONLY
#define DATA_RESEND_16BITS tcp_urp

/* Homa states */
#define HOMA_SEND_UNSTARTED 0x00
#define HOMA_SEND_GRANT_REQUEST_SENT 0x01
#define HOMA_SEND_GRANT_RECEIVING 0x02
#define HOMA_SEND_CLOSED 0x03
#define HOMA_RECV_UNSTARTED 0x04
#define HOMA_RECV_GRANT_SENDING 0x05
#define HOMA_RECV_CLOSED 0x06

/* Homa transport configuration (parameters and variables) */
#define RTT_BYTES 20000 // Calculated based on BDP
#define MAX_GRANT_TRANSMIT_ONE_TIME 32
#define MAX_REQUEST_RETRANSMIT_ONE_TIME 16
#define RETRANSMIT_TIMEOUT 0.01
#define BURST_THRESHOLD 32

#define UNSCHEDULED_PRIORITY 6 // Unscheduled priority levels
#define SCHEDULED_PRIORITY 2 // Scheduled priority levels
static const int prio_cut_off_bytes[] = 
    {2000, 4000, 6000, 8000, 10000}; // Map message size to divided n+1 unscheduled priorities
static const int prio_map[] = {0, 1, 2, 3, 4, 5, 6, 7}; // 0-n from low to high priority

double start_cycle, elapsed_cycle;
double flowgen_start_time;
int    sync_time = 3; // in sec
double hz;
struct fwd_stream *global_fs;

struct flow_info {
    uint32_t dst_ip;
    uint32_t src_ip;
    uint16_t dst_port;
    uint16_t src_port;

    uint8_t  flow_state;
    uint32_t flow_size; /* flow total size */
    uint32_t remain_size; /* flow remain size */
    double   start_time;
    double   finish_time;
    int      fct_printed;
    int      flow_finished;

    uint32_t data_seqnum;
    uint32_t data_recv_next; 
    uint32_t granted_seqnum;
    uint32_t granted_priority;

    /* Used to detect grant request timeout */
    double last_grant_request_sent_time;
    
    /* Used to detect data timeout */
    double last_data_sent_time; /* to-do */

    /* Used to avoid duplicate grants and detect timeout */
    double last_grant_sent_time;
    double last_grant_granted_seq;
    double last_grant_granted_prio;
};
struct flow_info *sender_flows;
struct flow_info *receiver_flows;

struct rte_mbuf *sender_pkts_burst[MAX_PKT_BURST];
struct rte_mbuf *receiver_pkts_burst[MAX_PKT_BURST];
int sender_current_burst_size;
int receiver_current_burst_size;

/* sender task states */
int sender_total_flow_num              = 0; // sender flows for this server
int sender_grant_request_sent_flow_num = 0;
int sender_active_flow_num             = 0;
int sender_finished_flow_num           = 0;
int sender_next_unstart_flow_id        = -1;
int sender_current_burst_size          = 0;

#define MAX_CONCURRENT_FLOW 100
int sender_request_sent_flow_array[MAX_CONCURRENT_FLOW];
int sender_active_flow_array[MAX_CONCURRENT_FLOW];

/* receiver task states */
int receiver_total_flow_num     = 0; // receiver flows for this server
int receiver_active_flow_num    = 0;
int receiver_finished_flow_num  = 0;
int receiver_current_burst_size = 0;

int receiver_active_flow_array[MAX_CONCURRENT_FLOW];

/* declaration of functions */
static void
main_flowgen(struct fwd_stream *fs);

static void
start_new_flow(void);

static void
start_warm_up_flow(void);

static void
sender_send_pkt(void);

static void
receiver_send_pkt(void);

static void
recv_pkt(struct fwd_stream *fs);

static void
recv_grant_request(struct tcp_hdr *transport_recv_hdr, struct ipv4_hdr *ipv4_hdr);

static void
recv_grant(struct tcp_hdr *transport_recv_hdr);

static void
recv_data(struct tcp_hdr *transport_recv_hdr);

static void
recv_resend_request(struct tcp_hdr *transport_recv_hdr);

static void
process_ack(struct tcp_hdr* transport_recv_hdr);

static void
construct_grant_request(uint32_t flow_id);

static void
construct_grant(uint32_t flow_id, uint32_t seq_granted, uint8_t priority_granted);

static void
construct_data(uint32_t flow_id, uint32_t ack_seq, int data_type);

static void
construct_resend_request(uint32_t flow_id, uint16_t resend_size);

static void
init(void);

static void
read_config(void);

static inline uint16_t
ip_sum(const unaligned_uint16_t *hdr, int hdr_len);

static inline void
print_ether_addr(const char *what, struct ether_addr *eth_addr);

static inline void
add_sender_grant_request_sent_flow(int flow_id);

static inline void
add_receiver_active_flow(int flow_id);

static inline int
find_next_sender_grant_request_sent_flow(int start_index);

static inline void
remove_sender_grant_request_sent_flow(int flow_id);

static inline void
remove_receiver_active_flow(int flow_id);

static inline void
sort_receiver_active_flow_by_remaining_size(void);

static inline int
find_next_unstart_flow_id(void);

static inline void 
remove_newline(char *str);

static inline uint32_t
map_to_unscheduled_priority(int flow_size);

static inline int
get_src_server_id(uint32_t flow_id, struct flow_info *flows);

static inline int
get_dst_server_id(uint32_t flow_id, struct flow_info *flows);

static inline void
print_elapsed_time(void);

static void
print_fct(void);


static inline uint16_t
ip_sum(const unaligned_uint16_t *hdr, int hdr_len)
{
    uint32_t sum = 0;

    while (hdr_len > 1)
    {
        sum += *hdr++;
        if (sum & 0x80000000)
            sum = (sum & 0xFFFF) + (sum >> 16);
        hdr_len -= 2;
    }

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return ~sum;
}

static void 
remove_newline(char *str)
{
    for (uint32_t i = 0; i < strlen(str); i++) {
        if (str[i] == '\r' || str[i] == '\n')
            str[i] = '\0';
    }
}

static inline void
print_ether_addr(const char *what, struct ether_addr *eth_addr)
{
    char buf[ETHER_ADDR_FMT_SIZE];
    ether_format_addr(buf, ETHER_ADDR_FMT_SIZE, eth_addr);
    printf("%s%s", what, buf);
}

/* Map flow size to unscheduled priority */
static inline uint32_t
map_to_unscheduled_priority(int flow_size)
{
    for (int i=0; i<UNSCHEDULED_PRIORITY-1; i++) {
        if (flow_size<prio_cut_off_bytes[i])
            return i;
    }
    return (UNSCHEDULED_PRIORITY-1);
}


/* Map flow src ip to server id */
static inline int
get_src_server_id(uint32_t flow_id, struct flow_info *flows) 
{
    for (int server_index =0; server_index < SERVERNUM; server_index++) {
        if (flows[flow_id].src_ip == ip_addr_array[server_index]) {
            return server_index; 
        }
    }
    return -1;
}

/* Map flow dst ip to server id */
static inline int
get_dst_server_id(uint32_t flow_id, struct flow_info *flows) 
{
    for (int server_index =0; server_index < SERVERNUM; server_index++) {
        if (flows[flow_id].dst_ip == ip_addr_array[server_index]) {
            return server_index; 
        }
    }
    return -1;
}

static inline void
add_sender_grant_request_sent_flow(int flow_id)
{
    int i;
    for (i=0; i<MAX_CONCURRENT_FLOW; i++) {
        if (sender_request_sent_flow_array[i] < 0) {
            sender_request_sent_flow_array[i] = flow_id;
            break;
        }
    }
    if (i == MAX_CONCURRENT_FLOW) {
        printf("error: run out of memory for add_sender_grant_request_sent_flow\n");
    }
    sender_grant_request_sent_flow_num++;
    sender_next_unstart_flow_id = find_next_unstart_flow_id();
    sender_flows[flow_id].flow_state = HOMA_SEND_GRANT_REQUEST_SENT; 
    sender_flows[flow_id].last_grant_request_sent_time = rte_rdtsc() / (double)hz;
}

static inline void
add_receiver_active_flow(int flow_id)
{
    int i;
    for (i=0; i<MAX_CONCURRENT_FLOW; i++) {
        if (receiver_active_flow_array[i] < 0) {
            receiver_active_flow_array[i] = flow_id;
            break;
        }
    }
    if (i == MAX_CONCURRENT_FLOW) {
        printf("error: run out of memory for add_receiver_active_flow\n");
    }
    receiver_active_flow_num++;
    receiver_total_flow_num++;
    receiver_flows[flow_id].flow_state = HOMA_RECV_GRANT_SENDING;
}

static inline int
find_next_sender_grant_request_sent_flow(int start_index)
{
    for (int i=start_index; i<MAX_CONCURRENT_FLOW; i++) {
        if (sender_request_sent_flow_array[i] != -1) {
            return i;
        }
    }
    return -1;
}

static inline void
remove_receiver_active_flow(int flow_id)
{
    int i;
    for (i=0; i<MAX_CONCURRENT_FLOW; i++) {
        if (receiver_active_flow_array[i] == flow_id) {
            receiver_active_flow_array[i] = -1;
            break;
        }
    }
    if (i == MAX_CONCURRENT_FLOW) {
        printf("error: cannot find the node for remove_receiver_active_flow\n");
    }
    receiver_active_flow_num--;
    receiver_flows[flow_id].flow_state = HOMA_RECV_CLOSED;
    receiver_flows[flow_id].flow_finished = 1;
    receiver_flows[flow_id].finish_time = rte_rdtsc() / (double)hz;
    receiver_finished_flow_num++;
}

static inline void
remove_sender_grant_request_sent_flow(int flow_id)
{
    int i;
    for (i=0; i<MAX_CONCURRENT_FLOW; i++) {
        if (sender_request_sent_flow_array[i] == flow_id) {
            sender_request_sent_flow_array[i] = -1;
            break;
        }
    }
    if (i == MAX_CONCURRENT_FLOW) {
        printf("error: cannot find the node for remove_sender_grant_request_sent_flow\n");
    }
    sender_grant_request_sent_flow_num--;
    sender_flows[flow_id].flow_state = HOMA_SEND_GRANT_RECEIVING; 
}

static inline void
sort_receiver_active_flow_by_remaining_size(void)
{
    /* Selection sorting */
    int temp_id;
    for (int i=0; i<MAX_CONCURRENT_FLOW; i++) {
        for (int j=i+1; j<MAX_CONCURRENT_FLOW; j++) {
            if (receiver_active_flow_array[j] >= 0) {
                if (receiver_active_flow_array[i] >= 0) {
                    if (receiver_flows[receiver_active_flow_array[i]].remain_size >
                        receiver_flows[receiver_active_flow_array[j]].remain_size) {
                            temp_id = receiver_active_flow_array[i];
                            receiver_active_flow_array[i] = receiver_active_flow_array[j];
                            receiver_active_flow_array[j] = temp_id;
                        }
                }
                else {
                    receiver_active_flow_array[i] = receiver_active_flow_array[j];
                    receiver_active_flow_array[j] = -1;
                }
            }
        }
    }
}

static inline int
find_next_unstart_flow_id(void)
{
    int i;
    for (i=sender_next_unstart_flow_id+1; i<total_flow_num; i++) {
        if (get_src_server_id(i, sender_flows) == current_server_id)
            return i;
    }
    return i;
}

/* Update and print global elapsed time */
static inline void
print_elapsed_time(void)
{
    elapsed_cycle = rte_rdtsc() - start_cycle;
    printf("Time: %lf", elapsed_cycle/(double)hz);
}

static void
print_fct(void)
{
    printf("Summary:\ntotal_flow_num = %d (including %d warm up flows)\n"
        "sender_total_flow_num = %d (including 1 warm up flow)\n"
        "receiver_total_flow_num = %d\nReceiver FCT:\n",
        total_flow_num, SERVERNUM-1, sender_total_flow_num, receiver_total_flow_num);

    for (int i=0; i<total_flow_num; i++) {
        if (receiver_flows[i].fct_printed == 0 && receiver_flows[i].flow_finished == 1) {
             printf("%d %lf\n", i, 
                receiver_flows[i].finish_time - receiver_flows[i].start_time);
             receiver_flows[i].fct_printed = 1;
         }
    }
}

/* Read basic info of server id, mac and ip */
static void
read_config(void)
{
    FILE *fd = NULL;
    char line[256] = {0};
    int  server_id;
    uint32_t src_ip_segment1 = 0, src_ip_segment2 = 0, src_ip_segment3 = 0, src_ip_segment4 = 0;

    /* Read ethernet address info */
    server_id = 0;
    fd = fopen(ethaddr_filename, "r");
    if (!fd)
        printf("%s: no such file\n", ethaddr_filename);
    while (fgets(line, sizeof(line), fd) != NULL) {
        remove_newline(line);
        sscanf(line, "%hhu %hhu %hhu %hhu %hhu %hhu", &eth_addr_array[server_id].addr_bytes[0], 
            &eth_addr_array[server_id].addr_bytes[1], &eth_addr_array[server_id].addr_bytes[2],
            &eth_addr_array[server_id].addr_bytes[3], &eth_addr_array[server_id].addr_bytes[4], 
            &eth_addr_array[server_id].addr_bytes[5]);
        if (verbose > 0) {
           printf("Server id = %d   ", server_id);
           print_ether_addr("eth = ", &eth_addr_array[server_id]);
           printf("\n");
        }
        server_id++;
    }
    fclose(fd);

    /* Read ip address info */
    server_id = 0;
    fd = fopen(ipaddr_filename, "r");
    if (!fd)
        printf("%s: no such file\n", ipaddr_filename);
    while (fgets(line, sizeof(line), fd) != NULL) {
        remove_newline(line);
        sscanf(line, "%u %u %u %u", &src_ip_segment1, &src_ip_segment2, &src_ip_segment3, &src_ip_segment4);
        ip_addr_array[server_id] = IPv4(src_ip_segment1, src_ip_segment2, src_ip_segment3, src_ip_segment4);
        if (verbose > 0) {
            printf("Server id = %d   ", server_id);
            printf("ip = %u.%u.%u.%u (%u)\n", src_ip_segment1, src_ip_segment2, 
                src_ip_segment3, src_ip_segment4, ip_addr_array[server_id]);
        }
        server_id++;
    }
    fclose(fd);
}

/* Init flow info */
static void
init(void)
{
    char     line[256] = {0};
    uint32_t flow_id = 0;
    uint32_t src_ip_segment1 = 0, src_ip_segment2 = 0, src_ip_segment3 = 0, src_ip_segment4 = 0;
    uint32_t dst_ip_segment1 = 0, dst_ip_segment2 = 0, dst_ip_segment3 = 0, dst_ip_segment4 = 0;
    uint16_t udp_src_port = 0, udp_dst_port = 0;
    uint32_t flow_size = 0;
    double   start_time;
    
    FILE *fd = fopen(flow_filename, "r");
    if (!fd)
        printf("%s: no such file\n", flow_filename);

    while (fgets(line, sizeof(line), fd) != NULL) {
        remove_newline(line);
        sscanf(line, "%u %u %u %u %u %u %u %u %u %hu %hu %u %lf", &flow_id, 
            &src_ip_segment1, &src_ip_segment2, &src_ip_segment3, &src_ip_segment4, 
            &dst_ip_segment1, &dst_ip_segment2, &dst_ip_segment3, &dst_ip_segment4, 
            &udp_src_port, &udp_dst_port, &flow_size, &start_time);
        sender_flows[flow_id].src_ip               = IPv4(src_ip_segment1, src_ip_segment2, 
                                                          src_ip_segment3, src_ip_segment4);
        sender_flows[flow_id].dst_ip               = IPv4(dst_ip_segment1, dst_ip_segment2, 
                                                          dst_ip_segment3, dst_ip_segment4);
        sender_flows[flow_id].src_port             = udp_src_port;
        sender_flows[flow_id].dst_port             = udp_dst_port;
        sender_flows[flow_id].flow_size            = flow_size;
        sender_flows[flow_id].remain_size          = flow_size;
        sender_flows[flow_id].data_seqnum          = 1;
        sender_flows[flow_id].start_time           = start_time;
        sender_flows[flow_id].last_grant_sent_time = 0;
        sender_flows[flow_id].flow_state           = HOMA_SEND_UNSTARTED;

        receiver_flows[flow_id].flow_state = HOMA_RECV_UNSTARTED;

        if (get_src_server_id(flow_id, sender_flows) == current_server_id)
            sender_total_flow_num++;
        
        if (verbose > 1) {
            printf("Flow info: flow_id=%u, src_ip=%u, dst_ip=%u, "
                "src_port=%hu, dst_port=%hu, flow_size=%u, start_time=%lf\n",  
                flow_id, sender_flows[flow_id].src_ip, sender_flows[flow_id].dst_ip, 
                sender_flows[flow_id].src_port, sender_flows[flow_id].dst_port, 
                sender_flows[flow_id].flow_size, sender_flows[flow_id].start_time); 
        }
    }
    /* find the fisrt flow to start for this server */
    sender_next_unstart_flow_id = -1;
    sender_next_unstart_flow_id = find_next_unstart_flow_id();
    fclose(fd);

    for (int i=0; i<MAX_CONCURRENT_FLOW; i++) {
        sender_request_sent_flow_array[i] = -1;
        sender_active_flow_array[i] = -1;
        receiver_active_flow_array[i] = -1;
    }

    if (verbose > 0)
        printf("Flow info summary: total_flow_num = %d, sender_total_flow_num = %d\n",
            total_flow_num, sender_total_flow_num);
}

static void
construct_grant_request(uint32_t flow_id)
{
    struct   ether_hdr *eth_hdr;
    struct   ipv4_hdr *ip_hdr;
    struct   tcp_hdr *transport_hdr;
    uint64_t ol_flags, tx_offloads;
    int      dst_server_id;
    unsigned pkt_size = HDR_ONLY_SIZE;
    
    struct rte_mempool *mbp = current_fwd_lcore()->mbp;
    struct rte_mbuf *pkt = rte_mbuf_raw_alloc(mbp);
    if (!pkt) {
        printf("flow_id = %d: allocation pkt error", flow_id);
    }

    pkt->data_len = pkt_size;
    pkt->next = NULL;

    /* Initialize Ethernet header. */
    eth_hdr = rte_pktmbuf_mtod(pkt, struct ether_hdr *);
    dst_server_id = get_dst_server_id(flow_id, sender_flows);
    if (dst_server_id == -1) {
        printf("server error: cannot find server id\n");
    }
    ether_addr_copy(&eth_addr_array[dst_server_id], &eth_hdr->d_addr);
    ether_addr_copy(&eth_addr_array[current_server_id], &eth_hdr->s_addr);
    eth_hdr->ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv4);

    /* Initialize IP header. */
    ip_hdr = (struct ipv4_hdr *)(eth_hdr + 1);
    memset(ip_hdr, 0, L3_LEN);
    ip_hdr->version_ihl     = IP_VHL_DEF;
    ip_hdr->type_of_service = 0; // highest priority for grants & resend requests
    ip_hdr->fragment_offset = 0;
    ip_hdr->time_to_live    = IP_DEFTTL;
    ip_hdr->next_proto_id   = IPPROTO_TCP;
    ip_hdr->packet_id       = 0;
    ip_hdr->src_addr        = rte_cpu_to_be_32(sender_flows[flow_id].src_ip);
    ip_hdr->dst_addr        = rte_cpu_to_be_32(sender_flows[flow_id].dst_ip);
    ip_hdr->total_length    = RTE_CPU_TO_BE_16(pkt_size - L2_LEN);
    ip_hdr->hdr_checksum    = ip_sum((unaligned_uint16_t *)ip_hdr, L3_LEN);

    /* Initialize transport header. */
    transport_hdr = (struct tcp_hdr *)(ip_hdr + 1);
    transport_hdr->src_port              = rte_cpu_to_be_16(sender_flows[flow_id].src_port);
    transport_hdr->dst_port              = rte_cpu_to_be_16(sender_flows[flow_id].dst_port);
    transport_hdr->sent_seq              = rte_cpu_to_be_32(sender_flows[flow_id].data_seqnum);
    transport_hdr->recv_ack              = 0;
    transport_hdr->PKT_TYPE_8BITS        = PT_HOMA_GRANT_REQUEST;
    transport_hdr->FLOW_ID_16BITS        = rte_cpu_to_be_16((uint16_t)(flow_id & 0xffff));
    transport_hdr->FLOW_SIZE_LOW_16BITS  = rte_cpu_to_be_16((uint16_t)(sender_flows[flow_id].flow_size & 0xffff));
    transport_hdr->FLOW_SIZE_HIGH_16BITS = (uint16_t)((sender_flows[flow_id].flow_size >> 16) & 0xffff);
    
    tx_offloads = ports[global_fs->tx_port].dev_conf.txmode.offloads;
    if (tx_offloads & DEV_TX_OFFLOAD_VLAN_INSERT)
        ol_flags = PKT_TX_VLAN_PKT;
    if (tx_offloads & DEV_TX_OFFLOAD_QINQ_INSERT)
        ol_flags |= PKT_TX_QINQ_PKT;
    if (tx_offloads & DEV_TX_OFFLOAD_MACSEC_INSERT)
        ol_flags |= PKT_TX_MACSEC;

    pkt->nb_segs        = 1;
    pkt->data_len       = pkt_size;
    pkt->pkt_len        = pkt_size;
    pkt->ol_flags       = ol_flags;
    pkt->vlan_tci       = ports[global_fs->tx_port].tx_vlan_id;
    pkt->vlan_tci_outer = ports[global_fs->tx_port].tx_vlan_id_outer;
    pkt->l2_len         = L2_LEN;
    pkt->l3_len         = L3_LEN;

    /* Grant requests should be sent immediately */
    sender_pkts_burst[sender_current_burst_size] = pkt;
    sender_current_burst_size++;
    if (sender_current_burst_size >= 1) {
        if (verbose > 2) {
            print_elapsed_time();
            printf(" - construct_grant_request of flow %u ready to send\n", flow_id);
        }
        sender_send_pkt();
        if (verbose > 2) {
            print_elapsed_time();
            printf(" - construct_grant_request of flow %u sent\n", flow_id);
        }
        sender_current_burst_size = 0;
    }
}

static void
construct_grant(uint32_t flow_id, uint32_t seq_granted, uint8_t priority_granted)
{
    struct   ether_hdr *eth_hdr;
    struct   ipv4_hdr *ip_hdr;
    struct   tcp_hdr *transport_hdr;
    uint64_t ol_flags, tx_offloads;
    int      dst_server_id;
    unsigned pkt_size = HDR_ONLY_SIZE;

    struct rte_mempool *mbp = current_fwd_lcore()->mbp;
    struct rte_mbuf *pkt = rte_mbuf_raw_alloc(mbp);
    if (!pkt) {
        printf("flow_id = %d: allocation pkt error", flow_id);
    }

    pkt->data_len = pkt_size;
    pkt->next = NULL;

    /* Initialize Ethernet header. */
    eth_hdr = rte_pktmbuf_mtod(pkt, struct ether_hdr *);
    dst_server_id = get_dst_server_id(flow_id, receiver_flows);
    if (dst_server_id == -1) {
        printf("server error: cannot find server id\n");
    }
    ether_addr_copy(&eth_addr_array[dst_server_id], &eth_hdr->d_addr);
    ether_addr_copy(&eth_addr_array[current_server_id], &eth_hdr->s_addr);
    eth_hdr->ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv4);

    /* Initialize IP header. */
    ip_hdr = (struct ipv4_hdr *)(eth_hdr + 1);
    memset(ip_hdr, 0, L3_LEN);
    ip_hdr->version_ihl     = IP_VHL_DEF;
    ip_hdr->type_of_service = 0; // highest priority for grants & resend requests
    ip_hdr->fragment_offset = 0;
    ip_hdr->time_to_live    = IP_DEFTTL;
    ip_hdr->next_proto_id   = IPPROTO_TCP;
    ip_hdr->packet_id       = 0;
    ip_hdr->src_addr        = rte_cpu_to_be_32(receiver_flows[flow_id].src_ip);
    ip_hdr->dst_addr        = rte_cpu_to_be_32(receiver_flows[flow_id].dst_ip);
    ip_hdr->total_length    = RTE_CPU_TO_BE_16(pkt_size - L2_LEN);
    ip_hdr->hdr_checksum    = ip_sum((unaligned_uint16_t *)ip_hdr, L3_LEN);

    /* Initialize transport header. */
    transport_hdr = (struct tcp_hdr *)(ip_hdr + 1);
    transport_hdr->src_port                = rte_cpu_to_be_16(receiver_flows[flow_id].src_port);
    transport_hdr->dst_port                = rte_cpu_to_be_16(receiver_flows[flow_id].dst_port);
    transport_hdr->sent_seq                = rte_cpu_to_be_32(receiver_flows[flow_id].data_seqnum);
    transport_hdr->recv_ack                = 0;
    transport_hdr->PKT_TYPE_8BITS          = PT_HOMA_GRANT;
    transport_hdr->FLOW_ID_16BITS          = rte_cpu_to_be_16((uint16_t)(flow_id & 0xffff));
    transport_hdr->PRIORITY_GRANTED_8BITS  = priority_granted;
    transport_hdr->SEQ_GRANTED_LOW_16BITS  = rte_cpu_to_be_16((uint16_t)(seq_granted & 0xffff));
    transport_hdr->SEQ_GRANTED_HIGH_16BITS = (uint16_t)((seq_granted >> 16) & 0xffff);

    tx_offloads = ports[global_fs->tx_port].dev_conf.txmode.offloads;
    if (tx_offloads & DEV_TX_OFFLOAD_VLAN_INSERT)
        ol_flags = PKT_TX_VLAN_PKT;
    if (tx_offloads & DEV_TX_OFFLOAD_QINQ_INSERT)
        ol_flags |= PKT_TX_QINQ_PKT;
    if (tx_offloads & DEV_TX_OFFLOAD_MACSEC_INSERT)
        ol_flags |= PKT_TX_MACSEC;

    pkt->nb_segs        = 1;
    pkt->data_len       = pkt_size;
    pkt->pkt_len        = pkt_size;
    pkt->ol_flags       = ol_flags;
    pkt->vlan_tci       = ports[global_fs->tx_port].tx_vlan_id;
    pkt->vlan_tci_outer = ports[global_fs->tx_port].tx_vlan_id_outer;
    pkt->l2_len         = L2_LEN;
    pkt->l3_len         = L3_LEN;

    receiver_pkts_burst[receiver_current_burst_size] = pkt;
    receiver_current_burst_size++;
    if (receiver_current_burst_size >= BURST_THRESHOLD) {
        receiver_send_pkt();
        receiver_current_burst_size = 0;
    }
}

static void
construct_data(uint32_t flow_id, uint32_t ack_seq, int data_type)
{
    struct   ether_hdr *eth_hdr;
    struct   ipv4_hdr *ip_hdr;
    struct   tcp_hdr *transport_hdr;
    uint64_t ol_flags, tx_offloads;
    uint16_t data_len;
    int      dst_server_id;
    unsigned pkt_size;

    /* Data type (data_type): 
        0 normal data pkt
        1 unscheduled data pkt (selective dropping in Aeolus)
        2 empty data pkt (probe after unscheduled data pkt in Aeolus) */
    if (data_type < 2)
        pkt_size = DEFAULT_PKT_SIZE;
    else
        pkt_size = HDR_ONLY_SIZE;

    struct rte_mempool *mbp = current_fwd_lcore()->mbp;
    struct rte_mbuf *pkt = rte_mbuf_raw_alloc(mbp);
    if (!pkt) {
        printf("flow_id = %d: allocation pkt error", flow_id);
    }

    pkt->data_len = pkt_size;
    pkt->next = NULL;

    /* Initialize Ethernet header. */
    eth_hdr = rte_pktmbuf_mtod(pkt, struct ether_hdr *);
    dst_server_id = get_dst_server_id(flow_id, sender_flows);
    if (dst_server_id == -1) {
        printf("server error: cannot find server id\n");
    }
    ether_addr_copy(&eth_addr_array[dst_server_id], &eth_hdr->d_addr);
    ether_addr_copy(&eth_addr_array[current_server_id], &eth_hdr->s_addr);
    eth_hdr->ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv4);

    /* Initialize IP header. */
    ip_hdr = (struct ipv4_hdr *)(eth_hdr + 1);
    memset(ip_hdr, 0, L3_LEN);
    ip_hdr->version_ihl     = IP_VHL_DEF;
    ip_hdr->type_of_service = sender_flows[flow_id].granted_priority;
    ip_hdr->fragment_offset = 0;
    ip_hdr->time_to_live    = IP_DEFTTL;
    ip_hdr->next_proto_id   = IPPROTO_TCP;
    ip_hdr->packet_id       = 0;
    ip_hdr->src_addr        = rte_cpu_to_be_32(sender_flows[flow_id].src_ip);
    ip_hdr->dst_addr        = rte_cpu_to_be_32(sender_flows[flow_id].dst_ip);
    ip_hdr->total_length    = RTE_CPU_TO_BE_16(pkt_size - L2_LEN);
    ip_hdr->hdr_checksum    = ip_sum((unaligned_uint16_t *)ip_hdr, L3_LEN);
    /* Aeolus enables selective dropping for unscheduled data pkts */
    if (ENABLE_AEOLUS && data_type == 1)  
        ip_hdr->type_of_service |= 1; 
    
    /* Initialize transport header. */
    transport_hdr = (struct tcp_hdr *)(ip_hdr + 1);
    transport_hdr->src_port       = rte_cpu_to_be_16(sender_flows[flow_id].src_port);
    transport_hdr->dst_port       = rte_cpu_to_be_16(sender_flows[flow_id].dst_port);
    transport_hdr->sent_seq       = rte_cpu_to_be_32(sender_flows[flow_id].data_seqnum);
    transport_hdr->recv_ack       = ack_seq;
    transport_hdr->PKT_TYPE_8BITS = PT_HOMA_DATA;
    transport_hdr->FLOW_ID_16BITS = rte_cpu_to_be_16((uint16_t)(flow_id & 0xffff));
    data_len = (pkt_size - HDR_ONLY_SIZE);
    if (data_len > sender_flows[flow_id].remain_size) {
        data_len = sender_flows[flow_id].remain_size;
        pkt_size = HDR_ONLY_SIZE + data_len;
    }
    transport_hdr->DATA_LEN_16BITS = RTE_CPU_TO_BE_16(data_len);
    sender_flows[flow_id].data_seqnum += data_len; 
    sender_flows[flow_id].remain_size -= data_len;

    tx_offloads = ports[global_fs->tx_port].dev_conf.txmode.offloads;
    if (tx_offloads & DEV_TX_OFFLOAD_VLAN_INSERT)
        ol_flags = PKT_TX_VLAN_PKT;
    if (tx_offloads & DEV_TX_OFFLOAD_QINQ_INSERT)
        ol_flags |= PKT_TX_QINQ_PKT;
    if (tx_offloads & DEV_TX_OFFLOAD_MACSEC_INSERT)
        ol_flags |= PKT_TX_MACSEC;

    pkt->nb_segs        = 1;
    pkt->data_len       = pkt_size;
    pkt->pkt_len        = pkt_size;
    pkt->ol_flags       = ol_flags;
    pkt->vlan_tci       = ports[global_fs->tx_port].tx_vlan_id;
    pkt->vlan_tci_outer = ports[global_fs->tx_port].tx_vlan_id_outer;
    pkt->l2_len         = L2_LEN;
    pkt->l3_len         = L3_LEN;

    sender_pkts_burst[sender_current_burst_size] = pkt;
    sender_current_burst_size++;
    if (sender_current_burst_size >= BURST_THRESHOLD) {
        sender_send_pkt();
        sender_current_burst_size = 0;
    }
}

static void
construct_resend_request(uint32_t flow_id, uint16_t resend_size)
{
    struct   ether_hdr *eth_hdr;
    struct   ipv4_hdr *ip_hdr;
    struct   tcp_hdr *transport_hdr;
    uint64_t ol_flags, tx_offloads;
    int      dst_server_id;
    unsigned pkt_size = HDR_ONLY_SIZE;

    struct rte_mempool *mbp = current_fwd_lcore()->mbp;
    struct rte_mbuf *pkt = rte_mbuf_raw_alloc(mbp);
    if (!pkt) {
        printf("flow_id = %d: allocation pkt error", flow_id);
    }

    pkt->data_len = pkt_size;
    pkt->next = NULL;

    /* Initialize Ethernet header. */
    eth_hdr = rte_pktmbuf_mtod(pkt, struct ether_hdr *);
    dst_server_id = get_dst_server_id(flow_id, receiver_flows);
    if (dst_server_id == -1) {
        printf("server error: cannot find server id\n");
    }
    ether_addr_copy(&eth_addr_array[dst_server_id], &eth_hdr->d_addr);
    ether_addr_copy(&eth_addr_array[current_server_id], &eth_hdr->s_addr);
    eth_hdr->ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv4);

    /* Initialize IP header. */
    ip_hdr = (struct ipv4_hdr *)(eth_hdr + 1);
    memset(ip_hdr, 0, L3_LEN);
    ip_hdr->version_ihl     = IP_VHL_DEF;
    ip_hdr->type_of_service = 0; // highest priority for grants & resend requests
    ip_hdr->fragment_offset = 0;
    ip_hdr->time_to_live    = IP_DEFTTL;
    ip_hdr->next_proto_id   = IPPROTO_TCP;
    ip_hdr->packet_id       = 0;
    ip_hdr->src_addr        = rte_cpu_to_be_32(receiver_flows[flow_id].src_ip);
    ip_hdr->dst_addr        = rte_cpu_to_be_32(receiver_flows[flow_id].dst_ip);
    ip_hdr->total_length    = RTE_CPU_TO_BE_16(pkt_size - L2_LEN);
    ip_hdr->hdr_checksum    = ip_sum((unaligned_uint16_t *)ip_hdr, L3_LEN);

    /* Initialize transport header. */
    transport_hdr = (struct tcp_hdr *)(ip_hdr + 1);
    transport_hdr->src_port           = rte_cpu_to_be_16(receiver_flows[flow_id].src_port);
    transport_hdr->dst_port           = rte_cpu_to_be_16(receiver_flows[flow_id].dst_port);
    transport_hdr->sent_seq           = rte_cpu_to_be_32(receiver_flows[flow_id].data_seqnum);
    transport_hdr->recv_ack           = 0;
    transport_hdr->PKT_TYPE_8BITS     = PT_HOMA_RESEND_REQUEST;
    transport_hdr->DATA_RESEND_16BITS = rte_cpu_to_be_16((uint16_t)(resend_size & 0xffff));

    tx_offloads = ports[global_fs->tx_port].dev_conf.txmode.offloads;
    if (tx_offloads & DEV_TX_OFFLOAD_VLAN_INSERT)
        ol_flags = PKT_TX_VLAN_PKT;
    if (tx_offloads & DEV_TX_OFFLOAD_QINQ_INSERT)
        ol_flags |= PKT_TX_QINQ_PKT;
    if (tx_offloads & DEV_TX_OFFLOAD_MACSEC_INSERT)
        ol_flags |= PKT_TX_MACSEC;

    pkt->nb_segs        = 1;
    pkt->data_len       = pkt_size;
    pkt->pkt_len        = pkt_size;
    pkt->ol_flags       = ol_flags;
    pkt->vlan_tci       = ports[global_fs->tx_port].tx_vlan_id;
    pkt->vlan_tci_outer = ports[global_fs->tx_port].tx_vlan_id_outer;
    pkt->l2_len         = L2_LEN;
    pkt->l3_len         = L3_LEN;

    /* Resend requests should be sent immediately */
    receiver_pkts_burst[receiver_current_burst_size] = pkt;
    receiver_current_burst_size++;
    if (receiver_current_burst_size >= 1) {
        if (verbose > 2) {
            print_elapsed_time();
            printf(" - construct_resend_request of flow %u ready to send\n", flow_id);
        }
        receiver_send_pkt();
        if (verbose > 2) {
            print_elapsed_time();
            printf(" - construct_resend_request of flow %u sent\n", flow_id);
        }
        receiver_current_burst_size = 0;
    }
}

static void
process_ack(struct tcp_hdr* transport_recv_hdr)
{
    int datalen = rte_be_to_cpu_16(transport_recv_hdr->DATA_LEN_16BITS);
    uint16_t flow_id = (uint16_t)rte_be_to_cpu_16(transport_recv_hdr->FLOW_ID_16BITS);

    if (rte_be_to_cpu_32(transport_recv_hdr->sent_seq) != receiver_flows[flow_id].data_recv_next) {
        print_elapsed_time();
        printf(" - flow %d: data reordering detected. (expected = %u, received = %u)\n", flow_id, 
            receiver_flows[flow_id].data_recv_next, rte_be_to_cpu_32(transport_recv_hdr->sent_seq));
    }
    receiver_flows[flow_id].data_recv_next += datalen;
    receiver_flows[flow_id].remain_size -= datalen;
    
    if (verbose > 0) {
        print_elapsed_time();
        printf(" - process_ack of flow %u, data_recv_next=%u, remain_size=%u\n", 
            flow_id, receiver_flows[flow_id].data_recv_next, 
            receiver_flows[flow_id].remain_size);
    }

    if (receiver_flows[flow_id].remain_size <= 0) {
        if (receiver_flows[flow_id].flow_state != HOMA_RECV_CLOSED)
            remove_receiver_active_flow(flow_id);
        return;
    }

    /* datalen == 0 indicates an empty probe pkt. */
    if (ENABLE_AEOLUS && datalen == 0) {
        if (verbose > 0) {
            print_elapsed_time();
            printf(" - probe pkt received of flow %u\n", flow_id);
        }
        uint16_t resend_size = 0;
        int received_size = receiver_flows[flow_id].flow_size - receiver_flows[flow_id].remain_size;
        if (receiver_flows[flow_id].flow_size < RTT_BYTES) // flow_size unscheduled
            resend_size = receiver_flows[flow_id].remain_size;
        else if (RTT_BYTES > received_size) // RTT_BYTES unsheduled
            resend_size = RTT_BYTES - received_size;
        /* Send RESEND requests to inform the lost unscheduled packets if any. */
        if (resend_size > 0) {
            if (verbose > 0) {
                print_elapsed_time();
                printf(" - construct_resend_request of flow %u, resend_size=%u\n", flow_id, resend_size);
            }
            construct_resend_request(flow_id, resend_size);
        }
    }
}

static void
recv_grant_request(struct tcp_hdr *transport_recv_hdr, struct ipv4_hdr *ipv4_hdr)
{
    uint16_t flow_id = rte_be_to_cpu_16(transport_recv_hdr->FLOW_ID_16BITS);
    uint32_t flow_size_lowpart = (uint32_t)rte_be_to_cpu_16(transport_recv_hdr->FLOW_SIZE_LOW_16BITS);
    uint32_t flow_size_highpart = ((uint32_t)transport_recv_hdr->FLOW_SIZE_HIGH_16BITS << 16) & 0xffff0000;
    uint32_t flow_size = flow_size_highpart + flow_size_lowpart;

    if (receiver_flows[flow_id].flow_state != HOMA_RECV_UNSTARTED)
        return; 
    
    if (verbose > 1) {
        print_elapsed_time();
        printf(" - recv_grant_request of flow %u\n", flow_id);
    }

    add_receiver_active_flow(flow_id);

    receiver_flows[flow_id].flow_size      = flow_size;
    receiver_flows[flow_id].remain_size    = flow_size;
    receiver_flows[flow_id].src_port       = RTE_BE_TO_CPU_16(transport_recv_hdr->dst_port);
    receiver_flows[flow_id].dst_port       = RTE_BE_TO_CPU_16(transport_recv_hdr->src_port);
    receiver_flows[flow_id].src_ip         = rte_be_to_cpu_32(ipv4_hdr->dst_addr);
    receiver_flows[flow_id].dst_ip         = rte_be_to_cpu_32(ipv4_hdr->src_addr);
    receiver_flows[flow_id].start_time     = rte_rdtsc() / (double)hz;
    receiver_flows[flow_id].fct_printed    = 0;
    receiver_flows[flow_id].flow_finished  = 0;
    receiver_flows[flow_id].data_recv_next = 1;
    receiver_flows[flow_id].data_seqnum    = 1;
}

static void
recv_grant(struct tcp_hdr *transport_recv_hdr)
{
    uint16_t flow_id = rte_be_to_cpu_16(transport_recv_hdr->FLOW_ID_16BITS);
    uint32_t seq_granted_lowpart = (uint32_t)rte_be_to_cpu_16(transport_recv_hdr->SEQ_GRANTED_LOW_16BITS);
    uint32_t seq_granted_highpart = ((uint32_t)transport_recv_hdr->SEQ_GRANTED_HIGH_16BITS << 16) & 0xffff0000;
    uint32_t seq_granted = seq_granted_highpart + seq_granted_lowpart;
    uint8_t  priority_granted = transport_recv_hdr->PRIORITY_GRANTED_8BITS;
    struct   rte_mbuf *queued_pkt;
    struct   tcp_hdr *transport_hdr;
    int      queued_pkt_type, queued_flow_id;

    if (verbose > 1) {
        print_elapsed_time();
        printf(" - recv_grant of flow %u, seq_granted=%u, priority_granted=%u\n", 
            flow_id, seq_granted, priority_granted);
    }

    switch (sender_flows[flow_id].flow_state) {
        case HOMA_SEND_GRANT_REQUEST_SENT:
            remove_sender_grant_request_sent_flow(flow_id);
        case HOMA_SEND_GRANT_RECEIVING:
            sender_flows[flow_id].granted_seqnum = seq_granted;
            sender_flows[flow_id].granted_priority = priority_granted;
            /* Construct new data according to SRPT */
            if (sender_current_burst_size > 0) {
                queued_pkt = sender_pkts_burst[sender_current_burst_size-1];
                transport_hdr = rte_pktmbuf_mtod_offset(queued_pkt, struct tcp_hdr *, L2_LEN + L3_LEN);
                queued_pkt_type = transport_hdr->PKT_TYPE_8BITS;
                queued_flow_id = transport_hdr->FLOW_ID_16BITS;
                if (queued_pkt_type == PT_HOMA_DATA && 
                    sender_flows[queued_flow_id].remain_size > sender_flows[flow_id].remain_size) {
                    while (sender_flows[flow_id].remain_size > 0 &&
                        sender_flows[flow_id].granted_seqnum > sender_flows[flow_id].data_seqnum) {
                        construct_data(flow_id, transport_recv_hdr->sent_seq, 0);
                    }
                }
            } else {
                while (sender_flows[flow_id].remain_size > 0 &&
                        sender_flows[flow_id].granted_seqnum > sender_flows[flow_id].data_seqnum) {
                        construct_data(flow_id, transport_recv_hdr->sent_seq, 0);
                    }
            }
            if (sender_flows[flow_id].remain_size == 0) {
                sender_finished_flow_num++;
                sender_flows[flow_id].flow_state = HOMA_SEND_CLOSED;
            }
            break;
        default:
            break;
    }
}

static void
recv_data(struct tcp_hdr *transport_recv_hdr)
{
    uint16_t flow_id = rte_be_to_cpu_16(transport_recv_hdr->FLOW_ID_16BITS);

    if (verbose > 1) {
        print_elapsed_time();
        printf(" - recv_data of flow %u\n", flow_id);
    }

    // Drop all data packets if received before the grant requests
    if (receiver_flows[flow_id].flow_state < HOMA_RECV_GRANT_SENDING) {
        if (verbose > 1) {
            print_elapsed_time();
            printf(" - recv_data of flow %u and dropped due to no grant request\n", flow_id);
        }
        return;
    }

    process_ack(transport_recv_hdr);
}

static void
recv_resend_request(struct tcp_hdr *transport_recv_hdr)
{
    uint16_t flow_id = rte_be_to_cpu_16(transport_recv_hdr->FLOW_ID_16BITS);
    uint16_t resend_size = rte_be_to_cpu_16(transport_recv_hdr->DATA_RESEND_16BITS);

    if (verbose > 1) {
        print_elapsed_time();
        printf(" - recv_resend_request of flow %u, resend_size=%u\n", flow_id, resend_size);
    }

    /* Roll back resend_size data pkts for resend if grants allowed. */
    sender_flows[flow_id].data_seqnum -= resend_size; 
    sender_flows[flow_id].remain_size += resend_size;
}


/* Receive and process a burst of packets. */
static void
recv_pkt(struct fwd_stream *fs)
{
    struct   rte_mbuf *pkts_burst[MAX_PKT_BURST];
    struct   rte_mbuf *mb;
    uint16_t nb_rx;
    struct   ipv4_hdr *ipv4_hdr;
    struct   tcp_hdr *transport_recv_hdr;
    uint8_t  l4_proto;
    uint8_t  pkt_type;

    /* Receive a burst of packets. */
    nb_rx = rte_eth_rx_burst(fs->rx_port, fs->rx_queue, pkts_burst, nb_pkt_per_burst);

    if (unlikely(nb_rx == 0))
        return;

#ifdef RTE_TEST_PMD_RECORD_BURST_STATS
    fs->rx_burst_stats.pkt_burst_spread[nb_rx]++;
#endif
    fs->rx_packets += nb_rx;

    /* Process a burst of packets. */
    for (int i = 0; i < nb_rx; i++) {
        mb = pkts_burst[i]; 
        ipv4_hdr = rte_pktmbuf_mtod_offset(mb, struct ipv4_hdr *, L2_LEN);
        l4_proto = ipv4_hdr->next_proto_id;
        if (l4_proto == IPPROTO_TCP) {
            transport_recv_hdr = rte_pktmbuf_mtod_offset(mb, struct tcp_hdr *, L2_LEN + L3_LEN);
            pkt_type = transport_recv_hdr->PKT_TYPE_8BITS;
            switch (pkt_type) {
                case PT_HOMA_GRANT_REQUEST:
                    recv_grant_request(transport_recv_hdr, ipv4_hdr);
                    break;
                case PT_HOMA_GRANT:
                    recv_grant(transport_recv_hdr);
                    break;
                case PT_HOMA_DATA:
                    recv_data(transport_recv_hdr);
                    break;
                case PT_HOMA_RESEND_REQUEST:
                    recv_resend_request(transport_recv_hdr);
                default:
                    break;
            }
        }
        rte_pktmbuf_free(mb);
    }
}

static void
send_grant(void)
{
    if (receiver_current_burst_size > 0) {
        receiver_send_pkt();
    }

    /* Generate new grants for SCHEDULED_PRIORITY messages */
    sort_receiver_active_flow_by_remaining_size();
    for (int i=0; i<SCHEDULED_PRIORITY; i++) {
        if (receiver_active_flow_array[i] >= 0) {
            int flow_id = receiver_active_flow_array[i];
            uint32_t seq_granted = min(receiver_flows[flow_id].flow_size+1, 
                receiver_flows[flow_id].data_recv_next+RTT_BYTES);
            uint8_t priority_granted = prio_map[i];
            double now = rte_rdtsc() / (double)hz;
            if ((now - sender_flows[flow_id].last_grant_sent_time) > RETRANSMIT_TIMEOUT ||
                sender_flows[flow_id].last_grant_granted_seq != seq_granted ||
                sender_flows[flow_id].last_grant_granted_prio != priority_granted) {
                if (verbose > 1) {
                    print_elapsed_time();
                    printf(" - send_grant of flow %u, seq_granted=%u, priority_granted=%u\n", 
                        flow_id, seq_granted, priority_granted);
                }
                construct_grant(flow_id, seq_granted, priority_granted);
                sender_flows[flow_id].last_grant_sent_time = now;
                sender_flows[flow_id].last_grant_granted_seq = seq_granted;
                sender_flows[flow_id].last_grant_granted_prio = priority_granted;
            }
        }
        else
            break;
    }
}

/* Sender send a burst of packets */
static void
sender_send_pkt(void)
{
    uint16_t nb_pkt = sender_current_burst_size;
    uint16_t nb_tx = rte_eth_tx_burst(global_fs->tx_port, global_fs->tx_queue, sender_pkts_burst, nb_pkt);

    if (unlikely(nb_tx < nb_pkt) && global_fs->retry_enabled) {
        uint32_t retry = 0;
        while (nb_tx < nb_pkt && retry++ < burst_tx_retry_num) {
            if (verbose > 1) {
                print_elapsed_time();
                printf(" - sender_send_pkt, retry = %u\n", retry);
            }
            rte_delay_us(burst_tx_delay_time);
            nb_tx += rte_eth_tx_burst(global_fs->tx_port, global_fs->tx_queue, 
                                        &sender_pkts_burst[nb_tx], nb_pkt - nb_tx);
        }
    }

    global_fs->tx_packets += nb_tx;
    sender_current_burst_size = 0;
}

/* Receiver send a burst of packets */
static void
receiver_send_pkt(void)
{
    uint16_t nb_pkt = receiver_current_burst_size;
    uint16_t nb_tx = rte_eth_tx_burst(global_fs->tx_port, global_fs->tx_queue, receiver_pkts_burst, nb_pkt);

    if (unlikely(nb_tx < nb_pkt) && global_fs->retry_enabled) {
        uint32_t retry = 0;
        while (nb_tx < nb_pkt && retry++ < burst_tx_retry_num) {
            if (verbose > 1) {
                print_elapsed_time();
                printf(" - receiver_send_pkt, retry = %u\n", retry);
            }
            rte_delay_us(burst_tx_delay_time);
            nb_tx += rte_eth_tx_burst(global_fs->tx_port, global_fs->tx_queue, 
                                        &receiver_pkts_burst[nb_tx], nb_pkt - nb_tx);
        }
    }

    global_fs->tx_packets += nb_tx;
    receiver_current_burst_size = 0;
}

/* Start one warm up flow - send data without receive */
static void
start_warm_up_flow(void)
{
    int flow_id = sender_next_unstart_flow_id;
    sender_next_unstart_flow_id = find_next_unstart_flow_id();
    
    while (1) {
        construct_data(flow_id, 0, 0);
        if (sender_flows[flow_id].remain_size <= 0)
            break;
    }

    sender_send_pkt();
    sender_finished_flow_num++;

    if (verbose > 1) {
        print_elapsed_time();
        printf(" - start_warm_up_flow %d done\n", flow_id);
    }
}

static void
start_new_flow(void)
{
    double now;
    int flow_id, request_retrans_check, retransmit_flow_index, max_request_retrans_check;

    if (sender_current_burst_size > 0) {
        sender_send_pkt();
    }

    /* Retransmit timeout grant request */
    max_request_retrans_check = min(MAX_REQUEST_RETRANSMIT_ONE_TIME, sender_grant_request_sent_flow_num);
    if (sender_grant_request_sent_flow_num > 0) {
        retransmit_flow_index = find_next_sender_grant_request_sent_flow(0);
        request_retrans_check = 0;
        while (request_retrans_check < max_request_retrans_check) {
            now =  rte_rdtsc() / (double)hz; 
            flow_id = sender_request_sent_flow_array[retransmit_flow_index];
            if ((now - sender_flows[flow_id].last_grant_request_sent_time) > RETRANSMIT_TIMEOUT) {
                if (verbose > 1) {
                    print_elapsed_time();
                    printf(" - construct_grant_request %d due to RETRANSMIT_TIMEOUT\n"
                        "                 last_grant_request_sent_time = %lf\n", flow_id, 
                        sender_flows[flow_id].last_grant_request_sent_time - flowgen_start_time);
                }
                construct_grant_request(flow_id);
                sender_flows[flow_id].last_grant_request_sent_time = now;
            }
            retransmit_flow_index = find_next_sender_grant_request_sent_flow(retransmit_flow_index+1);
            request_retrans_check++;
        }
    }

    /* Start new flow */
    if (sender_next_unstart_flow_id < total_flow_num) {
        flow_id = sender_next_unstart_flow_id;
        now = rte_rdtsc() / (double)hz;
        while ((sender_flows[flow_id].start_time + flowgen_start_time + sync_time) <= now) {
            if (verbose > 1) {
                print_elapsed_time();
                printf(" - start_new_flow %d\n", flow_id);
            }

            /* Send grant requests for new flows */
            construct_grant_request(flow_id);
            add_sender_grant_request_sent_flow(flow_id);

            /* Send RTT_BYTES unscheduled data */
            int max_unscheduled_pkt = RTT_BYTES / DEFAULT_PKT_SIZE + 1;
            sender_flows[flow_id].granted_priority = map_to_unscheduled_priority(sender_flows[flow_id].flow_size);
            for (int i=0; i<max_unscheduled_pkt; i++) {
                if (verbose > 1) {
                    print_elapsed_time();
                    printf(" - construct_data unscheduled %d of flow %d\n", i+1, flow_id);
                }
                construct_data(flow_id, 0, 1);
                if (sender_flows[flow_id].remain_size <= 0)
                    break;
            }

            /* Send probe pkt if Aeolus enabled */
            if (ENABLE_AEOLUS) {
                if (verbose > 1) {
                    print_elapsed_time();
                    printf(" - probe pkt sent of flow %u\n", flow_id);
                }
                construct_data(flow_id, 0, 2);
            }

            if (sender_next_unstart_flow_id < total_flow_num) {
                flow_id = sender_next_unstart_flow_id;
            } else {
                break;
            }
        }
    }
}

/* flowgen packet_fwd main function */
static void
main_flowgen(struct fwd_stream *fs)
{
    /* Initialize global variables*/
    rte_delay_ms(1000);
    global_fs = fs;
    hz = rte_get_timer_hz(); 
    sender_flows = rte_zmalloc("testpmd: struct flow_info",
            total_flow_num*sizeof(struct flow_info), RTE_CACHE_LINE_SIZE);
    receiver_flows = rte_zmalloc("testpmd: struct flow_info",
            total_flow_num*sizeof(struct flow_info), RTE_CACHE_LINE_SIZE);

    /* Read basic info of server number, mac and ip */
    printf("\nEnter read_config...\n\n");
    read_config();
    printf("\nExit read_config...\n\n");
    
    /* Init flow info */
    printf("\nEnter init...\n\n");
    init();
    printf("\nExit init...\n\n");

    /* Warm up and sync all servers */
    printf("\nEnter warm up and sync loop...\n\n");
    start_cycle = rte_rdtsc();
    elapsed_cycle = 0;
    flowgen_start_time = start_cycle / (double)hz;
    start_warm_up_flow();
    /* Receiver only to sync all servers */
    while (elapsed_cycle/(double)hz < sync_time) {
        recv_pkt(fs);
        send_grant();
        elapsed_cycle = rte_rdtsc() - start_cycle;
    }
    printf("Warm up and sync delay = %d sec\n", sync_time);
    printf("\nExit warm up and sync loop...\n\n");

    /* Main flowgen loop */
    printf("\nEnter main_flowgen loop...\n\n");
    int loop_time = sync_time + 2; // main loop time in sec
    int main_flowgen_loop = 1;
    do {
        if (verbose > 2) {
            printf("main_flowgen_loop = %d\n", main_flowgen_loop++);
            print_elapsed_time();
            printf(" - enter start_new_flow...\n");
        }

        start_new_flow();

        if (verbose > 2) {
            print_elapsed_time();
            printf(" - enter recv_pkt...\n");
        }
        
        recv_pkt(fs);

        if (verbose > 2) {
            print_elapsed_time();
            printf(" - enter send_grant...\n");
        }

        send_grant();

        if (verbose > 2) {
            print_elapsed_time();
            printf(" - exit  send_grant...\n\n");
        }

        /* Break when reaching loop_time */
        elapsed_cycle = rte_rdtsc() - start_cycle;
        if (elapsed_cycle > loop_time*hz)
            break;
    } /* Do not exit main loop if not reaching loop_time or not finishing all flows */
    while (elapsed_cycle < loop_time*hz || receiver_finished_flow_num < receiver_total_flow_num || 
        sender_finished_flow_num < sender_total_flow_num);
    printf("\nExit main_flowgen loop...\n\n");

    printf("\nEnter print_fct...\n\n");
    print_fct();
    printf("\nExit print_fct...\n\n");

    exit(0);
}

struct fwd_engine flow_gen_engine = {
    .fwd_mode_name  = "flowgen",
    .port_fwd_begin = NULL,
    .port_fwd_end   = NULL,
    .packet_fwd     = main_flowgen,
};
