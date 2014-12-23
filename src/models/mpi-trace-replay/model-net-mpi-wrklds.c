/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */
#include <ross.h>
#include <inttypes.h>

#include "codes/codes-nw-workload.h"
#include "codes/codes.h"
#include "codes/configuration.h"
#include "codes/codes_mapping.h"
#include "codes/model-net.h"

#define TRACE -1
#define DEBUG 0

char workload_type[128];
char workload_file[8192];
char offset_file[8192];
static int wrkld_id;
static int num_net_traces = 0;

typedef struct nw_state nw_state;
typedef struct nw_message nw_message;
typedef int16_t dumpi_req_id;

static int net_id = 0;
static float noise = 5.0;
static int num_net_lps, num_nw_lps;
long long num_bytes_sent=0;
long long num_bytes_recvd=0;
double max_time = 0,  max_comm_time = 0, max_wait_time = 0, max_send_time = 0, max_recv_time = 0;
double avg_time = 0, avg_comm_time = 0, avg_wait_time = 0, avg_send_time = 0, avg_recv_time = 0;

/* global variables for codes mapping */
static char lp_group_name[MAX_NAME_LENGTH], lp_type_name[MAX_NAME_LENGTH], annotation[MAX_NAME_LENGTH];
static int mapping_grp_id, mapping_type_id, mapping_rep_id, mapping_offset;

enum MPI_NW_EVENTS
{
	MPI_OP_GET_NEXT=1,
	MPI_SEND_ARRIVED,
	MPI_SEND_POSTED,
};

/* stores pointers of pending MPI operations to be matched with their respective sends/receives. */
struct mpi_msgs_queue
{
	mpi_event_list* mpi_op;
	struct mpi_msgs_queue* next;
};

/* stores request ID of completed MPI operations (Isends or Irecvs) */
struct completed_requests
{
	dumpi_req_id req_id;
	struct completed_requests* next;
};

/* for wait operations, store the pending operation and number of completed waits */
struct pending_waits
{
	mpi_event_list* mpi_op;
	int num_completed;
	tw_stime start_time;
};

/* maintains the head and tail of the queue, as well as the number of elements currently in queue */
struct mpi_queue_ptrs
{
	int num_elems;
	struct mpi_msgs_queue* queue_head;
	struct mpi_msgs_queue* queue_tail;
};

/* state of the network LP. It contains the pointers to send/receive lists */
struct nw_state
{
	long num_events_per_lp;
	tw_lpid nw_id;
	short wrkld_end;

	/* count of sends, receives, collectives and delays */
	unsigned long num_sends;
	unsigned long num_recvs;
	unsigned long num_cols;
	unsigned long num_delays;
	unsigned long num_wait;
	unsigned long num_waitall;
	unsigned long num_waitsome;

	/* time spent by the LP in executing the app trace*/
	double elapsed_time;

	/* time spent in compute operations */
	double compute_time;

	/* search time */
	double search_overhead;

	/* time spent in message send/isend */
	double send_time;

	/* time spent in message receive */
	double recv_time;
	
	/* time spent in wait operation */
	double wait_time;

	/* FIFO for isend messages arrived on destination */
	struct mpi_queue_ptrs* arrival_queue;

	/* FIFO for irecv messages posted but not yet matched with send operations */
	struct mpi_queue_ptrs* pending_recvs_queue;

	/* list of pending waits */
	struct pending_waits* pending_waits;

	/* List of completed send/receive requests */
	struct completed_requests* completed_reqs;
};

/* network event being sent. msg_type is the type of message being sent, found_match is the index of the list maintained for reverse computation, op is the MPI event to be executed/reversed */
struct nw_message
{
	int msg_type;
	int found_match;
	//dumpi_req_id matched_recv;
        struct mpi_event_list op;
};

/* initialize queues, get next operation */
static void get_next_mpi_operation(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp);

/* upon arrival of local completion message, inserts operation in completed send queue */
static void update_send_completion_queue(nw_state*s, tw_bf* bf, nw_message* m, tw_lp * lp);

/* reverse of the above function */
static void update_send_completion_queue_rc(nw_state*s, tw_bf* bf, nw_message* m, tw_lp * lp);

/* upon arrival of an isend operation, updates the arrival queue of the network */
static void update_arrival_queue(nw_state*s, tw_bf* bf, nw_message* m, tw_lp * lp);

/* reverse of the above function */
static void update_arrival_queue_rc(nw_state*s, tw_bf* bf, nw_message* m, tw_lp * lp);

/* insert MPI operation in the waiting queue*/
static void mpi_pending_queue_insert_op(struct mpi_queue_ptrs* mpi_queue, mpi_event_list* mpi_op);

/* remove completed request IDs from the queue for reuse */
static void remove_req_id(struct completed_requests** requests, int16_t req_id);

/* remove MPI operation from the waiting queue.
is_blocking is an output parameter which tells if the matched operation was blocking receive or not 
dumpi_req_id is an output parameter which tells the request ID of the matched receive operation*/
static int mpi_queue_remove_matching_op(nw_state* s, tw_lp* lp, tw_lpid lpid, struct mpi_queue_ptrs* mpi_queue, mpi_event_list* mpi_op, int* is_blocking, dumpi_req_id* req_id);

/* remove the tail of the MPI operation from waiting queue */
static int mpi_queue_remove_tail(tw_lpid lpid, struct mpi_queue_ptrs* mpi_queue, mpi_event_list* mpi_op);

/* insert completed MPI requests in the queue. */
static int mpi_completed_queue_insert_op(struct completed_requests** mpi_completed_queue, dumpi_req_id req_id);

/* conversion from seconds to nanaoseconds */
static tw_stime s_to_ns(tw_stime ns);

/* executes MPI wait operation */
static void codes_exec_mpi_wait(nw_state* s, nw_message* m, tw_lp* lp);

/* executes MPI waitsome operation */
static void codes_exec_mpi_waitsome(nw_state* s, nw_message* m, tw_lp* lp);

/* executes MPI isend and send operations */
static void codes_exec_mpi_send(nw_state* s, nw_message* m, tw_lp* lp);

/* execute MPI irecv operation */
static void codes_exec_mpi_recv(nw_state* s, nw_message* m, tw_lp* lp);

/* execute the computational delay */
static void codes_exec_comp_delay(nw_state* s, nw_message* m, tw_lp* lp);

/* execute collective operation */
static void codes_exec_mpi_col(nw_state* s, nw_message* m, tw_lp* lp);

/* issue next event */
static void codes_issue_next_event(tw_lp* lp);

/* notifies the wait operations for completion */
static int notify_waits(nw_state* s, tw_lp* lp, dumpi_req_id req_id);

/* initializes the queue and allocates memory */
static struct mpi_queue_ptrs* queue_init()
{
	struct mpi_queue_ptrs* mpi_queue = malloc(sizeof(struct mpi_queue_ptrs));

	mpi_queue->num_elems = 0;
	mpi_queue->queue_head = NULL;
	mpi_queue->queue_tail = NULL;
	
	return mpi_queue;
}

/* counts number of elements in the queue */
static int numQueue(struct mpi_queue_ptrs* mpi_queue)
{
	struct mpi_msgs_queue* tmp = malloc(sizeof(struct mpi_msgs_queue)); 
	assert(tmp);

	tmp = mpi_queue->queue_head;
	int count = 0;

	while(tmp)
	{
		++count;
		tmp = tmp->next;
	}
	return count;
	free(tmp);
}

/* prints elements in a send/recv queue */
static void printQueue(tw_lpid lpid, struct mpi_queue_ptrs* mpi_queue, char* msg)
{
	printf("\n ************ Printing the queue %s *************** ", msg);
	struct mpi_msgs_queue* tmp = malloc(sizeof(struct mpi_msgs_queue));
	assert(tmp);

	tmp = mpi_queue->queue_head;
	
	while(tmp)
	{
		if(tmp->mpi_op->op_type == CODES_NW_SEND || tmp->mpi_op->op_type == CODES_NW_ISEND)
			printf("\n lpid %ld send operation data type %d count %d tag %d source %d", 
				    lpid, tmp->mpi_op->u.send.data_type, tmp->mpi_op->u.send.count, 
				     tmp->mpi_op->u.send.tag, tmp->mpi_op->u.send.source_rank);
		else if(tmp->mpi_op->op_type == CODES_NW_IRECV || tmp->mpi_op->op_type == CODES_NW_RECV)
			printf("\n lpid %ld recv operation data type %d count %d tag %d source %d", 
				   lpid, tmp->mpi_op->u.recv.data_type, tmp->mpi_op->u.recv.count, 
				    tmp->mpi_op->u.recv.tag, tmp->mpi_op->u.recv.source_rank );
		else
			printf("\n Invalid data type in the queue %d ", tmp->mpi_op->op_type);
		tmp = tmp->next;
	}
	free(tmp);
}

/* re-insert element in the queue at the index --- maintained for reverse computation */
static void mpi_queue_update(struct mpi_queue_ptrs* mpi_queue, mpi_event_list* mpi_op, int pos)
{
	struct mpi_msgs_queue* elem = malloc(sizeof(struct mpi_msgs_queue));
	assert(elem);
	elem->mpi_op = mpi_op;
	
	/* inserting at the head */
	if(pos == 0)
	{
	   if(!mpi_queue->queue_tail)
		mpi_queue->queue_tail = elem;
	   elem->next = mpi_queue->queue_head;
	   mpi_queue->queue_head = elem;
	   mpi_queue->num_elems++;
	   return;
	}

	int index = 0;
	struct mpi_msgs_queue* tmp = mpi_queue->queue_head;
	while(index < pos - 1)
	{
		tmp = tmp->next;
		++index;
	}

	if(!tmp)
		printf("\n Invalid index! %d pos %d size %d ", index, pos, numQueue(mpi_queue));
	if(tmp == mpi_queue->queue_tail)
	    mpi_queue->queue_tail = elem;

	elem->next = tmp->next;
	tmp->next = elem;
	mpi_queue->num_elems++;

	return;
}

static void printCompletedQueue(nw_state* s, tw_lp* lp)
{
	   if(TRACE == lp->gid)
	   {
	   	printf("\n contents of completed operations queue ");
	   	struct completed_requests* current = s->completed_reqs;
	   	while(current)
	    	{
			printf(" %d ",current->req_id);
			current = current->next;
	   	}
	   }
}

/* notify the completed request in the pending waits queue. */
static int notify_waits(nw_state* s, tw_lp* lp, dumpi_req_id completed_req)
{
	int i;
	/* traverse the pending waits list and look what type of wait operations are 
	there. If its just a single wait and the request ID has just been completed, 
	then the network node LP can go on with fetching the next operation from the log.
	If its waitall then wait for all pending requests to complete and then proceed. */
	if(TRACE == lp->gid)
		printf("\n notifying wait operation completed req %d ", (int16_t)completed_req);
	
	struct pending_waits* wait_elem = s->pending_waits;

	if(!wait_elem)
		return 0;
	int op_type = wait_elem->mpi_op->op_type;

	if(op_type == CODES_NW_WAIT)
	{
		if(wait_elem->mpi_op->u.wait.req_id == completed_req)	
		  {
			s->wait_time += (tw_now(lp) - wait_elem->start_time);
			remove_req_id(&s->completed_reqs, completed_req);
				
			s->pending_waits = NULL;
			codes_issue_next_event(lp);	
			return 0;
		 }
	}
	else
	if(op_type == CODES_NW_WAITALL || op_type == CODES_NW_WAITSOME)
	{
	   for(i = 0; i < wait_elem->mpi_op->u.waits.count; i++)
	   {
	    if(wait_elem->mpi_op->u.waits.req_ids[i] == completed_req)
			wait_elem->num_completed++;	
	   }
	   if(TRACE == lp->gid)
		printf("\n completed wait count %d ", wait_elem->num_completed);
	   
	    int required_count = wait_elem->mpi_op->u.waits.count;
	    if((op_type == CODES_NW_WAITALL && wait_elem->num_completed == required_count)
		|| (op_type == CODES_NW_WAITSOME && wait_elem->num_completed > 0))
	     {
		if(TRACE == lp->gid)
			printf("\n waitall/some matched! ");
		s->wait_time += (tw_now(lp) - wait_elem->start_time);
		s->pending_waits = NULL; 
		for(i = 0; i < wait_elem->num_completed; i++)
			remove_req_id(&s->completed_reqs, wait_elem->mpi_op->u.waits.req_ids[i]);	
		 printCompletedQueue(s, lp);
		codes_issue_next_event(lp); //wait completed
	    }
	 }
	return 0;
}


/* execute MPI wait operation */
static void codes_exec_mpi_wait(nw_state* s, nw_message* m, tw_lp* lp)
{
/* check in the completed receives queue if the request ID has already been completed.*/

   dumpi_req_id req_id = m->op.u.wait.req_id;

   unsigned long search_start_time, search_end_time;
   struct completed_requests* current = s->completed_reqs; 
   search_start_time = tw_now(lp);
   while(current)
   {
	if(current->req_id == req_id)
	   {
		remove_req_id(&s->completed_reqs, req_id);
		s->wait_time += tw_now(lp) - search_start_time;
		codes_issue_next_event(lp);
		return;	
	   }
	current = current->next;		
   }
  search_end_time = tw_now(lp);
  s->search_overhead += (search_end_time - search_start_time);

 /* If not, add the wait operation in the pending 'waits' list. */
  struct pending_waits* wait_op = malloc(sizeof(struct pending_waits));
  wait_op->mpi_op = &(m->op);  
  wait_op->num_completed = 0; 
  wait_op->start_time = search_start_time;
  s->pending_waits = wait_op;
}

static void codes_exec_mpi_wait_all_some(nw_state* s, nw_message* m, tw_lp* lp)
{
  int count = m->op.u.waits.count;
  int i, num_completed = 0;
  dumpi_req_id req_id[count];
  struct completed_requests* current = s->completed_reqs;

  /* check number of completed irecvs in the completion queue */ 
  unsigned long start_time, search_end_time;
  start_time = tw_now(lp);

  if(lp->gid == TRACE)
    {
  	printf(" \n MPI waitall posted %d count", m->op.u.waits.count);
	for(i = 0; i < count; i++)
		printf(" %d ", (int)m->op.u.waits.req_ids[i]);
   	printCompletedQueue(s, lp);	 
   }
  while(current) 
   {
	  for(i = 0; i < count; i++)
	   {
	     req_id[i] = m->op.u.waits.req_ids[i];
	     if(req_id[i] == current->req_id)
 		 num_completed++;
   	  }
	 current = current->next;
   }

  search_end_time = tw_now(lp);

  if(TRACE== lp->gid)
	  printf("\n Num completed %d count %d ", num_completed, count);

  s->search_overhead += (search_end_time - start_time);
  if((m->op.op_type == CODES_NW_WAITALL && count == num_completed) ||
      (m->op.op_type == CODES_NW_WAITSOME && num_completed > 0))
  {
	for( i = 0; i < num_completed; i++)	
		remove_req_id(&s->completed_reqs, req_id[i]);
	
	s->wait_time += tw_now(lp) - start_time;
	codes_issue_next_event(lp);
	return;	
  }
  else
  {
 	/* If not, add the wait operation in the pending 'waits' list. */
	  struct pending_waits* wait_op = malloc(sizeof(struct pending_waits));
	  wait_op->mpi_op = &(m->op);  
	  wait_op->num_completed = num_completed;
	  wait_op->start_time = start_time;
	  s->pending_waits = wait_op;
  }
}

/* request ID is being reused so delete it from the list once the matching is done */
static void remove_req_id(struct completed_requests** mpi_completed_queue, dumpi_req_id req_id)
{
	struct completed_requests* current = *mpi_completed_queue;

	if(!current)
	  {
		printf("\n REQ ID DOES NOT EXIST");
		return;
	  }
	if(current->req_id == req_id)
	{
		*mpi_completed_queue = current->next;
		free(current);
		return;
	}
	
	struct completed_requests* elem;
	while(current->next)
	{
	   elem = current->next;
	   if(elem->req_id == req_id)	
	     {
		current->next = elem->next;
		free(elem);
		return;
	     }
	   current = current->next;	
	}
	return;
}

/* inserts mpi operation in the completed requests queue */
static int mpi_completed_queue_insert_op(struct completed_requests** mpi_completed_queue, dumpi_req_id req_id)
{
	struct completed_requests* reqs = malloc(sizeof(struct completed_requests));
	assert(reqs);

//	printf("\n inserting op %d ", req_id);
	reqs->req_id = req_id;
	reqs->next = NULL;

	if(!(*mpi_completed_queue))	
	{
			*mpi_completed_queue = reqs;
			return 0;
	}
	reqs->next = *mpi_completed_queue;
	*mpi_completed_queue = reqs;
	return 0;
}

/* remove mpi operation just inserted in the completed requests queue. */
static int mpi_completed_queue_remove_op(struct completed_requests** mpi_completed_queue)
{
	struct completed_requests* reqs = *mpi_completed_queue;

	if(!reqs)
	{
		printf("\n ERROR! NO ELEMENT IN THE QUEUE ");
		return -1;
	}

	*mpi_completed_queue = reqs->next;
	free(reqs);
	return 0;
}

/* insert MPI send or receive operation in the queues starting from tail. Unmatched sends go to arrival queue and unmatched receives go to pending receives queues. */
static void mpi_pending_queue_insert_op(struct mpi_queue_ptrs* mpi_queue, mpi_event_list* mpi_op)
{
	/* insert mpi operation */
	struct mpi_msgs_queue* elem = malloc(sizeof(struct mpi_msgs_queue));
	assert(elem);

	elem->mpi_op = mpi_op;
     	elem->next = NULL;

	if(!mpi_queue->queue_head)
	  mpi_queue->queue_head = elem;

	if(mpi_queue->queue_tail)
	    mpi_queue->queue_tail->next = elem;
	
        mpi_queue->queue_tail = elem;
	mpi_queue->num_elems++;

	return;
}

/* match the send/recv operations */
static int match_receive(nw_state* s, tw_lp* lp, tw_lpid lpid, mpi_event_list* op1, mpi_event_list* op2)
{
	assert(op1->op_type == CODES_NW_IRECV || op1->op_type == CODES_NW_RECV);
	assert(op2->op_type == CODES_NW_SEND || op2->op_type == CODES_NW_ISEND);

	if((op1->u.recv.num_bytes >= op2->u.send.num_bytes) &&
 	   	   ((op1->u.recv.tag == op2->u.send.tag) || op1->u.recv.tag == -1) &&
		   ((op1->u.recv.source_rank == op2->u.send.source_rank) || op1->u.recv.source_rank == -1))
		   {
			mpi_completed_queue_insert_op(&s->completed_reqs, op1->u.recv.req_id);
			s->recv_time += tw_now(lp) - op2->sim_start_time;
			return 1;
		   }
	return -1;
}

/* used for reverse computation. removes the tail of the queue */
static int mpi_queue_remove_tail(tw_lpid lpid, struct mpi_queue_ptrs* mpi_queue, mpi_event_list* mpi_op)
{
	assert(mpi_queue->queue_tail);
	if(mpi_queue->queue_tail == NULL)
	{
		printf("\n Error! tail not updated ");	
		return 0;
	}
	struct mpi_msgs_queue* tmp = mpi_queue->queue_head;

	if(mpi_queue->queue_head == mpi_queue->queue_tail)
	{
		mpi_queue->queue_head = NULL;
		mpi_queue->queue_tail = NULL;
		free(tmp);
		mpi_queue->num_elems--;
		 return 1;
	}

	struct mpi_msgs_queue* elem = mpi_queue->queue_tail;

	while(tmp->next != mpi_queue->queue_tail)
		tmp = tmp->next;

	mpi_queue->queue_tail = tmp;
	mpi_queue->queue_tail->next = NULL;
	mpi_queue->num_elems--;

	free(elem);
	return 1;
}

/* search for a matching mpi operation and remove it from the list. 
 * Record the index in the list from where the element got deleted. 
 * Index is used for inserting the element once again in the queue for reverse computation. */
static int mpi_queue_remove_matching_op(nw_state* s, tw_lp* lp, tw_lpid lpid, struct mpi_queue_ptrs* mpi_queue,  mpi_event_list* mpi_op, int* is_blocking, dumpi_req_id* req_id)
{
	if(mpi_queue->queue_head == NULL)
		return -1;

	/* remove mpi operation */
	struct mpi_msgs_queue* tmp = mpi_queue->queue_head;
	int indx = 0;

	/* if head of the list has the required mpi op to be deleted */
	int rcv_val = 0;
	if(mpi_op->op_type == CODES_NW_SEND || mpi_op->op_type == CODES_NW_ISEND)
	  {
		rcv_val = match_receive(s, lp, lpid, tmp->mpi_op, mpi_op);
		*req_id = tmp->mpi_op->u.recv.req_id;  
	 }
	else if(mpi_op->op_type == CODES_NW_RECV || mpi_op->op_type == CODES_NW_IRECV)
	  {
		rcv_val = match_receive(s, lp, lpid, mpi_op, tmp->mpi_op);
	  	*req_id = mpi_op->u.recv.req_id;
	  }

	if(rcv_val >= 0)
	{
		if(tmp->mpi_op->op_type == CODES_NW_RECV)
			*is_blocking = 1;
		
		if(mpi_queue->queue_head == mpi_queue->queue_tail)
		   {
			mpi_queue->queue_tail = NULL;
			mpi_queue->queue_head = NULL;
			 free(tmp);
		   }
		 else
		   {
			mpi_queue->queue_head = tmp->next;
			free(tmp);	
		   }
		
		
		mpi_queue->num_elems--;
		return indx;
	}

	/* record the index where matching operation has been found */
	struct mpi_msgs_queue* elem;

	while(tmp->next)	
	{
	   indx++;
	   elem = tmp->next;
	   
	    if(mpi_op->op_type == CODES_NW_SEND || mpi_op->op_type == CODES_NW_ISEND)
	     {
		rcv_val = match_receive(s, lp, lpid, elem->mpi_op, mpi_op);
	     	*req_id = elem->mpi_op->u.recv.req_id; 
	     }
	    else if(mpi_op->op_type == CODES_NW_RECV || mpi_op->op_type == CODES_NW_IRECV)
	     {
		rcv_val = match_receive(s, lp, lpid, mpi_op, elem->mpi_op);
	     }
   	     if(rcv_val >= 0)
		{
		    if(elem == mpi_queue->queue_tail)
			mpi_queue->queue_tail = tmp;
		    tmp->next = elem->next;

		    free(elem);
		    mpi_queue->num_elems--;
		
		   if(tmp->mpi_op->op_type == CODES_NW_RECV)
			*is_blocking = 1;
		    
		    return indx;
		}
	   tmp = tmp->next;
     }
	return -1;
}
/* Trigger getting next event at LP */
static void codes_issue_next_event(tw_lp* lp)
{
   tw_event *e;
   nw_message* msg;

   tw_stime ts;

   ts = g_tw_lookahead + 0.1 + tw_rand_exponential(lp->rng, noise);
   e = tw_event_new( lp->gid, ts, lp );
   msg = tw_event_data(e);

   msg->msg_type = MPI_OP_GET_NEXT;
   tw_event_send(e);
}

/* Simulate delays between MPI operations */
static void codes_exec_comp_delay(nw_state* s, nw_message* m, tw_lp* lp)
{
	struct mpi_event_list* mpi_op = &(m->op);
	tw_event* e;
	tw_stime ts;
	nw_message* msg;

	s->compute_time += mpi_op->u.delay.nsecs;
	ts = mpi_op->u.delay.nsecs + g_tw_lookahead + 0.1;
	ts += tw_rand_exponential(lp->rng, noise);
	
	e = tw_event_new( lp->gid, ts , lp );
	msg = tw_event_data(e);
	msg->msg_type = MPI_OP_GET_NEXT;

	tw_event_send(e); 
}

/* reverse computation operation for MPI irecv */
static void codes_exec_mpi_recv_rc(nw_state* s, nw_message* m, tw_lp* lp)
{
	num_bytes_recvd -= m->op.u.recv.num_bytes;
	if(m->found_match >= 0)
	  {
		//int count = numQueue(s->arrival_queue);
		mpi_queue_update(s->arrival_queue, &m->op, m->found_match);
		mpi_completed_queue_remove_op(&s->completed_reqs);	
		/*if(lp->gid == TRACE)
			printf("\n Reverse- after adding: arrival queue num_elems %d ", s->arrival_queue->num_elems);*/
	  }
	else if(m->found_match < 0)
	    {
		mpi_queue_remove_tail(lp->gid, s->pending_recvs_queue,  &m->op);
		if(m->op.op_type == CODES_NW_IRECV)
			tw_rand_reverse_unif(lp->rng);
		/*if(lp->gid == TRACE)
			printf("\n Reverse- after removing: pending receive queue num_elems %d ", s->pending_recvs_queue->num_elems);*/
	    }
			
	tw_rand_reverse_unif(lp->rng); 
}

/* Execute MPI Irecv operation (non-blocking receive) */ 
static void codes_exec_mpi_recv(nw_state* s, nw_message* m, tw_lp* lp)
{
/* Once an irecv is posted, list of completed sends is checked to find a matching isend.
   If no matching isend is found, the receive operation is queued in the pending queue of
   receive operations. */

	struct mpi_event_list* mpi_op = &(m->op);
	mpi_op->sim_start_time = tw_now(lp);
	unsigned long long start_searching, end_searching; 
	num_bytes_recvd += mpi_op->u.recv.num_bytes;
	//int count_before = numQueue(s->arrival_queue); 

	if(lp->gid == TRACE)
		printf("\n codes exec mpi recv req id %d", (int)mpi_op->u.recv.req_id);
	
	start_searching = tw_now(lp);  
	dumpi_req_id req_id;
	int found_matching_sends = mpi_queue_remove_matching_op(s, lp, lp->gid, s->arrival_queue, mpi_op, 0, &req_id);
	
	/* save the req id inserted in the completed queue for reverse computation. */
	//m->matched_recv = req_id;
	end_searching = tw_now(lp); 
	s->search_overhead += (end_searching - start_searching); 

	if(found_matching_sends < 0)
	  {
		m->found_match = -1;
		mpi_pending_queue_insert_op(s->pending_recvs_queue, mpi_op);
	
	       /* for mpi irecvs, this is a non-blocking receive so just post it and move on with the trace read. */
		if(lp->gid == TRACE)
		    printf("\n queued");
		if(mpi_op->op_type == CODES_NW_IRECV)
		   {
			codes_issue_next_event(lp);	
			return;
		   }
		else
			printf("\n CODES MPI RECV OPERATION!!! ");
	  }
	else
	  {
		/*if(lp->gid == TRACE)
			printf("\n Matched after removing: arrival queue num_elems %d ", s->arrival_queue->num_elems);*/
		/* update completed requests list */
		//int count_after = numQueue(s->arrival_queue);
		//assert(count_before == (count_after+1));
	   	//m->found_match = found_matching_sends;
		codes_issue_next_event(lp); 
	 }
}

/* executes MPI send and isend operations */
static void codes_exec_mpi_send(nw_state* s, nw_message* m, tw_lp* lp)
{
	struct mpi_event_list* mpi_op = &(m->op);
	/* model-net event */
	tw_lpid dest_rank;

	codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id, 
	    lp_type_name, &mapping_type_id, annotation, &mapping_rep_id, &mapping_offset);

	if(net_id == DRAGONFLY) /* special handling for the dragonfly case */
	{
		int num_routers, lps_per_rep, factor;
		num_routers = codes_mapping_get_lp_count("MODELNET_GRP", 1,
                  "dragonfly_router", NULL, 1);
	 	lps_per_rep = (2 * num_nw_lps) + num_routers;	
		factor = mpi_op->u.send.dest_rank / num_nw_lps;
		dest_rank = (lps_per_rep * factor) + (mpi_op->u.send.dest_rank % num_nw_lps);	
		//printf("\n local dest %d final dest %d ", mpi_op->u.send.dest_rank, dest_rank);
	}
	else
	{
		/* other cases like torus/simplenet/loggp etc. */
		codes_mapping_get_lp_id(lp_group_name, lp_type_name, NULL, 1,  
	    	  mpi_op->u.send.dest_rank, mapping_offset, &dest_rank);
	}

	num_bytes_sent += mpi_op->u.send.num_bytes;

	nw_message* local_m = malloc(sizeof(nw_message));
	nw_message* remote_m = malloc(sizeof(nw_message));
	assert(local_m && remote_m);
	mpi_op->sim_start_time = tw_now(lp);

	local_m->op = *mpi_op;
	local_m->msg_type = MPI_SEND_POSTED;
	
	remote_m->op = *mpi_op;
	remote_m->msg_type = MPI_SEND_ARRIVED;

	model_net_event(net_id, "test", dest_rank, mpi_op->u.send.num_bytes, 0.0, 
	    sizeof(nw_message), (const void*)remote_m, sizeof(nw_message), (const void*)local_m, lp);

	if(TRACE == lp->gid)	
		printf("\n send req id %d dest %d ", (int)mpi_op->u.send.req_id, (int)dest_rank);
	/* isend executed, now get next MPI operation from the queue */ 
	if(mpi_op->op_type == CODES_NW_ISEND)
	   codes_issue_next_event(lp);
}

/* MPI collective operations */
static void codes_exec_mpi_col(nw_state* s, nw_message* m, tw_lp* lp)
{
	codes_issue_next_event(lp);
}

/* convert seconds to ns */
static tw_stime s_to_ns(tw_stime ns)
{
    return(ns * (1000.0 * 1000.0 * 1000.0));
}


static void update_send_completion_queue_rc(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
	//mpi_queue_remove_matching_op(&s->completed_isend_queue_head, &s->completed_isend_queue_tail, &m->op, SEND);

	if(m->op.op_type == CODES_NW_SEND)
	   {
		tw_rand_reverse_unif(lp->rng);	
	   }

	if(m->op.op_type == CODES_NW_ISEND)
	  {
		mpi_completed_queue_remove_op(&s->completed_reqs);
	  }
}

/* completed isends are added in the list */
static void update_send_completion_queue(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
	//if(m->op.op_type == CODES_NW_SEND)
	//	printf("\n LP %ld Local isend operation completed ", lp->gid);

	if(m->op.op_type == CODES_NW_ISEND)
	   {	
		mpi_completed_queue_insert_op(&s->completed_reqs, m->op.u.send.req_id);
	   	notify_waits(s, lp, m->op.u.send.req_id);
	   }  
	
	/* blocking send operation */
	if(m->op.op_type == CODES_NW_SEND)
		codes_issue_next_event(lp);	

	 return;
}

/* reverse handler for updating arrival queue function */
static void update_arrival_queue_rc(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
	if(m->found_match >= 0)
	{
		//int count = numQueue(s->pending_recvs_queue);
		mpi_queue_update(s->pending_recvs_queue, &m->op, m->found_match);
		
		/*if(lp->gid == TRACE)
			printf("\n Reverse: after adding pending recvs queue %d ", s->pending_recvs_queue->num_elems);*/
	}
	else if(m->found_match < 0)
	{
		mpi_queue_remove_tail(lp->gid, s->arrival_queue, &(m->op));	
		/*if(lp->gid == TRACE)
			printf("\n Reverse: after removing arrivals queue %d ", s->arrival_queue->num_elems);*/
	}
}

/* once an isend operation arrives, the pending receives queue is checked to find out if there is a irecv that has already been posted. If no isend has been posted, */
static void update_arrival_queue(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
	//int count_before = numQueue(s->pending_recvs_queue);
	int is_blocking = 0; /* checks if the recv operation was blocking or not */
	unsigned long long start_searching, end_searching;

	s->send_time += tw_now(lp) - m->op.sim_start_time;
	dumpi_req_id req_id = -1;

	start_searching = tw_now(lp);
	int found_matching_recv = mpi_queue_remove_matching_op(s, lp, lp->gid, s->pending_recvs_queue, &(m->op), &is_blocking, &req_id);
	end_searching = tw_now(lp);

	s->search_overhead += (end_searching - start_searching);
		
	if(found_matching_recv < 0)
	 {
		m->found_match = -1;
		mpi_pending_queue_insert_op(s->arrival_queue, &(m->op));
		/*if(lp->gid == TRACE)
			printf("\n After adding arrivals queue %d ", s->arrival_queue->num_elems);*/
	}
	else
	  {
		/*if(lp->gid == TRACE)
			printf("\n  matched %d ", s->pending_recvs_queue->num_elems);*/
		//int count_after = numQueue(s->pending_recvs_queue);
		//assert(count_before == (count_after + 1));
		m->found_match = found_matching_recv;
	
		/* unblock the blocking receive */
		if(is_blocking)
			codes_issue_next_event(lp);	
		else
			notify_waits(s, lp, req_id);
	  }
	return;
}

/* initializes the network node LP, loads the trace file in the structs, calls the first MPI operation to be executed */
void nw_test_init(nw_state* s, tw_lp* lp)
{
   /* initialize the LP's and load the data */
   char * params;
   scala_trace_params params_sc;
   dumpi_trace_params params_d;
  
   codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id, lp_type_name, 
	&mapping_type_id, annotation, &mapping_rep_id, &mapping_offset);
  
   s->nw_id = (mapping_rep_id * num_nw_lps) + mapping_offset;
   s->wrkld_end = 0;

   s->num_sends = 0;
   s->num_recvs = 0;
   s->num_cols = 0;
   s->num_delays = 0;
   s->num_wait = 0;
   s->num_waitall = 0;
   s->num_waitsome = 0;
   s->elapsed_time = 0;
   s->compute_time = 0;
   s->search_overhead = 0;

   //`s->completed_sends = NULL;
   s->completed_reqs = NULL;

   s->pending_waits = NULL;
   if(!num_net_traces) 
	num_net_traces = num_net_lps;

   if (strcmp(workload_type, "scalatrace") == 0){
       if (params_sc.offset_file_name[0] == '\0'){
           tw_error(TW_LOC, "required argument for scalatrace offset_file");
           return;
       }
       strcpy(params_sc.offset_file_name, offset_file);
       strcpy(params_sc.nw_wrkld_file_name, workload_file);
       params = (char*)&params_sc;
   }
   else if (strcmp(workload_type, "dumpi") == 0){
       strcpy(params_d.file_name, workload_file);
       params_d.num_net_traces = num_net_traces;

       params = (char*)&params_d;
   }
  /* In this case, the LP will not generate any workload related events*/
   if(s->nw_id >= params_d.num_net_traces)
     {
	//printf("\n network LP not generating events %d ", (int)s->nw_id);
	return;
     }
   wrkld_id = codes_nw_workload_load("dumpi-trace-workload", params, (int)s->nw_id);

   s->arrival_queue = queue_init(); 
   s->pending_recvs_queue = queue_init();

   /* clock starts ticking */
   s->elapsed_time = tw_now(lp);
   codes_issue_next_event(lp);

   return;
}

void nw_test_event_handler(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
	switch(m->msg_type)
	{
		case MPI_SEND_POSTED:
			update_send_completion_queue(s, bf, m, lp);
		break;

		case MPI_SEND_ARRIVED:
			update_arrival_queue(s, bf, m, lp);
		break;

		case MPI_OP_GET_NEXT:
			get_next_mpi_operation(s, bf, m, lp);	
		break; 
	}
}

static void get_next_mpi_operation_rc(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
	codes_nw_workload_get_next_rc(wrkld_id, (int)s->nw_id, &m->op);
	if(m->op.op_type == CODES_NW_END)
		return;

	switch(m->op.op_type)
	{
		case CODES_NW_SEND:
		case CODES_NW_ISEND:
		{
			model_net_event_rc(net_id, lp, m->op.u.send.num_bytes);
			if(m->op.op_type == CODES_NW_ISEND)
				tw_rand_reverse_unif(lp->rng);	
			s->num_sends--;
			num_bytes_sent -= m->op.u.send.num_bytes;
		}
		break;

		case CODES_NW_RECV:
		case CODES_NW_IRECV:
		{
			codes_exec_mpi_recv_rc(s, m, lp);
			s->num_recvs--;
		}
		break;
		case CODES_NW_DELAY:
		{
			tw_rand_reverse_unif(lp->rng);
			s->num_delays--;
			s->compute_time -= m->op.u.delay.nsecs;
		}
		break;
		case CODES_NW_BCAST:
		case CODES_NW_ALLGATHER:
		case CODES_NW_ALLGATHERV:
		case CODES_NW_ALLTOALL:
		case CODES_NW_ALLTOALLV:
		case CODES_NW_REDUCE:
		case CODES_NW_ALLREDUCE:
		case CODES_NW_COL:
		{
			s->num_cols--;
			tw_rand_reverse_unif(lp->rng);
		}
		break;
	
		case CODES_NW_WAIT:
		{
		}
		break;
		case CODES_NW_WAITALL:
		case CODES_NW_WAITSOME:
		case CODES_NW_WAITANY:
		{
			printf("\n MPI waitall posted! ");
		}
		break;
		default:
			printf("\n Invalid op type %d ", m->op.op_type);
	}
}

static void get_next_mpi_operation(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
		mpi_event_list mpi_op;
    		codes_nw_workload_get_next(wrkld_id, (int)s->nw_id, &mpi_op);
		memcpy(&m->op, &mpi_op, sizeof(struct mpi_event_list));

    		if(mpi_op.op_type == CODES_NW_END)
    	 	{
			return;
     		}
		switch(mpi_op.op_type)
		{
			case CODES_NW_SEND:
			case CODES_NW_ISEND:
			 {
				s->num_sends++;
				codes_exec_mpi_send(s, m, lp);
			 }
			break;
	
			case CODES_NW_RECV:
			case CODES_NW_IRECV:
			  {
				s->num_recvs++;
				codes_exec_mpi_recv(s, m, lp);
			  }
			break;

			case CODES_NW_DELAY:
			  {
				s->num_delays++;
				codes_exec_comp_delay(s, m, lp);
			  }
			break;

			case CODES_NW_BCAST:
			case CODES_NW_ALLGATHER:
			case CODES_NW_ALLGATHERV:
			case CODES_NW_ALLTOALL:
			case CODES_NW_ALLTOALLV:
			case CODES_NW_REDUCE:
			case CODES_NW_ALLREDUCE:
			case CODES_NW_COL:
			  {
				s->num_cols++;
				codes_exec_mpi_col(s, m, lp);
			  }
			break;
			case CODES_NW_WAIT:
			{
				s->num_wait++;
				codes_exec_mpi_wait(s, m, lp);	
			}
			break;
			case CODES_NW_WAITALL:
			{
				s->num_waitall++;
				codes_exec_mpi_wait_all_some(s, m, lp);
			}
			break;
			case CODES_NW_WAITSOME:
			{
				s->num_waitsome++;
				codes_exec_mpi_wait_all_some(s, m, lp);
			}
			break;

			case CODES_NW_WAITANY:
			{
			   /* do nothing for now */
			  codes_exec_mpi_col(s, m, lp);
			}
			break;
			default:
				printf("\n Invalid op type %d ", m->op.op_type);
		}
}

void nw_test_finalize(nw_state* s, tw_lp* lp)
{
	if(s->nw_id < num_net_traces)
	{
		int count_irecv = numQueue(s->pending_recvs_queue);
        	int count_isend = numQueue(s->arrival_queue);
		printf("\n LP %ld unmatched irecvs %d unmatched sends %d Total sends %ld receives %ld collectives %ld delays %ld wait alls %ld waits %ld search overhead %lf send time %lf wait %lf", 
			lp->gid, s->pending_recvs_queue->num_elems, s->arrival_queue->num_elems, s->num_sends, s->num_recvs, s->num_cols, s->num_delays, s->num_waitall, s->num_wait, s->search_overhead, s->send_time, s->wait_time);
		if(lp->gid == TRACE)
		{
		   printQueue(lp->gid, s->pending_recvs_queue, "irecv ");
		  printQueue(lp->gid, s->arrival_queue, "isend");
	        }

		double total_time = tw_now(lp) - s->elapsed_time;
		assert(total_time >= s->compute_time);

		if(total_time - s->compute_time > max_comm_time)
			max_comm_time = total_time - s->compute_time;
		
		if(total_time > max_time )
			max_time = total_time;

		if(s->wait_time > max_wait_time)
			max_wait_time = s->wait_time;

		if(s->send_time > max_send_time)
			max_send_time = s->send_time;

		if(s->recv_time > max_recv_time)
			max_recv_time = s->recv_time;

		avg_time += total_time;
		avg_comm_time += (total_time - s->compute_time);	
		avg_wait_time += s->wait_time;
		avg_send_time += s->send_time;
		 avg_recv_time += s->recv_time;

		//printf("\n LP %ld Time spent in communication %llu ", lp->gid, total_time - s->compute_time);
		free(s->arrival_queue);
		free(s->pending_recvs_queue);
	}
}

void nw_test_event_handler_rc(nw_state* s, tw_bf * bf, nw_message * m, tw_lp * lp)
{
	switch(m->msg_type)
	{
		case MPI_SEND_POSTED:
			update_send_completion_queue_rc(s, bf, m, lp);
		break;

		case MPI_SEND_ARRIVED:
			update_arrival_queue_rc(s, bf, m, lp);
		break;

		case MPI_OP_GET_NEXT:
			get_next_mpi_operation_rc(s, bf, m, lp);
		break;
	}
}

const tw_optdef app_opt [] =
{
	TWOPT_GROUP("Network workload test"),
    	TWOPT_CHAR("workload_type", workload_type, "workload type (either \"scalatrace\" or \"dumpi\")"),
	TWOPT_CHAR("workload_file", workload_file, "workload file name"),
	TWOPT_UINT("num_net_traces", num_net_traces, "number of network traces"),
	TWOPT_CHAR("offset_file", offset_file, "offset file name"),
	TWOPT_END()
};

tw_lptype nw_lp = {
    (init_f) nw_test_init,
    (pre_run_f) NULL,
    (event_f) nw_test_event_handler,
    (revent_f) nw_test_event_handler_rc,
    (final_f) nw_test_finalize,
    (map_f) codes_mapping,
    sizeof(nw_state)
};

const tw_lptype* nw_get_lp_type()
{
            return(&nw_lp);
}

static void nw_add_lp_type()
{
  lp_type_register("nw-lp", nw_get_lp_type());
}

int main( int argc, char** argv )
{
  int rank, nprocs;
  int num_nets;
  int* net_ids;

  g_tw_ts_end = s_to_ns(60*60*24*365); /* one year, in nsecs */

  workload_type[0]='\0';
  tw_opt_add(app_opt);
  tw_init(&argc, &argv);

  if(strlen(workload_file) == 0)
    {
	if(tw_ismaster())
		printf("\n Usage: mpirun -np n ./codes-nw-test --sync=1/2/3 --workload_type=type --workload_file=workload-file-name");
	tw_end();
	return -1;
    }

    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);

   configuration_load(argv[2], MPI_COMM_WORLD, &config);

   nw_add_lp_type();
   model_net_register();

   net_ids = model_net_configure(&num_nets);
   assert(num_nets == 1);
   net_id = *net_ids;
   free(net_ids);


   codes_mapping_setup();

   num_net_lps = codes_mapping_get_lp_count("MODELNET_GRP", 0, "nw-lp", NULL, 0);
   
   num_nw_lps = codes_mapping_get_lp_count("MODELNET_GRP", 1, 
			"nw-lp", NULL, 1);	
   tw_run();

    long long total_bytes_sent, total_bytes_recvd;
    double max_run_time, avg_run_time;
   double max_comm_run_time, avg_comm_run_time;
    double total_avg_send_time, total_max_send_time;
     double total_avg_wait_time, total_max_wait_time;
     double total_avg_recv_time, total_max_recv_time;
	
    MPI_Reduce(&num_bytes_sent, &total_bytes_sent, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(&num_bytes_recvd, &total_bytes_recvd, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
   MPI_Reduce(&max_comm_time, &max_comm_run_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
   MPI_Reduce(&max_time, &max_run_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
   MPI_Reduce(&avg_time, &avg_run_time, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);

   MPI_Reduce(&avg_recv_time, &total_avg_recv_time, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
   MPI_Reduce(&avg_comm_time, &avg_comm_run_time, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
   MPI_Reduce(&max_wait_time, &total_max_wait_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);  
   MPI_Reduce(&max_send_time, &total_max_send_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);  
   MPI_Reduce(&max_recv_time, &total_max_recv_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);  
   MPI_Reduce(&avg_wait_time, &total_avg_wait_time, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
   MPI_Reduce(&avg_send_time, &total_avg_send_time, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);

   if(!g_tw_mynode)
	printf("\n Total bytes sent %lld recvd %lld \n max runtime %lf ns avg runtime %lf \n max comm time %lf avg comm time %lf \n max send time %lf avg send time %lf \n max recv time %lf avg recv time %lf \n max wait time %lf avg wait time %lf \n", total_bytes_sent, total_bytes_recvd, 
			max_run_time, avg_run_time/num_net_traces,
			max_comm_run_time, avg_comm_run_time/num_net_traces,
			total_max_send_time, total_avg_send_time/num_net_traces,
			total_max_recv_time, total_avg_recv_time/num_net_traces,
			total_max_wait_time, total_avg_wait_time/num_net_traces);
   tw_end();
  
  return 0;
}
