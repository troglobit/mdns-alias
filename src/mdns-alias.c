/* SPDX-License-Identifier: ISC */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/param.h>

#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/simple-watch.h>
#include <avahi-common/error.h>

#ifndef HAVE_STRLCAT
size_t strlcat(char *dst, const char *src, size_t dsize);
#endif

#define ERR(fmt, ...)    syslog(LOG_ERR,     fmt, ##__VA_ARGS__)
#define WARN(fmt, ...)   syslog(LOG_WARNING, fmt, ##__VA_ARGS__)
#define NOTICE(fmt, ...) syslog(LOG_NOTICE,  fmt, ##__VA_ARGS__)
#define DEBUG(fmt, ...)  syslog(LOG_DEBUG,   fmt, ##__VA_ARGS__)
#define ERRNO(fmt, ...)  ERR(fmt ": %s", ##__VA_ARGS__, strerror(errno))

static AvahiEntryGroup *group       = NULL;
static AvahiSimplePoll *loop        = NULL;
static const char     **cnames      = NULL;
static const char      *domain      = ".local";
static const size_t     minlen      = 6;
static char             auto_cname[HOST_NAME_MAX + 8];
static int              use_hostname = 0;
static int              sigpipe[2]  = { -1, -1 };


static void entry_group_callback(AvahiEntryGroup *g, AvahiEntryGroupState state, void *_)
{
	AvahiClient *client;

	assert(g == group || group == NULL);
	group = g;

	switch (state) {
	case AVAHI_ENTRY_GROUP_COLLISION:
		ERR("CNAME collision, already published by another host on the network.");
		avahi_simple_poll_quit(loop);
		break;

	case AVAHI_ENTRY_GROUP_FAILURE:
		client = avahi_entry_group_get_client(g);
		ERR("Entry group failure: %s", avahi_strerror(avahi_client_errno(client)));
		avahi_simple_poll_quit(loop);
		break;

	default:
		break;
	}
}

static void create_cnames(AvahiClient *client)
{
	AvahiPublishFlags flags = AVAHI_PUBLISH_USE_MULTICAST;
	char hostname[HOST_NAME_MAX + 64] = ".";
	const char *avahi_host;
	int i, rc, count;
	size_t len;

	/* If this is the first time we're called, let's create a new entry group if necessary */
	if (!group) {
		group = avahi_entry_group_new(client, entry_group_callback, NULL);
		if (!group) {
			ERR("Failed creating new entry group: %s",
			    avahi_strerror(avahi_client_errno(client)));
			goto fail;
		}
	}

	/*
	 * If the group is empty (either because it was just created, or
	 * because it was reset previously, add our entries.
	 */
	if (!avahi_entry_group_is_empty(group))
		return;

	avahi_host = avahi_client_get_host_name(client);
	if (!avahi_host) {
		ERR("Failed to get hostname: %s", avahi_strerror(avahi_client_errno(client)));
		goto fail;
	}
	snprintf(&hostname[1], sizeof(hostname) - 1, "%s", avahi_host);
	strlcat(hostname, ".local", sizeof(hostname));

	/* Convert the hostname string into DNS's labelled-strings format */
	len = strlen(hostname);
	for (i = (int)len - 1, count = 0; i >= 0; i--) {
		if (hostname[i] == '.') {
			hostname[i] = count;
			count = 0;
		} else
			count++;
	}

	for (i = 0; (cnames[i] != NULL); i++) {
		rc = avahi_entry_group_add_record(group, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, flags,
						  cnames[i], AVAHI_DNS_CLASS_IN, AVAHI_DNS_TYPE_CNAME,
						  AVAHI_DEFAULT_TTL, hostname, len + 1);
		if (rc >= 0)
			NOTICE("Published CNAME %s", cnames[i]);
		else {
			ERR("Failed publishing CNAME %s: %s", cnames[i], avahi_strerror(rc));
			goto fail;
		}
	}

	rc = avahi_entry_group_commit(group);
	if (rc < 0) {
		ERR("Failed to commit entry group: %s", avahi_strerror(rc));
		goto fail;
	}

	return;
 fail:
	avahi_simple_poll_quit(loop);
}

static void client_callback(AvahiClient *c, AvahiClientState state, void *_)
{
	switch (state) {
	case AVAHI_CLIENT_S_RUNNING:
		create_cnames(c);
		break;

	case AVAHI_CLIENT_FAILURE:
		ERR("Client failure: %s", avahi_strerror(avahi_client_errno(c)));
		avahi_simple_poll_quit(loop);
		break;

	case AVAHI_CLIENT_S_COLLISION:
	case AVAHI_CLIENT_S_REGISTERING:
		if (group)
			avahi_entry_group_reset(group);
		break;

	default:
		break;
	}
}

static void pipe_callback(AvahiWatch *w, int fd, AvahiWatchEvent events, void *userdata)
{
	AvahiClient *client = userdata;
	char new_cname[HOST_NAME_MAX + 8];
	char syshost[HOST_NAME_MAX + 1];
	char sig;

	(void)w;
	(void)events;

	if (read(fd, &sig, 1) <= 0)
		return;

	if ((int)(unsigned char)sig != SIGHUP) {
		avahi_simple_poll_quit(loop);
		return;
	}

	if (!use_hostname) {
		DEBUG("SIGHUP received, nothing to reload.");
		return;
	}

	if (gethostname(syshost, sizeof(syshost)) < 0) {
		ERRNO("Failed to get system hostname");
		return;
	}
	snprintf(new_cname, sizeof(new_cname), "%s.local", syshost);
	if (!strcmp(new_cname, auto_cname)) {
		DEBUG("SIGHUP received, hostname unchanged (%s).", syshost);
		return;
	}

	NOTICE("Hostname changed to %s, republishing CNAMEs.", syshost);
	snprintf(auto_cname, sizeof(auto_cname), "%s", new_cname);
	if (group)
		avahi_entry_group_reset(group);
	create_cnames(client);
}

static void signal_callback(int signo)
{
	char sig = (char)signo;
	ssize_t n = write(sigpipe[1], &sig, 1);
	(void)n;
}

static int loglevel(const char *arg)
{
	if (!strcmp(arg, "error"))   return LOG_ERR;
	if (!strcmp(arg, "warning")) return LOG_WARNING;
	if (!strcmp(arg, "notice"))  return LOG_NOTICE;
	if (!strcmp(arg, "info"))    return LOG_INFO;
	if (!strcmp(arg, "debug"))   return LOG_DEBUG;
	return -1;
}

static int usage(int rc)
{
	printf("Usage:\n"
	       "             %s [-hHv] [-l LEVEL] [alias.local [..]]\n"
	       "Options:\n"
	       "  -h         This help text\n"
	       "  -H         Add system hostname (from /etc/hostname) as a CNAME\n"
	       "  -l LEVEL   Set log level: error, warning, notice (default), info, debug\n"
	       "  -v         Show program version\n"
	       "\n"
	       "Publishes mDNS CNAMEs (aliases) for this host using Avahi.\n"
	       "Send SIGHUP to reload the system hostname (-H) without restarting.\n"
	       "\n", PACKAGE_NAME);

	return rc;
}

int main(int argc, char **argv)
{
	static const char *cnames_buf[64];
	const AvahiPoll *poll_api;
	AvahiWatch *sig_watch;
	AvahiClient *client;
	int c, error, level = LOG_NOTICE;

	while ((c = getopt(argc, argv, "hHl:v")) != EOF) {
		switch (c) {
		case 'h':
			return usage(0);
		case 'H':
			use_hostname = 1;
			break;
		case 'l':
			level = loglevel(optarg);
			if (level < 0) {
				fprintf(stderr, "Invalid log level '%s'\n", optarg);
				return usage(1);
			}
			break;
		case 'v':
			puts(PACKAGE_NAME " v" VERSION);
			puts(PACKAGE_BUGREPORT);
			return 0;
		default:
			return usage(1);
		}
	}

	if (!use_hostname && optind >= argc)
		return usage(1);

	openlog(PACKAGE_NAME, LOG_PID, LOG_DAEMON);
	setlogmask(LOG_UPTO(level));

	if (use_hostname) {
		char syshost[HOST_NAME_MAX + 1];

		if (gethostname(syshost, sizeof(syshost)) < 0) {
			ERRNO("Failed to get system hostname");
			closelog();
			return 1;
		}
		snprintf(auto_cname, sizeof(auto_cname), "%s.local", syshost);
		cnames_buf[0] = auto_cname;
	}

	for (c = optind; c < argc; c++) {
		const char *cname = argv[c];
		size_t len = strlen(cname);

		if (len < minlen)
			goto invalid;

		len -= minlen;
		if (strcmp(&cname[len], domain)) {
		invalid:
			ERR("Invalid CNAME: %s, must end with %s", cname, domain);
			return 1;
		}
		cnames_buf[use_hostname + (c - optind)] = cname;
	}
	cnames_buf[use_hostname + (argc - optind)] = NULL;
	cnames = cnames_buf;

	loop = avahi_simple_poll_new();
	if (!loop) {
		ERR("Failed creating Avahi loop.");
		closelog();
		return 1;
	}

	if (pipe(sigpipe) < 0) {
		ERRNO("Failed to create signal pipe");
		avahi_simple_poll_free(loop);
		closelog();
		return 1;
	}
	fcntl(sigpipe[1], F_SETFL, O_NONBLOCK);

	client = avahi_client_new(avahi_simple_poll_get(loop), 0, client_callback, NULL, &error);
	if (!client) {
		ERR("Failed to create Avahi client: %s", avahi_strerror(error));
		close(sigpipe[0]);
		close(sigpipe[1]);
		avahi_simple_poll_free(loop);
		closelog();
		return 1;
	}

	poll_api  = avahi_simple_poll_get(loop);
	sig_watch = poll_api->watch_new(poll_api, sigpipe[0], AVAHI_WATCH_IN, pipe_callback, client);

	signal(SIGTERM, signal_callback);
	signal(SIGHUP,  signal_callback);
	signal(SIGINT,  signal_callback);

	avahi_simple_poll_loop(loop);

	poll_api->watch_free(sig_watch);
	close(sigpipe[0]);
	close(sigpipe[1]);
	if (group)
		avahi_entry_group_free(group);
	avahi_client_free(client);
	avahi_simple_poll_free(loop);
	closelog();

	return 0;
}
