/*
** $Id: ghost.c,v 1.23 2010-10-27 22:11:55 rolf Exp $
**
** Rolf Riesen, Sandia National Laboratories, July 2010
** A communication benchmark that mimics a boundary exchange application
** using ghost cells.
**
*/
#include <stdio.h>
#include <stdlib.h>	/* For strtol(), exit() */
#include <unistd.h>	/* For getopt() */
#include <math.h>
#include <mpi.h>

#include "ghost.h"
#include "ranks.h"
#include "neighbors.h"
#include "memory.h"
#include "work.h"

#define GHOST_VERSION	"1.0"


/* Local functions */
static void usage(char *pname);



int
main(int argc, char *argv[])
{

int ch, error;
int verbose;
int my_rank, num_ranks;
int time_steps;
int x_dim, y_dim, z_dim;
int width, height, depth;
int res;
mem_ptr_t memory;
int t, i;
neighbors_t neighbor_list;
double total_time_start, total_time_end;
double comm_time_start, comm_time_total;
double comp_time_start, comp_time_total;
size_t mem_estimate;
long long int num_sends;
long long int fop_cnt;
long long int reduce_cnt;
long long int bytes_sent;
int TwoD;
int loop;
int decomposition_only;
int reduce_steps;
int compute_imbalance;
double compute_delay;



    MPI_Init(&argc, &argv);
 
    MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);
    MPI_Comm_size(MPI_COMM_WORLD, &num_ranks);

    if (num_ranks < 2)   {
	if (my_rank == 0)   {
	    fprintf(stderr, "Need to run on at least two ranks; more would be better\n");
	}
	MPI_Finalize();
	exit(-1);
    }


    /* Set the defaults */
    opterr= 0;		/* Disable getopt error printing */
    error= FALSE;
    verbose= 1;
    time_steps= DEFAULT_TIME_STEPS;
    x_dim= -1;
    y_dim= -1;
    z_dim= -1;
    loop= DEFAULT_LOOP;
    decomposition_only= FALSE;
    reduce_steps= DEFAULT_REDUCE_STEPS;
    compute_imbalance= FALSE;
    compute_delay= 0.0;


    /* Check command line args */
    while ((ch= getopt(argc, argv, ":vt:x:y:z:l:Dr:d:i")) != EOF)   {
        switch (ch)   {
            case 'v':
		verbose++;
		break;
            case 't':
		time_steps= strtol(optarg, (char **)NULL, 0);
		if (time_steps <= 0)   {
		    if (my_rank == 0)   {
			fprintf(stderr, "Time steps must be > 0!\n");
		    }
		    error= TRUE;
		}
		break;
            case 'x':
		x_dim= strtol(optarg, (char **)NULL, 0);
		if (x_dim <= 0)   {
		    if (my_rank == 0)   {
			fprintf(stderr, "X dimension must be > 0!\n");
		    }
		    error= TRUE;
		}
		break;
            case 'y':
		y_dim= strtol(optarg, (char **)NULL, 0);
		if (y_dim <= 0)   {
		    if (my_rank == 0)   {
			fprintf(stderr, "Y dimension must be > 0!\n");
		    }
		    error= TRUE;
		}
		break;
            case 'z':
		z_dim= strtol(optarg, (char **)NULL, 0);
		if (z_dim < 0)   {
		    if (my_rank == 0)   {
			fprintf(stderr, "Z dimension must be >= 0!\n");
		    }
		    error= TRUE;
		}
		break;
            case 'l':
		loop= strtol(optarg, (char **)NULL, 0);
		if (loop < 1)   {
		    if (my_rank == 0)   {
			fprintf(stderr, "Loop must be > 0!\n");
		    }
		    error= TRUE;
		}
		break;
            case 'D':
		decomposition_only= TRUE;
		break;
            case 'r':
		reduce_steps= strtol(optarg, (char **)NULL, 0);
		if (reduce_steps <= 1)   {
		    if (my_rank == 0)   {
			fprintf(stderr, "Number of steps between reduce ops must be > 0!\n");
		    }
		    error= TRUE;
		}
		break;
            case 'd':
		compute_delay= strtod(optarg, (char **)NULL);
		if (compute_delay < 0.0)   {
		    if (my_rank == 0)   {
			fprintf(stderr, "Compute delay cannot be less than 0!\n");
		    }
		    error= TRUE;
		}
		break;
            case 'i':
		srand48(543219876 * (my_rank + 1));
		compute_imbalance= TRUE;
		break;

	    /* Command line error checking */
            case '?':
		if (my_rank == 0)   {
		    fprintf(stderr, "Unknown option \"%s\"\n", argv[optind - 1]);
		}
		error= TRUE;
		break;
            case ':':
		if (my_rank == 0)   {
		    fprintf(stderr, "Missing option argument to \"%s\"\n", argv[optind - 1]);
		}
		error= TRUE;
		break;
            default:
		error= TRUE;
		break;
        }
    }
 
    if (error)   {
        if (my_rank == 0)   {
            usage(argv[0]);
        }
	MPI_Finalize();
        exit (-1);
    }

    if (my_rank == 0)   {
	printf("Ghost cell exchange benchmark. Version %s\n", GHOST_VERSION);
	printf("------------------------------------------\n");
	printf("Command line \"");
	for (i= 0; i < argc; i++)   {
	    printf("%s", argv[i]);
	    if (i < (argc - 1))   {
		printf(" ");
	    }
	}
	printf("\"\n");
    }



    /*
    ** Assign ranks to portions of the data.
    ** Width, height, and depth are in number of ranks.
    ** x_dim, y_dim, z_dim are the number of elements (float, double, etc.)
    ** per rank in each dimension.
    */
    TwoD= !z_dim;
    map_ranks(num_ranks, TwoD, &width, &height, &depth);
    check_element_assignment(verbose, decomposition_only, num_ranks, width, height, depth, my_rank,
	&TwoD, &x_dim, &y_dim, &z_dim);

    /* Figure out who my neighbors are */
    neighbors(verbose, my_rank, width, height, depth, &neighbor_list);

    /* Estimate how much memory we will need */
    mem_estimate= mem_needed(verbose, decomposition_only, num_ranks, my_rank, TwoD, x_dim, y_dim, z_dim);

    /* If we did a trial run to calculate the decomposition only, we can stop here. */
    if (decomposition_only)   {
	MPI_Finalize();
	return 0;
    }

    /* Go ahead and allocate and initialize the memory we need */
    do_mem_alloc(my_rank, TwoD, mem_estimate, &memory, x_dim, y_dim, z_dim);

    /* Some info about what happened so far */
    if ((my_rank == 0) && (verbose))   {
	if (TwoD)   {
	    printf("Border to area ratio is %.3g\n",
		(2.0 * (x_dim + y_dim)) / ((float)x_dim * y_dim));
	} else   {
	    printf("Area to volume ratio is %.3g\n",
		(2.0 * x_dim * y_dim +
		 2.0 * x_dim * z_dim +
		 2.0 * y_dim * z_dim) /
		((float)x_dim * y_dim * z_dim));
	}
    }



    /* Now perform the work */
    comm_time_total= 0.0;
    comp_time_total= 0.0;
    bytes_sent= 0;
    num_sends= 0;
    fop_cnt= 0;
    reduce_cnt= 0;
    MPI_Barrier(MPI_COMM_WORLD);
    total_time_start= MPI_Wtime();
    for (t= 0; t < time_steps; t++)   {
	/* Exchange ghost cells */
	comm_time_start= MPI_Wtime();
	exchange_ghosts(TwoD, &memory, &neighbor_list, &bytes_sent, &num_sends);
	comm_time_total += MPI_Wtime() - comm_time_start;

	/* Do some computations */
	comp_time_start= MPI_Wtime();
	compute(TwoD, &memory, &fop_cnt, loop, compute_delay, compute_imbalance);
	comp_time_total += MPI_Wtime() - comp_time_start;

	if ((my_rank == 0) && (verbose > 1) && (t > 0) && ((t % (time_steps / 10)) == 0))   {
	    printf("Time step %6d/%d complete\n", t, time_steps);
	}

	/* Once in a while do an allreduce to check on convergence */
	if ((t + 1) % reduce_steps == 0)   {
	    comm_time_start= MPI_Wtime();
	    MPI_Allreduce(MPI_IN_PLACE, &res, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
	    comm_time_total += MPI_Wtime() - comm_time_start;
	    reduce_cnt++;
	}
    }
    total_time_end= MPI_Wtime();

    /* Print some statistics */
    if (my_rank == 0)   {
	printf("Time to complete on %d ranks was %.3g seconds\n", num_ranks,
	    total_time_end - total_time_start);
	printf("Total %d time steps\n", time_steps);
	printf("Estimated timing error: %.2f%%\n",
	    100.0 - (100.0 / (total_time_end - total_time_start) * (comm_time_total + comp_time_total)));
	printf("Compute to communicate ratio was %.3g/%.3g = %.1f:1 (%.2f%% computation, %.2f%% "
	    "communication)\n",
	    comp_time_total, comm_time_total, comp_time_total / comm_time_total,
	    100.0 / (total_time_end - total_time_start) * comp_time_total,
	    100.0 / (total_time_end - total_time_start) * comm_time_total);
	printf("Bytes sent from each rank: %ld bytes (%.3f MB), %.1f MB total\n",
	    (long int)bytes_sent, (float)bytes_sent / 1024 / 1024,
	    (float)bytes_sent / 1024 / 1024 * num_ranks);
	printf("Sends per rank: %lld, %lld total\n", num_sends, num_sends * num_ranks);
	printf("Average size per send: %.0f B (%.3f MB)\n", (float)bytes_sent / num_sends,
	    (float)bytes_sent / num_sends / 1024 / 1024);
	printf("Number of allreduces: %lld (one every %d time steps)\n", reduce_cnt, reduce_steps);
	printf("Total number of flotaing point ops %lld (%.3f x 10^9)\n", fop_cnt * num_ranks,
	    (float)fop_cnt / 1000000000.0 * num_ranks);
	printf("Per second: %.0f Flops (%.3f GFlops)\n",
	    (float)fop_cnt * num_ranks / (total_time_end - total_time_start),
	    (float)fop_cnt * num_ranks / (total_time_end - total_time_start) / 1000000000.0);
	printf("Flops per byte sent: %.2f Flops\n", (float)fop_cnt / bytes_sent);
	if (compute_imbalance)   {
	    printf("Each compute step was imbalanced by an average %.1f%%\n", compute_delay);
	} else   {
	    printf("Each compute step was delayed by %.1f%%\n", compute_delay);
	}
    }

    mem_free(&memory);
    MPI_Finalize();

    return 0;

}  /* end of main() */



static void
usage(char *pname)
{

    fprintf(stderr, "Usage: %s [-v] [-t num] [-x dim] [-y dim] [-z dim] [-l loop] [-r reduce] [-D]\n", pname);
    fprintf(stderr, "    -t num      Run for num time steps. Default %d\n", DEFAULT_TIME_STEPS);
    fprintf(stderr, "    -v          Increase verbosity. Repeat option for higher levels.\n");
    fprintf(stderr, "    -x dim      Number of elements per rank in x dimension. Default %d (%d for 3-D)\n",
	DEFAULT_2D_X_DIM, DEFAULT_3D_X_DIM);
    fprintf(stderr, "    -y dim      Number of elements per rank in y dimension. Default %d (%d for 3-D)\n",
	DEFAULT_2D_Y_DIM, DEFAULT_3D_Y_DIM);
    fprintf(stderr, "    -z dim      Number of elements per rank in z dimension. Make this 0 for 2-D. Default %d for 3-D\n",
	DEFAULT_3D_Z_DIM);
    fprintf(stderr, "    -l loop     Number of loops to compute. Adjusts compute time. Default %d\n", DEFAULT_LOOP);
    fprintf(stderr, "    -r reduce   Number of time steps between reduce operations. Default %d\n", DEFAULT_REDUCE_STEPS);
    fprintf(stderr, "    -D          Do decomposition and stop.\n");
    fprintf(stderr, "    -d delay    Delay compute step by delay %%.\n");
    fprintf(stderr, "    -i          Create compute imbalance. Requires delay > 0%%.\n");

}  /* end of usage() */
