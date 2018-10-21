#include <sys/capsicum.h>
#include <sys/endian.h>
#include <sys/socket.h>
#include <sys/syslimits.h>
#include <sys/types.h>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "check.h"
#include "nbd-client.h"
#include "nbd-protocol.h"

enum {
	NBD_CLIENT_TIMEOUT = 8,
	NBD_REPLY_QUEUE_TIMEOUT = 1,
};

struct nbd_client {
	int sock;
	_Atomic(bool) disconnect;
	uint32_t flags;
	uint64_t size;
};

struct nbd_client *
nbd_client_alloc()
{
	struct nbd_client *client;

	client = (struct nbd_client *)malloc(sizeof (struct nbd_client));
	if (client == NULL) {
		assert(errno == ENOMEM);
		log(LOG_ERR, "%s: failed to allocate nbd client: %m",
		    __func__);
	}

	return client;
}

void
nbd_client_free(struct nbd_client *client)
{

	free(client);
}

int
nbd_client_init(struct nbd_client *client)
{
	struct timeval tv;
	int sock;
	int on;

	on = 1;

	memset(client, 0, sizeof *client);

	sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock == FAILURE) {
		log(LOG_ERR, "%s: failed to create socket: %m", __func__);
		return FAILURE;
	}

	if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on)
	    == FAILURE) {
		log(LOG_ERR,
		    "%s: failed to set socket option TCP_NODELAY: %m",
		    __func__);
		return FAILURE;
	}

	if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on)
	    == FAILURE) {
		log(LOG_ERR,
		    "%s: failed to set socket option SO_KEEPALIVE: %m",
		    __func__);
		return FAILURE;
	}
	/*
	tv.tv_sec = NBD_CLIENT_TIMEOUT;
	tv.tv_usec = 0;

	if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv)
	    == FAILURE) {
		log(LOG_ERR,
		    "%s: failed to set socket option SO_SNDTIMEO: %m",
		    __func__);
		return FAILURE;
	}

	if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv)
	    == FAILURE) {
		log(LOG_ERR,
		    "%s: failed to set socket option SO_RCVTIMEO: %m",
		    __func__);
		return FAILURE;
	}
	*/
        client->sock = sock;

	return SUCCESS;
}

void
nbd_client_close(struct nbd_client *client)
{

	close(client->sock);
}

int
nbd_client_rights_limit(struct nbd_client *client)
{
	cap_rights_t rights;

	cap_rights_init(&rights, CAP_SEND, CAP_RECV, CAP_SHUTDOWN);

	if (cap_rights_limit(client->sock, &rights) == FAILURE) {
		log(LOG_ERR, "%s: failed to limit capabilities: %m",
		    __func__);
		return FAILURE;
	}

	return SUCCESS;
}

uint64_t
nbd_client_get_size(struct nbd_client *client)
{

	return client->size;
}

bool
nbd_client_get_disconnect(struct nbd_client *client)
{

	return client->disconnect;
}

void
nbd_client_set_disconnect(struct nbd_client *client, bool disconnect)
{

	client->disconnect = disconnect;
}

void
nbd_client_disable_trim(struct nbd_client *client)
{

	client->flags &= ~NBD_FLAG_SEND_TRIM;
}

int
nbd_client_connect(struct nbd_client *client, struct addrinfo *ai)
{
	int sock;

	sock = client->sock;

	if (connect(sock, ai->ai_addr, sizeof *ai->ai_addr) == FAILURE) {
		log(LOG_ERR,
		    "%s: failed to connect to remote server (%s): %m",
		    __func__, ai->ai_canonname);
		return FAILURE;
	}

	return SUCCESS;
}

void
nbd_client_shutdown(struct nbd_client *client)
{

	shutdown(client->sock, SHUT_RDWR);
}

static inline char const *
nbd_client_flag_string(uint32_t flag)
{

	switch (flag) {

#define CASE_MESSAGE(c) case c: return #c

		CASE_MESSAGE(NBD_FLAG_HAS_FLAGS);
		CASE_MESSAGE(NBD_FLAG_READ_ONLY);
		CASE_MESSAGE(NBD_FLAG_SEND_FLUSH);
		CASE_MESSAGE(NBD_FLAG_SEND_FUA);
		CASE_MESSAGE(NBD_FLAG_ROTATIONAL);
		CASE_MESSAGE(NBD_FLAG_SEND_TRIM);

#undef CASE_MESSAGE

	default: return NULL;
	}
}

static inline void
nbd_client_dump(struct nbd_client *client)
{
	const uint32_t Flags[] = {
		NBD_FLAG_HAS_FLAGS, NBD_FLAG_READ_ONLY, NBD_FLAG_SEND_FLUSH,
		NBD_FLAG_SEND_FUA, NBD_FLAG_ROTATIONAL, NBD_FLAG_SEND_TRIM,
	};
	const size_t FlagsLen = sizeof Flags / sizeof Flags[0];

	char flag_string[128], *curflag;
	uint32_t flags;
	size_t i, len;

	log(LOG_DEBUG, "\tsock: %d", client->sock);
	log(LOG_DEBUG, "\tdisconnect: %s",
	    client->disconnect ? "true" : "false");
	flags = client->flags;
	curflag = flag_string;
	len = sizeof flag_string;
	i = FlagsLen;
	while (len && i--) {
		char const *name;
		size_t namelen, t;
		uint32_t value;
		bool match, last;

		value = Flags[i];
		name = nbd_client_flag_string(value);
		namelen = strlen(name);
		match = (flags & value) != 0;
		last = i > 0;

		assert(name != NULL);

		snprintf(curflag, len, "%s%s", match ? name : "",
			 last ? (match ? "|" : "") : "");

		t = namelen + (match ? 1 : 0); // doesn't matter on last
		curflag += t;
		len -= t;
	}
	log(LOG_DEBUG, "\tflags: %#010x [%s]", flags, flag_string);
	log(LOG_DEBUG, "\tsize: %lu", client->size);
}

static inline void
nbd_oldstyle_negotiation_ntoh(struct nbd_oldstyle_negotiation *handshake)
{

	handshake->magic = be64toh(handshake->magic);
	handshake->oldstyle_magic = be64toh(handshake->oldstyle_magic);
	handshake->size = be64toh(handshake->size);
	handshake->flags = be32toh(handshake->flags);
}

static inline bool
nbd_oldstyle_negotiation_is_valid(struct nbd_oldstyle_negotiation *handshake)
{

	if (handshake->magic != NBD_MAGIC) {
		log(LOG_ERR,
		    "%s: invalid magic: %#018lx (expected %#018lx)",
		    __func__, handshake->magic, NBD_MAGIC);
		return false;
	}
	if (handshake->oldstyle_magic != NBD_OLDSTYLE_MAGIC) {
		log(LOG_ERR,
		    "%s: invalid oldstyle magic: %#018lx (expected %#018lx)",
		    __func__, handshake->oldstyle_magic, NBD_OLDSTYLE_MAGIC);
		return false;
	}
	if (!(handshake->flags & NBD_FLAG_HAS_FLAGS)) {
		log(LOG_ERR,
		    "%s: invalid flags: %#010x (expected low bit set)",
		    __func__, handshake->flags);
		return false;
	}

	return true;
}

static inline void
nbd_oldstyle_negotiation_dump(struct nbd_oldstyle_negotiation *handshake)
{
	uint32_t flags = handshake->flags;

	log(LOG_DEBUG, "\tmagic: %#018lx", handshake->magic);
	log(LOG_DEBUG, "\toldstyle_magic: %#018lx",
	    handshake->oldstyle_magic);
	log(LOG_DEBUG, "\tsize: %lu", handshake->size);
	log(LOG_DEBUG, "\tflags: %#010x%s", flags,
	    (flags & NBD_FLAG_HAS_FLAGS) ? "" : " (invalid)");
}

static int
nbd_client_oldstyle_handshake(struct nbd_client *client)
{
	struct nbd_oldstyle_negotiation handshake;
	ssize_t len;
	int sock;

	sock = client->sock;

	while (true) {
		len = recv(sock, &handshake, sizeof handshake, MSG_WAITALL);
		if (client->disconnect)
			return FAILURE;
		if (len == -1 && errno == EINTR)
			continue;
		if (len != sizeof handshake) {
			log(LOG_ERR, "%s: connection failed: %m", __func__);
			return FAILURE;
		}
		break;
	}

	nbd_oldstyle_negotiation_ntoh(&handshake);

	//log(LOG_DEBUG, "%s: negotiation", __func__);
	//nbd_oldstyle_negotiation_dump(&handshake);

	if (!nbd_oldstyle_negotiation_is_valid(&handshake)) {
		log(LOG_ERR, "%s: invalid handshake", __func__);
		nbd_oldstyle_negotiation_dump(&handshake);
		return FAILURE;
	}

	client->size = handshake.size;
	client->flags = handshake.flags;

	//log(LOG_DEBUG, "%s: client", __func__);
	//nbd_client_dump(client);

	if (!(handshake.flags & NBD_FLAG_SEND_FLUSH))
		log(LOG_INFO,
		    "%s: server does not support FLUSH command", __func__);
	if (!(handshake.flags & NBD_FLAG_SEND_TRIM))
		log(LOG_INFO,
		    "%s: server does not support TRIM command", __func__);

	return SUCCESS;
}

static inline void
nbd_newstyle_negotiation_ntoh(struct nbd_newstyle_negotiation *handshake)
{

	handshake->magic = be64toh(handshake->magic);
	handshake->newstyle_magic = be64toh(handshake->newstyle_magic);
	handshake->handshake_flags = be16toh(handshake->handshake_flags);
}

#define VALID_NEWSTYLE_FLAGS (NBD_FLAG_FIXED_NEWSTYLE|NBD_FLAG_NO_ZEROES)

static inline bool
nbd_newstyle_negotiation_is_valid(struct nbd_newstyle_negotiation *handshake)
{
	uint16_t flags = handshake->handshake_flags;

	if (handshake->magic != NBD_MAGIC) {
		log(LOG_ERR,
		    "%s: invalid magic: %#018lx (expected %#018lx)",
		    __func__, handshake->magic, NBD_MAGIC);
		return false;
	}
	if (handshake->newstyle_magic != NBD_NEWSTYLE_MAGIC) {
		log(LOG_ERR,
		    "%s: invalid newstyle magic: %#018lx (expected %#018lx)",
		    __func__, handshake->newstyle_magic, NBD_NEWSTYLE_MAGIC);
		return false;
	}
	if (flags & ~VALID_NEWSTYLE_FLAGS)
		log(LOG_ERR, "%s: ignoring unknown handshake flags: %#06x",
		    __func__, flags);
	if (!(flags & NBD_FLAG_FIXED_NEWSTYLE)) {
		log(LOG_ERR, "%s: this server does not support the fixed "
		    "newstyle protocol", __func__);
		return false;
	}

	return true;
}

static inline void
nbd_newstyle_negotiation_dump(struct nbd_newstyle_negotiation *handshake)
{
	uint16_t flags = handshake->handshake_flags;

	log(LOG_DEBUG, "\tmagic: %#018lx", handshake->magic);
	log(LOG_DEBUG, "\tnewstyle_magic: %#018lx",
	    handshake->newstyle_magic);
	log(LOG_DEBUG, "\thandshake_flags: %#06x [%s%s%s]%s", flags,
	    (flags & NBD_FLAG_FIXED_NEWSTYLE) ? "FIXED_NEWSTYLE" : "",
	    ((flags & VALID_NEWSTYLE_FLAGS)
	     == VALID_NEWSTYLE_FLAGS) ? "|" : "",
	    (flags & NBD_FLAG_NO_ZEROES) ? "NO_ZEROES" : "",
	    (flags & ~VALID_NEWSTYLE_FLAGS) ? " (invalid)" : "");
}

static inline void
nbd_client_flags_set_client_flags(struct nbd_client_flags *client_flags,
				  uint32_t flags)
{

	client_flags->client_flags = htobe32(flags);
}

/*
 * Client handshake
 *
 * If the client and server agree not to send the reserved portion of the
 * EXPORT_NAME option reply, 1 is returned, otherwise 0.
 *
 * Returns -1 if an error is encountered.
 */
static int
nbd_client_newstyle_handshake(struct nbd_client *client)
{
	struct nbd_newstyle_negotiation handshake;
	struct nbd_client_flags response;
	uint32_t client_flags;
	ssize_t len;
	int sock;

	sock = client->sock;

	while (true) {
		len = recv(sock, &handshake, sizeof handshake, MSG_WAITALL);
		if (client->disconnect)
			return FAILURE;
		if (len == -1 && errno == EINTR)
			continue;
		if (len != sizeof handshake)
			goto connection_fail;
		break;
	}

	nbd_newstyle_negotiation_ntoh(&handshake);

	if (!nbd_newstyle_negotiation_is_valid(&handshake)) {
		log(LOG_ERR, "%s: invalid handshake", __func__);
		nbd_newstyle_negotiation_dump(&handshake);
		return FAILURE;
	}

	client_flags = NBD_CLIENT_FLAG_FIXED_NEWSTYLE;
	if (handshake.handshake_flags & NBD_FLAG_NO_ZEROES)
		client_flags |= NBD_CLIENT_FLAG_NO_ZEROES;

	nbd_client_flags_set_client_flags(&response, client_flags);

	len = send(sock, &response, sizeof response, MSG_NOSIGNAL);
	if (len != sizeof response)
		goto connection_fail;

	client->flags = handshake.handshake_flags << 16;

	return SUCCESS;

 connection_fail:
	log(LOG_ERR, "%s: connection failed: %m", __func__);
	return FAILURE;
}

static inline void
nbd_option_init(struct nbd_option *option)
{

	memset(option, 0, sizeof *option);
	option->magic = htobe64(NBD_OPTION_MAGIC);
}

static inline void
nbd_option_set_option(struct nbd_option *option, uint32_t opt)
{

	option->option = htobe32(opt);
}

static inline void
nbd_option_set_length(struct nbd_option *option, uint32_t length)
{

	option->length = htobe32(length);
}

static int
nbd_client_send_option(struct nbd_client *client,
		       struct nbd_option *option,
		       size_t length, uint8_t *data)
{
	ssize_t len;
	int sock;

	sock = client->sock;

	len = send(sock, option, sizeof *option, MSG_NOSIGNAL);
	if (len != sizeof *option)
		goto connection_fail;

	if (length == 0)
		return SUCCESS;

	assert(data != NULL);

	len = send(sock, data, length, MSG_NOSIGNAL);
	if (len != length)
		goto connection_fail;

	return SUCCESS;

 connection_fail:
	log(LOG_ERR, "%s: connection failed: %m", __func__);
	return FAILURE;
}

static inline void
nbd_option_reply_ntoh(struct nbd_option_reply *reply)
{

	reply->magic = be64toh(reply->magic);
	reply->option = be32toh(reply->option);
	reply->type = be32toh(reply->type);
	reply->length = be32toh(reply->length);
}

static inline bool
nbd_option_reply_is_valid(struct nbd_option_reply *reply,
			  struct nbd_option *option)
{
	uint32_t opt;

	opt = be32toh(option->option);

	assert(option->option != NBD_OPTION_EXPORT_NAME);

	if (reply->magic != NBD_OPTION_REPLY_MAGIC) {
		log(LOG_ERR,
		    "%s: invalid magic: %#018lx (expected %#018lx)",
		    __func__, reply->magic, NBD_OPTION_REPLY_MAGIC);
		return false;
	}
	if (reply->option != opt) {
		log(LOG_ERR,
		    "%s: unexpected option: %#010x (expected %#010x)",
		    __func__, reply->option, opt);
		return false;
	}

	return true;
}

static inline char const *
nbd_option_reply_option_string(struct nbd_option_reply *reply)
{

	switch (reply->option) {

#define CASE_MESSAGE(c) case c: return #c
#define EXTENSION " [unsupported extension]"
#define WITHDRAWN " [withdrawn]"

		CASE_MESSAGE(NBD_OPTION_EXPORT_NAME);
		CASE_MESSAGE(NBD_OPTION_ABORT);
		CASE_MESSAGE(NBD_OPTION_LIST);
		CASE_MESSAGE(NBD_OPTION_PEEK_EXPORT) WITHDRAWN;
		CASE_MESSAGE(NBD_OPTION_STARTTLS);
		CASE_MESSAGE(NBD_OPTION_INFO) EXTENSION;
		CASE_MESSAGE(NBD_OPTION_GO) EXTENSION;
		CASE_MESSAGE(NBD_OPTION_STRUCTURED_REPLY) EXTENSION;
		CASE_MESSAGE(NBD_OPTION_BLOCK_SIZE) EXTENSION;

#undef WITHDRAWN
#undef EXTENSION
#undef CASE_MESSAGE

	default: return NULL;
	}
}

static inline char const *
nbd_option_reply_type_string(struct nbd_option_reply *reply)
{

	switch (reply->type) {

#define CASE_MESSAGE(c) case c: return #c
#define EXTENSION " [unsupported extension]"
#define UNUSED    " [unused]"
#define TODO      " [todo]"

		CASE_MESSAGE(NBD_REPLY_ACK);
		CASE_MESSAGE(NBD_REPLY_SERVER);
		CASE_MESSAGE(NBD_REPLY_INFO) EXTENSION;
		CASE_MESSAGE(NBD_REPLY_ERROR_UNSUPPORTED);
		CASE_MESSAGE(NBD_REPLY_ERROR_POLICY);
		CASE_MESSAGE(NBD_REPLY_ERROR_INVALID);
		CASE_MESSAGE(NBD_REPLY_ERROR_PLATFORM) UNUSED;
		CASE_MESSAGE(NBD_REPLY_ERROR_TLS_REQUIRED) TODO;
		CASE_MESSAGE(NBD_REPLY_ERROR_UNKNOWN) EXTENSION;
		CASE_MESSAGE(NBD_REPLY_ERROR_SHUTDOWN);
		CASE_MESSAGE(NBD_REPLY_ERROR_BLOCK_SIZE_REQD) EXTENSION;

#undef TODO
#undef UNUSED
#undef EXTENSION
#undef CASE_MESSAGE

	default: return NULL;
	}
}

static inline void
nbd_option_reply_dump(struct nbd_option_reply *reply)
{
	char const *option = nbd_option_reply_option_string(reply);
	char const *type = nbd_option_reply_type_string(reply);

	log(LOG_DEBUG, "\tmagic: %#018lx", reply->magic);

	if (option == NULL)
		log(LOG_DEBUG, "\toption: [unknown] %#010x (%d)",
		    reply->option, reply->option);
	else
		log(LOG_DEBUG, "\toption: %s", option);

	if (type == NULL)
		log(LOG_DEBUG, "\ttype: [unknown] %#010x (%d)",
		    reply->type, reply->type);
	else
		log(LOG_DEBUG, "\ttype: %s", type);

	log(LOG_DEBUG, "\tlength: %u", reply->length);
}

static int
nbd_client_recv_option_reply(struct nbd_client *client,
			     struct nbd_option *option,
			     struct nbd_option_reply *reply,
			     size_t datalen, uint8_t *data)
{
	size_t recvlen;
	ssize_t len;
	int sock;

	sock = client->sock;

	while (true) {
		len = recv(sock, reply, sizeof *reply, MSG_WAITALL);
		if (client->disconnect)
			return FAILURE;
		if (len == -1 && errno == EINTR)
			continue;
		if (len != sizeof *reply)
			goto connection_fail;
		break;
	}

	nbd_option_reply_ntoh(reply);

	if (!nbd_option_reply_is_valid(reply, option)) {
		log(LOG_ERR, "%s: invalid option reply", __func__);
		nbd_option_reply_dump(reply);
		return FAILURE;
	}

	if (reply->length == 0)
		return SUCCESS;

	if (datalen == 0)
		return MOREDATA;

	assert(data != NULL);

	recvlen = MIN(reply->length, datalen);

	while (true) {
		len = recv(sock, data, recvlen, MSG_WAITALL);
		if (client->disconnect)
			return FAILURE;
		if (len == -1 && errno == EINTR)
			continue;
		if (len != recvlen)
			goto connection_fail;
		break;
	}

	if (recvlen > datalen)
		return MOREDATA;

	return SUCCESS;

 connection_fail:
	log(LOG_ERR, "%s: connection failed: %m", __func__);
	return FAILURE;
}

static inline void
nbd_export_info_ntoh(struct nbd_export_info *info)
{

	info->size = be64toh(info->size);
	info->transmission_flags = be16toh(info->transmission_flags);
}

static int
nbd_client_recv_export_info(struct nbd_client *client,
			    struct nbd_export_info *info)
{
	static size_t const SHORT_INFO_LEN =
		sizeof *info - sizeof info->reserved;
	ssize_t len;
	int sock;

	sock = client->sock;

	while (true) {
		len = recv(sock, info, SHORT_INFO_LEN, MSG_WAITALL);
		if (client->disconnect)
			return FAILURE;
		if (len == -1 && errno == EINTR)
			continue;
		if (len != SHORT_INFO_LEN)
			goto connection_fail;
		break;
	}

	nbd_export_info_ntoh(info);

	client->size = info->size;
	client->flags |= info->transmission_flags;

	if ((client->flags >> 16) & NBD_FLAG_NO_ZEROES)
		return SUCCESS;

	while (true) {
		len = recv(sock, info + SHORT_INFO_LEN,
			   sizeof info->reserved, MSG_WAITALL);
		if (client->disconnect)
			return FAILURE;
		if (len == -1 && errno == EINTR)
			continue;
		if (len != sizeof info->reserved)
			goto connection_fail;
		break;
	}

	return SUCCESS;

 connection_fail:
	log(LOG_ERR, "%s: connection failed: %m", __func__);
	return FAILURE;
}

static int
nbd_client_negotiate_options_fixed_newstyle(struct nbd_client *client)
{
	struct nbd_option option;
	struct nbd_option_reply reply;
	struct nbd_export_info info;

	nbd_option_init(&option);

	nbd_option_set_option(&option, NBD_OPTION_EXPORT_NAME);
	if (nbd_client_send_option(client, &option, 0, NULL) == FAILURE) {
		log(LOG_ERR, "%s: sending option EXPORT_NAME failed",
		    __func__);
		return FAILURE;
	}
	if (nbd_client_recv_export_info(client, &info) == FAILURE) {
		log(LOG_ERR, "%s: receiving export info failed",
		    __func__);
		return FAILURE;
	}

	if (!(info.transmission_flags & NBD_FLAG_SEND_FLUSH))
		log(LOG_INFO, "%s: server does not support FLUSH command",
		    __func__);
	if (!(info.transmission_flags & NBD_FLAG_SEND_TRIM))
		log(LOG_INFO, "%s: server does not support TRIM command",
		    __func__);

	return SUCCESS;
}

static inline void
nbd_option_reply_server_ntoh(struct nbd_option_reply_server *server_export)
{

	server_export->length = be32toh(server_export->length);
}

static int
nbd_client_negotiate_list_fixed_newstyle(struct nbd_client *client)
{
	struct nbd_option option;
	struct nbd_option_reply reply;
	struct nbd_option_reply_server *server_export;
	size_t const BUFLEN = NAME_MAX;
	char *export_name, buf[BUFLEN + 1];
	size_t recvlen, remaining;
	ssize_t len;
	int rc;

	nbd_option_init(&option);
	nbd_option_set_option(&option, NBD_OPTION_LIST);
	if (nbd_client_send_option(client, &option, 0, NULL) == FAILURE) {
		log(LOG_ERR, "%s: sending option LIST failed", __func__);
		return FAILURE;
	}
	while (true) {
		rc = nbd_client_recv_option_reply(client, &option, &reply,
						  BUFLEN, (uint8_t *)buf);
		if (rc == FAILURE) {
			log(LOG_ERR,
			    "%s: receiving option LIST reply failed",
			    __func__);
			return FAILURE;
		}
		if (reply.type < 0) {
			char const *msg =
				nbd_option_reply_type_string(&reply);

			if (msg == NULL) {
				log(LOG_ERR, "%s: unknown server error (%d)",
				    __func__, reply.type);
				log(LOG_DEBUG, "\tbe32toh: %d",
				    be32toh(reply.type));
			} else {
				log(LOG_ERR, "%s: server error: %s",
				    __func__, msg);
			}

			nbd_option_reply_dump(&reply);

			return FAILURE;
		}

		if (reply.type == NBD_REPLY_ACK)
			break;

		assert(reply.type == NBD_REPLY_SERVER);

		server_export = (struct nbd_option_reply_server *)buf;
		nbd_option_reply_server_ntoh(server_export);
		if (server_export->length == 0) {
			printf("\t[default export]\n");
			continue;
		}

		recvlen = MIN(server_export->length, BUFLEN - 4);
		export_name = buf + sizeof *server_export;
		export_name[recvlen] = '\0';

		printf("\t%s", export_name);

		if (rc == SUCCESS) {
			printf("\n");
			continue;
		}

		remaining = server_export->length - BUFLEN;

		assert(remaining > 0);

		do {
			recvlen = MIN(remaining, BUFLEN);
			len = recv(client->sock, buf, recvlen, 0);
			if (client->disconnect)
				return FAILURE;
			if (len == -1 && errno == EINTR)
				continue;
			if (len != recvlen) {
				log(LOG_ERR, "%s: connection failed: %m",
				    __func__);
				return FAILURE;
			}
			buf[recvlen] = '\0';
			printf("%s", buf);
			remaining -= recvlen;
		} while (remaining > 0);

		printf("\n");
	}

	return SUCCESS;
}

struct handshake_magic {
	uint64_t magic;
	uint64_t style;
} __packed;

static inline void
handshake_magic_ntoh(struct handshake_magic *handshake)
{

	handshake->magic = be64toh(handshake->magic);
	handshake->style = be64toh(handshake->style);
}

static inline void
handshake_magic_dump(struct handshake_magic *handshake)
{

	log(LOG_DEBUG, "\tmagic: %#018lx (expected %#018lx)",
	    handshake->magic, NBD_MAGIC);
	log(LOG_DEBUG, "\tstyle: %#018lx (expected %#018lx or %#018lx)",
	    handshake->style, NBD_OLDSTYLE_MAGIC, NBD_NEWSTYLE_MAGIC);
}

int
nbd_client_negotiate(struct nbd_client *client)
{
	struct handshake_magic handshake;
	ssize_t len;
	int sock;

	sock = client->sock;

	while (true) {
		len = recv(sock, &handshake, sizeof handshake, MSG_PEEK);
		if (client->disconnect)
			return FAILURE;
		if (len == -1 && errno == EINTR)
			continue;
		if (len == -1) {
			log(LOG_ERR, "%s: connection failed: %m", __func__);
			return FAILURE;
		}
		if (len == sizeof handshake)
			break;
	}

	handshake_magic_ntoh(&handshake);

	if (handshake.magic != NBD_MAGIC) {
		log(LOG_ERR, "%s: handshake failed: invalid magic", __func__);
		return FAILURE;
	}

	if (handshake.style == NBD_OLDSTYLE_MAGIC) {

		log(LOG_INFO, "%s: oldstyle handshake detected", __func__);

		if (nbd_client_oldstyle_handshake(client) == FAILURE) {
			log(LOG_ERR, "%s: handshake failed", __func__);
			return FAILURE;
		}

		return SUCCESS;

	} else if (handshake.style == NBD_NEWSTYLE_MAGIC) {

		log(LOG_INFO, "%s: newstyle handshake detected", __func__);

		if (nbd_client_newstyle_handshake(client) == FAILURE) {
			log(LOG_ERR, "%s: handshake failed", __func__);
			return FAILURE;
		}

		if (nbd_client_negotiate_options_fixed_newstyle(client)
		    == FAILURE) {
			log(LOG_ERR, "%s: option negotiation failed",
			    __func__);
			return FAILURE;
		}

		return SUCCESS;

	}

	log(LOG_ERR, "%s: handshake failed: unknown style", __func__);
	handshake_magic_dump(&handshake);

	return FAILURE;
}

int
nbd_client_list(struct nbd_client *client)
{

	if (nbd_client_newstyle_handshake(client) == FAILURE) {
		log(LOG_ERR, "%s: handshake failed", __func__);
		return FAILURE;
	}

	if (nbd_client_negotiate_list_fixed_newstyle(client) == FAILURE) {
		log(LOG_ERR, "%s: server listing failed", __func__);
		return FAILURE;
	}

	return SUCCESS;
}

static inline void
nbd_request_init(struct nbd_request *request)
{

	memset(request, 0,  sizeof *request);
	request->magic = htobe32(NBD_REQUEST_MAGIC);
}

static inline void
nbd_request_set_flags(struct nbd_request *request, uint16_t flags)
{

	request->flags = htobe16(flags);
}

static inline void
nbd_request_set_command(struct nbd_request *request, uint16_t command)
{

	request->command = htobe16(command);
}

static inline void
nbd_request_set_handle(struct nbd_request *request, uint64_t handle)
{

	request->handle = htobe64(handle);
}

static inline void
nbd_request_set_offset(struct nbd_request *request, uint64_t offset)
{

	request->offset = htobe64(offset);
}

static inline void
nbd_request_set_length(struct nbd_request *request, uint32_t length)
{

	request->length = htobe32(length);
}

static int
nbd_client_send_request(struct nbd_client *client, uint16_t command,
			uint64_t handle, off_t offset, size_t length,
			size_t datalen, uint8_t *data)
{
	struct nbd_request request;
	size_t sendlen;
	ssize_t len;
	int sock;

	assert(offset + length <= client->size);

	sock = client->sock;

	nbd_request_init(&request);
	nbd_request_set_flags(&request, 0);
	nbd_request_set_command(&request, command);
	nbd_request_set_handle(&request, handle);
	nbd_request_set_offset(&request, offset);
	nbd_request_set_length(&request, length);

	len = send(sock, &request, sizeof request, MSG_NOSIGNAL);
	if (len != sizeof request) {
		log(LOG_ERR, "%s: failed to send request header", __func__);
		goto connection_fail;
	}

	if (datalen == 0)
		return SUCCESS;

	assert(data != NULL);

	sendlen = MIN(length, datalen);

	len = send(sock, data, sendlen, MSG_NOSIGNAL);
	if (len != sendlen) {
		log(LOG_ERR, "%s: failed to send request data", __func__);
		goto connection_fail;
	}

	if (sendlen < length)
		return MOREDATA;

	return SUCCESS;

 connection_fail:
	log(LOG_ERR, "%s: connection failed: %m", __func__);
	return FAILURE;
}

int
nbd_client_send_read(struct nbd_client *client, uint64_t handle,
		     off_t offset, size_t length)
{

	return nbd_client_send_request(client, NBD_CMD_READ, handle,
				       offset, length, 0, NULL);
}

int
nbd_client_send_write(struct nbd_client *client, uint64_t handle,
		      off_t offset, size_t length,
		      size_t datalen, uint8_t *data)
{

	return nbd_client_send_request(client, NBD_CMD_WRITE, handle,
				       offset, length, datalen, data);
}

int
nbd_client_send_flush(struct nbd_client *client, uint64_t handle)
{

	if (!(client->flags & NBD_FLAG_SEND_FLUSH)) {
		log(LOG_NOTICE, "%s: unsupported FLUSH operation", __func__);
		return EOPNOTSUPP;
	}

	return nbd_client_send_request(client, NBD_CMD_FLUSH, handle,
				       0, 0, 0, NULL);
}

int
nbd_client_send_trim(struct nbd_client *client, uint64_t handle,
		     off_t offset, size_t length)
{

	if (!(client->flags & NBD_FLAG_SEND_TRIM)) {
		log(LOG_NOTICE, "%s: unsupported TRIM operation", __func__);
		return EOPNOTSUPP;
	}

	return nbd_client_send_request(client, NBD_CMD_TRIM, handle,
				       offset, length, 0, NULL);
}

int
nbd_client_send_disconnect(struct nbd_client *client)
{

	return nbd_client_send_request(client, NBD_CMD_DISCONNECT,
				       (uint64_t)-1, 0, 0, 0, NULL);
}

static inline void
nbd_reply_ntoh(struct nbd_reply *reply)
{

	reply->magic = be32toh(reply->magic);
	reply->error = be32toh(reply->error);
	reply->handle = be64toh(reply->handle);
}

static inline bool
nbd_reply_is_valid(struct nbd_reply *reply)
{

	if (reply->magic != NBD_REPLY_MAGIC) {
		log(LOG_ERR, "%s: invalid magic: %#010x (expected %#010x)",
		    __func__, reply->magic, NBD_REPLY_MAGIC);
		return false;
	}

	return true;
}

static inline char const *
nbd_reply_error_string(struct nbd_reply *reply)
{

	switch (reply->error) {

#define CASE_MESSAGE(c) case c: return #c
#define EXTENSION " [unsupported extension]"

		CASE_MESSAGE(NBD_EPERM);
		CASE_MESSAGE(NBD_EIO);
		CASE_MESSAGE(NBD_ENOMEM);
		CASE_MESSAGE(NBD_EINVAL);
		CASE_MESSAGE(NBD_ENOSPC);
		CASE_MESSAGE(NBD_EOVERFLOW) EXTENSION;
		CASE_MESSAGE(NBD_ESHUTDOWN);

#undef EXTENSION
#undef CASE_MESSAGE

	default: return NULL;
	}
}

static inline void
nbd_reply_dump(struct nbd_reply *reply)
{
	char const *error = nbd_reply_error_string(reply);

	log(LOG_DEBUG, "\tmagic: %#010x", reply->magic);

	if (error == NULL)
		log(LOG_DEBUG, "\terror: [unknown] %#010x (%d)",
		    reply->error, reply->error);
	else
		log(LOG_DEBUG, "\terror: %s (%s)",
		    strerror(reply->error), error);

	log(LOG_DEBUG, "\thandle: %#018lx", reply->handle);
}

int
nbd_client_recv_reply_header(struct nbd_client *client, uint64_t *handle)
{
	struct nbd_reply reply;
	ssize_t len;
	int sock;

	sock = client->sock;

	while (true) {
		len = recv(sock, &reply, sizeof reply, MSG_WAITALL);
		if (client->disconnect)
			return FAILURE;
		if (len == -1 && errno == EINTR)
			continue;
		if (len != sizeof reply) {
			log(LOG_ERR, "%s: connection failed: %m", __func__);
			return FAILURE;
		}
		break;
	}

	nbd_reply_ntoh(&reply);

	if (!nbd_reply_is_valid(&reply)) {
		log(LOG_ERR, "%s: invalid reply", __func__);
		goto bad_reply;
	}

	switch (reply.error) {
	case SUCCESS:
		break;
	case NBD_EINVAL:
		log(LOG_WARNING, "%s: server replied invalid command usage",
		    __func__);
		return EINVAL;
	default:
		log(LOG_ERR, "%s: request error", __func__);
		goto bad_reply;
	}

	*handle = reply.handle;

	return SUCCESS;

 bad_reply:
	nbd_reply_dump(&reply);
	return FAILURE;
}

int
nbd_client_recv_reply_data(struct nbd_client *client, size_t length,
			   size_t buflen, uint8_t *buf)
{
	size_t recvlen;
	ssize_t len;
	int sock;

	if (length == 0)
		return SUCCESS;

	assert(buflen > 0);
	assert(buf != NULL);

	sock = client->sock;

	recvlen = MIN(length, buflen);

	while (true) {
		len = recv(sock, buf, recvlen, MSG_WAITALL);
		if (client->disconnect)
			return FAILURE;
		if (len == -1 && errno == EINTR)
			continue;
		if (len != recvlen) {
			log(LOG_ERR, "%s: connection failed: %m", __func__);
			return FAILURE;
		}
		break;
	}

	if (length > buflen)
		return MOREDATA;

	return SUCCESS;
}
