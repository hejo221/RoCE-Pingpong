#include "roce_common.h"
#include "roce_common.c"

//Basic Resources for RDMA connection
static struct rdma_event_channel *cm_event_channel = NULL;
static struct rdma_cm_id *cm_server_id = NULL, *cm_client_id = NULL;
static struct ibv_pd *pd = NULL;
static struct ibv_comp_channel *io_completion_channel = NULL;
static struct ibv_cq *cq = NULL;
static struct ibv_qp_init_attr qp_init_attr;
static struct ibv_qp *client_qp = NULL;

//Memory resources for RDMA connection
static struct ibv_mr *client_metadata_mr = NULL, *server_buffer_mr = NULL, *server_metadata_mr = NULL;
static struct rdma_buffer_attr client_metadata_attr, server_metadata_attr;
static struct ibv_recv_wr client_recv_wr, *bad_client_recv_wr = NULL;
static struct ibv_send_wr server_send_wr, *bad_server_send_wr = NULL;
static struct ibv_sge client_recv_sge, server_send_sge;

//Prepare client connection before accepting it
static int setup_client_resources() {
	int ret = -1;
	if(!cm_client_id){
		printf("Client id NULL \n");
		return -EINVAL;
	}
	
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

	cq = ibv_create_cq(cm_client_id->verbs, CQ_CAPACITY, NULL, io_completion_channel, 0);
	if (!cq) {
		printf("CC \n");
		return -errno;
	}

	ret = ibv_req_notify_cq(cq, 0);
	if (ret) {
		printf("Activities on CQ could not be requested \n");
		return -errno;
	}

    bzero(&qp_init_attr, sizeof qp_init_attr);
	qp_init_attr.cap.max_recv_sge = MAX_SGE;
    qp_init_attr.cap.max_recv_wr = MAX_WR;
    qp_init_attr.cap.max_send_sge = MAX_SGE;
    qp_init_attr.cap.max_send_wr = MAX_WR;
    qp_init_attr.qp_type = IBV_QPT_RC;
    qp_init_attr.recv_cq = cq;
    qp_init_attr.send_cq = cq;
       
    ret = rdma_create_qp(cm_client_id, pd, &qp_init_attr);
    if (ret) {
	    printf("Could not create Queue Pair \n");
	    return -errno;
    }

    client_qp = cm_client_id->qp;
    return ret;
}

//Start RDMA server
static int start_roce_server(struct sockaddr_in *server_addr) {
	struct rdma_cm_event *cm_event = NULL;
	int ret = -1;
	
	cm_event_channel = rdma_create_event_channel();
	if (!cm_event_channel) {
		printf("Could not create CM Event Channel \n");
		return -errno;
	}
	
	ret = rdma_create_id(cm_event_channel, &cm_server_id, NULL, RDMA_PS_UDP);
	if (ret) {
		printf("Could not create CM ID \n");
		return -errno;
	}
	
	ret = rdma_bind_addr(cm_server_id, (struct sockaddr*) server_addr);
	if (ret) {
		printf("Could not bind server address \n");
		return -errno;
	}
	
	ret = rdma_listen(cm_server_id, 8);
	if (ret) {
		printf("Could not listen on server address \n");
		return -errno;
	}
	printf("Server is listening successfully at: %s , port: %d \n",
			inet_ntoa(server_addr->sin_addr),
			ntohs(server_addr->sin_port));
	
	ret = process_rdma_cm_event(cm_event_channel, RDMA_CM_EVENT_CONNECT_REQUEST, &cm_event);
	if (ret) {
		printf("Could not get CM Event \n");
		return ret;
	}

	cm_client_id = cm_event->id;
	ret = rdma_ack_cm_event(cm_event);
	if (ret) {
		printf("Could not acknowledge CM Event \n");
		return -errno;
	}

	return ret;
}

// Pre-post RB and accept client connection
static int accept_client_connection() {
	struct rdma_conn_param conn_param;
	struct rdma_cm_event *cm_event = NULL;
	struct sockaddr_in remote_sockaddr; 
	int ret = -1;

	if (!cm_client_id || !client_qp) {
		printf("Could not set up client resources \n");
		return -EINVAL;
	}
	

    client_metadata_mr = roce_register_buffer(pd, &client_metadata_attr, sizeof(client_metadata_attr), (IBV_ACCESS_LOCAL_WRITE));
	if (!client_metadata_mr){
		printf("Could not register client metadata \n");
		return -ENOMEM;
	}

	client_recv_sge.addr = (uint64_t) client_metadata_mr->addr; 
	client_recv_sge.length = client_metadata_mr->length;
	client_recv_sge.lkey = client_metadata_mr->lkey;
	
	bzero(&client_recv_wr, sizeof(client_recv_wr));
	client_recv_wr.sg_list = &client_recv_sge;
	client_recv_wr.num_sge = 1;

	ret = ibv_post_recv(client_qp, &client_recv_wr, &bad_client_recv_wr);
	if (ret) {
		printf("Could not pre-post RB \n");
		return ret;
	}
    
	memset(&conn_param, 0, sizeof(conn_param));
    conn_param.initiator_depth = 3;
    conn_param.responder_resources = 3;
       
	ret = rdma_accept(cm_client_id, &conn_param);
    if (ret) {
	    printf("Could not accept connection \n");
	    return -errno;
    }
       
	ret = process_rdma_cm_event(cm_event_channel, RDMA_CM_EVENT_ESTABLISHED, &cm_event);
    if (ret) {
		printf("Could not get CM event \n");
		return -errno;
	}

	ret = rdma_ack_cm_event(cm_event);
	if (ret) {
		printf("Could not acknowledge CM event \n");
		return -errno;
	}

	memcpy(&remote_sockaddr, rdma_get_peer_addr(cm_client_id), sizeof(struct sockaddr_in));

	printf("A new connection was accepted from %s \n", inet_ntoa(remote_sockaddr.sin_addr));

	return ret;
}

//Send server metadata to client
static int send_server_metadata_to_client() {
	struct ibv_wc wc;
	int ret = -1;
	
	ret = process_wc_events(io_completion_channel, &wc, 1);
	if (ret != 1) {
		printf("Failed to receive , ret = %d \n", ret);
		return ret;
	}

    server_buffer_mr = roce_alloc_buffer(pd, client_metadata_attr.length, (IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE));
    if(!server_buffer_mr){
	    printf("Server failed to create a buffer \n");
	    return -ENOMEM;
    }
    
    server_metadata_attr.address = (uint64_t) server_buffer_mr->addr;
    server_metadata_attr.length = (uint32_t) server_buffer_mr->length;
    server_metadata_attr.stag.local_stag = (uint32_t) server_buffer_mr->lkey;

    server_metadata_mr = roce_register_buffer(pd, &server_metadata_attr, sizeof(server_metadata_attr), IBV_ACCESS_LOCAL_WRITE);
    if(!server_metadata_mr){
	    printf("Server failed to create to hold server metadata \n");
	    return -ENOMEM;
    }
    
    server_send_sge.addr = (uint64_t) &server_metadata_attr;
    server_send_sge.length = sizeof(server_metadata_attr);
    server_send_sge.lkey = server_metadata_mr->lkey;
    
    bzero(&server_send_wr, sizeof(server_send_wr));
    server_send_wr.sg_list = &server_send_sge;
    server_send_wr.num_sge = 1;
    server_send_wr.opcode = IBV_WR_SEND; 
    server_send_wr.send_flags = IBV_SEND_SIGNALED;

    ret = ibv_post_send(client_qp, &server_send_wr, &bad_server_send_wr);
    if (ret) {
	    printf("Could not post server metadata \n");
	    return -errno;
    }

    ret = process_wc_events(io_completion_channel, &wc, 1);
    if (ret != 1) {
	    printf("Could not send server metadata \n");
	    return ret;
    }

    return 0;
}

//Wait for client to disconnect and clean up resources
static int disconnect_and_cleanup() {
	struct rdma_cm_event *cm_event = NULL;
	int ret = -1;

    ret = process_rdma_cm_event(cm_event_channel, RDMA_CM_EVENT_DISCONNECTED, &cm_event);
    if (ret) {
	    printf("Could not get disconnect Event \n");
	    return ret;
    }

	ret = rdma_ack_cm_event(cm_event);
	if (ret) {
		printf("Could not acknowledge CM Event \n");
		return -errno;
	}
	
	rdma_destroy_qp(cm_client_id);
	
	ret = rdma_destroy_id(cm_client_id);
	if (ret) {
		printf("Could not destroy Client CM ID \n");
	}

	ret = ibv_destroy_cq(cq);
	if (ret) {
		printf("Could not destroy CQ \n");
	}

	ret = ibv_destroy_comp_channel(io_completion_channel);
	if (ret) {
		printf("Could not destr \n");
	}
	
	roce_free_buffer(server_buffer_mr);
	roce_deregister_buffer(server_metadata_mr);	
	roce_deregister_buffer(client_metadata_mr);	

	ret = ibv_dealloc_pd(pd);
	if (ret) {
		printf("Could not destroy PD \n");
	}

	ret = rdma_destroy_id(cm_server_id);
	if (ret) {
		printf("Could not destroy Server CM ID \n");
	}

	rdma_destroy_event_channel(cm_event_channel);

	return 0;
}

//Print usage for to start roce_server
void show_usage() 
{
	printf("How to use: \n");
	printf("roce_server: [-a <server_ip>] [-p <server_port>] \n");
	exit(1);
}

int main(int argc, char **argv) 
{
	int ret, option;
	struct sockaddr_in server_sockaddr;
	bzero(&server_sockaddr, sizeof server_sockaddr);
	server_sockaddr.sin_family = AF_INET;
	server_sockaddr.sin_addr.s_addr = htonl(INADDR_ANY);
	
	while ((option = getopt(argc, argv, "a:p:")) != -1) {
		switch (option) {
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
			default:
				show_usage();
				break;
		}
	}

	if (!server_sockaddr.sin_port) {
		server_sockaddr.sin_port = htons(DEFAULT_RDMA_PORT);
	}

	ret = start_roce_server(&server_sockaddr);
	if (ret) {
		printf("Could not start server \n");
		return ret;
	}

	ret = setup_client_resources();
	if (ret) { 
		printf("Could not set up client resources /n");
		return ret;
	}

	ret = accept_client_connection();
	if (ret) {
		printf("Could not accept client connection \n");
		return ret;
	}

	ret = send_server_metadata_to_client();
	if (ret) {
		printf("Could not send server metadata to client \n");
		return ret;
	}

	ret = disconnect_and_cleanup();
	if (ret) { 
		printf("Could not disconnect/clean up \n");
		return ret;
	}

	return 0;
}
