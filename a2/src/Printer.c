/** 2014 Neil Edelman

 This is a simulation of a printer.

 @author Neil
 @version 1
 @since 2014 */

#include <stdlib.h> /* malloc free */
#include <stdio.h>  /* fprintf */

#include <pthread.h>
#include <semaphore.h>

#include "Job.h"
#include "Client.h"
#include "Spool.h"
#include "Printer.h"

struct Printer {
	pthread_t thread;
	int is_running;
	int id;
	int ms_per_page; /* not used */
};

extern sem_t *mutex, *empty, *full;

static void *thread(struct Printer *p);

/* private */

static int unique = 1; /* counter to assign id */

/* public */

/** constructor
 @param  ms_per_page speed of the printer
 @return             an object or a null pointer if the object couldn't be created */
struct Printer *Printer(const int ms_per_page) {
	struct Printer *printer;

	if(ms_per_page <= 0) {
		fprintf(stderr, "Printer: invalid parameters.\n");
		return 0;
	}
	if(!(printer = malloc(sizeof(struct Printer)))) {
		perror("Printer constructor");
		Printer_(&printer);
		return 0;
	}
	printer->is_running  = 0;
	printer->id          = unique++;
	printer->ms_per_page = ms_per_page;
	fprintf(stderr, "Printer: new, %d with %fs/page #%p.\n",
			printer->id,
			printer->ms_per_page / 1000.0,
			(void *)printer);

	return printer;
}

/** destructor
 @param printer_ptr a reference to the object that is to be deleted */
void Printer_(struct Printer **printer_ptr) {
	struct Printer *printer;

	if(!printer_ptr || !(printer = *printer_ptr)) return;
	if(printer->is_running) {
		void *value;

		pthread_join(printer->thread, &value);
		printer->is_running = 0;
		fprintf(stderr, "~Printer: %d thread return #%p, erase #%p.\n", printer->id, value, (void *)printer);
	} else {
		fprintf(stderr, "~Client: %d (not running) erase #%p.\n", printer->id, (void *)printer);
	}
	free(printer);
	*printer_ptr = printer = 0;
}

/** @return id */
int PrinterGetId(const struct Printer *p) { return p ? p->id : 0; }

/** print a job (sleep)
 @param printer
 @param job
 @param buffer */
/*void PrinterPrintJob(const struct Printer *printer, struct Job *job, const int buffer) {
	if(!printer || !job) return;

	fprintf(stderr, "Printer %d starts printing %d pages from buffer[%d]\n",
			printer->id,
			JobGetPages(job),
			JobGetBuffer(job));
	sleep(JobGetPages(job));
	Job_(&job);
}*/

/** run the printer
 @return non-zero on success */
int PrinterRun(struct Printer *p) {
	if(!p || p->is_running) return 0;
	if(pthread_create(&p->thread, 0, (void *(*)(void *))&thread, p)) {
		fprintf(stderr, "Printer %d: broken.\n", p->id);
		return 0;
	}
	sem_post(empty);
	p->is_running = -1;
	return -1;
}

/* private */

/** run the printer
 @return the number of pages it has printed */
static void *thread(struct Printer *p) {
	struct Job *job;

	for( ; ; ) {
		printf("Printer %d waiting.\n", p->id);
		sem_wait(full);
		printf("Printer %d go!\n", p->id);
		if(!(job = SpoolPopJob())) {
			fprintf(stderr, "Printer %d: nothing to print; exiting.\n", p->id);
			break;
		} else {
			const char *name = ClientGetName(JobGetClient(job));
			int pp           = JobGetPages(job);
			int buf          = JobGetBuffer(job);
			fprintf(stderr, "Printer %d starts printing %d pages from buffer[%d] (from %s)\n",
					p->id, pp, buf, name);
			/*sleep(pp); fixme */
			JobPrintPages(job, pp);
			Job_(&job);
		}
		sem_post(empty);
	}
	sem_post(full);

	return 0; /* fixme */
}
