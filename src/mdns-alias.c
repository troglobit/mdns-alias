/* SPDX-License-Identifier: ISC */

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/alternative.h>
#include <avahi-common/simple-watch.h>
#include <avahi-common/malloc.h>
#include <avahi-common/error.h>

#ifndef HAVE_STRLCAT
size_t strlcat(char *dst, const char *src, size_t dsize);
#endif

static AvahiEntryGroup *group = NULL;
static AvahiSimplePoll *loop = NULL;
static const char **cnames = NULL;
static const char *domain  = ".local";
static const size_t minlen = 6;


static void entry_group_callback(AvahiEntryGroup *g, AvahiEntryGroupState state, void *_)
{
	AvahiClient *client;

	assert(g == group || group == NULL);
	group = g;

	switch (state) {
	case AVAHI_ENTRY_GROUP_FAILURE:
		client = avahi_entry_group_get_client(g);
		fprintf(stderr, "Entry group failure: %s\n", avahi_strerror(avahi_client_errno(client)));
		avahi_simple_poll_quit(loop);
		break;

	default:
		break;
	}
}

static void create_cnames(AvahiClient *client)
{
	AvahiPublishFlags flags = (AVAHI_PUBLISH_USE_MULTICAST | AVAHI_PUBLISH_ALLOW_MULTIPLE);
	char hostname[HOST_NAME_MAX + 64] = ".";
	int i, rc, count;
	size_t len;

	/* If this is the first time we're called, let's create a new entry group if necessary */
	if (!group) {
		group = avahi_entry_group_new(client, entry_group_callback, NULL);
		if (!group) {
			fprintf(stderr, "Failed creating new entry group: %s\n",
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

	if (gethostname(&hostname[1], sizeof(hostname) - 1) < 0)
		perror("gethostname");

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
			printf("Published DNS-SD CNAME %s\n", cnames[i]);
		else {
			fprintf(stderr, "Failed publishing DNS-SD CNAME %s: %s\n", cnames[i], avahi_strerror(rc));
			goto fail;
		}
	}

	rc = avahi_entry_group_commit(group);
	if (rc < 0) {
		fprintf(stderr, "Failed to commit entry group: %s\n", avahi_strerror(rc));
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
		fprintf(stderr, "Client failure: %s\n", avahi_strerror(avahi_client_errno(c)));
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

static void signal_callback(int signo)
{
	printf("Got signal %d, exiting.\n", signo);
	avahi_simple_poll_quit(loop);
}

static int usage(int rc)
{
	printf("Usage:\n"
	       "             %s [-hv] [alias.local [..]]\n"
	       "Options:\n"
	       "  -h         This help text\n"
	       "  -v         Show program version\n"
	       "\n"
	       "Publishes mDNS CNAMEs (aliases) for this host using Avahi.\n"
	       "\n", PACKAGE_NAME);

	return rc;
}

int main(int argc, char **argv)
{
	AvahiClient *client;
	int c, error;

	while ((c = getopt(argc, argv, "hv")) != EOF) {
		switch (c) {
		case 'h':
			return usage(0);
		case 'v':
			puts(PACKAGE_NAME " v" VERSION);
			puts(PACKAGE_BUGREPORT);
			return 0;
		default:
			return usage(1);
		}
	}

	if (optind >= argc)
		return usage(1);

	for (c = optind; c < argc; c++) {
		const char *cname = argv[c];
		size_t len = strlen(cname);

		if (len < minlen)
			goto invalid;

		len -= minlen;
		if (strcmp(&cname[len], domain)) {
		invalid:
			fprintf(stderr, "Invalid CNAME: %s, must end with %s\n", cname, domain);
		}
	}

	cnames = (const char **)&argv[optind];

	loop = avahi_simple_poll_new();
	if (!loop) {
		fprintf(stderr, "Failed creating Avahi loop.\n");
		return 1;
	}

	client = avahi_client_new(avahi_simple_poll_get(loop), 0, client_callback, NULL, &error);
	if (!client) {
		fprintf(stderr, "Failed to create Avahi client: %s\n", avahi_strerror(error));
		avahi_simple_poll_free(loop);
		return 1;
	}

	signal(SIGTERM, signal_callback);
	signal(SIGHUP, signal_callback);
	signal(SIGINT, signal_callback);

	avahi_simple_poll_loop(loop);
	if (group)
		avahi_entry_group_free(group);
	avahi_client_free(client);
	avahi_simple_poll_free(loop);

	return 0;
}
