/*
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2014  Google Inc.
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <limits.h>
#include <errno.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/l2cap.h>
#include "lib/uuid.h"

#include "monitor/mainloop.h"
#include "src/shared/util.h"
#include "src/shared/att.h"
#include "src/shared/gatt-client.h"

#define ATT_CID 4

#define PRLOG(...) \
	printf(__VA_ARGS__); print_prompt();

#define COLOR_OFF	"\x1B[0m"
#define COLOR_RED	"\x1B[0;91m"
#define COLOR_GREEN	"\x1B[0;92m"
#define COLOR_YELLOW	"\x1B[0;93m"
#define COLOR_BLUE	"\x1B[0;94m"
#define COLOR_MAGENTA	"\x1B[0;95m"
#define COLOR_BOLDGRAY	"\x1B[1;30m"
#define COLOR_BOLDWHITE	"\x1B[1;37m"

static bool verbose = false;

struct client {
	int fd;
	struct bt_gatt_client *gatt;
};

static void print_prompt(void)
{
	printf(COLOR_BLUE "[GATT client]" COLOR_OFF "# ");
	fflush(stdout);
}

static void att_disconnect_cb(void *user_data)
{
	printf("Device disconnected\n");

	mainloop_quit();
}

static void att_debug_cb(const char *str, void *user_data)
{
	const char *prefix = user_data;

	PRLOG(COLOR_BOLDGRAY "%s" COLOR_BOLDWHITE "%s\n" COLOR_OFF, prefix, str);
}

static void gatt_debug_cb(const char *str, void *user_data)
{
	const char *prefix = user_data;

	PRLOG(COLOR_GREEN "%s%s\n" COLOR_OFF, prefix, str);
}

static void ready_cb(bool success, uint8_t att_ecode, void *user_data);

static struct client *client_create(int fd, uint16_t mtu)
{
	struct client *cli;
	struct bt_att *att;

	cli = new0(struct client, 1);
	if (!cli) {
		fprintf(stderr, "Failed to allocate memory for client\n");
		return NULL;
	}

	att = bt_att_new(fd);
	if (!att) {
		fprintf(stderr, "Failed to initialze ATT transport layer\n");
		bt_att_unref(att);
		free(cli);
		return NULL;
	}

	if (!bt_att_set_close_on_unref(att, true)) {
		fprintf(stderr, "Failed to set up ATT transport layer\n");
		bt_att_unref(att);
		free(cli);
		return NULL;
	}

	if (!bt_att_register_disconnect(att, att_disconnect_cb, NULL, NULL)) {
		fprintf(stderr, "Failed to set ATT disconnect handler\n");
		bt_att_unref(att);
		free(cli);
		return NULL;
	}

	cli->fd = fd;
	cli->gatt = bt_gatt_client_new(att, mtu);
	if (!cli->gatt) {
		fprintf(stderr, "Failed to create GATT client\n");
		bt_att_unref(att);
		free(cli);
		return NULL;
	}

	if (verbose) {
		bt_att_set_debug(att, att_debug_cb, "att: ", NULL);
		bt_gatt_client_set_debug(cli->gatt, gatt_debug_cb, "gatt: ",
									NULL);
	}

	bt_gatt_client_set_ready_handler(cli->gatt, ready_cb, cli, NULL);

	/* bt_gatt_client already holds a reference */
	bt_att_unref(att);

	return cli;
}

static void client_destroy(struct client *cli)
{
	bt_gatt_client_unref(cli->gatt);
}

static void print_uuid(const uint8_t uuid[16])
{
	char uuid_str[MAX_LEN_UUID_STR];
	bt_uuid_t tmp;

	tmp.type = BT_UUID128;
	memcpy(tmp.value.u128.data, uuid, 16 * sizeof(uint8_t));
	bt_uuid_to_string(&tmp, uuid_str, sizeof(uuid_str));

	printf("%s\n", uuid_str);
}

static void print_services(struct client *cli)
{
	struct bt_gatt_service_iter iter;
	bt_gatt_service_t service;
	const bt_gatt_characteristic_t *chrc;

	if (!bt_gatt_service_iter_init(&iter, cli->gatt)) {
		PRLOG("Failed to initialize service iterator\n");
		return;
	}

	printf("\n");

	while (bt_gatt_service_iter_next(&iter, &service)) {
		size_t i, j;

		printf("service - start: 0x%04x, end: 0x%04x, uuid: ",
				service.start_handle, service.end_handle);
		print_uuid(service.uuid);

		for (i = 0; i < service.num_chrcs; i++) {
			chrc = service.chrcs + i;
			printf("\t  charac - start: 0x%04x, end: 0x%04x, "
					"value: 0x%04x, props: 0x%02x, uuid: ",
					chrc->start_handle,
					chrc->end_handle,
					chrc->value_handle,
					chrc->properties);
			print_uuid(service.chrcs[i].uuid);

			for (j = 0; j < chrc->num_descs; j++) {
				printf("\t\t  descr - handle: 0x%04x, uuid: ",
							chrc->descs[j].handle);
				print_uuid(chrc->descs[j].uuid);
			}
		}

		printf("\n");
	}
}

static void ready_cb(bool success, uint8_t att_ecode, void *user_data)
{
	struct client *cli = user_data;

	if (!success) {
		PRLOG("GATT discovery procedures failed - error code: 0x%02x\n",
								att_ecode);
		return;
	}

	PRLOG("GATT discovery procedures complete\n");

	print_services(cli);
	print_prompt();
}

static void cmd_services(struct client *cli)
{
	if (!bt_gatt_client_is_ready(cli->gatt)) {
		printf("GATT client not initialized\n");
		return;
	}

	print_services(cli);
}

static void cmd_help(struct client *cli);

typedef void (*command_func_t)(struct client *cli);

static struct {
	char *cmd;
	command_func_t func;
	char *doc;
} command[] = {
	{ "help", cmd_help, "\tDisplay help message" },
	{ "services", cmd_services, "\tShow discovered services" },
	{ }
};

static void cmd_help(struct client *cli)
{
	int i;

	printf("Commands:\n");
	for (i = 0; command[i].cmd; i++)
		printf("\t%-15s\t%s\n", command[i].cmd, command[i].doc);
}

static void prompt_read_cb(int fd, uint32_t events, void *user_data)
{
	ssize_t read;
	size_t len = 0;
	char *line = NULL;
	struct client *cli = user_data;
	int i;

	if (events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
		mainloop_quit();
		return;
	}

	if ((read = getline(&line, &len, stdin)) == -1)
		return;

	for (i = 0; command[i].cmd; i++) {
		if (strncmp(command[i].cmd, line, read - 1) == 0)
			break;
	}

	if (command[i].cmd)
		command[i].func(cli);
	else
		fprintf(stderr, "Unknown command: %s\n", line);

	print_prompt();

	free(line);
}

static void signal_cb(int signum, void *user_data)
{
	switch (signum) {
	case SIGINT:
	case SIGTERM:
		mainloop_quit();
		break;
	default:
		break;
	}
}

static int l2cap_le_att_connect(bdaddr_t *src, bdaddr_t *dst, uint8_t dst_type,
									int sec)
{
	int sock;
	struct sockaddr_l2 srcaddr, dstaddr;
	struct bt_security btsec;

	if (verbose) {
		char srcaddr_str[18], dstaddr_str[18];

		ba2str(src, srcaddr_str);
		ba2str(dst, dstaddr_str);

		printf("btgatt-client: Opening L2CAP LE connection on ATT "
					"channel:\n\t src: %s\n\tdest: %s\n",
					srcaddr_str, dstaddr_str);
	}

	sock = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
	if (sock < 0) {
		perror("Failed to create L2CAP socket");
		return -1;
	}

	/* Set up source address */
	memset(&srcaddr, 0, sizeof(srcaddr));
	srcaddr.l2_family = AF_BLUETOOTH;
	srcaddr.l2_cid = htobs(ATT_CID);
	srcaddr.l2_bdaddr_type = 0;
	bacpy(&srcaddr.l2_bdaddr, src);

	if (bind(sock, (struct sockaddr *)&srcaddr, sizeof(srcaddr)) < 0) {
		perror("Failed to bind L2CAP socket");
		close(sock);
		return -1;
	}

	/* Set the security level */
	memset(&btsec, 0, sizeof(btsec));
	btsec.level = sec;
	if (setsockopt(sock, SOL_BLUETOOTH, BT_SECURITY, &btsec,
							sizeof(btsec)) != 0) {
		fprintf(stderr, "Failed to set L2CAP security level\n");
		close(sock);
		return -1;
	}

	/* Set up destination address */
	memset(&dstaddr, 0, sizeof(dstaddr));
	dstaddr.l2_family = AF_BLUETOOTH;
	dstaddr.l2_cid = htobs(ATT_CID);
	dstaddr.l2_bdaddr_type = dst_type;
	bacpy(&dstaddr.l2_bdaddr, dst);

	printf("Connecting to device...");
	fflush(stdout);

	if (connect(sock, (struct sockaddr *) &dstaddr, sizeof(dstaddr)) < 0) {
		perror(" Failed to connect");
		close(sock);
		return -1;
	}

	printf(" Done\n");

	return sock;
}

static void usage(void)
{
	printf("btgatt-client\n");
	printf("Usage:\n\tbtgatt-client [options]\n");

	printf("Options:\n"
		"\t-i, --index <id>\t\tSpecify adapter index, e.g. hci0\n"
		"\t-d, --dest <addr>\t\tSpecify the destination address\n"
		"\t-t, --type [random|public] \tSpecify the LE address type\n"
		"\t-m, --mtu <mtu> \t\tThe ATT MTU to use\n"
		"\t-s, --security-level <sec> \tSet security level (low|"
								"medium|high)\n"
		"\t-v, --verbose\t\t\tEnable extra logging\n"
		"\t-h, --help\t\t\tDisplay help\n");
}

static struct option main_options[] = {
	{ "index",		1, 0, 'i' },
	{ "dest",		1, 0, 'd' },
	{ "type",		1, 0, 't' },
	{ "mtu",		1, 0, 'm' },
	{ "security-level",	1, 0, 's' },
	{ "verbose",		0, 0, 'v' },
	{ "help",		0, 0, 'h' },
	{ }
};

int main(int argc, char *argv[])
{
	int opt;
	int sec = BT_SECURITY_LOW;
	uint16_t mtu = 0;
	uint8_t dst_type = BDADDR_LE_PUBLIC;
	bool dst_addr_given = false;
	bdaddr_t src_addr, dst_addr;
	int dev_id = -1;
	int fd;
	sigset_t mask;
	struct client *cli;

	while ((opt = getopt_long(argc, argv, "+hvs:m:t:d:i:",
						main_options, NULL)) != -1) {
		switch (opt) {
		case 'h':
			usage();
			return EXIT_SUCCESS;
		case 'v':
			verbose = true;
			break;
		case 's':
			if (strcmp(optarg, "low") == 0)
				sec = BT_SECURITY_LOW;
			else if (strcmp(optarg, "medium") == 0)
				sec = BT_SECURITY_MEDIUM;
			else if (strcmp(optarg, "high") == 0)
				sec = BT_SECURITY_HIGH;
			else {
				fprintf(stderr, "Invalid security level\n");
				return EXIT_FAILURE;
			}
			break;
		case 'm': {
			int arg;

			arg = atoi(optarg);
			if (arg <= 0) {
				fprintf(stderr, "Invalid MTU: %d\n", arg);
				return EXIT_FAILURE;
			}

			if (arg > UINT16_MAX) {
				fprintf(stderr, "MTU too large: %d\n", arg);
				return EXIT_FAILURE;
			}

			mtu = (uint16_t)arg;
			break;
		}
		case 't':
			if (strcmp(optarg, "random") == 0)
				dst_type = BDADDR_LE_RANDOM;
			else if (strcmp(optarg, "public") == 0)
				dst_type = BDADDR_LE_PUBLIC;
			else {
				fprintf(stderr,
					"Allowed types: random, public\n");
				return EXIT_FAILURE;
			}
			break;
		case 'd':
			if (str2ba(optarg, &dst_addr) < 0) {
				fprintf(stderr, "Invalid remote address: %s\n",
									optarg);
				return EXIT_FAILURE;
			}

			dst_addr_given = true;
			break;

		case 'i':
			dev_id = hci_devid(optarg);
			if (dev_id < 0) {
				perror("Invalid adapter");
				return EXIT_FAILURE;
			}

			break;
		default:
			fprintf(stderr, "Invalid option: %c\n", opt);
			return EXIT_FAILURE;
		}
	}

	if (!argc) {
		usage();
		return EXIT_SUCCESS;
	}

	argc -= optind;
	argv += optind;
	optind = 0;

	if (argc) {
		usage();
		return EXIT_SUCCESS;
	}

	if (dev_id == -1)
		bacpy(&src_addr, BDADDR_ANY);
	else if (hci_devba(dev_id, &src_addr) < 0) {
		perror("Adapter not available");
		return EXIT_FAILURE;
	}

	if (!dst_addr_given) {
		fprintf(stderr, "Destination address required!\n");
		return EXIT_FAILURE;
	}

	mainloop_init();

	fd = l2cap_le_att_connect(&src_addr, &dst_addr, dst_type, sec);
	if (fd < 0)
		return EXIT_FAILURE;

	cli = client_create(fd, mtu);
	if (!cli) {
		close(fd);
		return EXIT_FAILURE;
	}

	if (mainloop_add_fd(fileno(stdin),
				EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR,
				prompt_read_cb, cli, NULL) < 0) {
		fprintf(stderr, "Failed to initialize console\n");
		return EXIT_FAILURE;
	}

	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);

	mainloop_set_signal(&mask, signal_cb, NULL, NULL);

	print_prompt();

	mainloop_run();

	printf("\n\nShutting down...\n");

	client_destroy(cli);

	return EXIT_SUCCESS;
}