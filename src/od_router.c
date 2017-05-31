
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
#include <inttypes.h>
#include <signal.h>

#include <machinarium.h>
#include <soprano.h>

#include "od_macro.h"
#include "od_version.h"
#include "od_list.h"
#include "od_pid.h"
#include "od_syslog.h"
#include "od_log.h"
#include "od_daemon.h"
#include "od_scheme.h"
#include "od_lex.h"
#include "od_config.h"
#include "od_msg.h"
#include "od_system.h"
#include "od_instance.h"

#include "od_server.h"
#include "od_server_pool.h"
#include "od_client.h"
#include "od_client_pool.h"
#include "od_route_id.h"
#include "od_route.h"
#include "od_route_pool.h"
#include "od_io.h"

#include "od_pooler.h"
#include "od_relay.h"
#include "od_router.h"
#include "od_frontend.h"
#include "od_backend.h"
#include "od_cancel.h"
#include "od_periodic.h"

typedef struct
{
	od_routerstatus_t status;
	od_client_t *client;
	machine_queue_t response;
} od_msgrouter_t;

static od_route_t*
od_router_fwd(od_router_t *router, so_bestartup_t *startup)
{
	od_instance_t *instance = router->system->instance;

	assert(startup->database != NULL);
	assert(startup->user != NULL);

	/* match required route according to scheme */
	od_schemeroute_t *route_scheme;
	route_scheme =
		od_schemeroute_match(&instance->scheme,
		                     so_parameter_value(startup->database));
	if (route_scheme == NULL) {
		/* try to use default route */
		route_scheme = instance->scheme.routing_default;
		if (route_scheme == NULL)
			return NULL;
	}
	od_routeid_t id = {
		.database     = so_parameter_value(startup->database),
		.database_len = startup->database->value_len,
		.user         = so_parameter_value(startup->user),
		.user_len     = startup->user->value_len
	};

	/* force settings required by route */
	if (route_scheme->database) {
		id.database = route_scheme->database;
		id.database_len = strlen(route_scheme->database) + 1;
	}
	if (route_scheme->user) {
		id.user = route_scheme->user;
		id.user_len = strlen(route_scheme->user) + 1;
	}

	/* match or create dynamic route */
	od_route_t *route;
	route = od_routepool_match(&router->route_pool, &id);
	if (route)
		return route;
	route = od_routepool_new(&router->route_pool, route_scheme, &id);
	if (route == NULL) {
		od_error(&instance->log, "failed to allocate route");
		return NULL;
	}
	return route;
}

static inline void
od_router_attacher(void *arg)
{
	machine_msg_t msg = arg;

	od_msgrouter_t *msg_attach;
	msg_attach = machine_msg_get_data(msg);

	od_client_t *client;
	client = msg_attach->client;

	od_route_t *route;
	route = client->route;
	assert(route != NULL);

	od_server_t *server;
	server = od_serverpool_next(&route->server_pool, OD_SIDLE);
	if (server)
		goto on_connect;

	/* TODO: wait */

	od_router_t *router;
	router = client->system->router;

	/* create new backend connection */
	server = od_backend_new(router, route);
	if (server == NULL) {
		msg_attach->status = OD_RERROR;
		machine_queue_put(msg_attach->response, msg);
		return;
	}
	server->id = router->server_seq++;

	/* detach server io from router context */
	machine_io_detach(server->io);

on_connect:
	od_serverpool_set(&route->server_pool, server, OD_SACTIVE);
	od_clientpool_set(&route->client_pool, client, OD_CACTIVE);
	msg_attach->status = OD_ROK;
	client->server = server;
	server->idle_time = 0;
	machine_queue_put(msg_attach->response, msg);
}

static inline void
od_router(void *arg)
{
	od_router_t *router = arg;
	od_instance_t *instance = router->system->instance;

	od_log(&instance->log, "(router) started");

	/* start periodic task coroutine */
	int64_t coroutine_id;
	coroutine_id = machine_coroutine_create(od_periodic, router);
	if (coroutine_id == -1) {
		od_error(&instance->log, "failed to create periodic coroutine");
		return;
	}

	for (;;)
	{
		machine_msg_t msg;
		msg = machine_queue_get(router->queue, UINT32_MAX);
		if (msg == NULL)
			break;

		od_msg_t msg_type;
		msg_type = machine_msg_get_type(msg);
		switch (msg_type) {
		case OD_MROUTER_ROUTE:
		{
			/* attach client to route */
			od_msgrouter_t *msg_route;
			msg_route = machine_msg_get_data(msg);

			od_route_t *route;
			route = od_router_fwd(router, &msg_route->client->startup);
			if (route == NULL) {
				msg_route->status = OD_RERROR_NOT_FOUND;
				machine_queue_put(msg_route->response, msg);
				break;
			}

			/* ensure route client_max limit */
			int client_total;
			client_total = od_clientpool_total(&route->client_pool);
			if (client_total >= route->scheme->client_max) {
				od_log(&instance->log,
				       "(router) route '%s' client_max reached (%d)",
				       route->scheme->target,
				       route->scheme->client_max);
				msg_route->status = OD_RERROR_LIMIT;
				machine_queue_put(msg_route->response, msg);
				break;
			}

			/* add client to route client pool */
			od_clientpool_set(&route->client_pool, msg_route->client, OD_CPENDING);
			msg_route->client->route = route;

			msg_route->status = OD_ROK;
			machine_queue_put(msg_route->response, msg);
			break;
		}

		case OD_MROUTER_ATTACH:
		{
			/* get client server from route server pool */
			od_msgrouter_t *msg_attach;
			msg_attach = machine_msg_get_data(msg);

			coroutine_id = machine_coroutine_create(od_router_attacher, msg);
			if (coroutine_id == -1) {
				msg_attach->status = OD_RERROR;
				machine_queue_put(msg_attach->response, msg);
				break;
			}
			break;
		}

		case OD_MROUTER_DETACH:
		{
			/* push client server back to route server pool */
			od_msgrouter_t *msg_detach;
			msg_detach = machine_msg_get_data(msg);

			od_client_t *client = msg_detach->client;
			od_route_t *route = client->route;
			od_server_t *server = client->server;
			client->server = NULL;
			od_serverpool_set(&route->server_pool, server, OD_SIDLE);
			od_clientpool_set(&route->client_pool, client, OD_CPENDING);

			/* todo: wakeup attachers */

			msg_detach->status = OD_ROK;
			machine_queue_put(msg_detach->response, msg);
			break;
		}

		case OD_MROUTER_DETACH_AND_UNROUTE:
		{
			/* push client server back to route server pool,
			 * unroute client */
			od_msgrouter_t *msg_detach;
			msg_detach = machine_msg_get_data(msg);

			od_client_t *client = msg_detach->client;
			od_route_t *route = client->route;
			od_server_t *server = client->server;
			client->server = NULL;
			od_serverpool_set(&route->server_pool, server, OD_SIDLE);
			client->route = NULL;
			od_clientpool_set(&route->client_pool, client, OD_CUNDEF);

			/* todo: wakeup attachers */

			msg_detach->status = OD_ROK;
			machine_queue_put(msg_detach->response, msg);
			break;
		}

		case OD_MROUTER_CLOSE_AND_UNROUTE:
		{
			/* detach and close server connection,
			 * unroute client */
			od_msgrouter_t *msg_close;
			msg_close = machine_msg_get_data(msg);

			od_client_t *client = msg_close->client;
			od_route_t *route = client->route;
			od_server_t *server = client->server;
			client->server = NULL;
			od_serverpool_set(&route->server_pool, server, OD_SUNDEF);

			/* remove client from route client pool */
			od_clientpool_set(&route->client_pool, client, OD_CUNDEF);
			client->route = NULL;

			od_backend_terminate(server);
			od_backend_close(server);

			msg_close->status = OD_ROK;
			machine_queue_put(msg_close->response, msg);
			break;
		}

		case OD_MROUTER_CANCEL:
		{
			/* match server by client key and initiate
			 * cancel request connection */
			od_msgrouter_t *msg_cancel;
			msg_cancel = machine_msg_get_data(msg);
			int rc;
			rc = od_cancel_match(instance, &router->route_pool,
			                     &msg_cancel->client->startup.key);
			if (rc == -1)
				msg_cancel->status = OD_RERROR;
			else
				msg_cancel->status = OD_ROK;
			machine_queue_put(msg_cancel->response, msg);
			break;
		}
		default:
			assert(0);
			break;
		}
	}

}

int od_router_init(od_router_t *router, od_system_t *system)
{
	od_instance_t *instance = system->instance;
	od_routepool_init(&router->route_pool);
	router->machine = -1;
	router->system = system;
	router->server_seq = 0;
	router->queue = machine_queue_create();
	if (router->queue == NULL) {
		od_error(&instance->log, "failed to create router queue");
		return -1;
	}
	return 0;
}

int od_router_start(od_router_t *router)
{
	od_instance_t *instance = router->system->instance;
	router->machine = machine_create("router", od_router, router);
	if (router->machine == -1) {
		od_error(&instance->log, "failed to start router");
		return 1;
	}
	return 0;
}

static od_routerstatus_t
od_router_do(od_client_t *client, od_msg_t msg_type, int wait_for_response)
{
	od_router_t *router = client->system->router;

	/* send request to router */
	machine_msg_t msg;
	msg = machine_msg_create(msg_type, sizeof(od_msgrouter_t));
	if (msg == NULL)
		return OD_RERROR;
	od_msgrouter_t *msg_route;
	msg_route = machine_msg_get_data(msg);
	msg_route->status = OD_RERROR;
	msg_route->client = client;
	msg_route->response = NULL;

	/* create response queue */
	machine_queue_t response;
	if (wait_for_response) {
		response = machine_queue_create();
		if (response == NULL) {
			machine_msg_free(msg);
			return OD_RERROR;
		}
		msg_route->response = response;
	}
	machine_queue_put(router->queue, msg);

	if (! wait_for_response)
		return OD_ROK;

	/* wait for reply */
	msg = machine_queue_get(response, UINT32_MAX);
	if (msg == NULL) {
		/* todo:  */
		abort();
		machine_queue_free(response);
		return OD_RERROR;
	}
	msg_route = machine_msg_get_data(msg);
	od_routerstatus_t status;
	status = msg_route->status;
	machine_queue_free(response);
	machine_msg_free(msg);
	return status;
}

od_routerstatus_t
od_route(od_client_t *client)
{
	return od_router_do(client, OD_MROUTER_ROUTE, 1);
}

od_routerstatus_t
od_router_attach(od_client_t *client)
{
	od_routerstatus_t status;
	status = od_router_do(client, OD_MROUTER_ATTACH, 1);
	if (client->server) {
		/* attach server io to clients machine context */
		machine_io_attach(client->server->io);
	}
	return status;
}

od_routerstatus_t
od_router_detach(od_client_t *client)
{
	/* detach server io from clients machine context */
	machine_io_detach(client->server->io);
	return od_router_do(client, OD_MROUTER_DETACH, 1);
}

od_routerstatus_t
od_router_detach_and_unroute(od_client_t *client)
{
	/* detach server io from clients machine context */
	machine_io_detach(client->server->io);
	return od_router_do(client, OD_MROUTER_DETACH_AND_UNROUTE, 1);
}

od_routerstatus_t
od_router_close_and_unroute(od_client_t *client)
{
	/* detach server io from clients machine context */
	machine_io_detach(client->server->io);
	return od_router_do(client, OD_MROUTER_CLOSE_AND_UNROUTE, 1);
}

od_routerstatus_t
od_router_cancel(od_client_t *client)
{
	return od_router_do(client, OD_MROUTER_CANCEL, 1);
}
