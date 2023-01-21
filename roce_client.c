#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <rdma/rdma_cma.h>

int main(int argc, char *argv[]) 
{
	int	n;
	int	error;
     
     // Create Event Channel
    struct rdma_event_channel *cm_channel;
    cm_channel = rdma_create_event_channel(); 
	if (!cm_channel) 
		return 1; 

    //Get CM ID
    struct rdma_cm_id *cm_id;
	error = rdma_create_id(cm_channel, &cm_id, NULL, RDMA_PS_UDP);
	if (error)  
		return error;

    //Get Address Info
    struct addrinfo *res;
    struct addrinfo hints = { 
   		.ai_family    = AF_INET,
   		.ai_socktype  = SOCK_STREAM
   	};
	n = getaddrinfo(argv[1], "4791", &hints, &res);
	if (n < 0)  
		return 1;

	//Resolve Address
	error = rdma_resolve_addr(cm_id, NULL, res->ai_addr, 5000);
	if (error)
		return error;

    //Get event on NIC
    struct rdma_cm_event *event;
	error = rdma_get_cm_event(cm_channel, &event);
	if (error)
		return error;
	if (event->event != RDMA_CM_EVENT_ADDR_RESOLVED)
		return 1;

    //Ack event
	rdma_ack_cm_event(event);

	error = rdma_resolve_route(cm_id, 5000);
	if (error)
		return error;
	error = rdma_get_cm_event(cm_channel, &event);
	if (error)
		return error;
	if (event->event != RDMA_CM_EVENT_ROUTE_RESOLVED)
		return 1; 
	rdma_ack_cm_event(event);

	//Create Protection Domain
    struct ibv_pd *pd; 
	pd = ibv_alloc_pd(cm_id->verbs); 
	if (!pd) 
		return 1;

    //Set up completion event channel
    struct ibv_comp_channel	*comp_chan; 
	comp_chan = ibv_create_comp_channel(cm_id->verbs);
	if (!comp_chan) 
		return 1;

    //Create Completion Queue
    struct ibv_cq *cq;
	cq = ibv_create_cq(cm_id->verbs,2,NULL,comp_chan,0); 
	if (!cq) 
		return 1;

    //Get Work Completion
	if (ibv_req_notify_cq(cq,0))
		return 1;

    uint32_t *buf; 
	buf = calloc(2,sizeof(uint32_t)); 
	if (!buf) 
		return 1;

    //Allocate Memory Region
    struct ibv_mr *mr;
	mr = ibv_reg_mr(pd, buf,2 * sizeof(uint32_t), IBV_ACCESS_LOCAL_WRITE); 
	if (!mr) 
		return 1;

    struct ibv_qp_init_attr	qp_attr = { }; 
	qp_attr.cap.max_send_wr = 4; 
	qp_attr.cap.max_send_sge = 1;
	qp_attr.cap.max_recv_wr = 1; 
	qp_attr.cap.max_recv_sge = 1; 

	qp_attr.send_cq = cq;
	qp_attr.recv_cq = cq;
	qp_attr.qp_type = IBV_QPT_RC;
    
    //Create Queue Pair
	error = rdma_create_qp(cm_id,pd,&qp_attr);
	if (error)
		return error;

    struct rdma_conn_param conn_param = { };
	conn_param.initiator_depth = 1;
	conn_param.retry_count     = 7;

	error = rdma_connect(cm_id,&conn_param);
	if (error)
		return error;

	error = rdma_get_cm_event(cm_channel,&event);
	if (error)
		return error;

	if (event->event != RDMA_CM_EVENT_ESTABLISHED)
		return 1;

    //Get server memory info
    struct pdata { 
    uint64_t buf_va; 
    uint32_t buf_rkey;
    };
    struct pdata server_pdata;

	memcpy(&server_pdata,event->param.conn.private_data,sizeof(server_pdata));
	rdma_ack_cm_event(event);

	//Prepare and post Work Request
    struct ibv_sge sge; 
	sge.addr = (uintptr_t)buf; 
	sge.length = sizeof(uint32_t);
	sge.lkey = mr->lkey;

    struct ibv_recv_wr recv_wr = { }; 
   	struct ibv_recv_wr *bad_recv_wr; 

	recv_wr.wr_id = 0;                
	recv_wr.sg_list = &sge;
	recv_wr.num_sge = 1;

	if (ibv_post_recv(cm_id->qp,&recv_wr,&bad_recv_wr))
		return 1;

	buf[0] = strtoul(argv[2],NULL,0);
	buf[1] = strtoul(argv[3],NULL,0);
	buf[0] = htonl(buf[0]);
	buf[1] = htonl(buf[1]);

	sge.addr = (uintptr_t)buf; 
	sge.length = sizeof(buf);
	sge.lkey = mr->lkey;

    struct ibv_send_wr send_wr = { }; 
   	struct ibv_send_wr *bad_send_wr;

	send_wr.wr_id = 1;
	send_wr.opcode = IBV_WR_RDMA_WRITE;
	send_wr.send_flags = IBV_SEND_SIGNALED;
	send_wr.sg_list = &sge;
	send_wr.num_sge = 1;
	send_wr.wr.rdma.rkey = ntohl(server_pdata.buf_rkey);
	send_wr.wr.rdma.remote_addr = ntohl(server_pdata.buf_va);

    struct ibv_cq *evt_cq;
    struct ibv_wc wc; 
    void *cq_context;

	if (ibv_post_send(cm_id->qp,&send_wr,&bad_send_wr))
		return 1;

	while (1) {
		if (ibv_get_cq_event(comp_chan,&evt_cq,&cq_context))
			return 1;
		if (ibv_req_notify_cq(cq,0))
			return 1;
		if (ibv_poll_cq(cq,1,&wc) != 1)
			return 1;
		if (wc.status != IBV_WC_SUCCESS)
			return 1;
		if (wc.wr_id == 0) {
			printf("The answer of the server was: %d\n", ntohl(buf[0]));
			break;
		}
    }
    //Finish transmission and disconnect
	ibv_ack_cq_events(cq,2);
	rdma_disconnect(cm_id);
	error = rdma_get_cm_event(cm_channel,&event);
	if (error)
		return error;

	rdma_ack_cm_event(event);
	rdma_destroy_qp(cm_id);
	ibv_dereg_mr(mr);
	free(buf);
	error = rdma_destroy_id(cm_id);
	if (error)  {
		printf("Destroying CM ID failed");
		return error;
	}
	rdma_destroy_event_channel(cm_channel);
    return 0;
}