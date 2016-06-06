/*
 * jack_cat - read and write JACK data
 *
 * The purpose of this program is to record and playback data from the JACK
 * audio connection kit.  It uses a single format data file: the 32 bit floats 
 * produced by jackd.
 *
 * Copyright 2016 Glen Overby
 * 
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License Version 2, as published
 * by the Free Software Foundation
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see 
 * <http://www.gnu.org/licenses/old-licenses/gpl-2.0.html/>.
 * 
 *
 * jack_cat
 *	-c filename	capture to file
 *	-p filename	play back from file
 *	-n count	number of ports (do not auto connect)
 *	-N name		client name to use with jack (default: jack_cat)
 *	-b size		block size to use
 *	-B size		ring buffer size
 *	-m size		maximum file size (for -C)
 *	-t time		run for time seconds
 *
 *	port1 .. portn	names of ports to connect to
 *
 * The file starts with:
 *	JACK#\0
 * where # is replaced by the count of streams written to the file.
 * Stream data is interleaved.  That seems like the most universal way to
 * represent the data so that it can be played back when jackd is running with
 * a different jack period size.
 *
 * Program Outline:
 *	For capture, 
 *		jack_capture_callback reads data from JACK and write it to
 *		a jack ringbuffer.
 *
 *		disk_write writes data in the buffer to disk.
 *	For playback:
 */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <jack/jack.h>
#include <jack/types.h>
#include <jack/session.h>
#include <jack/ringbuffer.h>
#include <pthread.h>

#define MAX_PORTS	32	/* maximum number of ports (artificial limit) */
#define MAX_NAME	32	/* character string sizes */

#define FILE_HEADER_LEN	6	/* length of file header: "JACK00" */

#define	CFG_CAPTURE	1
#define CFG_PLAYBACK	2
struct config {
	char *filename;		/* filename for read or write */
	int io;			/* input(1) or output(2) */
	int ports;		/* count of ports */
	char **portnames;	/* port names */
	char *jackname;		/* name of client for jack */
	char *portbase;		/* base name of jack ports */
	char **connect;		/* ports to connect to */
	int blocksize;		/* I/O block size */
	int rbsize;		/* jack ringbuffer size */
	int runtime;		/* how long to run for */
};

struct status {
	int	jack_calls;
	int	disk_io;
	long	disk_bytes;
	int	overflows;	/* times ringbuffer was full (capture) */
	int	underruns;	/* times ringbuffer was empty (playback */
	int	stop;		/* terminate program */
	int	eof;		/* set when disk read thread sees end of file */
};

/*
 * Data for jack callback functions.
 * They need all of the jack_port_t structures returned from jack_port_register
 * and a pointer for a buffer from each of the ports.  The buffer pointers are
 * of the audio sample type.
 * The pointer to config is for the count of ports.
 */
struct callbackdata {
	struct config *cfg;
	jack_default_audio_sample_t *buf[MAX_PORTS];	/* Buffers for each port */
	jack_port_t *ports[MAX_PORTS];	/* Jack ports */
	int ready;		/* initialization complete */
};

struct status status;		/* Global status */
jack_ringbuffer_t *buffer;	/* Jack-to-disk ring buffer */
pthread_t disk_thread;		/* pthread for disk reader/writer */
pthread_cond_t disk_cond;	/* for synchronizing disk and jack */
pthread_mutex_t disk_mutex;	/* mutex protecting disk_cond */
jack_client_t *jclient;		/* Jack client */

void set_signal_handler();
void start_io(struct config *c);
void stop_io(struct config *c);
void usage();
void help();
void cleanup_jack();

main(int argc, char **argv)
{
	struct config config;

	memset((void*)&status, 0, sizeof(struct status));

	memset((void*)&config, 0, sizeof(struct config));
	config.rbsize = 1048576;
	config.blocksize = 1048576;

	if (parse_args(argc, argv, &config) != 0)
		exit(1);

	set_signal_handler();

	buffer = jack_ringbuffer_create(config.rbsize);
	/* touch all allocated space to allocate pages */
	memset(buffer->buf, 0, buffer->size);

	pthread_cond_init(&disk_cond, NULL);
	pthread_mutex_init(&disk_mutex, NULL);

	start_io(&config);

	setup_jack(&config);

	if (config.runtime != 0) {
		alarm(config.runtime);
	}

	while(status.stop == 0) {
		sleep(1);
		printf("jack calls  %d\n", status.jack_calls);
		printf("disk i/o calls %d bytes %ld\n", status.disk_io,
			status.disk_bytes);
		printf("overflows %d underruns %d\n", status.overflows,
			status.underruns);
	}

	printf("main() stopping\n");
	printf("jack calls  %d\n", status.jack_calls);
	printf("disk i/o calls %d bytes %ld\n", status.disk_io,
		status.disk_bytes);
	printf("overflows %d underruns %d\n", status.overflows,
		status.underruns);

	cleanup_jack();

	// pthread join disk thread
	stop_io(&config);
}

int
units(char u)
{
	int	multiplier = 1;

	switch(u) {
	case 'k':	multiplier = 1024;		break;
	case 'm':	multiplier = 1048576;		break;
	case 'g':	multiplier = 1073741824;	break;
	default:	multiplier = -1;		break;
	}
	return (multiplier);
}

int
parse_args(int argc, char **argv, struct config *c)
{
	int opt;		/* option returned from getopt */
	int r;			/* local return from function calls */
	int m;			/* multiplier */
	char u;			/* units portion of numbers */

	while ((opt = getopt(argc, argv, "+b:B:c:C:hj:n:N:p:P:t:")) != -1) {
		switch(opt) {
		case 'b':
			r = sscanf(optarg, "%i%c", &c->blocksize, &u);
			if (r > 1) {
				if ((m = units(u)) == -1) {
					fprintf(stderr, "-b units was invalid\n");
					break;
				}
				c->blocksize *= m;
			}
			break;
		case 'B':
			r = sscanf(optarg, "%i%c", &c->rbsize, &u);
			if (r > 1) {
				if ((m = units(u)) == -1) {
					fprintf(stderr, "-B units was invalid\n");
					break;
				}
				c->rbsize *= m;
			}
			break;
		case 'c':
			c->filename = strdup(optarg);
			c->io = CFG_CAPTURE;
			break;
		case 'j':
			c->jackname = strdup(optarg);
			break;
		case 'n':
			r = sscanf(optarg, "%i", &c->ports); /* no units */
			break;
		case 'N':
			c->portbase = strdup(optarg);
			break;
		case 'p':
			c->filename = strdup(optarg);
			c->io = CFG_PLAYBACK;
			break;
		case 't':
			r = sscanf(optarg, "%i", &c->runtime); /* no units */
			break;
		case 'h':
			help();
			return(1);
		case '?':	/* unknown option */
			usage();
			return(1);
		}
	}
	if (argc > optind) {
		printf("%d port names\n", argc-optind);
		c->connect = &argv[optind];
		c->ports = argc-optind;
	} else if (c->ports == 0) {
		fprintf(stderr, "Either a count of ports (-n) or a list of ports to connect to is required\n");
		return(1);
	}

	/*
	 * Required argument checks
	 */
	if (c->io == 0) {
		fprintf(stderr, "-c or -p is required\n");
		return(1);
	}
	if (c->filename == NULL) {
		fprintf(stderr, "-[cp] filename is required\n");
		return(1);
	}
	return(0);
}

/* JACK Callback for capture
 *
 * Callback returns 0 for normal operation.  Non-zero shuts it down as a jack
 * client.
 */
int
jack_capture_callback(jack_nframes_t nframes, void *arg)
{
	int f, i;
	size_t space;			/* space in ring buffer */
	struct callbackdata *cbd;	/* data for use here */
	int nports;			/* number of ports */

	status.jack_calls++;

	/* arg is callback data */
	cbd = (struct callbackdata *)arg;

	nports = cbd->cfg->ports;

	/* Is there enough space in the ring buffer for all data in all the
	 * ports?  */
	space = jack_ringbuffer_write_space(buffer);
	if (space < nframes * sizeof(jack_default_audio_sample_t) * nports) {
		status.overflows++;
		// signal disk thread?
		return(0);
	}
	/* get buffers for each port */
	for (i=0; i < nports; i++) {
		cbd->buf[i] = jack_port_get_buffer(cbd->ports[i], nframes);
	}

	/* Write data to ring buffer one sample at a time, interleaving ports */
	for (f=0; f < nframes; f++) {
		for (i=0; i < nports; i++) {
			jack_ringbuffer_write(buffer, (char *)cbd->buf[i]++,
				sizeof(jack_default_audio_sample_t));
		}
	}

	/* Signal disk thread that data is available */
	if(pthread_mutex_trylock(&disk_mutex) == 0) {
		pthread_cond_signal(&disk_cond);
		pthread_mutex_unlock(&disk_mutex);
	}
	return(0);
}

int
jack_playback_callback(jack_nframes_t nframes, void *arg)
{
	int f, i;
	size_t space;			/* space in ring buffer */
	struct callbackdata *cbd;	/* data for use here */
	int nports;			/* number of ports */

	status.jack_calls++;
	//printf("jpc: %d %d\n", nframes, (int)(nframes*sizeof(jack_default_audio_sample_t)));

	/* arg is callback data */
	cbd = (struct callbackdata *)arg;

	nports = cbd->cfg->ports;

	/* get buffers for each port */
	for (i=0; i < nports; i++) {
		cbd->buf[i] = jack_port_get_buffer(cbd->ports[i], nframes);
	}

	/* Is there enough data in the ring buffer for all data in all the
	 * ports?
	 */
	space = jack_ringbuffer_read_space(buffer);
	if (space < nframes * sizeof(jack_default_audio_sample_t) * nports) {
		status.underruns++;
		for (i=0; i < nports; i++) {
			memset((char *)cbd->buf[i], 0,
				sizeof(jack_default_audio_sample_t)*nframes);
		}
		if (status.eof) {
			status.stop = 1;
			jack_deactivate(jclient);
		}
		return(0);
	}

	/* Read data from the ring buffer one sample at a time.  Ports are interleaved */
	for (f=0; f < nframes; f++) {
		for (i=0; i < nports; i++) {
			jack_ringbuffer_read(buffer, (char *)cbd->buf[i]++,
				sizeof(jack_default_audio_sample_t));
		}
	}

	if(pthread_mutex_trylock(&disk_mutex) == 0) {
		pthread_cond_signal(&disk_cond);
		pthread_mutex_unlock(&disk_mutex);
	}
	return(0);
}

int
setup_jack(struct config *c)
{
	char *clientname = "jack_cat";
	char port_name[MAX_NAME];
	jack_status_t jackstatus;
	unsigned long port_flags;
	jack_port_t *jp;
	int i;
	int rc;
	int error;
	struct callbackdata *cbd;

	error = 0;
	cbd = (struct callbackdata *)malloc(sizeof(struct callbackdata ));
	cbd->ready = 0;
	cbd->cfg = c;

	if (c->jackname != NULL) {
		clientname = c->jackname;
	}

	jclient = jack_client_open(clientname, 0, &jackstatus);
	if (jclient == NULL) {
		fprintf(stderr, "Error from jack_client_open\n");
		return;
	}

	switch (c->io) {
	case CFG_CAPTURE:
		jack_set_process_callback(jclient, jack_capture_callback, cbd);
		break;
	case CFG_PLAYBACK: 
		jack_set_process_callback(jclient, jack_playback_callback, cbd);
		break;
	}

	jack_activate(jclient);
	
	switch (c->io) {
	case CFG_CAPTURE:	port_flags = JackPortIsInput;	break;
	case CFG_PLAYBACK: 	port_flags = JackPortIsOutput;	break;
	}

	for (i=0; i < c->ports; i++) {
		sprintf(port_name, "%d", i);
		jp = jack_port_register(jclient, port_name,
			JACK_DEFAULT_AUDIO_TYPE, port_flags, 0);
		if (jp == NULL) {
			fprintf(stderr, "Error registering port %s\n",
				port_name);
			error = 1;
			break;
		}
		cbd->ports[i] = jp;

		if (c->connect != NULL) {
			sprintf(port_name, "%s:%d", clientname, i);
			if (c->io == CFG_CAPTURE) {
				printf("connect %s to %s\n", c->connect[i], port_name);
				rc = jack_connect(jclient, c->connect[i], port_name);
			} else {
				printf("connect %s to %s\n", port_name, c->connect[i]);
				rc = jack_connect(jclient, port_name, c->connect[i]);
			}
			if (rc != 0) {
				fprintf(stderr, "Error connecting %s %s = %d\n",
					c->connect[i], port_name, rc);
				break;
				error = 1;
			}
		}
	}
	if (!error)
		cbd->ready = 1;
}

/*
 * The jack API isn't clear on which is prefereable: jack_client_close or
 * jack_deactivate.  When using only jack_deactivate, the server was reporting
 * broken pipe and write errors.  It doesn't when using jack_client_close.
 */
void
cleanup_jack()
{
	jack_client_close(jclient);
}

/*
 * Thread to write data from buffer to disk.
 *
 * When there is no more data to write, it sleeps on disk_cond, expecting a
 * wakeup from the jack callback handler.
 *
 * I/O size is limited to avoid having one long (slow) write block emptying
 * the buffer.
 */
void
disk_write(void *arg)
{
	struct config *c;
	int fd, n;
	size_t available, l, w;
	jack_ringbuffer_data_t vec[3];
	char label[FILE_HEADER_LEN];

	c = (struct config*)arg;
	printf("disk_write %s\n", c->filename);

	if ((fd = open(c->filename, O_CREAT|O_APPEND|O_RDWR, 0666)) == -1) {
		fprintf(stderr, "Cannot create file %s\n", c->filename);
		perror("create");
		status.stop = 1;
		return;
	}

	/* Write a header */
	n = sprintf(label, "JACK%1d", c->ports);
	write(fd, label, n+1);

	pthread_mutex_lock(&disk_mutex);
	while (status.stop == 0) {
		available = jack_ringbuffer_read_space(buffer);
		if (available > 0) {
			/* This writes data directly from the ringbuffer.  */
			jack_ringbuffer_get_read_vector(buffer, vec);
			l = vec[0].len;
			if (l == 0) 
				continue;	/* should not happen */
			if (l > c->blocksize)	/* limit writes to blocksize */
				l = c->blocksize;
			status.disk_io++;
			status.disk_bytes += l;
			w = write(fd, vec[0].buf, l);
			if (w != l) {
				fprintf(stderr, "write(%ld) = %ld %d\n", l, w, errno);
			}
			jack_ringbuffer_read_advance(buffer, l);
		} else {
			pthread_cond_wait(&disk_cond, &disk_mutex);
		}
	}

	pthread_mutex_unlock(&disk_mutex);
	close(fd);
	pthread_exit(NULL);
}

/*
 * Thread to read data from disk into the buffer
 *
 * When there is no more space for data, it sleeps on disk_cond, expecting a
 * wakeup from the jack callback handler.
 */
void
disk_read(void *arg)
{
	struct config *c;
	int fd, n;
	size_t available, l, r;
	jack_ringbuffer_data_t vec[3];
	char label[FILE_HEADER_LEN+1];

	c = (struct config*)arg;

	if ((fd = open(c->filename, O_RDONLY, 0)) == -1) {
		perror(c->filename);
		status.stop = 1;
		return;
	}

	/* Read the header */
	r = read(fd, label, FILE_HEADER_LEN);
	if (r < FILE_HEADER_LEN) {
		fprintf(stderr, "cannot read data from input file: %s\n", c->filename);
		status.stop = 1;
		return;
	}
	printf("disk_read %s %s\n", c->filename, label);

	pthread_mutex_lock(&disk_mutex);
	while (status.stop == 0) {
		available = jack_ringbuffer_write_space(buffer);
		if (available > 0) {
			/* This writes data directly to the ringbuffer.  */
			jack_ringbuffer_get_write_vector(buffer, vec);
			l = vec[0].len;
			if (l == 0) 
				continue;	/* should not happen */
			if (l > c->blocksize)	/* limit writes to blocksize */
				l = c->blocksize;
			//printf("read(%ld)\n", l);
			status.disk_io++;
			status.disk_bytes += l;
			r = read(fd, vec[0].buf, l);
			if (r != l) {
				/* end of file */
				if (r == 0) {
					fprintf(stderr, "read() = EOF\n");
					status.eof = 1;
					break;
				}
			} else {
				jack_ringbuffer_write_advance(buffer, l);
			}
		} else {
			pthread_cond_wait(&disk_cond, &disk_mutex);
		}
	}

	pthread_mutex_unlock(&disk_mutex);
	close(fd);
	pthread_exit(NULL);
}

/*
 * Create threads that read/write disk files, open files.
 */
void
start_io(struct config *c)
{
	void *func;

	switch (c->io) {
	case CFG_CAPTURE:	
		func = &disk_write;
		pthread_create(&disk_thread, NULL, func, c);
		break;
	case CFG_PLAYBACK:
		func = &disk_read;
		pthread_create(&disk_thread, NULL, func, c);
		break;
	default:
		fprintf(stderr, "Unknown i/o state: %d\n", c->io);
		exit(2);
	}
}

/*
 * Wait for I/O threads to end.
 * Assumes that status.stop is set
 */
void
stop_io(struct config *c)
{
	if(pthread_mutex_trylock(&disk_mutex) == 0) {
		pthread_cond_signal(&disk_cond);
		pthread_mutex_unlock(&disk_mutex);
	}
	pthread_cancel(disk_thread);
	pthread_join(disk_thread, NULL);
	printf("i/o stopped\n");
}

void
timeout_handler()
{
	status.stop = 1;
}

void
signal_handler()
{
	status.stop = 1;

	/* signal disk write code to flush the data it has */
	if(pthread_mutex_trylock(&disk_mutex) == 0) {
		pthread_cond_signal(&disk_cond);
		pthread_mutex_unlock(&disk_mutex);
	}

	/* do something stronger on 2nd signal */
	//pthread_cancel(disk_thread);

}

void
set_signal_handler()
{
	struct sigaction  action;

	memset((void*)&action, 0, sizeof(struct sigaction));
	action.sa_handler = signal_handler;
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGHUP, &action, NULL);

	action.sa_handler = timeout_handler;
	sigaction(SIGALRM, &action, NULL);
}

void
usage()
{
	printf("jack_cat -c filename | -p filename port(s)\n");
}

void
help()
{
	printf("jack_cat -c filename | -p filename port(s)\n");
	printf("  -c filename    capture to file\n");
	printf("  -p filename    play back from file\n");
	printf("  -n count       number of ports (do not auto connect)\n");
 	printf("  -N name        client name to use with jack (default: jack_cat)\n");
	printf("  -b size        block size to use\n");
	printf("  -B size        ring buffer size\n");
	printf("  -m size        maximum file size (for -C)\n");
	printf("  -t time        run for time seconds\n");

	printf("  port1 .. portn	names of ports to connect to\n");
}

