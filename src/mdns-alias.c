/* SPDX-License-Identifier: ISC */

#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/param.h>
#include <sys/time.h>

#include <uev/uev.h>
#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/watch.h>
#include <avahi-common/error.h>

#ifndef HAVE_STRLCAT
size_t strlcat(char *dst, const char *src, size_t dsize);
#endif

#define ERR(fmt, ...)    syslog(LOG_ERR,     fmt, ##__VA_ARGS__)
#define WARN(fmt, ...)   syslog(LOG_WARNING, fmt, ##__VA_ARGS__)
#define NOTICE(fmt, ...) syslog(LOG_NOTICE,  fmt, ##__VA_ARGS__)
#define DEBUG(fmt, ...)  syslog(LOG_DEBUG,   fmt, ##__VA_ARGS__)
#define ERRNO(fmt, ...)  ERR(fmt ": %s", ##__VA_ARGS__, strerror(errno))

/*
 * Avahi uses an abstract AvahiPoll vtable to interface with whatever
 * event loop the application provides.  These two structs are the
 * opaque handles Avahi allocates through our watch_new/timeout_new
 * callbacks; we back them with libuev I/O and timer watchers.
 */
struct AvahiWatch {
	uev_t              io;
	AvahiWatchCallback cb;
	void              *userdata;
	AvahiWatchEvent    revents;
};

struct AvahiTimeout {
	uev_t                timer;
	AvahiTimeoutCallback cb;
	void                *userdata;
};

static uev_ctx_t        ctx;
static AvahiEntryGroup *group        = NULL;
static const char     **cnames       = NULL;
static const char      *domain       = ".local";
static const size_t     minlen       = 6;
static char             auto_cname[HOST_NAME_MAX + 8];
static int              use_hostname = 0;


/*
 * Avahi <-> libuev bridge
 *
 * AVAHI_WATCH_IN/OUT/ERR/HUP map to POLLIN/POLLOUT/POLLERR/POLLHUP,
 * which are numerically identical to EPOLLIN/EPOLLOUT/EPOLLERR/EPOLLHUP
 * on Linux, so direct casts between AvahiWatchEvent and libuev events
 * are safe.
 */
static void avahi_watch_cb(uev_t *w, void *arg, int events)
{
	AvahiWatch *aw = arg;

	aw->revents = (AvahiWatchEvent)events;
	aw->cb(aw, w->fd, aw->revents, aw->userdata);
}

static AvahiWatch *avahi_watch_new(const AvahiPoll *api, int fd, AvahiWatchEvent event,
				   AvahiWatchCallback callback, void *userdata)
{
	AvahiWatch *w;

	(void)api;
	w = malloc(sizeof(*w));
	if (!w)
		return NULL;

	w->cb       = callback;
	w->userdata = userdata;
	w->revents  = 0;

	if (uev_io_init(&ctx, &w->io, avahi_watch_cb, w, fd, (int)event) < 0) {
		free(w);
		return NULL;
	}

	return w;
}

static void avahi_watch_update(AvahiWatch *w, AvahiWatchEvent event)
{
	uev_io_set(&w->io, w->io.fd, (int)event);
}

static AvahiWatchEvent avahi_watch_get_events(AvahiWatch *w)
{
	return w->revents;
}

static void avahi_watch_free(AvahiWatch *w)
{
	uev_io_stop(&w->io);
	free(w);
}

/* Convert an absolute struct timeval to a relative millisecond value for libuev. */
static int timeval_to_ms(const struct timeval *tv)
{
	struct timeval now, diff;

	gettimeofday(&now, NULL);
	if (!timercmp(tv, &now, >))
		return 1; /* already expired, fire ASAP */
	timersub(tv, &now, &diff);

	return (int)(diff.tv_sec * 1000 + diff.tv_usec / 1000);
}

static void avahi_timeout_cb(uev_t *w, void *arg, int events)
{
	AvahiTimeout *t = arg;

	(void)w;
	(void)events;
	t->cb(t, t->userdata);
}

static AvahiTimeout *avahi_timeout_new(const AvahiPoll *api, const struct timeval *tv,
				       AvahiTimeoutCallback callback, void *userdata)
{
	AvahiTimeout *t;

	(void)api;
	t = malloc(sizeof(*t));
	if (!t)
		return NULL;

	t->cb       = callback;
	t->userdata = userdata;

	/* tv == NULL means create disarmed; timeout=0 disarms in libuev. */
	if (uev_timer_init(&ctx, &t->timer, avahi_timeout_cb, t,
			   tv ? timeval_to_ms(tv) : 0, 0) < 0) {
		free(t);
		return NULL;
	}

	return t;
}

static void avahi_timeout_update(AvahiTimeout *t, const struct timeval *tv)
{
	uev_timer_set(&t->timer, tv ? timeval_to_ms(tv) : 0, 0);
}

static void avahi_timeout_free(AvahiTimeout *t)
{
	uev_timer_stop(&t->timer);
	free(t);
}

static const AvahiPoll avahi_uev_poll = {
	.userdata         = NULL,
	.watch_new        = avahi_watch_new,
	.watch_update     = avahi_watch_update,
	.watch_get_events = avahi_watch_get_events,
	.watch_free       = avahi_watch_free,
	.timeout_new      = avahi_timeout_new,
	.timeout_update   = avahi_timeout_update,
	.timeout_free     = avahi_timeout_free,
};


static void entry_group_callback(AvahiEntryGroup *g, AvahiEntryGroupState state, void *_)
{
	AvahiClient *client;

	assert(g == group || group == NULL);
	group = g;

	switch (state) {
	case AVAHI_ENTRY_GROUP_COLLISION:
		ERR("CNAME collision, already published by another host on the network.");
		uev_exit(&ctx);
		break;

	case AVAHI_ENTRY_GROUP_FAILURE:
		client = avahi_entry_group_get_client(g);
		ERR("Entry group failure: %s", avahi_strerror(avahi_client_errno(client)));
		uev_exit(&ctx);
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
	uev_exit(&ctx);
}

static void client_callback(AvahiClient *c, AvahiClientState state, void *_)
{
	switch (state) {
	case AVAHI_CLIENT_S_RUNNING:
		create_cnames(c);
		break;

	case AVAHI_CLIENT_FAILURE:
		ERR("Client failure: %s", avahi_strerror(avahi_client_errno(c)));
		uev_exit(&ctx);
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

static void sighup_cb(uev_t *w, void *arg, int events)
{
	AvahiClient *client = arg;
	char new_cname[HOST_NAME_MAX + 8];
	char syshost[HOST_NAME_MAX + 1];

	(void)w;
	(void)events;

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

static void sigterm_cb(uev_t *w, void *arg, int events)
{
	(void)arg;
	(void)events;
	uev_exit(w->ctx);
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
	uev_t sigterm_w, sigint_w, sighup_w;
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

	uev_init(&ctx);

	client = avahi_client_new(&avahi_uev_poll, 0, client_callback, NULL, &error);
	if (!client) {
		ERR("Failed to create Avahi client: %s", avahi_strerror(error));
		closelog();
		return 1;
	}

	uev_signal_init(&ctx, &sigterm_w, sigterm_cb, NULL,   SIGTERM);
	uev_signal_init(&ctx, &sigint_w,  sigterm_cb, NULL,   SIGINT);
	uev_signal_init(&ctx, &sighup_w,  sighup_cb,  client, SIGHUP);

	uev_run(&ctx, 0);

	if (group)
		avahi_entry_group_free(group);
	avahi_client_free(client);
	closelog();

	return 0;
}
