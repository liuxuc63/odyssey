
/*
 * odissey.
 *
 * PostgreSQL connection pooler and request router.
*/

#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <machinarium.h>
#include <soprano.h>

#include "od_macro.h"
#include "od_list.h"
#include "od_pid.h"
#include "od_syslog.h"
#include "od_log.h"
#include "od_scheme.h"
#include "od_lex.h"
#include "od_config.h"
#include "od_server.h"
#include "od_server_pool.h"
#include "od_route_id.h"
#include "od_route.h"
#include "od_route_pool.h"
#include "od_client.h"
#include "od_client_pool.h"
#include "od.h"
#include "od_io.h"
#include "od_pooler.h"
#include "od_router.h"
#include "od_router_session.h"
#include "od_cancel.h"
#include "od_fe.h"
#include "od_be.h"

od_routerstatus_t
od_router_session(od_client_t *client)
{
	od_pooler_t *pooler = client->pooler;
	int rc, type;

	/* client routing */
	od_route_t *route = od_route(pooler, &client->startup);
	if (route == NULL) {
		od_error(&pooler->od->log, client->io,
		         "C: database route '%s' is not declared",
		         client->startup.database);
		return OD_RS_EROUTE;
	}
	/* ensure client_max limit per route */
	if (route->client_count >= route->scheme->client_max) {
		od_log(&pooler->od->log, client->io,
		       "C: route '%s' client_max reached (%d), closing connection",
		       route->scheme->target,
		       route->scheme->client_max);
		return OD_RS_ELIMIT;
	}
	client->route = route;
	client->route->client_count++;

	/* get server connection for the route */
	od_server_t *server = od_bepop(pooler, route);
	if (server == NULL)
		return OD_RS_EPOOL;

	/* assign client session key */
	server->key_client = client->key;
	client->server = server;

	od_debug(&pooler->od->log, client->io,
	         "C: route to %s server", route->scheme->server->name);

	so_stream_t *stream = &client->stream;
	for (;;)
	{
		/* client to server */
		rc = od_read(client->io, stream, 0);
		if (rc == -1)
			return OD_RS_ECLIENT_READ;
		type = *stream->s;
		od_debug(&pooler->od->log, client->io, "C: %c", *stream->s);

		/* client graceful shutdown */
		if (type == 'X')
			break;

		rc = od_write(server->io, stream);
		if (rc == -1)
			return OD_RS_ESERVER_WRITE;

		server->count_request++;

		for (;;) {
			/* read server reply */
			for (;;) {
				rc = od_read(server->io, stream, 1000);
				if (rc == 0)
					break;
				/* client watchdog.
				 *
				 * ensure that client has not closed
				 * the connection */
				if (! mm_read_is_timeout(server->io))
					return OD_RS_ESERVER_READ;
				if (mm_is_connected(client->io))
					continue;
				od_debug(&pooler->od->log, server->io,
				         "S (watchdog): client disconnected");
				return OD_RS_ECLIENT_READ;
			}
			type = *stream->s;
			od_debug(&pooler->od->log, server->io, "S: %c", type);

			if (type == 'Z')
				od_beset_ready(server, stream);

			/* transmit reply to client */
			rc = od_write(client->io, stream);
			if (rc == -1)
				return OD_RS_ECLIENT_WRITE;

			if (type == 'Z')
				break;
		}
	}

	return OD_RS_OK;
}
