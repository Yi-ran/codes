/*
 * Copyright (C) 2014 University of Chicago
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mpi.h>
#include <ross.h>
#include <assert.h>
#include "dumpi/libundumpi/bindings.h"
#include "dumpi/libundumpi/libundumpi.h"
#include "codes/codes-workload.h"
#include "codes/quickhash.h"
#include "codes/codes-jobmap.h"
#include "codes/jenkins-hash.h"
#include "codes/model-net.h"

#if ENABLE_CORTEX
#include <cortex/cortex.h>
#include <cortex/datatype.h>
#include <cortex/cortex-mpich.h>
#ifdef ENABLE_CORTEX_PYTHON
#include <cortex/cortex-python.h>
#endif
#define PROFILE_TYPE cortex_dumpi_profile*
//#define UNDUMPI_OPEN cortex_undumpi_open
#define DUMPI_START_STREAM_READ cortex_dumpi_start_stream_read
#define UNDUMPI_CLOSE cortex_undumpi_close
#else
#define PROFILE_TYPE dumpi_profile*
//#define UNDUMPI_OPEN undumpi_open
#define DUMPI_START_STREAM_READ dumpi_start_stream_read
#define UNDUMPI_CLOSE undumpi_close
#endif


#define MAX_LENGTH_FILE 512
#define MAX_OPERATIONS 32768
#define DUMPI_IGNORE_DELAY 100

#define INITIAL_OP_QUEUE_SIZE    4
#define INITIAL_OP_RC_STACK_SIZE 4

/* This variable is defined in src/network-workloads/model-net-mpi-replay.c */
extern struct codes_jobmap_ctx *jobmap_ctx; 

static struct qhash_table *rank_tbl = NULL;
static int rank_tbl_pop = 0;

static unsigned int max_threshold = INT_MAX;

typedef struct dumpi_op_data_array
{
    /* operations loaded, not yet consumed by simulator */
    struct codes_workload_op* next_ops_queue;      // queue implemented with a circular buffer
    size_t                    next_ops_queue_size; // size (allocated) of the array
    uint64_t                  next_ops_queue_count; // number of elements currently in the queue
    uint64_t                  next_ops_queue_first; // pointer to first element (next to consume)
    uint64_t                  next_ops_queue_last;  // pointer to the first free space in the array
    /* operations consumed, which may be reversed */
    struct codes_workload_op* prev_ops_stack;      // stack implemented with an array
    size_t                    prev_ops_stack_size; // size (allocated) of the array
    uint64_t                  prev_ops_stack_top;  // index of the top of the stack (first free place)
    /* sequence id */
    uint64_t                  sequence_id;       // to attribute a sequence number of operations
    int                       finalize_reached;  // reached MPI_Finalize call
    int                       active;            // dumpi stream is active
} dumpi_op_data_array;

/* context of the MPI workload */
typedef struct rank_mpi_context
{
    PROFILE_TYPE profile;
    libundumpi_cbpair callarr[DUMPI_END_OF_STREAM];
#ifdef ENABLE_CORTEX
    libundumpi_cbpair transarr[DUMPI_END_OF_STREAM];
#endif
    int my_app_id;
    // whether we've seen an init op (needed for timing correctness)
    int is_init;
    unsigned int num_reqs;
    unsigned int num_ops;
    int64_t my_rank;
    double last_op_time;
    double init_time;
    dumpi_op_data_array dumpi_mpi_array;
    struct qhash_head hash_link;
    
    struct rc_stack * completed_ctx;
} rank_mpi_context;

typedef struct rank_mpi_compare
{
    int app;
    int rank;
} rank_mpi_compare;

/* timing utilities */

#ifdef __GNUC__
__attribute__((unused))
#endif
static dumpi_clock timediff(
        dumpi_clock end,
        dumpi_clock start)
{
    dumpi_clock temp;
    if ((end.nsec-start.nsec)<0) {
        temp.sec = end.sec-start.sec-1;
        temp.nsec = 1000000000+end.nsec-start.nsec;
    } else {
        temp.sec = end.sec-start.sec;
        temp.nsec = end.nsec-start.nsec;
    }
    return temp;
}

/*static inline double time_to_ms_lf(dumpi_clock t){
        return (double) t.sec * 1e3 + (double) t.nsec / 1e6;
}
static inline double time_to_us_lf(dumpi_clock t){
        return (double) t.sec * 1e6 + (double) t.nsec / 1e3;
}*/
static inline double time_to_ns_lf(dumpi_clock t){
        return (double) t.sec * 1e9 + (double) t.nsec;
}
/*static int32_t get_unique_req_id(int32_t request_id)
{
    uint32_t pc = 0, pb = 0;
    bj_hashlittle2(&request_id, sizeof(int32_t), &pc, &pb);
    return pc;
}*/
/*static inline double time_to_s_lf(dumpi_clock t){
        return (double) t.sec + (double) t.nsec / 1e9;
}*/

/* load the trace */
static int dumpi_trace_nw_workload_load(const char* params, int app_id, int rank);

/* dumpi implementation of get next operation in the workload */
static void dumpi_trace_nw_workload_get_next(int app_id, int rank, struct codes_workload_op *op);

/* get number of bytes from the workload data type and count */
static uint64_t get_num_bytes(rank_mpi_context* my_ctx, dumpi_datatype dt);

/* computes the delay between MPI operations */
static void update_compute_time(const dumpi_time* time, rank_mpi_context* my_ctx);

static void dumpi_init_op_data(struct rank_mpi_context* ctx);
/* removes next operations from the dynamic array */
static void dumpi_remove_next_op(struct rank_mpi_context* ctx, struct codes_workload_op *mpi_op,
                                              double last_op_time);
/* resets the counters for the dynamic array once the workload is completely loaded*/
static void dumpi_finalize_mpi_op_data(struct rank_mpi_context* ctx);
/* insert next operation */
static void dumpi_insert_next_op(struct rank_mpi_context* ctx, struct codes_workload_op *mpi_op);

/* initialize the array data structure */
static void dumpi_init_op_data(struct rank_mpi_context* ctx)
{
    dumpi_op_data_array* t  = &(ctx->dumpi_mpi_array);
    t->next_ops_queue       = calloc(INITIAL_OP_QUEUE_SIZE, sizeof(struct codes_workload_op));
    t->next_ops_queue_size  = INITIAL_OP_QUEUE_SIZE;
    t->next_ops_queue_count = 0;
    t->next_ops_queue_first = 0;
    t->next_ops_queue_last  = 0;
    t->prev_ops_stack       = calloc(INITIAL_OP_RC_STACK_SIZE, sizeof(struct codes_workload_op));
    t->prev_ops_stack_size  = INITIAL_OP_RC_STACK_SIZE;
    t->prev_ops_stack_top   = 0;
    t->sequence_id          = 0;
    t->finalize_reached     = 0;
    t->active               = 1;
}

/* inserts next operation in the queue */
static void dumpi_insert_next_op(struct rank_mpi_context* ctx, struct codes_workload_op *mpi_op)
{
    dumpi_op_data_array* t = &(ctx->dumpi_mpi_array);
    // check if we have some space in the queue
    if(t->next_ops_queue_size == t->next_ops_queue_count) {
        t->next_ops_queue = realloc(t->next_ops_queue, t->next_ops_queue_size*2*sizeof(struct codes_workload_op));
        assert(t->next_ops_queue);
        if(t->next_ops_queue_first >= t->next_ops_queue_last) {
            size_t s = t->next_ops_queue_size;
            size_t x = s - t->next_ops_queue_first;
            memcpy(t->next_ops_queue + x + s, t->next_ops_queue + x, s*sizeof(struct codes_workload_op));
            t->next_ops_queue_first += s;
        }
        t->next_ops_queue_size *= 2;
    }
    t->next_ops_queue[t->next_ops_queue_last] = *mpi_op;
    t->next_ops_queue_last += 1;
    t->next_ops_queue_last %= t->next_ops_queue_size;
    t->next_ops_queue_count += 1;
}
/* resets the counters after file is fully loaded */
static void dumpi_finalize_mpi_op_data(struct rank_mpi_context* ctx)
{
    dumpi_op_data_array* t = &(ctx->dumpi_mpi_array);
    free(t->next_ops_queue);
    free(t->prev_ops_stack);
}
/* rolls back to previous index */
static void dumpi_roll_back_prev_op(struct rank_mpi_context* ctx)
{
    dumpi_op_data_array* t = &(ctx->dumpi_mpi_array);
    // if there is something on the stack of previous operations,
    // put it back in the queue (from the other end of the queue)
    assert(t->prev_ops_stack_top);
    // check if the queue size needs to be increased
    if(t->next_ops_queue_size == t->next_ops_queue_count) {
        t->next_ops_queue = realloc(t->next_ops_queue, t->next_ops_queue_size*2*sizeof(struct codes_workload_op));
        assert(t->next_ops_queue);
        if(t->next_ops_queue_first >= t->next_ops_queue_last) {
            size_t s = t->next_ops_queue_size;
            size_t x = s - t->next_ops_queue_first;
            memcpy(t->next_ops_queue + x + s, t->next_ops_queue + x, s*sizeof(struct codes_workload_op));
            t->next_ops_queue_first += s;
        }
        t->next_ops_queue_size *= 2;
    }
    // move the cursor of the first element in the queue
    if(t->next_ops_queue_first != 0)
        t->next_ops_queue_first -= 1;
    else
        t->next_ops_queue_first = t->next_ops_queue_size - 1;
    // add the element in the queue
    t->next_ops_queue[t->next_ops_queue_last]
        = t->prev_ops_stack[t->prev_ops_stack_top-1];
    t->next_ops_queue_count += 1;
    // remove the element from the stack
    t->prev_ops_stack_top -= 1;
    t->sequence_id -= 1;
}
/* get the next operation from the array */
static void dumpi_remove_next_op(struct rank_mpi_context* ctx, struct codes_workload_op *mpi_op,
                                      double last_op_time)
{
    dumpi_op_data_array* t = &(ctx->dumpi_mpi_array);
retry:
    if(t->next_ops_queue_count == 0) {
        mpi_op->op_type = CODES_WK_END;

        // no more operation in the queue, try to load from the file
        if(t->active && !t->finalize_reached)
        {
#ifdef ENABLE_CORTEX
            t->active = cortex_undumpi_read_single_call(ctx->profile, 
                   ctx->callarr, ctx->transarr, (void*)ctx, &(t->finalize_reached));
#else
            t->active = undumpi_read_single_call(ctx->profile, 
                   ctx->callarr, (void*)ctx, &(t->finalize_reached));
#endif
            goto retry;
        }
    } else {
        *mpi_op = t->next_ops_queue[t->next_ops_queue_first];
        t->next_ops_queue_first += 1;
        if(t->next_ops_queue_first == t->next_ops_queue_size)
            t->next_ops_queue_first = 0;
        t->next_ops_queue_count -= 1;
    }
    mpi_op->sequence_id = t->sequence_id;
    t->sequence_id += 1;
    // put the event in the stack of previous events
    if(t->prev_ops_stack_top == t->prev_ops_stack_size) {
        t->prev_ops_stack = realloc(t->prev_ops_stack, 2*(t->prev_ops_stack_size)*sizeof(struct codes_workload_op));
        t->prev_ops_stack_size *= 2;
    }
    t->prev_ops_stack[t->prev_ops_stack_top] = *mpi_op;
    t->prev_ops_stack_top += 1;
}

/* check for initialization and normalize reported time */
static inline void check_set_init_time(const dumpi_time *t, rank_mpi_context * my_ctx)
{
    if (!my_ctx->is_init) {
        my_ctx->is_init = 1;
        my_ctx->init_time = time_to_ns_lf(t->start);
        my_ctx->last_op_time = time_to_ns_lf(t->stop) - my_ctx->init_time;
    }
}

/* introduce delay between operations: delay is the compute time NOT spent in MPI operations*/
void update_compute_time(const dumpi_time* time, rank_mpi_context* my_ctx)
{
    double start = time_to_ns_lf(time->start) - my_ctx->init_time;
    double stop = time_to_ns_lf(time->stop) - my_ctx->init_time;
    if((start - my_ctx->last_op_time) > DUMPI_IGNORE_DELAY)
    {
        struct codes_workload_op wrkld_per_rank;

        wrkld_per_rank.op_type = CODES_WK_DELAY;
        wrkld_per_rank.start_time = my_ctx->last_op_time;
        wrkld_per_rank.end_time = start;
        wrkld_per_rank.u.delay.seconds = (start - my_ctx->last_op_time) / 1e9;
        dumpi_insert_next_op(my_ctx, &wrkld_per_rank); 
    }
    my_ctx->last_op_time = stop;
}

static int handleDUMPIInit(
        const dumpi_init *prm,
        uint16_t thread,
        const dumpi_time *cpu,
        const dumpi_time *wall,
        const dumpi_perfinfo *perf,
        void *uarg)
{
    (void)prm;
    (void)thread;
    (void)cpu;
    (void)wall;
    (void)perf;

    rank_mpi_context *myctx = (rank_mpi_context*)uarg;
    check_set_init_time(wall, myctx);
    return 0;
}

int handleDUMPIError(const void* prm, uint16_t thread, const dumpi_time *cpu, const dumpi_time *wall, const dumpi_perfinfo *perf, void *uarg)
{
    (void)prm;
    (void)thread;
    (void)cpu;
    (void)wall;
    (void)perf;
    (void)uarg;

    tw_error(TW_LOC, "\n MPI operation not supported by the MPI-Sim Layer ");
}

int handleDUMPIIgnore(const void* prm, uint16_t thread, const dumpi_time *cpu, const dumpi_time *wall, const dumpi_perfinfo *perf, void *uarg)
{
    (void)prm;
    (void)thread;
    (void)cpu;
    (void)wall;
    (void)perf;
	
    rank_mpi_context* myctx = (rank_mpi_context*)uarg;

    check_set_init_time(wall, myctx);
	update_compute_time(wall, myctx);

	return 0;
}

static void update_times_and_insert(
        struct codes_workload_op *op,
        const dumpi_time *t,
        rank_mpi_context *ctx)
{
    check_set_init_time(t, ctx);
    op->start_time = time_to_ns_lf(t->start) - ctx->init_time;
    op->end_time = time_to_ns_lf(t->stop) - ctx->init_time;
    update_compute_time(t, ctx);
    dumpi_insert_next_op(ctx, op);
}


int handleDUMPIWait(const dumpi_wait *prm, uint16_t thread,
                    const dumpi_time *cpu, const dumpi_time *wall,
                    const dumpi_perfinfo *perf, void *userarg)
{
        (void)prm;
        (void)thread;
        (void)cpu;
        (void)perf;
        
        rank_mpi_context* myctx = (rank_mpi_context*)userarg;
        struct codes_workload_op wrkld_per_rank;

        wrkld_per_rank.op_type = CODES_WK_WAIT;
        wrkld_per_rank.u.wait.req_id = prm->request;

        update_times_and_insert(&wrkld_per_rank, wall, myctx);

        return 0;
}

int handleDUMPIWaitsome(const dumpi_waitsome *prm, uint16_t thread,
                    const dumpi_time *cpu, const dumpi_time *wall,
                    const dumpi_perfinfo *perf, void *userarg)
{
        (void)thread;
        (void)cpu;
        (void)wall;
        (void)perf;
        
        int i;
        rank_mpi_context* myctx = (rank_mpi_context*)userarg;
        struct codes_workload_op wrkld_per_rank;

        wrkld_per_rank.op_type = CODES_WK_WAITSOME;
        wrkld_per_rank.u.waits.count = prm->count;
        wrkld_per_rank.u.waits.req_ids = (unsigned int*)malloc(prm->count * sizeof(unsigned int));

        for( i = 0; i < prm->count; i++ )
                wrkld_per_rank.u.waits.req_ids[i] = prm->requests[i];

        update_times_and_insert(&wrkld_per_rank, wall, myctx);
        return 0;
}

int handleDUMPIWaitany(const dumpi_waitany *prm, uint16_t thread,
                    const dumpi_time *cpu, const dumpi_time *wall,
                    const dumpi_perfinfo *perf, void *userarg)
{
        (void)prm;
        (void)thread;
        (void)cpu;
        (void)wall;
        (void)perf;
        
        int i;
        rank_mpi_context* myctx = (rank_mpi_context*)userarg;
        struct codes_workload_op wrkld_per_rank;

        wrkld_per_rank.op_type = CODES_WK_WAITANY;
        wrkld_per_rank.u.waits.count = prm->count;
        wrkld_per_rank.u.waits.req_ids = (unsigned int*)malloc(prm->count * sizeof(unsigned int));

        for( i = 0; i < prm->count; i++ )
                wrkld_per_rank.u.waits.req_ids[i] = prm->requests[i];

        update_times_and_insert(&wrkld_per_rank, wall, myctx);
        return 0;
}

int handleDUMPIWaitall(const dumpi_waitall *prm, uint16_t thread,
                    const dumpi_time *cpu, const dumpi_time *wall,
                    const dumpi_perfinfo *perf, void *userarg)
{
        (void)prm;
        (void)thread;
        (void)cpu;
        (void)wall;
        (void)perf;
        int i;
        
        rank_mpi_context* myctx = (rank_mpi_context*)userarg;
        struct codes_workload_op wrkld_per_rank;

        wrkld_per_rank.op_type = CODES_WK_WAITALL;

        wrkld_per_rank.u.waits.count = prm->count;
        wrkld_per_rank.u.waits.req_ids = (unsigned int*)malloc(prm->count * sizeof(unsigned int));
        for( i = 0; i < prm->count; i++ )
                wrkld_per_rank.u.waits.req_ids[i] = prm->requests[i];

        update_times_and_insert(&wrkld_per_rank, wall, myctx);
        return 0;
}

int handleDUMPIISend(const dumpi_isend *prm, uint16_t thread, const dumpi_time *cpu, const dumpi_time *wall, const dumpi_perfinfo *perf, void *userarg)
{
        (void)prm;
        (void)thread;
        (void)cpu;
        (void)wall;
        (void)perf;
	
        rank_mpi_context* myctx = (rank_mpi_context*)userarg;
        struct codes_workload_op wrkld_per_rank;

        wrkld_per_rank.op_type = CODES_WK_ISEND;
        wrkld_per_rank.u.send.tag = prm->tag;
        wrkld_per_rank.u.send.count = prm->count;
        wrkld_per_rank.u.send.data_type = prm->datatype;
        wrkld_per_rank.u.send.num_bytes = prm->count * get_num_bytes(myctx,prm->datatype);
        
        assert(wrkld_per_rank.u.send.num_bytes >= 0);
    	wrkld_per_rank.u.send.req_id = prm->request;
        wrkld_per_rank.u.send.dest_rank = prm->dest;
        wrkld_per_rank.u.send.source_rank = myctx->my_rank;

        update_times_and_insert(&wrkld_per_rank, wall, myctx);
	
        return 0;
}

int handleDUMPIIRecv(const dumpi_irecv *prm, uint16_t thread, const dumpi_time *cpu, const dumpi_time *wall, const dumpi_perfinfo *perf, void *userarg)
{
        (void)prm;
        (void)thread;
        (void)cpu;
        (void)wall;
        (void)perf;
	
        //printf("\n irecv source %d count %d data type %d", prm->source, prm->count, prm->datatype);
        rank_mpi_context* myctx = (rank_mpi_context*)userarg;
        struct codes_workload_op wrkld_per_rank;

        wrkld_per_rank.op_type = CODES_WK_IRECV;
	    wrkld_per_rank.u.recv.data_type = prm->datatype;
	    wrkld_per_rank.u.recv.count = prm->count;
	    wrkld_per_rank.u.recv.tag = prm->tag;
        wrkld_per_rank.u.recv.num_bytes = prm->count * get_num_bytes(myctx,prm->datatype);
	    
        assert(wrkld_per_rank.u.recv.num_bytes >= 0);
        wrkld_per_rank.u.recv.source_rank = prm->source;
        wrkld_per_rank.u.recv.dest_rank = -1;
	    wrkld_per_rank.u.recv.req_id = prm->request;

        update_times_and_insert(&wrkld_per_rank, wall, myctx);
        return 0;
}

int handleDUMPISend(const dumpi_send *prm, uint16_t thread,
                      const dumpi_time *cpu, const dumpi_time *wall,
                      const dumpi_perfinfo *perf, void *uarg)
{
        (void)prm;
        (void)thread;
        (void)cpu;
        (void)wall;
        (void)perf;
	    
        rank_mpi_context* myctx = (rank_mpi_context*)uarg;
        struct codes_workload_op wrkld_per_rank;

        wrkld_per_rank.op_type = CODES_WK_SEND;
	    wrkld_per_rank.u.send.tag = prm->tag;
        wrkld_per_rank.u.send.count = prm->count;
        wrkld_per_rank.u.send.data_type = prm->datatype;
        wrkld_per_rank.u.send.num_bytes = prm->count * get_num_bytes(myctx,prm->datatype);
	    assert(wrkld_per_rank.u.send.num_bytes >= 0);
        wrkld_per_rank.u.send.dest_rank = prm->dest;
        wrkld_per_rank.u.send.source_rank = myctx->my_rank;
         wrkld_per_rank.u.send.req_id = -1;

        
         update_times_and_insert(&wrkld_per_rank, wall, myctx);
        return 0;
}

int handleDUMPIRecv(const dumpi_recv *prm, uint16_t thread,
                      const dumpi_time *cpu, const dumpi_time *wall,
                      const dumpi_perfinfo *perf, void *uarg)
{
     (void)prm;
     (void)thread;
     (void)cpu;
     (void)wall;
     (void)perf;

	rank_mpi_context* myctx = (rank_mpi_context*)uarg;
	struct codes_workload_op wrkld_per_rank;

	wrkld_per_rank.op_type = CODES_WK_RECV;
    wrkld_per_rank.u.recv.tag = prm->tag;
    wrkld_per_rank.u.recv.count = prm->count;
    wrkld_per_rank.u.recv.data_type = prm->datatype;
    wrkld_per_rank.u.recv.num_bytes = prm->count * get_num_bytes(myctx,prm->datatype);
	assert(wrkld_per_rank.u.recv.num_bytes >= 0);
	wrkld_per_rank.u.recv.req_id = -1;
    wrkld_per_rank.u.recv.source_rank = prm->source;
    wrkld_per_rank.u.recv.dest_rank = -1;

	//printf("\n recv source %d count %d data type %d bytes %lld ", prm->source, prm->count, prm->datatype, wrkld_per_rank.u.recv.num_bytes);
    update_times_and_insert(&wrkld_per_rank, wall, myctx);
    return 0;

}

int handleDUMPISendrecv(const dumpi_sendrecv* prm, uint16_t thread,
			const dumpi_time *cpu, const dumpi_time *wall,
			const dumpi_perfinfo *perf, void *uarg)
{
     (void)prm;
     (void)thread;
     (void)cpu;
     (void)wall;
     (void)perf;
	
     rank_mpi_context* myctx = (rank_mpi_context*)uarg;

     /* Issue a non-blocking send */
	{
		struct codes_workload_op wrkld_per_rank;
		wrkld_per_rank.op_type = CODES_WK_ISEND;
		wrkld_per_rank.u.send.tag = prm->sendtag;
		wrkld_per_rank.u.send.count = prm->sendcount;
		wrkld_per_rank.u.send.data_type = prm->sendtype;
		wrkld_per_rank.u.send.num_bytes = prm->sendcount * get_num_bytes(myctx,prm->sendtype);

		
        assert(wrkld_per_rank.u.send.num_bytes >= 0);
		wrkld_per_rank.u.send.dest_rank = prm->dest;
		wrkld_per_rank.u.send.source_rank = myctx->my_rank;
		wrkld_per_rank.u.send.req_id = myctx->num_reqs;
		update_times_and_insert(&wrkld_per_rank, wall, myctx);

	}
    /* issue a blocking receive */
	{
		struct codes_workload_op wrkld_per_rank;
		wrkld_per_rank.op_type = CODES_WK_RECV;
		wrkld_per_rank.u.recv.tag = prm->recvtag;
		wrkld_per_rank.u.recv.count = prm->recvcount;
		wrkld_per_rank.u.recv.data_type = prm->recvtype;
		wrkld_per_rank.u.recv.num_bytes = prm->recvcount * get_num_bytes(myctx,prm->recvtype);

        assert(wrkld_per_rank.u.recv.num_bytes >= 0);
		wrkld_per_rank.u.recv.source_rank = prm->source;
		wrkld_per_rank.u.recv.dest_rank = -1;
	    wrkld_per_rank.u.recv.req_id = -1;
		update_times_and_insert(&wrkld_per_rank, wall, myctx);
	}
    
    /* Issue a wait operation */
    {
        struct codes_workload_op wrkld_per_rank;

        wrkld_per_rank.op_type = CODES_WK_WAIT;
        wrkld_per_rank.u.wait.req_id = myctx->num_reqs;

        update_times_and_insert(&wrkld_per_rank, wall, myctx);
    
        myctx->num_reqs++;
    }


	return 0;
}

int handleDUMPIBcast(const dumpi_bcast *prm, uint16_t thread,
                       const dumpi_time *cpu, const dumpi_time *wall,
                       const dumpi_perfinfo *perf, void *uarg)
{
        (void)prm;
        (void)thread;
        (void)cpu;
        (void)wall;
        (void)perf;
        rank_mpi_context* myctx = (rank_mpi_context*)uarg;
        struct codes_workload_op wrkld_per_rank;

        wrkld_per_rank.op_type = CODES_WK_BCAST;
        wrkld_per_rank.u.collective.num_bytes = prm->count * get_num_bytes(myctx,prm->datatype);
	    assert(wrkld_per_rank.u.collective.num_bytes >= 0);

        update_times_and_insert(&wrkld_per_rank, wall, myctx);
        return 0;
}

int handleDUMPIAllgather(const dumpi_allgather *prm, uint16_t thread,
                           const dumpi_time *cpu, const dumpi_time *wall,
                           const dumpi_perfinfo *perf, void *uarg)
{
    (void)prm;
    (void)thread;
    (void)cpu;
    (void)wall;
    (void)perf;
	rank_mpi_context* myctx = (rank_mpi_context*)uarg;
	struct codes_workload_op wrkld_per_rank;

    wrkld_per_rank.op_type = CODES_WK_ALLGATHER;
    wrkld_per_rank.u.collective.num_bytes = prm->sendcount * get_num_bytes(myctx,prm->sendtype);
	assert(wrkld_per_rank.u.collective.num_bytes > 0);

    update_times_and_insert(&wrkld_per_rank, wall, myctx);
    return 0;
}

int handleDUMPIAllgatherv(const dumpi_allgatherv *prm, uint16_t thread,
                            const dumpi_time *cpu, const dumpi_time *wall,
                            const dumpi_perfinfo *perf, void *uarg)
{
        (void)prm;
        (void)thread;
        (void)cpu;
        (void)wall;
        (void)perf;
	    rank_mpi_context* myctx = (rank_mpi_context*)uarg;
	    struct codes_workload_op wrkld_per_rank;

        wrkld_per_rank.op_type = CODES_WK_ALLGATHERV;
        wrkld_per_rank.u.collective.num_bytes = prm->sendcount * get_num_bytes(myctx,prm->sendtype);
	    assert(wrkld_per_rank.u.collective.num_bytes > 0);

        update_times_and_insert(&wrkld_per_rank, wall, myctx);
        return 0;
}

int handleDUMPIAlltoall(const dumpi_alltoall *prm, uint16_t thread,
                          const dumpi_time *cpu, const dumpi_time *wall,
                          const dumpi_perfinfo *perf, void *uarg)
{
        (void)prm;
        (void)thread;
        (void)cpu;
        (void)wall;
        (void)perf;
	    rank_mpi_context* myctx = (rank_mpi_context*)uarg;
	    struct codes_workload_op wrkld_per_rank;

        wrkld_per_rank.op_type = CODES_WK_ALLTOALL;
        wrkld_per_rank.u.collective.num_bytes = prm->sendcount * get_num_bytes(myctx,prm->sendtype);
	    assert(wrkld_per_rank.u.collective.num_bytes > 0);

        update_times_and_insert(&wrkld_per_rank, wall, myctx);
        return 0;
}

int handleDUMPIAlltoallv(const dumpi_alltoallv *prm, uint16_t thread,
                           const dumpi_time *cpu, const dumpi_time *wall,
                           const dumpi_perfinfo *perf, void *uarg)
{
        (void)prm;
        (void)thread;
        (void)cpu;
        (void)wall;
        (void)perf;
	
        rank_mpi_context* myctx = (rank_mpi_context*)uarg;
	    struct codes_workload_op wrkld_per_rank;

        wrkld_per_rank.op_type = CODES_WK_ALLTOALLV;
        wrkld_per_rank.u.collective.num_bytes = prm->sendcounts[0] * get_num_bytes(myctx,prm->sendtype);
	    assert(wrkld_per_rank.u.collective.num_bytes > 0);

        update_times_and_insert(&wrkld_per_rank, wall, myctx);
        return 0;
}

int handleDUMPIReduce(const dumpi_reduce *prm, uint16_t thread,
                        const dumpi_time *cpu, const dumpi_time *wall,
                        const dumpi_perfinfo *perf, void *uarg)
{
        (void)prm;
        (void)thread;
        (void)cpu;
        (void)wall;
        (void)perf;
	
        rank_mpi_context* myctx = (rank_mpi_context*)uarg;
	    struct codes_workload_op wrkld_per_rank;

        wrkld_per_rank.op_type = CODES_WK_REDUCE;
        wrkld_per_rank.u.collective.num_bytes = prm->count * get_num_bytes(myctx,prm->datatype);
	    assert(wrkld_per_rank.u.collective.num_bytes > 0);

        update_times_and_insert(&wrkld_per_rank, wall, myctx);
        return 0;
}

int handleDUMPIAllreduce(const dumpi_allreduce *prm, uint16_t thread,
                           const dumpi_time *cpu, const dumpi_time *wall,
                           const dumpi_perfinfo *perf, void *uarg)
{
        (void)prm;
        (void)thread;
        (void)cpu;
        (void)wall;
        (void)perf;
	
        rank_mpi_context* myctx = (rank_mpi_context*)uarg;
	    struct codes_workload_op wrkld_per_rank;

        wrkld_per_rank.op_type = CODES_WK_ALLREDUCE;
        wrkld_per_rank.u.collective.num_bytes = prm->count * get_num_bytes(myctx,prm->datatype);
	    assert(wrkld_per_rank.u.collective.num_bytes > 0);

        update_times_and_insert(&wrkld_per_rank, wall, myctx);
        return 0;
}

int handleDUMPIFinalize(const dumpi_finalize *prm, uint16_t thread, const dumpi_time *cpu, const dumpi_time *wall, const dumpi_perfinfo *perf, void *uarg)
{
        (void)prm;
        (void)thread;
        (void)cpu;
        (void)wall;
        (void)perf;
	
        rank_mpi_context* myctx = (rank_mpi_context*)uarg;
	    struct codes_workload_op wrkld_per_rank;

        wrkld_per_rank.op_type = CODES_WK_END;

        update_times_and_insert(&wrkld_per_rank, wall, myctx);
        return 0;
}

int handleDUMPIReqFree(const dumpi_request_free *prm, uint16_t thread, const dumpi_time *cpu, const dumpi_time *wall, const dumpi_perfinfo *perf, void *userarg)
{
        (void)prm;
        (void)thread;
        (void)cpu;
        (void)wall;
        (void)perf;
    
        rank_mpi_context* myctx = (rank_mpi_context*)userarg;
        struct codes_workload_op wrkld_per_rank;

        wrkld_per_rank.op_type = CODES_WK_REQ_FREE;
        wrkld_per_rank.u.free.req_id = prm->request;

        update_times_and_insert(&wrkld_per_rank, wall, myctx);
        return 0;
}

static int hash_rank_compare(void *key, struct qhash_head *link)
{
    rank_mpi_compare *in = key;
    rank_mpi_context *tmp;

    tmp = qhash_entry(link, rank_mpi_context, hash_link);
    if (tmp->my_rank == in->rank && tmp->my_app_id == in->app)
        return 1;
    return 0;
}

int dumpi_trace_nw_workload_load(const char* params, int app_id, int rank)
{
	libundumpi_callbacks callbacks;

    PROFILE_TYPE profile;
	dumpi_trace_params* dumpi_params = (dumpi_trace_params*)params;
	char file_name[MAX_LENGTH_FILE];

	if(rank >= dumpi_params->num_net_traces)
		return -1;

    int hash_size = (dumpi_params->num_net_traces / dumpi_params->nprocs) + 1;
	if(!rank_tbl)
    	{
            rank_tbl = qhash_init(hash_rank_compare, quickhash_64bit_hash, hash_size);
            if(!rank_tbl)
                  return -1;
    	}
	
	rank_mpi_context *my_ctx;
	my_ctx = malloc(sizeof(rank_mpi_context));
	assert(my_ctx);
	my_ctx->my_rank = rank;
    my_ctx->my_app_id = app_id;
	my_ctx->last_op_time = 0.0;
    my_ctx->is_init = 0;
    my_ctx->num_reqs = 0;
    dumpi_init_op_data(my_ctx);
    my_ctx->num_ops = 0;

	if(rank < 10)
            sprintf(file_name, "%s000%d.bin", dumpi_params->file_name, rank);
         else if(rank >=10 && rank < 100)
            sprintf(file_name, "%s00%d.bin", dumpi_params->file_name, rank);
           else if(rank >=100 && rank < 1000)
             sprintf(file_name, "%s0%d.bin", dumpi_params->file_name, rank);
             else
              sprintf(file_name, "%s%d.bin", dumpi_params->file_name, rank);
#ifdef ENABLE_CORTEX
	if(strcmp(dumpi_params->file_name,"none") == 0) {
		profile = cortex_undumpi_open(NULL, app_id, dumpi_params->num_net_traces, rank);
	} else {
		profile = cortex_undumpi_open(file_name, app_id, dumpi_params->num_net_traces, rank);
	}
	
	{ int i;
	for(i=0; i < dumpi_params->num_net_traces; i++) {
		struct codes_jobmap_id id = {
			.job = app_id,
			.rank = i
		};
		uint32_t cn_id;
		if(jobmap_ctx) {
			cn_id = codes_jobmap_to_global_id(id, jobmap_ctx);
		} else {
			cn_id = i;
		}
		cortex_placement_set(profile, i, cn_id);
	}
	}
	
	cortex_topology_set(profile,&model_net_topology);
#else
	profile =  undumpi_open(file_name);
#endif
        my_ctx->profile = profile;
        if(NULL == profile) {
                printf("Error: unable to open DUMPI trace: %s", file_name);
                exit(-1);
        }
	
	memset(&callbacks, 0, sizeof(libundumpi_callbacks));
        memset(&my_ctx->callarr, 0, sizeof(libundumpi_cbpair) * DUMPI_END_OF_STREAM);
#ifdef ENABLE_CORTEX
	memset(&transarr, 0, sizeof(libundumpi_cbpair) * DUMPI_END_OF_STREAM);
#endif

	/* handle MPI function calls */	        
        callbacks.on_init = handleDUMPIInit;
	callbacks.on_send = (dumpi_send_call)handleDUMPISend;
        callbacks.on_recv = (dumpi_recv_call)handleDUMPIRecv;
        callbacks.on_isend = (dumpi_isend_call)handleDUMPIISend;
        callbacks.on_irecv = (dumpi_irecv_call)handleDUMPIIRecv;
        callbacks.on_allreduce = (dumpi_allreduce_call)handleDUMPIAllreduce;
	callbacks.on_bcast = (dumpi_bcast_call)handleDUMPIBcast;
	callbacks.on_get_count = (dumpi_get_count_call)handleDUMPIIgnore;
	callbacks.on_bsend = (dumpi_bsend_call)handleDUMPIIgnore;
	callbacks.on_ssend = (dumpi_ssend_call)handleDUMPIIgnore;
	callbacks.on_rsend = (dumpi_rsend_call)handleDUMPIIgnore;
	callbacks.on_buffer_attach = (dumpi_buffer_attach_call)handleDUMPIIgnore;
	callbacks.on_buffer_detach = (dumpi_buffer_detach_call)handleDUMPIIgnore;
	callbacks.on_ibsend = (dumpi_ibsend_call)handleDUMPIIgnore;
	callbacks.on_issend = (dumpi_issend_call)handleDUMPIIgnore;
	callbacks.on_irsend = (dumpi_irsend_call)handleDUMPIIgnore;
	callbacks.on_wait = (dumpi_wait_call)handleDUMPIWait;
	callbacks.on_test = (dumpi_test_call)handleDUMPIIgnore;
	callbacks.on_request_free = (dumpi_request_free_call)handleDUMPIReqFree;
	callbacks.on_waitany = (dumpi_waitany_call)handleDUMPIWaitany;
	callbacks.on_testany = (dumpi_testany_call)handleDUMPIIgnore;
	callbacks.on_waitall = (dumpi_waitall_call)handleDUMPIWaitall;
	callbacks.on_testall = (dumpi_testall_call)handleDUMPIIgnore;
	callbacks.on_waitsome = (dumpi_waitsome_call)handleDUMPIWaitsome;
	callbacks.on_testsome = (dumpi_testsome_call)handleDUMPIIgnore;
	callbacks.on_iprobe = (dumpi_iprobe_call)handleDUMPIIgnore;
	callbacks.on_probe = (dumpi_probe_call)handleDUMPIIgnore;
	callbacks.on_cancel = (dumpi_cancel_call)handleDUMPIIgnore;
	callbacks.on_test_cancelled = (dumpi_test_cancelled_call)handleDUMPIIgnore;
	callbacks.on_send_init = (dumpi_send_init_call)handleDUMPIIgnore;
	callbacks.on_bsend_init = (dumpi_bsend_init_call)handleDUMPIIgnore;
	callbacks.on_ssend_init = (dumpi_ssend_init_call)handleDUMPIIgnore;
	callbacks.on_rsend_init = (dumpi_rsend_init_call)handleDUMPIIgnore;
	callbacks.on_recv_init = (dumpi_recv_init_call)handleDUMPIIgnore;
	callbacks.on_start = (dumpi_start_call)handleDUMPIIgnore;
	callbacks.on_startall = (dumpi_startall_call)handleDUMPIIgnore;
	callbacks.on_sendrecv = (dumpi_sendrecv_call)handleDUMPISendrecv;
	callbacks.on_sendrecv_replace = (dumpi_sendrecv_replace_call)handleDUMPIIgnore;
	callbacks.on_type_contiguous = (dumpi_type_contiguous_call)handleDUMPIIgnore;
	callbacks.on_barrier = (dumpi_barrier_call)handleDUMPIIgnore;
        callbacks.on_gather = (dumpi_gather_call)handleDUMPIIgnore;
        callbacks.on_gatherv = (dumpi_gatherv_call)handleDUMPIIgnore;
        callbacks.on_scatter = (dumpi_scatter_call)handleDUMPIIgnore;
        callbacks.on_scatterv = (dumpi_scatterv_call)handleDUMPIIgnore;
        callbacks.on_allgather = (dumpi_allgather_call)handleDUMPIIgnore;
        callbacks.on_allgatherv = (dumpi_allgatherv_call)handleDUMPIIgnore;
        callbacks.on_alltoall = (dumpi_alltoall_call)handleDUMPIIgnore;
        callbacks.on_alltoallv = (dumpi_alltoallv_call)handleDUMPIIgnore;
        callbacks.on_alltoallw = (dumpi_alltoallw_call)handleDUMPIIgnore;
        callbacks.on_reduce = (dumpi_reduce_call)handleDUMPIIgnore;
        callbacks.on_reduce_scatter = (dumpi_reduce_scatter_call)handleDUMPIIgnore;
        callbacks.on_group_size = (dumpi_group_size_call)handleDUMPIIgnore;
        callbacks.on_group_rank = (dumpi_group_rank_call)handleDUMPIIgnore;
        callbacks.on_comm_size = (dumpi_comm_size_call)handleDUMPIIgnore;
        callbacks.on_comm_rank = (dumpi_comm_rank_call)handleDUMPIIgnore;
        callbacks.on_comm_get_attr = (dumpi_comm_get_attr_call)handleDUMPIIgnore;
        callbacks.on_comm_dup = (dumpi_comm_dup_call)handleDUMPIError;
        callbacks.on_comm_create = (dumpi_comm_create_call)handleDUMPIError;
        callbacks.on_wtime = (dumpi_wtime_call)handleDUMPIIgnore;
        callbacks.on_finalize = (dumpi_finalize_call)handleDUMPIFinalize;

        libundumpi_populate_callbacks(&callbacks, my_ctx->callarr);

#ifdef ENABLE_CORTEX
#ifdef ENABLE_CORTEX_PYTHON
	if(dumpi_params->cortex_script[0] != 0) {
		libundumpi_populate_callbacks(CORTEX_PYTHON_TRANSLATION, my_ctx->transarr);
	} else {
		libundumpi_populate_callbacks(CORTEX_MPICH_TRANSLATION, my_ctx->transarr);
	}
#else
	libundumpi_populate_callbacks(CORTEX_MPICH_TRANSLATION, my_ctx->transarr);
#endif
#endif
        DUMPI_START_STREAM_READ(profile);
        //dumpi_header* trace_header = undumpi_read_header(profile);
        //dumpi_free_header(trace_header);

#ifdef ENABLE_CORTEX_PYTHON
	if(dumpi_params->cortex_script[0] != 0) {
		if(dumpi_params->cortex_class[0] != 0) {
			cortex_python_set_module(dumpi_params->cortex_script, dumpi_params->cortex_class);
		} else {
			cortex_python_set_module(dumpi_params->cortex_script, NULL);
		}
		if(dumpi_params->cortex_gen[0] != 0) {
			cortex_python_call_generator(profile, dumpi_params->cortex_gen);
		}
	}
#endif

#if 0
        int finalize_reached = 0;
        int active = 1;
        int num_calls = 0;
        while(active && !finalize_reached)
        {
           num_calls++;
           my_ctx->num_ops++;
#ifdef ENABLE_CORTEX
           if(my_ctx->num_ops < max_threshold)
	        active = cortex_undumpi_read_single_call(profile, callarr, transarr, (void*)my_ctx, &finalize_reached);
           else
           {
                struct codes_workload_op op;
                op.op_type = CODES_WK_END;

                op.start_time = my_ctx->last_op_time;
                op.end_time = my_ctx->last_op_time + 1;
                dumpi_insert_next_op(my_ctx->dumpi_mpi_array, &op);
                break;
           }
#else
           active = undumpi_read_single_call(profile, callarr, (void*)my_ctx, &finalize_reached);
#endif
        }

        // now that DUMPI events are read on the fly, we shouldn't close the profile here
#endif

#if 0
    UNDUMPI_CLOSE(profile);
	dumpi_finalize_mpi_op_data(my_ctx);
#endif
	/* add this rank context to hash table */	
        rank_mpi_compare cmp;
        cmp.app = my_ctx->my_app_id;
        cmp.rank = my_ctx->my_rank;
	qhash_add(rank_tbl, &cmp, &(my_ctx->hash_link));
	rank_tbl_pop++;

	return 0;
}
/* Data types are for 64-bit archs. Source:
 * https://www.tutorialspoint.com/cprogramming/c_data_types.htm 
 * */
static uint64_t get_num_bytes(rank_mpi_context* myctx, dumpi_datatype dt)
{
    (void)myctx;

#ifdef ENABLE_CORTEX
   return cortex_datatype_get_size(myctx->profile,dt);
#endif
   switch(dt)
   {
	case DUMPI_DATATYPE_ERROR:
	case DUMPI_DATATYPE_NULL:
		tw_error(TW_LOC, "\n data type error");
	break;

	case DUMPI_CHAR:
	case DUMPI_UNSIGNED_CHAR:
	case DUMPI_SIGNED_CHAR:
	case DUMPI_BYTE:
		return 1; /* 1 byte for char */
	break;

	case DUMPI_WCHAR:
		return 4; /* 4 bytes for a 64-bit version */
	break;

	case DUMPI_SHORT:
	case DUMPI_SHORT_INT:
	case DUMPI_UNSIGNED_SHORT:
		return 2;
	break;

	 case DUMPI_INT:
		return 4;
	 break;

	 case DUMPI_UNSIGNED:
     return 4;
     break;

	 case DUMPI_FLOAT:
	 case DUMPI_FLOAT_INT:
        return 4;
     break;

	case DUMPI_DOUBLE:
     return 8;
    break;

	case DUMPI_LONG:
     return 8;
     break;

	case DUMPI_LONG_INT:
     return 8;
     break;

	case DUMPI_UNSIGNED_LONG:
     return 8;
     break;

	case DUMPI_LONG_LONG_INT:
     return 8;
     break;

	case DUMPI_UNSIGNED_LONG_LONG:
     return 8;
     break;

	case DUMPI_LONG_LONG:
     return 8;
     break;

	case DUMPI_DOUBLE_INT:
		return 8;
	break;

	case DUMPI_LONG_DOUBLE_INT:
	case DUMPI_LONG_DOUBLE:
        return 10;
        break;

	default:
	  {
        tw_error(TW_LOC, "\n undefined data type");
		return 0;	
	  }	
   } 
}

void dumpi_trace_nw_workload_get_next_rc2(int app_id, int rank)
{
    rank_mpi_context* ctx; 
    struct qhash_head *hash_link = NULL;  
    rank_mpi_compare cmp;  
    cmp.rank = rank;
    cmp.app = app_id;

    hash_link = qhash_search(rank_tbl, &cmp);

    assert(hash_link);
    ctx = qhash_entry(hash_link, rank_mpi_context, hash_link); 
    assert(ctx);

    dumpi_roll_back_prev_op(ctx);
}
void dumpi_trace_nw_workload_get_next(int app_id, int rank, struct codes_workload_op *op)
{
   rank_mpi_context* ctx;
   struct qhash_head *hash_link = NULL;
   rank_mpi_compare cmp;
   cmp.rank = rank;
   cmp.app = app_id;
   hash_link = qhash_search(rank_tbl, &cmp);
   if(!hash_link)
   {
      printf("\n not found for rank id %d , %d", rank, app_id);
      op->op_type = CODES_WK_END;
      return;
   }
  ctx = qhash_entry(hash_link, rank_mpi_context, hash_link);
  assert(ctx);

  struct codes_workload_op mpi_op;
  dumpi_remove_next_op(ctx, &mpi_op, ctx->last_op_time);
  *op = mpi_op;
  return;
}

/* implements the codes workload method */
struct codes_workload_method dumpi_trace_workload_method =
{
    .method_name = "dumpi-trace-workload",
    .codes_workload_read_config = NULL,
    .codes_workload_load = dumpi_trace_nw_workload_load,
    .codes_workload_get_next = dumpi_trace_nw_workload_get_next,
    .codes_workload_get_next_rc2 = dumpi_trace_nw_workload_get_next_rc2
};

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 *  indent-tabs-mode: nil
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
