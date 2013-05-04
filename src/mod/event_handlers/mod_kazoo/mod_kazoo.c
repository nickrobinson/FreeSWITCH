/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2012, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Karl Anderson <karl@2600hz.com>
 * Darren Schreiber <darren@2600hz.com>
 *
 *
 * mod_kazoo.c -- Socket Controlled Event Handler
 *
 */
#include <switch.h>
#include <switch_apr.h>
#include <ei.h>
#include <apr_portable.h>
#include "mod_kazoo.h"

SWITCH_MODULE_LOAD_FUNCTION(mod_kazoo_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_kazoo_shutdown);
SWITCH_MODULE_RUNTIME_FUNCTION(mod_kazoo_runtime);
SWITCH_MODULE_DEFINITION(mod_kazoo, mod_kazoo_load, mod_kazoo_shutdown, mod_kazoo_runtime);

SWITCH_DECLARE_GLOBAL_STRING_FUNC(set_pref_ip, globals.ip);
SWITCH_DECLARE_GLOBAL_STRING_FUNC(set_pref_ei_cookie, globals.ei_cookie);
SWITCH_DECLARE_GLOBAL_STRING_FUNC(set_pref_ei_nodename, globals.ei_nodename);

static char *API_COMMANDS[] = {
	"listeners",
	"sessions",
	"bindings"
};

typedef enum {
    API_CMD_LISTENERS,
    API_CMD_SESSIONS,
	API_CMD_BINDINGS,
	API_CMD_MAX
} api_commands_t;

static switch_status_t find_api_command(const char *arg, int *command) {
	for (int i = 0; i < API_CMD_MAX; i++) {
		if(!strcmp(arg, API_COMMANDS[i])) {
			*command = i;
			return SWITCH_STATUS_SUCCESS;
		}
	}
	
	return SWITCH_STATUS_FALSE;
}

static switch_status_t api_erlang_listeners(switch_stream_handle_t *stream) {
	/*
	listener_t *listener;

	switch_thread_rwlock_rdlock(acceptor.listeners_lock);
	for (listener = acceptor.listeners; listener; listener = listener->next) {
		stream->write_function(stream, "Listener to %s with %d outbound sessions\n"
							   ,listener->peer_nodename, count_session_bindings(listener));
	}
	switch_thread_rwlock_unlock(acceptor.listeners_lock);
	*/
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t api_erlang_sessions(switch_stream_handle_t *stream) {
	/*
	listener_t *listener;

	switch_thread_rwlock_rdlock(acceptor.listeners_lock);
	for (listener = acceptor.listeners; listener; listener = listener->next) {
		stream->write_function(stream, "Listener to %s sessions\n", listener->peer_nodename);
		stream->write_function(stream, "-----------------------------------------------------------\n");
		display_session_bindings(listener, stream);
	}
	switch_thread_rwlock_unlock(acceptor.listeners_lock);
	*/
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t api_erlang_event(switch_stream_handle_t *stream) {
	/*
	listener_t *listener;

	switch_thread_rwlock_rdlock(acceptor.listeners_lock);
	for (listener = acceptor.listeners; listener; listener = listener->next) {
		stream->write_function(stream, "Listener to %s bindings\n", listener->peer_nodename);
		stream->write_function(stream, "-----------------------------------------------------------\n");
		display_fetch_bindings(listener, stream);
		display_event_bindings(listener, stream);
	}
	switch_thread_rwlock_unlock(acceptor.listeners_lock);
	*/
	return SWITCH_STATUS_SUCCESS;
}

static int read_cookie_from_file(char *filename) {
	int fd;
	char cookie[MAXATOMLEN + 1];
	char *end;
	struct stat buf;
	ssize_t res;

	if (!stat(filename, &buf)) {
		if ((buf.st_mode & S_IRWXG) || (buf.st_mode & S_IRWXO)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "%s must only be accessible by owner only.\n", filename);
			return 2;
		}
		if (buf.st_size > MAXATOMLEN) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "%s contains a cookie larger than the maximum atom size of %d.\n", filename, MAXATOMLEN);
			return 2;
		}
		fd = open(filename, O_RDONLY);
		if (fd < 1) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unable to open cookie file %s : %d.\n", filename, errno);
			return 2;
		}

		if ((res = read(fd, cookie, MAXATOMLEN)) < 1) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unable to read cookie file %s : %d.\n", filename, errno);
		}

		cookie[MAXATOMLEN] = '\0';

		/* replace any end of line characters with a null */
		if ((end = strchr(cookie, '\n'))) {
			*end = '\0';
		}

		if ((end = strchr(cookie, '\r'))) {
			*end = '\0';
		}

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Set cookie from file %s: %s\n", filename, cookie);

		set_pref_ei_cookie(cookie);
		return 0;
	} else {
		/* don't error here, because we might be blindly trying to read $HOME/.erlang.cookie, and that can fail silently */
		return 1;
	}
}

static switch_status_t config(void) {
	char *cf = "kazoo.conf";
	switch_xml_t cfg, xml, settings, param;

	if (!(xml = switch_xml_open_cfg(cf, &cfg, NULL))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to open configuration file %s\n", cf);
		return SWITCH_STATUS_FALSE;
	} else {
		if ((settings = switch_xml_child(cfg, "settings"))) {
			for (param = switch_xml_child(settings, "param"); param; param = param->next) {
				char *var = (char *) switch_xml_attr_soft(param, "name");
				char *val = (char *) switch_xml_attr_soft(param, "value");

				if (!strcmp(var, "listen-ip")) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Set bind ip address: %s\n", val);
					set_pref_ip(val);
				} else if (!strcmp(var, "cookie")) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Set cookie: %s\n", val);
					set_pref_ei_cookie(val);
				} else if (!strcmp(var, "cookie-file")) {
					if (read_cookie_from_file(val) == 1) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unable to read cookie from %s\n", val);
					}
				} else if (!strcmp(var, "nodename")) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Set node name: %s\n", val);
					set_pref_ei_nodename(val);
				} else if (!strcmp(var, "shortname")) {
					globals.ei_shortname = switch_true(val);
				} else if (!strcmp(var, "compat-rel")) {
					if (atoi(val) >= 7)
						globals.ei_compat_rel = atoi(val);
					else
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Invalid compatibility release '%s' specified\n", val);
				} else if (!strcmp(var, "nat-map")) {
					globals.nat_map = switch_true(val);
				}
			}
		}
		switch_xml_free(xml);
	}

	if (zstr(globals.ip)) {
		set_pref_ip("0.0.0.0");
	}

	if (zstr(globals.ei_cookie)) {
		int res;
		char *home_dir = getenv("HOME");
		char path_buf[1024];

		if (!zstr(home_dir)) {
			/* $HOME/.erlang.cookie */
			switch_snprintf(path_buf, sizeof (path_buf), "%s%s%s", home_dir, SWITCH_PATH_SEPARATOR, ".erlang.cookie");
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Checking for cookie at path: %s\n", path_buf);

			res = read_cookie_from_file(path_buf);
			if (res) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "No cookie or valid cookie file specified, using default cookie\n");
				set_pref_ei_cookie("ClueCon");
			}
		}
	}

	if (!globals.ei_nodename) {
		set_pref_ei_nodename("freeswitch");
	}

	if (!globals.nat_map) {
		globals.nat_map = 0;
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t create_acceptor() {
	switch_sockaddr_t *sa;
	uint16_t port;
    char ipbuf[25];
    const char *ip_addr;

	/* if the config has specified an erlang release compatibility then pass that along to the erlang interface */
	if (globals.ei_compat_rel) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Compatability with OTP R%d requested\n", globals.ei_compat_rel);
		ei_set_compat_rel(globals.ei_compat_rel);
	}

	if (!(globals.acceptor = create_socket(globals.pool))) {
		return SWITCH_STATUS_SOCKERR;
	}

	switch_socket_addr_get(&sa, SWITCH_FALSE, globals.acceptor);

	port = switch_sockaddr_get_port(sa);
    ip_addr = switch_get_addr(ipbuf, sizeof (ipbuf), sa);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Erlang connection acceptor listening on %s:%u\n", ip_addr, port);

	/* try to initialize the erlang interface */
	if (create_ei_cnode(ip_addr, globals.ei_nodename, &globals.ei_cnode) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_SOCKERR;
	}

	/* tell the erlang port manager where we can be reached.  this returns a file descriptor pointing to epmd or -1 */
	if ((globals.epmdfd = ei_publish(&globals.ei_cnode, port)) == -1) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
						  "Failed to publish port to epmd. Try starting it yourself or run an erl shell with the -sname or -name option.\n");
		return SWITCH_STATUS_SOCKERR;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Connected to epmd and published erlang cnode name %s at port %d\n", globals.ei_cnode.thisnodename, port);

	return SWITCH_STATUS_SUCCESS;
}


static ei_node_t *create_ei_node(int nodefd, ErlConnect *conn) {
	switch_memory_pool_t *pool = NULL;
	ei_node_t *ei_node;

	/* create memory pool for this erlang node */
	if (switch_core_new_memory_pool(&pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Out of memory: Too bad drinking scotch isn't a paying job or Kenny's dad would be a millionare!\n");
		return NULL;
	}
	
	/* from the erlang node's memory pool, allocate some memory for the structure */
	if (!(ei_node = switch_core_alloc(pool, sizeof (*ei_node)))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Out of memory: Stan, don't you know the first law of physics? Anything that's fun costs at least eight dollars.\n");
		return NULL;
	}

	memset(ei_node, 0, sizeof(*ei_node));

	/* store the location of our pool and reset the var we used to temporarily store that */
	ei_node->pool = pool;
	
	/* save the file descriptor that the erlang interface lib uses to communicate with the new node */
	ei_node->nodefd = nodefd;
	
	/* store the IP and node name we are talking with */
	switch_inet_ntop(AF_INET, conn->ipadr, ei_node->remote_ip, sizeof (ei_node->remote_ip));
	ei_node->peer_nodename = switch_core_strdup(ei_node->pool, conn->nodename);

	/* when we start we are running */
	switch_set_flag(ei_node, LFLAG_RUNNING);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "New erlang connection from node %s (%s)\n", ei_node->peer_nodename, ei_node->remote_ip);
	
	return ei_node;
}
 
SWITCH_STANDARD_API(exec_api_cmd)
{
	char *argv[1024] = { 0 };
	int command, argc = 0;
	char *mycmd = NULL;

	const char *usage_string = "USAGE:\n"
		"--------------------------------------------------------------------------------\n"
		"erlang listeners\n"
		"erlang sessions\n"
		"erlang bindings\n"
		"--------------------------------------------------------------------------------\n";

	if (zstr(cmd)) {
		stream->write_function(stream, "%s", usage_string);
		return SWITCH_STATUS_SUCCESS;
	}

	if (!(mycmd = strdup(cmd))) {
		return SWITCH_STATUS_MEMERR;
	}

	if (!(argc = switch_separate_string(mycmd, ' ', argv, (sizeof(argv) / sizeof(argv[0]))))) {
		stream->write_function(stream, "%s", usage_string);
		switch_safe_free(mycmd);
		return SWITCH_STATUS_SUCCESS;
	}

	if (find_api_command(argv[0], &command) != SWITCH_STATUS_SUCCESS) {
		stream->write_function(stream, "%s", usage_string);
		switch_safe_free(mycmd);
		return SWITCH_STATUS_SUCCESS;
	}

	switch(command) {
	case API_CMD_LISTENERS:
		api_erlang_listeners(stream);
		break;
	case API_CMD_SESSIONS:
		api_erlang_sessions(stream);
		break;
	case API_CMD_BINDINGS:
		api_erlang_event(stream);
		break;
	default:
		stream->write_function(stream, "%s", usage_string);
	}

	switch_safe_free(mycmd);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_LOAD_FUNCTION(mod_kazoo_load) {
	switch_api_interface_t *api_interface;

	memset(&globals, 0, sizeof(globals));

	globals.pool = pool;

	if(config() != SWITCH_STATUS_SUCCESS) {
		// TODO: what would we need to clean up here?
		return SWITCH_STATUS_TERM;
	}

	if(create_acceptor() != SWITCH_STATUS_SUCCESS) {
		// TODO: what would we need to clean up here
		close_socket(&globals.acceptor);
		return SWITCH_STATUS_TERM;
	}

	/* connect my internal structure to the blank pointer passed to me */
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	/* create an api for cli debug commands */
	SWITCH_ADD_API(api_interface, "erlang", "kazoo information", exec_api_cmd, "<command> [<args>]");
	switch_console_set_complete("add erlang listeners");
	switch_console_set_complete("add erlang sessions");
	switch_console_set_complete("add erlang bindings");

	switch_set_flag(&globals, LFLAG_RUNNING);

	/* indicate that the module should continue to be loaded */
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_kazoo_shutdown) {
	int sanity = 0;

	/* stop taking new requests and start shuting down the threads */
	switch_clear_flag(&globals, LFLAG_RUNNING);

	/* give everyone time to cleanly shutdown */
	while (switch_atomic_read(&globals.threads)) {
		switch_yield(100000);
		if (++sanity >= 200) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unable to kill all threads, continuing. Good luck!\n");
			break;
		}
	}

	/* close the connection to epmd and the acceptor */
	close_socketfd(&globals.epmdfd);
	close_socket(&globals.acceptor);

	/* Close the port we reserved for uPnP/Switch behind firewall, if necessary */
	//	if (globals.nat_map && switch_nat_get_type()) {
	//		switch_nat_del_mapping(globals.port, SWITCH_NAT_TCP);
	//	}

	/* clean up our allocated preferences */
	switch_safe_free(globals.ip);
	switch_safe_free(globals.ei_cookie);
	switch_safe_free(globals.ei_nodename);

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_RUNTIME_FUNCTION(mod_kazoo_runtime) {
	switch_os_socket_t os_socket;
		
	switch_atomic_inc(&globals.threads);

	switch_os_sock_get(&os_socket, globals.acceptor);

	while (switch_test_flag(&globals, LFLAG_RUNNING)) {
		ei_node_t *ei_node;
		int nodefd;
		ErlConnect conn;

		/* zero out errno because ei_accept doesn't differentiate between a */
		/* failed authentication or a socket failure, or a client version */
		/* mismatch or a godzilla attack (and a godzilla attack is highly likely) */
		errno = 0;

		/* wait here for an erlang node to connect, timming out to check if our module is still running every now-and-again */
		if ((nodefd = ei_accept_tmo(&globals.ei_cnode, (int) os_socket, &conn, 500)) == ERL_ERROR) {
			if (erl_errno == ETIMEDOUT) {
				continue;
			} else if (errno) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Erlang connection acceptor socket error %d %d\n", erl_errno, errno);
			} else {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						"Erlang node connection failed - ensure your cookie matches '%s' and you are using a good nodename\n", globals.ei_cookie);
			}
			continue;
		}

		if (!switch_test_flag(&globals, LFLAG_RUNNING)) {
			break;
		}

		/* NEW ERLANG NODE CONNECTION! Hello friend! */
		if ((ei_node = create_ei_node(nodefd, &conn))) {
			/* Go do some real work - start the threads for this erlang node! */
			launch_node_handler(ei_node); 
		}
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Erlang connection acceptor shut down\n");

	switch_atomic_dec(&globals.threads);

	return SWITCH_STATUS_TERM;
}


/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4:
 */
