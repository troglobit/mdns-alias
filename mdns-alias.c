/* SPDX-License-Identifier: ISC */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/alternative.h>
#include <avahi-common/simple-watch.h>
#include <avahi-common/malloc.h>
#include <avahi-common/error.h>

static AvahiEntryGroup *group = NULL;
static AvahiSimplePoll *loop = NULL;
static const char **cnames = NULL;


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

	strncat(hostname, ".local", sizeof(hostname));
	hostname[sizeof(hostname) - 1] = '\0';

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
			printf("Published DNS-SD hostname %s with CNAME %s\n", hostname, cnames[i]);
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

int main(int argc, char **argv)
{
	AvahiClient *client;
	int error;

	if (argc < 2) {
		printf("Usage: mdns-alias foo.local bar.local [...]\n");
		return 1;
	}

	cnames = (const char **)&argv[1];

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

	avahi_simple_poll_loop(loop);
	avahi_client_free(client);
	avahi_simple_poll_free(loop);

	return 0;
}
