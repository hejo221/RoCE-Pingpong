#include "roce_common.h"

//Allocate buffer of given size
struct ibv_mr* roce_alloc_buffer(struct ibv_pd *pd, uint32_t size, enum ibv_access_flags permission) {
	struct ibv_mr *mr = NULL;
	if (!pd) {
		printf("Protection domain NULL \n");
		return NULL;
	}

	void *buf = calloc(1, size);
	if (!buf) {
		printf("Could not allocate buffer \n");
		return NULL;
	}

	mr = roce_register_buffer(pd, buf, size, permission);
	if(!mr) {
		free(buf);
	}

	return mr;
}

//Register allocated memory
struct ibv_mr *roce_register_buffer(struct ibv_pd *pd, void *addr, uint32_t length, enum ibv_access_flags permission) {
	struct ibv_mr *mr = NULL;
	if (!pd) {
		printf("Protection domain NULL \n");
		return NULL;
	}

	mr = ibv_reg_mr(pd, addr, length, permission);
	if (!mr) {
		printf("Could not create memory region \n");
		return NULL;
	}

	return mr;
}

//Free allocated bufferss
void roce_free_buffer(struct ibv_mr *mr) {
	if (!mr) {
		printf("Passed memory region NULL \n");
		return ;
	}

	void *to_free = mr->addr;
	roce_deregister_buffer(mr);
	free(to_free);
}

//Deregister registered memory
void roce_deregister_buffer(struct ibv_mr *mr) {
	if (!mr) { 
		printf("Passed memory region NULL \n");
		return;
	}
	ibv_dereg_mr(mr);
}

//Process RDMA CM Eveent
int process_rdma_cm_event(struct rdma_event_channel *echannel, enum rdma_cm_event_type expected_event, struct rdma_cm_event **cm_event) {
	int ret = 1;

	ret = rdma_get_cm_event(echannel, cm_event);
	if (ret) {
		printf("Could not get CM Event \n");
		return -errno;
	}

	if(0 != (*cm_event)->status){
		printf("CM Event with wrong status \n");
		ret = -((*cm_event)->status);
		rdma_ack_cm_event(*cm_event);
		return ret;
	}
	
	//Check if Event is of expected type
	if ((*cm_event)->event != expected_event) {
		printf("Unexpected event received \n");
		rdma_ack_cm_event(*cm_event);
		return -1;
	}

	return ret;
}

//Process WC Events
int process_wc_events (struct ibv_comp_channel *comp_channel, struct ibv_wc *wc, int max_wc) {
	struct ibv_cq *cq_ptr = NULL;
	void *context = NULL;
	int ret = -1, i, total_wc = 0;

	ret = ibv_get_cq_event(comp_channel, &cq_ptr, &context);
    if (ret) {
	    printf("Could not get next CQ event \n");
	    return -errno;
    }
    
    ret = ibv_req_notify_cq(cq_ptr, 0);
    if (ret){
	    printf("Could not request more notifications \n");
	    return -errno;
    }
    
    total_wc = 0;
    do {
	    ret = ibv_poll_cq(cq_ptr, max_wc - total_wc, wc + total_wc);
	    if (ret < 0) {
		    printf("Could not poll CQ for WC \n");
		    return ret;
	    }
	    total_wc += ret;
    } while (total_wc < max_wc); 
       
	for( i = 0 ; i < total_wc ; i++) {
	    if (wc[i].status != IBV_WC_SUCCESS) {
		    printf("WC returned error \n");
		    return -(wc[i].status);
	    }
    }

    ibv_ack_cq_events(cq_ptr, 1);
    return total_wc; 
}

int get_addr(char *dst, struct sockaddr *addr) {
	struct addrinfo *res;
	int ret = -1;

	ret = getaddrinfo(dst, NULL, NULL, &res);
	if (ret) {
		printf("Could not get address information \n");
		return ret;
	}

	memcpy(addr, res->ai_addr, sizeof(struct sockaddr_in));
	freeaddrinfo(res);
	return ret;
}
