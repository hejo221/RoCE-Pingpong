// Code acknowledgement: Based on works by Animesh Trivedi (https://github.com/animeshtrivedi/rdma-example)
// Code extended and adapted for the use with RoCE

#ifndef ROCE_COMMON_H
#define ROCE_COMMON_H


#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>
#include <getopt.h>

#include <netdb.h>
#include <netinet/in.h>	
#include <arpa/inet.h>
#include <sys/socket.h>

#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>

//Define default values
#define CQ_CAPACITY (2048)
#define MAX_SGE (32)
#define MAX_WR (512)
#define DEFAULT_RDMA_PORT (4791)

//Structure to exchange buffer information between client and server
struct __attribute((packed)) roce_buffer_attr {
  uint64_t address;
  uint32_t length;
  union stag {
	  uint32_t local_stag;
	  uint32_t remote_stag;
  } stag;
};

//Resolve given address
int get_addr(char *dst, struct sockaddr *addr);

//Process RDMA CM Eveent
int process_rdma_cm_event(struct rdma_event_channel *echannel, 
						  enum rdma_cm_event_type expected_event,
						  struct rdma_cm_event **cm_event);

//Allocate buffer of given size
struct ibv_mr* roce_alloc_buffer(struct ibv_pd *pd, uint32_t length, enum ibv_access_flags permission);

//Free allocated buffers
void roce_free_buffer(struct ibv_mr *mr);

//Register allocated memory
struct ibv_mr *roce_register_buffer(struct ibv_pd *pd, void *addr, uint32_t length, enum ibv_access_flags permission);

//Deregister registered memory
void roce_deregister_buffer(struct ibv_mr *mr);

//Process WC Events
int process_wc_events(struct ibv_comp_channel *comp_channel, struct ibv_wc *wc,	int max_wc);

#endif /* ROCE_COMMON_H */
