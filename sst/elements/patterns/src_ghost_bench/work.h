/*
** $Id: work.h,v 1.3 2010-10-25 16:51:42 rolf Exp $
**
** Rolf Riesen, Sandia National Laboratories, July 2010
** Routines to do the work and the ghost cell exchanges.
*/

#ifndef _WORK_H_
#define _WORK_H_

#include "memory.h"	/* For mem_ptr_t */
#include "neighbors.h"	/* For neighbors_t */


void exchange_ghosts(int TwoD, mem_ptr_t *m, neighbors_t *n, long long int *bytes_sent, long long int *num_sends);
void compute(int TwoD, mem_ptr_t *m, long long int *fop_cnt, int max_loop, double delay, int imbalance);

#endif /* _WORK_H_ */
