// Code acknowledgement: Based on works by Animesh Trivedi (https://github.com/animeshtrivedi/rdma-example)
// Code extended and adapted for the use with RoCE

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
static struct roce_buffer_attr client_metadata_attr, server_metadata_attr;
static struct ibv_send_wr client_send_wr, *bad_client_send_wr = NULL;
static struct ibv_recv_wr server_recv_wr, *bad_server_recv_wr = NULL;
static struct ibv_sge client_send_sge, server_recv_sge;

//Send and receive buffer for RDMA connection
static char *send_buf = NULL, *recv_buf = NULL; 

//Basic functionality test to compare buffer memory blocks
static int check_send_buf_recv_buf() {
	return memcmp((void*) send_buf, (void*) recv_buf, strlen(send_buf));
}

//Prepare client side connection resources for RDMA connectio
static int client_prepare_connection(struct sockaddr_in *s_addr) {
	struct rdma_cm_event *cm_event = NULL;
	int ret = -1;

	//Open event channel and report asynchronous event to CM
	cm_event_channel = rdma_create_event_channel();
	if (!cm_event_channel) {
		printf("Could not create CM Event Channel \n");
		return -errno;
	}

	//Create connection identifier and associate it with RDMA connection
	ret = rdma_create_id(cm_event_channel, &cm_client_id, NULL, RDMA_PS_TCP);
	if (ret) {
		printf("Could not create CM ID /n"); 
		return -errno;
	}

	//Resolve destination address to RDMA address
	ret = rdma_resolve_addr(cm_client_id, NULL, (struct sockaddr*) s_addr, 2000);
	if (ret) {
		printf("Could not resolve address \n");
		return -errno;
	}

	//Report Address Resolved event to CM
	ret  = process_rdma_cm_event(cm_event_channel, RDMA_CM_EVENT_ADDR_RESOLVED, &cm_event);
	if (ret) {
		printf("Could not receive valid CM Event \n");
		return ret;
	}

	//Acknowledge CM event
	ret = rdma_ack_cm_event(cm_event);
	if (ret) {
		printf("Could not acknowledge CM Event \n");
		return -errno;
	}

	//Resolve RDMA route to destination address
	ret = rdma_resolve_route(cm_client_id, 2000);
	if (ret) {
		printf("Could not resolve route \n");
	       return -errno;
	}

	//Report Route Resolved event to CM
	ret = process_rdma_cm_event(cm_event_channel, RDMA_CM_EVENT_ROUTE_RESOLVED, &cm_event);
	if (ret) {
		printf("Could not receive valid event \n");
		return ret;
	}

	//Acknowledge CM event
	ret = rdma_ack_cm_event(cm_event);
	if (ret) {
		printf("Could not acknowledge CM Event \n");
		return -errno;
	}

	printf("Trying to connect to server at : %s port: %d \n", inet_ntoa(s_addr->sin_addr), ntohs(s_addr->sin_port));

	//Create Protection Domain
	pd = ibv_alloc_pd(cm_client_id->verbs);
	if (!pd) {
		printf("Could not allocate PD \n");
		return -errno;
	}

	//Create Completion Channel for I/O Completion Notifications
	io_completion_channel = ibv_create_comp_channel(cm_client_id->verbs);
	if (!io_completion_channel) {
		printf("Could not create Comp Channel \n");
		return -errno;
	}

	//Create Completion Queue for I/O Completion Metadata
	client_cq = ibv_create_cq(cm_client_id->verbs, CQ_CAPACITY, NULL, io_completion_channel, 0);
	if (!client_cq) {
		printf("Could not create CQ \n");
		return -errno;
	}

	//Request CQ Notifications
	ret = ibv_req_notify_cq(client_cq, 0);
	if (ret) {
		printf("Could not request CQ notifications /n");
		return -errno;
	}

	//Set up send and receive Queue Pair queues and their capacity
    bzero(&qp_init_attr, sizeof qp_init_attr);
    qp_init_attr.cap.max_recv_sge = MAX_SGE;
    qp_init_attr.cap.max_recv_wr = MAX_WR;
    qp_init_attr.cap.max_send_sge = MAX_SGE;
    qp_init_attr.cap.max_send_wr = MAX_WR;
    qp_init_attr.qp_type = IBV_QPT_RC;
	qp_init_attr.recv_cq = client_cq; 
    qp_init_attr.send_cq = client_cq;

	//Create Queue Pair
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

	//Register buffer for metadata
	server_metadata_mr = roce_register_buffer(pd, &server_metadata_attr, sizeof(server_metadata_attr), (IBV_ACCESS_LOCAL_WRITE));
	if(!server_metadata_mr){
		printf("Could not set up server metadata \n");
		return -ENOMEM;
	}

	//Fill up SGE
	server_recv_sge.addr = (uint64_t) server_metadata_mr->addr;
	server_recv_sge.length = (uint32_t) server_metadata_mr->length;
	server_recv_sge.lkey = (uint32_t) server_metadata_mr->lkey;

	//Link SGE to Receive Work Request
	bzero(&server_recv_wr, sizeof(server_recv_wr));

	server_recv_wr.sg_list = &server_recv_sge;
	server_recv_wr.num_sge = 1;

	//Pre-post receive buffer
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

	//Set up connection parameters
	bzero(&conn_param, sizeof(conn_param));
	conn_param.initiator_depth = 3;
	conn_param.responder_resources = 3;
	conn_param.retry_count = 3;

	//Connect to server
	ret = rdma_connect(cm_client_id, &conn_param);
	if (ret) {
		printf("Could not connect to remote host \n");
		return -errno;
	}

	//Process CM event
	ret = process_rdma_cm_event(cm_event_channel, RDMA_CM_EVENT_ESTABLISHED, &cm_event);
	if (ret) {
		printf("Could not get CM Event \n");
	       return ret;
	}

	//Acknowledge CM event
	ret = rdma_ack_cm_event(cm_event);
	if (ret) {
		printf("Could not acknowledge CM Event \n");
		return -errno;
	}

	printf("The client was connected successfully \n");

	return 0;
}

//Exchange buffer metadata with server
static int exchange_metadata() {
	struct ibv_wc wc[2];
	int ret = -1;

	//Register Memory Region for send buffer
	client_send_buf_mr = roce_register_buffer(pd, send_buf, strlen(send_buf), (IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE));
	if(!client_send_buf_mr) {
		printf("Could not register buffer \n");
		return ret;
	}

	//Prepate metadata for buffer
	client_metadata_attr.address = (uint64_t) client_send_buf_mr->addr; 
	client_metadata_attr.length = client_send_buf_mr->length; 
	client_metadata_attr.stag.local_stag = client_send_buf_mr->lkey;

	//Register metadata Memory Region
	client_metadata_mr = roce_register_buffer(pd, &client_metadata_attr, sizeof(client_metadata_attr), IBV_ACCESS_LOCAL_WRITE);
	if(!client_metadata_mr) {
		printf("Could not register buffer \n");
		return ret;
	}

	//Fill up SGE
	client_send_sge.addr = (uint64_t) client_metadata_mr->addr;
	client_send_sge.length = (uint32_t) client_metadata_mr->length;
	client_send_sge.lkey = client_metadata_mr->lkey;

	//Link SGE to Send Work Request
	bzero(&client_send_wr, sizeof(client_send_wr));

	client_send_wr.sg_list = &client_send_sge;
	client_send_wr.num_sge = 1;
	client_send_wr.opcode = IBV_WR_SEND;
	client_send_wr.send_flags = IBV_SEND_SIGNALED;

	//Post Send Work Request
	ret = ibv_post_send(client_qp, &client_send_wr, &bad_client_send_wr);
	if (ret) {
		printf("Could not send client metadata \n");
		return -errno;
	}

	//Expect WC Events for send and receive
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

	//Define variables for benchmarks
	int msg_size = strlen(send_buf);
	struct timeval write_start, write_end, read_start, read_end;
	double write_elapsed_time, read_elapsed_time, write_throughput, read_throughput;

	//Start WRITE benchmark
	gettimeofday(&write_start, NULL);

	//Perform RDMA Write
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

	//Expect WC event for WRITE 
	ret = process_wc_events(io_completion_channel, &wc, 1);
	if(ret != 1) {
		printf("Could not get WC Events \n");
		return ret;
	}
	//WRITE is complete

	//Finish WRITE benchmark
	gettimeofday(&write_end, NULL);

	//Calculate WRITE throughput
	write_elapsed_time = (write_end.tv_sec - write_start.tv_sec) * 1000000.0 + (write_end.tv_usec - write_start.tv_usec);
	write_throughput = (msg_size / 1e6) / (write_elapsed_time / 1e6);

	printf("WRITE throughput: %f MB/s \n", write_throughput);

	//Start READ benchmark
	gettimeofday(&read_start, NULL);

	//Perform RDMA Read
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

	//Expect WC event for READ 
	ret = process_wc_events(io_completion_channel, &wc, 1);
	if(ret != 1) {
		printf("Could not get WC Events \n");
		return ret;
	}
	//READ is complete

	//Finish READ benchmark
	gettimeofday(&read_end, NULL);

	//Calculate READ throughput
	read_elapsed_time = (read_end.tv_sec - read_start.tv_sec) * 1000000.0 + (read_end.tv_usec - read_start.tv_usec);
	read_throughput = (msg_size / 1e6) / (read_elapsed_time / 1e6);

	printf("READ throughput: %f MB/s \n", read_throughput);

	return 0;
}

//Disconnect from server and clean up resources
static int client_disconnect_and_clean() {
	struct rdma_cm_event *cm_event = NULL;
	int ret = -1;

	//Client side actively disconnects from server
	ret = rdma_disconnect(cm_client_id);
	if (ret) {
		printf("Could not disconnect \n");
	}

	//Report Disconnect event to CM
	ret = process_rdma_cm_event(cm_event_channel, RDMA_CM_EVENT_DISCONNECTED, &cm_event);
	if (ret) {
		printf("Could not receive valid CM event \n");
	}

	//Acknowledge Disconnect event
	ret = rdma_ack_cm_event(cm_event);
	if (ret) {
		printf("Could not acknowledge CM Event \n");
	}

	//Destroy Queue Pair
	rdma_destroy_qp(cm_client_id);

	//Destroy Client CM ID
	ret = rdma_destroy_id(cm_client_id);
	if (ret) {
		printf("Could not destroy Client ID \n");
	}

	//Destroy Completion Queue
	ret = ibv_destroy_cq(client_cq);
	if (ret) {
		printf("Could not destroy CQ \n");
	}

	//Destroy I/O Completion Channel
	ret = ibv_destroy_comp_channel(io_completion_channel);
	if (ret) {
		printf("Could not destroy Comp Channel \n");
	}

	//Destroy buffers
	roce_deregister_buffer(server_metadata_mr);
	roce_deregister_buffer(client_metadata_mr);	
	roce_deregister_buffer(client_send_buf_mr);	
	roce_deregister_buffer(client_recv_buf_mr);	

	//Free buffers
	free(send_buf);
	free(recv_buf);

	//Destroy Protection Domain
	ret = ibv_dealloc_pd(pd);
	if (ret) {
		printf("Could not destroy Client PD \n");
	}

	//Destroy and close Event Channel
	rdma_destroy_event_channel(cm_event_channel);

	return 0;
}

//Print usage of roce_client.c
void show_usage() {
	printf("How to use: \n");
	printf("roce_client: -a <server_ip> (required) [-p <server_port> (optional)] -s <message size> (required)\n");
	exit(1);
}

//Main function
int main(int argc, char **argv) {
	struct sockaddr_in server_sockaddr;
	int ret, option, msg_size;
	bzero(&server_sockaddr, sizeof server_sockaddr);
	server_sockaddr.sin_family = AF_INET;
	server_sockaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	//Set buffers to NULL
	send_buf = recv_buf = NULL; 

	//Parse command line arguments
	while ((option = getopt(argc, argv, "s:a:p:")) != -1) {
		switch (option) {
			case 's':
				//Parse message size from command line and initialise buffers accordingly
				msg_size = atoi(optarg);
				printf("Payload size: %d bytes \n", msg_size);
				send_buf = calloc(msg_size, sizeof(char));

				if (!send_buf) {
					printf("Could not allocate memory \n");
					return -ENOMEM;
				}

				for (int i = 0; i < msg_size; i++) {
					send_buf[i] = 'A';
				}

				recv_buf = calloc(msg_size, sizeof(char));
				if (!recv_buf) {
					printf("Could not allocate memory \n");
					free(send_buf);
					return -ENOMEM;
				}

				break;
			case 'a':
				//Set destination IP address
				ret = get_addr(optarg, (struct sockaddr*) &server_sockaddr);
				if (ret) {
					printf("IP invalid \n");
					return ret;
				}
				break;
			case 'p':
				//Override default port
				server_sockaddr.sin_port = htons(strtol(optarg, NULL, 0)); 
				break;
			default:
				show_usage();
				break;
		}
	}

	//Set default port if not specified
	if (!server_sockaddr.sin_port) {
	  server_sockaddr.sin_port = htons(DEFAULT_RDMA_PORT);
	}

	//Check if message size is specified
	if (send_buf == NULL) {
		printf("Please provide a message");
		show_usage();
    }

	//Call all client-side functions 
	ret = client_prepare_connection(&server_sockaddr);
	if (ret) { 
		printf("Could not start client connection \n");
		return ret;
	}

	ret = client_pre_post_recv_buffer(); 
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

	printf("--------------------\n");

	return ret;
}
