/** 2014 Neil Edelman

 This is a printer simulation in POSIX.

 @author Neil
 @version 1
 @since 2014 */

#include <stdlib.h> /* malloc free rand */
#include <stdio.h>  /* fprintf */
#include <time.h>   /* clock */
#include <string.h> /* strerror */
#include <errno.h>  /* strerror */

/* some unices don't include this (eg mimi.cs.mcgill.ca) */
#include <fcntl.h>    /* O_* constants */
#include <sys/stat.h> /* mode constants */
#include <getopt.h>   /* getopt (c99 sometimes doesn't include this) */

#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>   /* getopt */

#include "Job.h"
#include "Client.h"
#include "Printer.h"
#include "Spool.h"

/* constants */
static const char *programme   = "PrinterSimulation";
static const char *year        = "2014";
static const int versionMajor  = 1;
static const int versionMinor  = 0;

struct Spool {
	struct Job **job;
	int jobs_size;
	int head, tail, empty;
	struct Printer **printer;
	int printers_size;
	struct Client **client;
	int clients_size;
};
/*static const int buffer_size = sizeof((struct Spool *)0)->buffer / sizeof(struct Job *);*/

/* 644 */
static const int permission   = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
static const char *mutex_name = "/PrinterSimulation-mutex";
static const char *empty_name = "/PrinterSimulation-empty";
static const char *full_name  = "/PrinterSimulation-full";

/* globals are initailased */

sem_t        *empty, *full;
static sem_t *mutex;

static struct Spool *the_spool;

/* private */
static int semaphores(const int);
static void semaphores_(void);
static void usage(void);

/** entry point
 @param argc the number of arguments starting with the programme name
 @param argv the arguments
 @return     either EXIT_SUCCESS or EXIT_FAILURE */
int main(int argc, char **argv) {
	int i;
	int jobs = 3, clients = 4, printers = 2; /* defualt */
	int is_wtf = 0;
	char c;
	extern char *optarg;
	extern int optind;

	/* parse command line */
	while((c = getopt(argc, argv, "c:p:b:h")) != -1) {
		switch(c) {
			case 'c':
				clients = atoi(optarg);
				break;
			case 'p':
				printers = atoi(optarg);
				break;
			case 'b':
				jobs = atoi(optarg);
				break;
			case 'h':
				usage();
				return EXIT_SUCCESS;
			case '?':
				is_wtf = -1;
				break;
		}
	}
	if(is_wtf || optind != argc || jobs < 1 || clients < 1 || printers < 1) {
		usage();
		return EXIT_FAILURE;
	}

	/* seed the random */
	srand(clock());

	/* semaphores */
	if(!semaphores(jobs)) return EXIT_FAILURE;
	atexit(&semaphores_);

	/* print stuff with multiple threads */
	if(!(the_spool = Spool(jobs, printers, clients))) return EXIT_FAILURE;
	for(i = 0; i < printers; i++) PrinterRun(the_spool->printer[i]);
	/* fixme: the printers have to be started first, I think */
	for(i = 0; i < clients;  i++) ClientRun(the_spool->client[i]);
	Spool_(&the_spool);

	printf("Shutting down.\n");

	return EXIT_SUCCESS;
}

/* public */

/** constructor
 @return an object or a null pointer if the object couldn't be created */
struct Spool *Spool(const int jobs_size, const int printers_size, const int clients_size) {
	struct Spool *s;
	int i;

	if(jobs_size < 1 || printers_size < 1 || clients_size < 1) {
		fprintf(stderr, "Spool: invalid parameters.\n");
		return 0;
	}
	if(!(s = malloc(sizeof(struct Spool)
					+ sizeof(struct Job *) * jobs_size
					+ sizeof(struct Printer *) * printers_size
					+ sizeof(struct Client *) * clients_size))) {
		perror("Spool constructor");
		Spool_(&s);
		return 0;
	}
	s->job           = (struct Job **)(s + 1);
	s->jobs_size     = jobs_size;
	s->head = s->tail = 0;
	s->empty = -1;
	s->printer       = (struct Printer **)&s->job[jobs_size];
	s->printers_size = printers_size;
	s->client        = (struct Client **)&s->printer[printers_size];
	s->clients_size  = clients_size;
	for(i = 0; i <     jobs_size; i++) s->job[i]     = 0;
	for(i = 0; i < printers_size; i++) s->printer[i] = 0;
	for(i = 0; i <  clients_size; i++) s->client[i]  = 0;

	for(i = 0; i < printers_size; i++) s->printer[i] = Printer();
	for(i = 0; i <  clients_size; i++) s->client[i]  = Client();

	fprintf(stderr, "Spool: new, jobs %d, printers %d, clients %d, #%p.\n",
			jobs_size, printers_size, clients_size, (void *)s);

	return s;
}

/** destructor
 @param oo_ptr a reference to the object that is to be deleted */
void Spool_(struct Spool **s_ptr) {
	struct Spool *s;
	int i;

	if(!s_ptr || !(s = *s_ptr)) return;
	fprintf(stderr, "~Spool: begin erasing!\n");
	for(i = 0; i < s->jobs_size;     i++) Job_(&s->job[i]);
	for(i = 0; i < s->printers_size; i++) Printer_(&s->printer[i]);
	for(i = 0; i < s->clients_size;  i++) Client_(&s->client[i]);
	fprintf(stderr, "~Spool: erase, #%p.\n", (void *)s);
	free(s);
	*s_ptr = s = 0;
}

/** attempts to spool the job to the printing queue in the_spool
 @param  job
 @return non-zero on success
 @fixme  refactor, ugly */
int SpoolPushJob(/*const */struct Job *job) {
	int ret = 0;
	int spot = 0;

	if(!the_spool || !job || JobGetPages(job) <= 0) return 0;

	if(sem_trywait(empty) == -1) {
		if(errno == EAGAIN) {
			printf("%s has %d pages to print, but buffer full.\n",
				   ClientGetName(JobGetClient(job)),
				   JobGetPages(job));
		} else {
			perror("empty");
			return 0;
		}
		if(sem_wait(empty) == -1) {
			perror("empty");
			return 0;
		}
	}

	/* critical section -- place job in queue */
	if(sem_wait(mutex) == -1) { perror("mutex"); exit(EXIT_FAILURE); }
	if(the_spool->empty || the_spool->head != the_spool->tail) {
		the_spool->empty = 0;
		JobSetBuffer(job, spot = the_spool->head); /* cosmetic */
		the_spool->job[spot] = (struct Job *)job;
		the_spool->head = (spot + 1) % the_spool->jobs_size;
		ret = -1;
	}
	if(sem_post(mutex) == -1) { perror("mutex"); exit(EXIT_FAILURE); }

	if(!ret) {
		/* spool is full */
		fprintf(stderr, "SpoolPushJob: mismatch between semaphore and counter.\n");
	} else {
		printf("%s has %d pages to print, puts request in Buffer[%d] [%d,%d]\n",
			   ClientGetName(JobGetClient(job)), JobGetPages(job),
			   spot, the_spool->tail, the_spool->head);
		/* signal that we have got a print job; fixme: kind of drastic */
		if(sem_post(full) == -1) { perror("full"); exit(EXIT_FAILURE); }
	}

	return ret;
}

/** attempts to pop a job from the queue
 @return job or null if there is no job */
struct Job *SpoolPopJob(void) {
	struct Job *job = 0;

	if(!the_spool) return 0;

	if(sem_wait(mutex) == -1) { perror("mutex"); exit(EXIT_FAILURE); }
	if(!the_spool->empty) {
		job = the_spool->job[the_spool->tail];
		the_spool->tail = (the_spool->tail + 1) % the_spool->jobs_size;
		if(the_spool->tail == the_spool->head) the_spool->empty = -1;
	}
	if(sem_post(mutex) == -1) { perror("mutex"); exit(EXIT_FAILURE); }

	return job;
}

/** prints out the semaphores for debuging
 produces "mutex: Function not implemented" on my os */
void SpoolSemaphores(void) {
	int m, e, f;
	if(sem_getvalue(mutex, &m) == -1) {
		perror("mutex");
		return;
	}
	if(sem_getvalue(empty, &e) == -1) {
		perror("empty");
		return;
	}
	if(sem_getvalue(full, &f) == -1) {
		perror("full");
		return;
	}
	fprintf(stderr, " -> mutex %d; empty %d; full %d\n", m, e, f);
}

/* private */

/** initailises the global semaphores
 @return non-zero on success */
static int semaphores(const int jobs) {
	/* "mutex: Function not implemented;" my system doesn't allow unnamed
	 semaphores */
	/*if(sem_init(&s->mutex, 0, 1) == -1) {
	 perror("mutex");
	 Spool_(&s);
	 return 0;
	}
	s->is_mutex = -1; ... */	
	/*sem_t *sem = sem_open(name, 0);*/
	fprintf(stderr, "Spool::semaphores: opening.\n");
	/* just in case a mutex_name semaphore already existed; the probable cause
	 is that the programme was terminated the last time without cleaning the
	 semaphores (I looked it up . . . posix semaphores are all but useless;)
	 hack to try and clean up now */
	if(sem_unlink(mutex_name) == -1);
	if((mutex = sem_open(mutex_name, O_CREAT | O_EXCL, permission, 1)) == SEM_FAILED) {
		perror("mutex");
		mutex = 0;
		semaphores_();
		return 0;
	}
	if(sem_unlink(empty_name) == -1);
	if((empty = sem_open(empty_name, O_CREAT | O_EXCL, permission, jobs)) == SEM_FAILED) {
		perror("empty");
		empty = 0;
		semaphores_();
		return 0;
	}
	if(sem_unlink(full_name)  == -1);
	if((full  = sem_open(full_name, O_CREAT | O_EXCL, permission, 0)) == SEM_FAILED) {
		perror("full");
		full  = 0;
		semaphores_();
		return 0;
	}

	/* print the semaphores */
	/*SpoolSemaphores();*/

	return -1;
}

/** deletes the global semaphores */
static void semaphores_(void) {
	/* "Only a semaphore that was created using sem_init() may be destroyed
	 using sem_destroy()" while hmm */
	/*if(s->is_mutex) { sem_destroy(&s->mutex); s->is_mutex = 0; }
	 if(s->is_empty) { sem_destroy(&s->empty); s->is_empty = 0; }
	 if(s->is_full)  { sem_destroy(&s->full);  s->is_full = 0; }*/

	/* print the semaphores */
	/*SpoolSemaphores();*/

	/* we're exiting, we have to just put up with errors I suppose */
	fprintf(stderr, "Spool::~semaphores: closing.\n");
	if(mutex) {
		if(sem_close(mutex) == -1) perror("mutex");
		mutex = 0;
		if(sem_unlink(mutex_name) == -1) perror("mutex");
	}
	if(empty) {
		if(sem_close(empty) == -1) perror("empty");
		empty = 0;
		if(sem_unlink(empty_name) == -1) perror("empty");
	}
	if(full) {
		if(sem_close(full) == -1) perror("full");
		full  = 0;
		if(sem_unlink(full_name) == -1) perror("full");
	}
}

/** prints command-line help */
static void usage(void) {
	fprintf(stderr, "Usage: %s [-c <clients>] [-p <printers>] [-b <buffer>] [-h(elp)]\n", programme);
	fprintf(stderr, "Version %d.%d.\n\n", versionMajor, versionMinor);
	fprintf(stderr, "%s %s Neil Edelman\n", programme, year);
	fprintf(stderr, "This program comes with ABSOLUTELY NO WARRANTY.\n\n");
}
