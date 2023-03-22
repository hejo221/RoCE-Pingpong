#include "roce_common.h"
#include "roce_common.c"

//Basic Resources for RDMA connection
static struct rdma_event_channel *cm_event_channel = NULL;
static struct rdma_cm_id *cm_client_id = NULL;
static struct ibv_pd *pd = NULL;
static struct ibv_comp_channel *io_completion_channel = NULL;
static struct ibv_cq *client_cq = NULL;
static struct ibv_qp_init_attr qp_init_attr;
static struct ibv_qp *client_qp;

//Memory resources for RDMA connection
static struct ibv_mr *client_metadata_mr = NULL, *client_send_buf_mr = NULL, *client_recv_buf_mr = NULL, *server_metadata_mr = NULL;
static struct rdma_buffer_attr client_metadata_attr, server_metadata_attr;
static struct ibv_send_wr client_send_wr, *bad_client_send_wr = NULL;
static struct ibv_recv_wr server_recv_wr, *bad_server_recv_wr = NULL;
static struct ibv_sge client_send_sge, server_recv_sge;

//Send and receive buffer for RDMA connection
static char *send_buf = NULL, *recv_buf = NULL;
static char *msg;
static int buf_size;

//Basic functionality test
static int check_send_buf_recv_buf() {
	return memcmp((void*) send_buf, (void*) recv_buf, buf_size);
}

//Prepare client side connection resources for RDMA connectio
static int client_prepare_connection(struct sockaddr_in *s_addr) {
	struct rdma_cm_event *cm_event = NULL;
	int ret = -1;

	cm_event_channel = rdma_create_event_channel();
	if (!cm_event_channel) {
		printf("Could not create CM Event Channel \n");
		return -errno;
	}

	ret = rdma_create_id(cm_event_channel, &cm_client_id, NULL, RDMA_PS_UDP);
	if (ret) {
		printf("Could not create CM ID /n"); 
		return -errno;
	}

	ret = rdma_resolve_addr(cm_client_id, NULL, (struct sockaddr*) s_addr, 2000);
	if (ret) {
		printf("Could not resolve address \n");
		return -errno;
	}

	ret  = process_rdma_cm_event(cm_event_channel, RDMA_CM_EVENT_ADDR_RESOLVED, &cm_event);
	if (ret) {
		printf("Could not receive valid CM Event \n");
		return ret;
	}

	ret = rdma_ack_cm_event(cm_event);
	if (ret) {
		printf("Could not acknowledge CM Event \n");
		return -errno;
	}

	ret = rdma_resolve_route(cm_client_id, 2000);
	if (ret) {
		printf("Could not resolve route \n");
	       return -errno;
	}

	ret = process_rdma_cm_event(cm_event_channel, RDMA_CM_EVENT_ROUTE_RESOLVED, &cm_event);
	if (ret) {
		printf("Could not receive valid event \n");
		return ret;
	}

	ret = rdma_ack_cm_event(cm_event);
	if (ret) {
		printf("Could not acknowledge CM Event \n");
		return -errno;
	}
	
	printf("Trying to connect to server at : %s port: %d \n", inet_ntoa(s_addr->sin_addr), ntohs(s_addr->sin_port));

	pd = ibv_alloc_pd(cm_client_id->verbs);
	if (!pd) {
		printf("Could not allocate PD \n");
		return -errno;
	}

	io_completion_channel = ibv_create_comp_channel(cm_client_id->verbs);
	if (!io_completion_channel) {
		printf("Could not create Comp Channel \n");
		return -errno;
	}

	client_cq = ibv_create_cq(cm_client_id->verbs, CQ_CAPACITY, NULL, io_completion_channel, 0);
	if (!client_cq) {
		printf("Could not create CQ \n");
		return -errno;
	}

	ret = ibv_req_notify_cq(client_cq, 0);
	if (ret) {
		printf("Could not request CQ notifications /n");
		return -errno;
	}

    bzero(&qp_init_attr, sizeof qp_init_attr);
    qp_init_attr.cap.max_recv_sge = MAX_SGE;
    qp_init_attr.cap.max_recv_wr = MAX_WR;
    qp_init_attr.cap.max_send_sge = MAX_SGE;
    qp_init_attr.cap.max_send_wr = MAX_WR;
    qp_init_attr.qp_type = IBV_QPT_RC;
	qp_init_attr.recv_cq = client_cq; 
    qp_init_attr.send_cq = client_cq;

    ret = rdma_create_qp(cm_client_id, pd, &qp_init_attr);
	if (ret) {
		printf("Could not create QP \n");
	       return -errno;
	}
	
	client_qp = cm_client_id->qp;

	return 0;
}

//Pre-post RB
static int client_pre_post_recv_buffer()
{
	int ret = -1;

	server_metadata_mr = roce_register_buffer(pd, &server_metadata_attr, sizeof(server_metadata_attr), (IBV_ACCESS_LOCAL_WRITE));
	if(!server_metadata_mr){
		printf("Could not set up server metadata \n");
		return -ENOMEM;
	}

	server_recv_sge.addr = (uint64_t) server_metadata_mr->addr;
	server_recv_sge.length = (uint32_t) server_metadata_mr->length;
	server_recv_sge.lkey = (uint32_t) server_metadata_mr->lkey;
	bzero(&server_recv_wr, sizeof(server_recv_wr));
	server_recv_wr.sg_list = &server_recv_sge;
	server_recv_wr.num_sge = 1;

	ret = ibv_post_recv(client_qp, &server_recv_wr, &bad_server_recv_wr);
	if (ret) {
		printf("Could not pre-post receive buffer \n");
		return ret;

	}
	return 0;
}

//Connect to server
static int client_connect_to_server() 
{
	struct rdma_conn_param conn_param;
	struct rdma_cm_event *cm_event = NULL;
	int ret = -1;

	bzero(&conn_param, sizeof(conn_param));
	conn_param.initiator_depth = 3;
	conn_param.responder_resources = 3;
	conn_param.retry_count = 3;

	ret = rdma_connect(cm_client_id, &conn_param);
	if (ret) {
		printf("Could not connect to remote host \n");
		return -errno;
	}

	ret = process_rdma_cm_event(cm_event_channel, RDMA_CM_EVENT_ESTABLISHED, &cm_event);
	if (ret) {
		printf("Could not get CM Event \n");
	       return ret;
	}

	ret = rdma_ack_cm_event(cm_event);
	if (ret) {
		printf("Could not acknowledge CM Event \n");
		return -errno;
	}

	printf("The client was connected successfully \n");

	return 0;
}

//Exchange metadata with server
static int exchange_metadata() {
	struct ibv_wc wc[2];
	int ret = -1;

	client_send_buf_mr = roce_register_buffer(pd, send_buf, strlen(send_buf), (IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE));
	if(!client_send_buf_mr) {
		printf("Could not register buffer \n");
		return ret;
	}

	client_metadata_attr.address = (uint64_t) client_send_buf_mr->addr; 
	client_metadata_attr.length = client_send_buf_mr->length; 
	client_metadata_attr.stag.local_stag = client_send_buf_mr->lkey;

	client_metadata_mr = roce_register_buffer(pd, &client_metadata_attr, sizeof(client_metadata_attr), IBV_ACCESS_LOCAL_WRITE);
	if(!client_metadata_mr) {
		printf("Could not register buffer \n");
		return ret;
	}

	client_send_sge.addr = (uint64_t) client_metadata_mr->addr;
	client_send_sge.length = (uint32_t) client_metadata_mr->length;
	client_send_sge.lkey = client_metadata_mr->lkey;

	bzero(&client_send_wr, sizeof(client_send_wr));
	client_send_wr.sg_list = &client_send_sge;
	client_send_wr.num_sge = 1;
	client_send_wr.opcode = IBV_WR_SEND;
	client_send_wr.send_flags = IBV_SEND_SIGNALED;
	
	ret = ibv_post_send(client_qp, &client_send_wr, &bad_client_send_wr);
	if (ret) {
		printf("Could not send client metadata \n");
		return -errno;
	}
	
	ret = process_wc_events(io_completion_channel, wc, 2);
	if(ret != 2) {
		printf("Could not get WC Events \n");
		return ret;
	}

	return 0;
}

//Perform RDAM Write and RDMA Read
static int perform_write_read() {
	struct ibv_wc wc;
	int ret = -1;

	int msg = strlen(send_buf);
	clock_t write_start, write_end, read_start, read_end;
	double write_elapsed_time, read_elapsed_time, write_throughput, read_throughput;

	write_start = clock();

	client_recv_buf_mr = roce_register_buffer(pd,recv_buf, strlen(send_buf), (IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ));
	if (!client_recv_buf_mr) {
		printf("Could not create RB \n");
		return -ENOMEM;
	}

	client_send_sge.addr = (uint64_t) client_send_buf_mr->addr;
	client_send_sge.length = (uint32_t) client_send_buf_mr->length;
	client_send_sge.lkey = client_send_buf_mr->lkey;

	bzero(&client_send_wr, sizeof(client_send_wr));
	client_send_wr.sg_list = &client_send_sge;
	client_send_wr.num_sge = 1;
	client_send_wr.opcode = IBV_WR_RDMA_WRITE;
	client_send_wr.send_flags = IBV_SEND_SIGNALED;

	client_send_wr.wr.rdma.rkey = server_metadata_attr.stag.remote_stag;
	client_send_wr.wr.rdma.remote_addr = server_metadata_attr.address;

	ret = ibv_post_send(client_qp, &client_send_wr, &bad_client_send_wr);
	if (ret) {
		printf("Could not write to buffer \n");
		return -errno;
	}

	ret = process_wc_events(io_completion_channel, &wc, 1);
	if(ret != 1) {
		printf("Could not get WC Events \n");
		return ret;
	}
	//WRITE is complete

	write_end = clock();

	write_elapsed_time = (double)(write_end - write_start) / CLOCKS_PER_SEC;
	write_throughput = (double)msg / (write_elapsed_time * 1000000);
	printf("WRITE throughput: %f MB/s \n", write_throughput);

	read_start = clock();

	client_send_sge.addr = (uint64_t) client_recv_buf_mr->addr;
	client_send_sge.length = (uint32_t) client_recv_buf_mr->length;
	client_send_sge.lkey = client_recv_buf_mr->lkey;

	bzero(&client_send_wr, sizeof(client_send_wr));
	client_send_wr.sg_list = &client_send_sge;
	client_send_wr.num_sge = 1;
	client_send_wr.opcode = IBV_WR_RDMA_READ;
	client_send_wr.send_flags = IBV_SEND_SIGNALED;

	client_send_wr.wr.rdma.rkey = server_metadata_attr.stag.remote_stag;
	client_send_wr.wr.rdma.remote_addr = server_metadata_attr.address;

	ret = ibv_post_send(client_qp, &client_send_wr, &bad_client_send_wr);
	if (ret) {
		printf("Could not read from buffer \n");
		return -errno;
	}

	ret = process_wc_events(io_completion_channel, &wc, 1);
	if(ret != 1) {
		printf("Could not get WC Events \n");
		return ret;
	}
	//READ is complete

	read_end = clock();
	read_elapsed_time = (double)(read_end - read_start) / CLOCKS_PER_SEC;
	read_throughput = (double)msg / (read_elapsed_time * 1000000);
	printf("READ throughput: %f MB/s \n", read_throughput);

	return 0;
}

//Disconnect and clean up resources
static int client_disconnect_and_clean() {
	struct rdma_cm_event *cm_event = NULL;
	int ret = -1;

	ret = rdma_disconnect(cm_client_id);
	if (ret) {
		printf("Could not disconnect \n");
	}

	ret = process_rdma_cm_event(cm_event_channel, RDMA_CM_EVENT_DISCONNECTED, &cm_event);
	if (ret) {
		printf("Could not get DISCONNECTED event \n");
	}

	ret = rdma_ack_cm_event(cm_event);
	if (ret) {
		printf("Could not acknowledge CM Event \n");
	}

	rdma_destroy_qp(cm_client_id);

	ret = rdma_destroy_id(cm_client_id);
	if (ret) {
		printf("Could not destroy Client ID \n");
	}
	
	ret = ibv_destroy_cq(client_cq);
	if (ret) {
		printf("Could not destroy CQ \n");
	}

	ret = ibv_destroy_comp_channel(io_completion_channel);
	if (ret) {
		printf("Could not destroy Comp Channel \n");
	}

	roce_deregister_buffer(server_metadata_mr);
	roce_deregister_buffer(client_metadata_mr);	
	roce_deregister_buffer(client_send_buf_mr);	
	roce_deregister_buffer(client_recv_buf_mr);	
	
	free(send_buf);
	free(recv_buf);
	
	ret = ibv_dealloc_pd(pd);
	if (ret) {
		printf("Could not destroy Client PD \n");
	}

	rdma_destroy_event_channel(cm_event_channel);

	return 0;
}

void show_usage() {
	printf("How to use: \n");
	printf("roce_client: [-a <server_ip>] [-p <server_port>] -s message (required)\n");
	exit(1);
}

int main(int argc, char **argv) {
	struct sockaddr_in server_sockaddr;
	int ret, option;
	bzero(&server_sockaddr, sizeof server_sockaddr);
	server_sockaddr.sin_family = AF_INET;
	server_sockaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	send_buf = recv_buf = msg = NULL;
	buf_size = 0;

	while ((option = getopt(argc, argv, "s:a:p:b:")) != -1) {
		switch (option) {
			case 's':
				printf("Passed message is : %s , with size %u \n", optarg, (unsigned int) strlen(optarg));
				msg = optarg;
				break;
			case 'a':
				ret = get_addr(optarg, (struct sockaddr*) &server_sockaddr);
				if (ret) {
					printf("IP invalid \n");
					return ret;
				}
				break;
			case 'p':
				server_sockaddr.sin_port = htons(strtol(optarg, NULL, 0)); 
				break;
			case 'b':
				buf_size = atoi(optarg);
				send_buf = calloc(buf_size, 1);
				recv_buf = calloc(buf_size, 1);
				if (!send_buf || !recv_buf) {
					printf("Could not allocate memory for buffers \n");
					return 1;
				}
				strncpy(send_buf, msg, buf_size);
				break;
 			default:
				show_usage();
				break;
		}
	}

	if (!server_sockaddr.sin_port) {
	  server_sockaddr.sin_port = htons(DEFAULT_RDMA_PORT);
	}

	if (send_buf == NULL) {
		printf("Please provide a message");
		show_usage();
    }

	ret = client_prepare_connection(&server_sockaddr);
	if (ret) { 
		printf("Could not start client connection \n");
		return ret;
	}

	ret = client_pre_post_recv_buffer(buf_size); 
	if (ret) { 
		printf("Could not set up client connection \n");
		return ret;
	}

	ret = client_connect_to_server();
	if (ret) { 
		printf("Could not connect to server \n");
		return ret;
	}

	ret = exchange_metadata();
	if (ret) {
		printf("Failed to setup client connection , ret = %d \n", ret);
		return ret;
	}

	ret = perform_write_read();
	if (ret) {
		printf("Could not perform WRITE/READ operations \n");
		return ret;
	}

	if (check_send_buf_recv_buf()) {
		printf("Functional test failed \n");
	} else {
		printf("Functional test was successful \n");
	}

	ret = client_disconnect_and_clean();
	if (ret) {
		printf("Could not disconnect/clean up \n");
	}

	return ret;
}
