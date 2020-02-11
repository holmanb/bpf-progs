// SPDX-License-Identifier: GPL-2.0
/* Analyze latency of the host networking stack using PTP timestamps.
 * Works for virtual machines using tap devices for networking.
 *
 * Copyright (c) 2019-2020 David Ahern <dsahern@gmail.com>
 */
#include <linux/bpf.h>
#include <linux/perf_event.h>
#include <linux/rbtree.h>
#include <sys/time.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <libgen.h>

#include <bpf/bpf.h>

#include "pktlatency.h"
#include "libbpf_helpers.h"
#include "perf_events.h"
#include "print_pkt.h"
#include "timestamps.h"

#include "perf_events.c"

struct task {
	struct rb_node rb_node;

	u32 pid;
	char comm[16];

	/* previous histogram buckets */
	u64 buckets[8];
};

static bool done;
static u64 display_rate = 10 * NSEC_PER_SEC;
static u64 latency_gen_sample = 200 * NSEC_PER_USEC;

static struct rb_root all_tasks;

static void remove_task(struct task *task)
{
	rb_erase(&task->rb_node, &all_tasks);
	free(task);
}

static int insert_task(struct rb_root *root, struct task *new_task)
{
	struct rb_node **node = &root->rb_node;
	struct rb_node *parent = NULL;

	while (*node != NULL) {
		struct task *task;

		parent = *node;
		task = container_of(parent, struct task, rb_node);
		if (task->pid > new_task->pid)
			node = &(*node)->rb_left;
		else if (task->pid < new_task->pid)
			node = &(*node)->rb_right;
		else
			return -EEXIST;
	}

	rb_link_node(&new_task->rb_node, parent, node);
	rb_insert_color(&new_task->rb_node, root);

	return 0;
}

static void get_task_name(struct task *task)
{
	const char *name = "";
	char fname[PATH_MAX];
	char buf[8192];
	ssize_t len;
	int fd = -1;
	char *nl;

	if (snprintf(fname, sizeof(fname),
		     "/proc/%d/status", task->pid) >= sizeof(fname)) {
		fprintf(stderr, "fname buffer too small for pid %d\n",
			task->pid);
		goto out;
	}

	fd = open(fname, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "failed to open %s: %s\n",
			fname, strerror(errno));
		goto out;
	}

	len = read(fd, buf, sizeof(buf)-1);
	if (len < 0) {
		fprintf(stderr, "failed to read status file for pid %d\n",
			task->pid);
		goto out;
	}
	buf[len] = '\0';

	name = strstr(buf, "Name:");
	if (!name) {
		fprintf(stderr, "failed to find name for pid %d\n",
			task->pid);
		name = "";
		goto out;
	}
	name += 5; /* strlen("Name:"); */
	while ((*name != '\0') && isspace(*name))
		++name;

	nl = strchr(name, '\n');
	if (nl) {
		char *p = (char *) name;
		int len;

		*nl = '\0';
		len = strlen(name);
		if (len > 20)
			p[19] = '\0';
	} else
		goto out;

	strncpy(task->comm, name, sizeof(task->comm)-1);

out:
	if (fd >= 0)
		close(fd);
}

static struct task *get_task(u32 pid, bool create)
{
	struct rb_node **p = &all_tasks.rb_node;
	struct rb_node *parent = NULL;
	struct task *task;

	while (*p != NULL) {
		parent = *p;

		task = container_of(parent, struct task, rb_node);
		if (task->pid > pid)
			p = &(*p)->rb_left;
		else if (task->pid < pid)
			p = &(*p)->rb_right;
		else
			return task;
	}

	if (!create)
		return NULL;

	task = calloc(1, sizeof(*task));
	if (task) {
		task->pid = pid;
		get_task_name(task);

		insert_task(&all_tasks, task);
	}

	return task;
}

static void print_header(void)
{
	printf("\n%15s  %3s  %3s  %5s  %s\n",
		"TIME    ", "CPU", "DEV", "LEN", "LATENCY");
	printf("%15s  %3s  %3s  %5s  %s\n",
		"", "", "", "", "(msec)");
}

static u64 ptp_mono_ref, ptp_ref;

static int update_ptp_reftime(void)
{
	static clockid_t ptp_clkid = CLOCK_INVALID;

	if (ptp_clkid == CLOCK_INVALID) {
		ptp_clkid = phc_open("/dev/ptp0");
		if (ptp_clkid == CLOCK_INVALID) {
			fprintf(stderr, "Failed to get ptp clock id\n");
			return 1;
		}
	}

	/* PTP clock takes MUCH longer to read (many usec).
	 * MONOTONIC is fast (< 1 usec)
	 */
	ptp_ref = get_time_ns(ptp_clkid);
	ptp_mono_ref = get_time_ns(CLOCK_MONOTONIC);
	if (!ptp_ref || !ptp_mono_ref) {
		fprintf(stderr, "Failed to update PTP reference time\n");
		return 1;
	}

	return 0;
}

static void hwtimestamp(__u64 hwtime, __u64 stime)
{
	u64 dt = 0;

	if (ptp_ref) {
		u64 hw_mono;

		/* logic:
		 * ----|------|--------|----
		 *  hwtime  stime     ptp_ref
		 * <hw_mono>        ptp_mono_ref
		 *
		 * hwtime is the PTP time value from the skb.
		 * stime is the monotonic time from the bpf sample
		 *
		 * ptp_ref and ptp_mono_ref are the reference times
		 * used to correlate PTP time to MONOTONIC times.
		 *
		 * Idea here is to compute the nanosecond delta in ptp
		 * times and then transfer that to the monotonic time
		 * to estimate hw_mono to some accuracy (< 10 usec)
		 */
		if (hwtime > ptp_ref)
			hw_mono = ptp_mono_ref + (hwtime - ptp_ref);
		else
			hw_mono = ptp_mono_ref - (ptp_ref - hwtime);
		dt = stime - hw_mono;
	}

	if (0) {
		struct timeval tv_hwtime = ns_to_timeval(hwtime);
		struct timeval tv_stime = ns_to_timeval(stime);

		printf(" hw %ld.%06ld  ref %ld.%06ld dt %ld",
		       tv_hwtime.tv_sec, tv_hwtime.tv_usec,
		       tv_stime.tv_sec, tv_stime.tv_usec,
		       dt);
	} else {
		u64 msecs, usecs;

		msecs = dt / NSEC_PER_MSEC;
		dt -= msecs * NSEC_PER_MSEC;
		usecs = dt / NSEC_PER_USEC;
		printf(" %2lu.%03lu", msecs, usecs);
	}
}

static void process_event(struct data *data)
{
	static unsigned char num_events;
	struct task *task;
	char buf[64];

	switch (data->event_type) {
	case EVENT_SAMPLE:
		if (!data->tstamp)
			return;

		/* unsigned char means print header every 255 events */
		if (!num_events)
			print_header();
		num_events++;

		printf("%15s  %3u  %5u  %3u  %5u ",
		       timestamp(buf, sizeof(buf), data->time), data->cpu,
		       data->pid, data->ifindex, data->pkt_len);
		hwtimestamp(data->tstamp, data->time);
		printf("\n");

		if (data->protocol) {
			u32 len = data->pkt_len;

			if (len > sizeof(data->pkt_data))
				len = sizeof(data->pkt_data);

			print_pkt(data->protocol, data->pkt_data, len);
		}
		printf("\n");
		break;
	case EVENT_EXIT:
		task = get_task(data->pid, false);
		if (task)
			remove_task(task);
		break;
	}
}

static void dump_buckets(struct task *task, u64 *buckets)
{
	u64 diff[8], sum = 0;
	int i;

	/* get difference between samples and save
	 * new sample as old
	 */
	for (i = 0; i < 8; ++i) {
		diff[i] = buckets[i] - task->buckets[i];
		sum += diff[i];

		task->buckets[i] = buckets[i];
	}

	printf("\n%s[%u]", task->comm, task->pid);
	printf(":\n");

	if (sum == 0) {
		printf("No packets\n");
		return;
	}

	printf("      0 -   15:  %lu\n", diff[0]);
	printf("     16 -   50:  %lu\n", diff[1]);
	printf("     51 -  100:  %lu\n", diff[2]);
	printf("    101 -  200:  %lu\n", diff[3]);
	printf("    201 -  500:  %lu\n", diff[4]);
	printf("    501 - 1000:  %lu\n", diff[5]);
	printf("   1001 -   up:  %lu\n", diff[6]);
	printf("   missing timestamp:  %lu\n", diff[7]);
}

static int hist_map_fd;

static void pktlat_dump_hist(void)
{
	struct pktlat_hist_key key = {}, next_key = {};
	struct pktlat_hist_val val;
	bool last_key = false;
	struct task *task;
	char buf[64];

	printf("%s:\n", timestamp(buf, sizeof(buf), 0));

	while (!last_key) {
		if (bpf_map_get_next_key(hist_map_fd, &key, &next_key))
			last_key = true;

		//printf("key %u next_key %u\n", key.pid, next_key.pid);
		if (bpf_map_lookup_elem(hist_map_fd, &key, &val))
			goto next;

		task = get_task(key.pid, true);
		if (!task) {
			fprintf(stderr,
				"Failed to create task entry for pid %u\n",
				key.pid);
			goto next;
		}

		dump_buckets(task, val.buckets);
		printf("\n");
next:
		key = next_key;
	}
}

static int ctl_map_fd;
static int gen_samples;

static int pktlat_setup_ctl_map(void)
{
	struct pktlat_ctl ctl;
	u32 idx = 0;
	int err;

	if (update_ptp_reftime())
		return 1;

	ctl.ptp_ref = ptp_ref;
	ctl.mono_ref = ptp_mono_ref;
	ctl.ifindex_min = 4;
	ctl.gen_samples = gen_samples;
	ctl.latency_gen_sample = latency_gen_sample;

	err = bpf_map_update_elem(ctl_map_fd, &idx, &ctl, BPF_ANY);
	if (err) {
		fprintf(stderr, "Failed to set control entry\n");
		return 1;
	}

	return 0;
}

static int pktlat_process_events(void)
{
	u64 t_mono = get_time_ns(CLOCK_MONOTONIC);

	process_events();

	if (t_mono > ptp_mono_ref + display_rate) {
		pktlat_setup_ctl_map();
		pktlat_dump_hist();
	}

	return done;
}

static void sig_handler(int signo)
{
	if (signo == SIGUSR1) {
		gen_samples = 1 - gen_samples;
		return;
	}

	printf("Terminating by signal %d\n", signo);

	disable_hw_tstamp("eth0");
	disable_hw_tstamp("eth1");

	done = 1;
}

static void print_usage(char *prog)
{
	printf(
	"usage: %s OPTS\n\n"
	"	-f bpf-file    bpf filename to load\n"
	"	-l time        latency at which to generate samples (usec, default: 200)\n"
	"	-t rate        time rate (seconds) to dump stats\n"
	"	-s             show samples\n"
	, basename(prog));
}

int main(int argc, char **argv)
{
	struct bpf_prog_load_attr prog_load_attr = { };
	char *objfile = "pktlatency.o";
	const char *tps[] = {
		"skb/skb_copy_datagram_iovec",
		"sched/sched_process_exit",
		NULL,
	};
	bool filename_set = false;
	struct bpf_object *obj;
	struct bpf_map *map;
	int nevents = 1000;
	int rc, r;

	while ((rc = getopt(argc, argv, "f:t:l:s")) != -1)
	{
		switch(rc) {
		case 'f':
			objfile = optarg;
			filename_set = true;
			break;
		case 't':
			r = atoi(optarg);
			if (!r) {
				fprintf(stderr, "Invalid display rate\n");
				return 1;
			}
			display_rate = r * NSEC_PER_SEC;
			break;
		case 'l':
			latency_gen_sample = atoi(optarg) * NSEC_PER_USEC;
			break;
		case 's':
			gen_samples = true;
			break;
		default:
			print_usage(argv[0]);
			return 1;
		}
	}

	if (set_reftime())
		return 1;

	if (load_obj_file(&prog_load_attr, &obj, objfile, filename_set))
		return 1;

	rc = do_tracepoint(obj, tps);
	if (rc)
		return rc;

	/*
	 * get fd to the control map and histogram map
	 */
	map = bpf_object__find_map_by_name(obj, "pktlat_ctl_map");
	if (!map) {
		printf("Failed to get control map in obj file\n");
		return 1;
	}
	ctl_map_fd = bpf_map__fd(map);
	if (pktlat_setup_ctl_map())
		return 1;

	map = bpf_object__find_map_by_name(obj, "pktlat_map");
	if (!map) {
		printf("Failed to get histogram map in obj file\n");
		return 1;
	}
	hist_map_fd = bpf_map__fd(map);

	if (enable_hw_tstamp("eth0") ||
	    enable_hw_tstamp("eth1"))
		return 1;

	if (signal(SIGINT, sig_handler) ||
	    signal(SIGHUP, sig_handler) ||
	    signal(SIGUSR1, sig_handler) ||
	    signal(SIGTERM, sig_handler)) {
		perror("signal");
		return 1;
	}

	setlinebuf(stdout);
	setlinebuf(stderr);

	if (configure_perf_event_channel(obj, nevents))
		return 1;

	/* main event loop */
	return perf_event_loop(NULL, NULL, pktlat_process_events);
}