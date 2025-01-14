/*	$OpenBSD: usbhidaction.c,v 1.25 2022/08/02 09:07:34 mestre Exp $ */
/*      $NetBSD: usbhidaction.c,v 1.7 2002/01/18 14:38:59 augustss Exp $ */

/*
 * Copyright (c) 2000, 2002 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Lennart Augustsson <lennart@augustsson.net>.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <err.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <dev/usb/usb.h>
#include <dev/usb/usbhid.h>
#include <usbhid.h>
#include <syslog.h>
#include <signal.h>
#include <paths.h>

int verbose = 0;
int isdemon = 0;

volatile sig_atomic_t reparse = 0;

struct command {
	struct command *next;
	int line;

	struct hid_item item;
	int value;
	char anyvalue;
	char *name;
	char *action;
};
struct command *commands;

#define SIZE 4000

void usage(void);
struct command *parse_conf(const char *, report_desc_t, int, int);
void docmd(struct command *, int, const char *, int, char **);
void freecommands(struct command *);

/* ARGSUSED */
static void
sighup(int signo)
{
	reparse = 1;
}

int
main(int argc, char **argv)
{
	const char *conf = NULL;
	const char *dev = NULL;
	int fd, ch, sz, n, val, i;
	int demon, ignore;
	report_desc_t repd;
	char buf[100];
	char devnamebuf[PATH_MAX];
	struct command *cmd;
	int reportid;

	demon = 1;
	ignore = 0;
	while ((ch = getopt(argc, argv, "c:df:iv")) != -1) {
		switch(ch) {
		case 'c':
			conf = optarg;
			break;
		case 'd':
			demon ^= 1;
			break;
		case 'i':
			ignore++;
			break;
		case 'f':
			dev = optarg;
			break;
		case 'v':
			demon = 0;
			verbose++;
			break;
		case '?':
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (conf == NULL || dev == NULL)
		usage();

	if (hid_start(NULL) == -1)
		errx(1, "hid_init");

	if (dev[0] != '/') {
		snprintf(devnamebuf, sizeof(devnamebuf), "/dev/%s%s",
		    isdigit((unsigned char)dev[0]) ? "uhid" : "", dev);
		dev = devnamebuf;
	}

	if (demon && conf[0] != '/')
		errx(1, "config file must have an absolute path, %s", conf);

	fd = open(dev, O_RDWR | O_CLOEXEC);
	if (fd == -1)
		err(1, "%s", dev);

	if (ioctl(fd, USB_GET_REPORT_ID, &reportid) == -1)
		reportid = -1;
	repd = hid_get_report_desc(fd);
	if (repd == NULL)
		err(1, "hid_get_report_desc() failed");

	commands = parse_conf(conf, repd, reportid, ignore);

	sz = hid_report_size(repd, hid_input, reportid);

	if (verbose)
		printf("report size %d\n", sz);
	if (sz > sizeof buf)
		errx(1, "report too large");

	(void)signal(SIGHUP, sighup);

	/* we do not care about the children, so ignore them */
	(void)signal(SIGCHLD, SIG_IGN);

	if (demon) {
		if (daemon(0, 0) == -1)
			err(1, "daemon()");
		isdemon = 1;
	}

	if (unveil(conf, "r") == -1)
		err(1, "unveil %s", conf);
	if (unveil(_PATH_BSHELL, "x") == -1)
		err(1, "unveil %s", _PATH_BSHELL);
	if (unveil(NULL, NULL) == -1)
		err(1, "unveil");

	for(;;) {
		n = read(fd, buf, sz);
		if (verbose > 2) {
			printf("read %d bytes:", n);
			for (i = 0; i < n; i++)
				printf(" %02x", buf[i]);
			printf("\n");
		}
		if (n == -1) {
			if (verbose)
				err(1, "read");
			else
				exit(1);
		}
		if (n != sz) {
			err(2, "read size");
		}
		for (cmd = commands; cmd; cmd = cmd->next) {
			val = hid_get_data(buf, &cmd->item);
			if (cmd->value == val || cmd->anyvalue)
				docmd(cmd, val, dev, argc, argv);
		}
		if (reparse) {
			struct command *cmds =
			    parse_conf(conf, repd, reportid, ignore);
			if (cmds) {
				freecommands(commands);
				commands = cmds;
			}
			reparse = 0;
		}
	}

	exit(0);
}

void
usage(void)
{
	extern char *__progname;

	fprintf(stderr, "usage: %s [-div] -c config-file -f device arg ...\n",
	    __progname);
	exit(1);
}

static int
peek(FILE *f)
{
	int c;

	c = getc(f);
	if (c != EOF)
		ungetc(c, f);
	return c;
}

struct command *
parse_conf(const char *conf, report_desc_t repd, int reportid, int ignore)
{
	FILE *f;
	char *p;
	int line;
	char buf[SIZE], name[SIZE], value[SIZE], action[SIZE];
	char usage[SIZE], coll[SIZE];
	struct command *cmd, *cmds;
	struct hid_data *d;
	struct hid_item h;
	int u, lo, hi, range;

	f = fopen(conf, "r");
	if (f == NULL)
		err(1, "%s", conf);

	cmds = NULL;
	for (line = 1; ; line++) {
		if (fgets(buf, sizeof buf, f) == NULL)
			break;
		if (buf[0] == '#' || buf[0] == '\n')
			continue;
		p = strchr(buf, '\n');
		while (p && isspace(peek(f))) {
			if (fgets(p, sizeof buf - strlen(buf), f) == NULL)
				break;
			p = strchr(buf, '\n');
		}
		if (p)
			*p = 0;
		if (sscanf(buf, "%s %s %[^\n]", name, value, action) != 3) {
			if (isdemon) {
				syslog(LOG_WARNING, "config file `%s', line %d"
				    ", syntax error: %s", conf, line, buf);
				freecommands(cmds);
				fclose(f);
				return (NULL);
			} else {
				errx(1, "config file `%s', line %d"
				    ", syntax error: %s", conf, line, buf);
			}
		}

		cmd = malloc(sizeof *cmd);
		if (cmd == NULL)
			err(1, "malloc failed");
		cmd->next = cmds;
		cmds = cmd;
		cmd->line = line;

		if (strcmp(value, "*") == 0) {
			cmd->anyvalue = 1;
		} else {
			cmd->anyvalue = 0;
			if (sscanf(value, "%d", &cmd->value) != 1) {
				if (isdemon) {
					syslog(LOG_WARNING,
					    "config file `%s', line %d, "
					    "bad value: %s",
					    conf, line, value);
					freecommands(cmds);
					fclose(f);
					return (NULL);
				} else {
					errx(1, "config file `%s', line %d, "
					    "bad value: %s",
					    conf, line, value);
				}
			}
		}

		coll[0] = 0;
		d = hid_start_parse(repd, 1 << hid_input, reportid);
		if (d == NULL)
			err(1, "hid_start_parse failed");
		while (hid_get_item(d, &h)) {
			if (verbose > 2)
				printf("kind=%d usage=%x\n", h.kind, h.usage);
			if (h.flags & HIO_CONST)
				continue;
			switch (h.kind) {
			case hid_input:
				if (h.usage_minimum != 0 ||
				    h.usage_maximum != 0) {
					lo = h.usage_minimum;
					hi = h.usage_maximum;
					range = 1;
				} else {
					lo = h.usage;
					hi = h.usage;
					range = 0;
				}
				for (u = lo; u <= hi; u++) {
					snprintf(usage, sizeof usage,  "%s:%s",
						 hid_usage_page(HID_PAGE(u)),
						 hid_usage_in_page(u));
					if (verbose > 2)
						printf("usage %s\n", usage);
					if (!strcasecmp(usage, name))
						goto foundhid;
					if (coll[0]) {
						snprintf(usage, sizeof usage,
						  "%s.%s:%s", coll+1,
						  hid_usage_page(HID_PAGE(u)),
						  hid_usage_in_page(u));
						if (verbose > 2)
							printf("usage %s\n",
							    usage);
						if (!strcasecmp(usage, name))
							goto foundhid;
					}
				}
				break;
			case hid_collection:
				snprintf(coll + strlen(coll),
				    sizeof coll - strlen(coll),  ".%s:%s",
				    hid_usage_page(HID_PAGE(h.usage)),
				    hid_usage_in_page(h.usage));
				break;
			case hid_endcollection:
				if (coll[0])
					*strrchr(coll, '.') = 0;
				break;
			default:
				break;
			}
		}
		hid_end_parse(d);
		if (ignore) {
			if (verbose)
				warnx("ignore item '%s'", name);
			/* pop and free this ignored item */
			cmds = cmd->next;
			free(cmd);
			continue;
		}
		if (isdemon) {
			syslog(LOG_WARNING, "config file `%s', line %d, HID "
			    "item not found: `%s'", conf, line, name);
			freecommands(cmds);
			fclose(f);
			return (NULL);
		} else {
			errx(1, "config file `%s', line %d, HID item "
			    "not found: `%s'", conf, line, name);
		}

	foundhid:
		hid_end_parse(d);
		cmd->item = h;
		cmd->name = strdup(name);
		cmd->action = strdup(action);
		if (range) {
			if (cmd->value == 1)
				cmd->value = u - lo;
			else
				cmd->value = -1;
		}

		if (verbose)
			printf("PARSE:%d %s, %d, '%s'\n", cmd->line, name,
			    cmd->value, cmd->action);
	}
	fclose(f);
	return (cmds);
}

void
docmd(struct command *cmd, int value, const char *hid, int argc, char **argv)
{
	char cmdbuf[SIZE], *p, *q;
	size_t len;
	int n, r;
	pid_t pid;

	if (cmd->action == NULL) {
		if (verbose)
			printf("no action for device %s value %d\n",
			    hid, value);
		return;
	}
	for (p = cmd->action, q = cmdbuf; *p && q < &cmdbuf[SIZE-1]; ) {
		if (*p == '$') {
			p++;
			len = &cmdbuf[SIZE-1] - q;
			if (isdigit((unsigned char)*p)) {
				n = strtol(p, &p, 10) - 1;
				if (n >= 0 && n < argc) {
					strncpy(q, argv[n], len);
					q += strlen(q);
				}
			} else if (*p == 'V') {
				p++;
				snprintf(q, len, "%d", value);
				q += strlen(q);
			} else if (*p == 'N') {
				p++;
				strncpy(q, cmd->name, len);
				q += strlen(q);
			} else if (*p == 'H') {
				p++;
				strncpy(q, hid, len);
				q += strlen(q);
			} else if (*p) {
				*q++ = *p++;
			}
		} else {
			*q++ = *p++;
		}
	}
	*q = 0;

	pid = fork();
	if (pid == -1)
		warn("fork failed");
	else if (pid == 0) {
		setpgid(0, 0);
		if (verbose)
			printf("executing '%s'\n", cmdbuf);
		r = execl(_PATH_BSHELL, "sh", "-c", cmdbuf, (char *)NULL);
		err(1, "execl");
	}
}

void
freecommands(struct command *cmd)
{
	struct command *next;

	while (cmd) {
		next = cmd->next;
		free(cmd);
		cmd = next;
	}
}
