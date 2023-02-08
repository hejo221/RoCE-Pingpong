/* Based on the "Remote Direct Memory Access" document from IBM Corp. (see ibm.com/docs/en/ssw_aix_72/rdma/rdma_pdf.pdf) and adapted for the use in RoCE */


#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <rdma/rdma_cma.h>

int main(int argc, char *argv[]) 
{ 
    int error;
     
    //Set up event chanel
    struct rdma_event_channel *cm_channel;
    cm_channel = rdma_create_event_channel();
    if (!cm_channel) {
        return 1;
    }

    //Get CM ID
    struct rdma_cm_id *listen_id;
    error = rdma_create_id(cm_channel, &listen_id, NULL, RDMA_PS_UDP); 
    if (error) {
        return error;
    }

    struct sockaddr_in sin;
    sin.sin_family = AF_INET; 
    sin.sin_port = htons(4791);
    sin.sin_addr.s_addr = INADDR_ANY;
    
    //Listen for connection request
    struct rdma_cm_event *event;
    struct rdma_cm_id *cm_id; 
    error = rdma_bind_addr(listen_id,(struct sockaddr *)&sin);
    if (error) {
        return 1;
    }
    error = rdma_listen(listen_id, 1);
    if (error) {
        return 1;
    }

    while(1) {
        error = rdma_get_cm_event(cm_channel,&event);
        if (error)
            return error;
        if (event->event != RDMA_CM_EVENT_CONNECT_REQUEST)
            return 1;
        cm_id = event->id;
        rdma_ack_cm_event(event);

        //Allocate Protection Domain
        struct ibv_pd *pd;
        pd = ibv_alloc_pd(cm_id->verbs);
        if (!pd) {
            return 1;
        }    

        //Set up completion event channel
        struct ibv_comp_channel *comp_chan; 
        comp_chan = ibv_create_comp_channel(cm_id->verbs);
        if (!comp_chan) {
            return 1;
        }    

        //Set up Completion Queue
        struct ibv_cq *cq;  
        struct ibv_cq  *evt_cq;
        cq = ibv_create_cq(cm_id->verbs,2,NULL,comp_chan,0); 
        if (!cq) {
            return 1;
        }    

        //Waits for work completion
        if (ibv_req_notify_cq(cq,0)) {
            return 1;
        }     

        uint32_t *buf;
        buf = calloc(2,sizeof(uint32_t));
        if (!buf) {
            return 1;
        }

        //Allocate memory region
        struct ibv_mr *mr;
        mr = ibv_reg_mr(pd,buf,2*sizeof(uint32_t), 
            IBV_ACCESS_LOCAL_WRITE | 
            IBV_ACCESS_REMOTE_READ | 
            IBV_ACCESS_REMOTE_WRITE); 
        if (!mr) {
            return 1;
        }
        struct rdma_conn_param conn_param = { };
        struct ibv_qp_init_attr qp_attr = { };
        
        memset(&qp_attr,0,sizeof(qp_attr));
        qp_attr.cap.max_send_wr = 1;
        qp_attr.cap.max_send_sge = 1;
        qp_attr.cap.max_recv_wr = 1;
        qp_attr.cap.max_recv_sge = 1;

        qp_attr.send_cq = cq;
        qp_attr.recv_cq = cq;

        qp_attr.qp_type = IBV_QPT_RC;
        
        //Create Queue Pair
        error = rdma_create_qp(cm_id,pd,&qp_attr); 
        if (error) {
	        printf("Creating Queue Pair failed");
            return error;
	    }

        struct pdata { 
            uint64_t buf_va; 
            uint32_t buf_rkey; 
        };
        struct pdata rep_pdata;        
        
        rep_pdata.buf_va = htonl((uintptr_t)buf);
        
        //Prepare key for client
        rep_pdata.buf_rkey = htonl(mr->rkey); 
	    conn_param.responder_resources = 1;  
        conn_param.private_data = &rep_pdata; 
        conn_param.private_data_len = sizeof(rep_pdata);

        //Accept connection
        error = rdma_accept(cm_id,&conn_param); 
        if (error) 
            return 1;
        error = rdma_get_cm_event(cm_channel,&event);
        if (error) 
            return error;
        if (event->event != RDMA_CM_EVENT_ESTABLISHED)
            return 1;
        rdma_ack_cm_event(event);

        //Send message back
	    printf("The message was from the client was: %d and %d\n", ntohl(buf[0]), ntohl(buf[1]));
        buf[0] = htonl(ntohl(buf[0]) * ntohl(buf[1]));
	
        //Post send
        struct ibv_sge sge; 
        struct ibv_send_wr send_wr = { };
        struct ibv_send_wr *bad_send_wr; 
        struct ibv_recv_wr recv_wr = { };
        struct ibv_recv_wr *bad_recv_wr;

        sge.addr = (uintptr_t)buf; 
        sge.length = sizeof(uint32_t); 
        sge.lkey = mr->lkey;
    
        send_wr.opcode = IBV_WR_SEND;
        send_wr.send_flags = IBV_SEND_SIGNALED;
        send_wr.sg_list = &sge;
        send_wr.num_sge = 1;

        if (ibv_post_send(cm_id->qp,&send_wr,&bad_send_wr)) 
            return 1;

        //Ack ibv events
        struct ibv_wc wc;
        void *cq_context;

		if (ibv_get_cq_event(comp_chan,&evt_cq,&cq_context))
			return 1;
		if (ibv_req_notify_cq(cq,0))
			return 1;
		if (ibv_poll_cq(cq,1,&wc) != 1)
			return 1;
		if (wc.status != IBV_WC_SUCCESS)
			return 1;

        ibv_ack_cq_events(cq,2);

        
	    error = rdma_get_cm_event(cm_channel,&event);
    	if (error) {
        	return error;
        }
	    rdma_ack_cm_event(event);

        //Finish transmission and close event channel
	    if (event->event == RDMA_CM_EVENT_DISCONNECTED) {
		    rdma_destroy_qp(cm_id);
		    ibv_dereg_mr(mr);
		    free(buf);
  		    error = rdma_destroy_id(cm_id);
		    if (error != 0)
			    printf("Destroying CM ID failed");
	    }
    }
	rdma_destroy_event_channel(cm_channel);
    return 0;
}
