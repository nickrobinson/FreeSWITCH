#define MAX_ACL 100
#define CMD_BUFLEN 1024 * 1000
#define MAX_QUEUE_LEN 25000
#define MAX_MISSED 500
#define MAX_PID_CHARS 255

typedef enum {
	LFLAG_READY = (1 << 0),
	LFLAG_RUNNING = (1 << 1),
	LFLAG_EVENTS = (1 << 2),
	LFLAG_LOG = (1 << 3),
	LFLAG_FULL = (1 << 4),
	LFLAG_MYEVENTS = (1 << 5),
	LFLAG_SESSION = (1 << 6),
	LFLAG_ASYNC = (1 << 7),
	LFLAG_STATEFUL = (1 << 8),
	LFLAG_OUTBOUND = (1 << 9),
	LFLAG_LINGER = (1 << 10),
	LFLAG_HANDLE_DISCO = (1 << 11),
	LFLAG_CONNECTED = (1 << 12),
	LFLAG_RESUME = (1 << 13),
	LFLAG_AUTH_EVENTS = (1 << 14),
	LFLAG_ALL_EVENTS_AUTHED = (1 << 15),
	LFLAG_ALLOW_LOG = (1 << 16)
} event_flag_t;

typedef enum {
	ERLANG_STRING = 0,
	ERLANG_BINARY
} erlang_encoding_t;

struct ei_event_binding_s {
	char id[SWITCH_UUID_FORMATTED_LENGTH + 1];
	switch_event_node_t *node;
	switch_event_types_t type;
	const char *subclass_name;
	struct ei_event_binding_s *next;
};
typedef struct ei_event_binding_s ei_event_binding_t;

struct ei_event_bindings_s {
	switch_memory_pool_t *pool;
	ei_event_binding_t *binding;
	switch_queue_t *queue;
	switch_socket_t *acceptor;
	switch_pollset_t *pollset;
    switch_pollfd_t *pollfd;
	switch_socket_t *socket;
	erlang_pid pid;
	uint32_t flags;
	struct ei_event_bindings_s *next;
};
typedef struct ei_event_bindings_s ei_event_bindings_t;

struct ei_node_s {
	int nodefd;
	switch_memory_pool_t *pool;
	ei_event_bindings_t *event_bindings;
	char remote_ip[50];
	char *peer_nodename;
	uint32_t flags;
	struct ei_node_s *next;
};
typedef struct ei_node_s ei_node_t;

struct xml_fetch_msg_s {
	switch_xml_t xml;
	switch_mutex_t *mutex;
    switch_thread_cond_t *response_available;
	char uuid_str[SWITCH_UUID_FORMATTED_LENGTH + 1];
	const char *section;
	const char *tag_name;
	const char *key_name;
	const char *key_value;
	switch_event_t *params;	
};
typedef struct xml_fetch_msg_s xml_fetch_msg_t;

/*
struct nat_map_s {
	int nat_map;
	struct nat_map_s *next;
};
typedef struct nat_map_s nat_map_t;
*/

struct globals_s {
	switch_memory_pool_t *pool;
	switch_atomic_t threads;
	switch_socket_t *acceptor;
	struct ei_cnode_s ei_cnode;
	int epmdfd;
	switch_bool_t nat_map;
	switch_bool_t ei_shortname;
	int ei_compat_rel;
	char *ip;
	char *ei_cookie;
	char *ei_nodename;
	uint32_t flags;
} globals;
typedef struct globals_s globals_t;

/* kazoo_node.c */
void launch_node_handler(ei_node_t *ei_node);
ei_node_t *create_node_handler(int nodefd, ErlConnect *conn);

/* kazoo_event_bindings.c */
switch_status_t add_event_bindings(ei_node_t *ei_node, erlang_pid *from, switch_event_types_t event_type, const char *subclass_name);
switch_status_t remove_event_bindings(ei_node_t *ei_node, erlang_pid *from, switch_event_types_t event_type, const char *subclass_name);
switch_status_t remove_pid_event_bindings(ei_node_t *ei_node, erlang_pid *from);

/* kazoo_utils.c */
void close_socket(switch_socket_t **sock);
void close_socketfd(int *sockfd);
switch_socket_t *create_socket(switch_memory_pool_t *pool);
switch_status_t create_ei_cnode(const char *ip_addr, const char *name, struct ei_cnode_s *ei_cnode);

void ei_link(ei_node_t *ei_node, erlang_pid * from, erlang_pid * to);
void ei_encode_switch_event_headers(ei_x_buff * ebuf, switch_event_t *event);
void ei_encode_switch_event_tag(ei_x_buff * ebuf, switch_event_t *event, char *tag);
int ei_pid_from_rpc(struct ei_cnode_s *ec, int sockfd, erlang_ref * ref, char *module, char *function);
void ei_x_print_reg_msg(ei_x_buff * buf, char *dest, int send);
void ei_x_print_msg(ei_x_buff * buf, erlang_pid * pid, int send);
int ei_helper_send(ei_node_t *ei_node, erlang_pid* to, ei_x_buff *buf);
void ei_hash_ref(erlang_ref * ref, char *output);
int ei_compare_pids(erlang_pid * pid1, erlang_pid * pid2);
int ei_decode_atom_safe(char *buf, int *index, char *dst);
int ei_decode_string_or_binary_limited(char *buf, int *index, int maxsize, char *dst);
int ei_decode_string_or_binary(char *buf, int *index, char **dst);

#define ei_encode_switch_event(_b, _e) ei_encode_switch_event_tag(_b, _e, "event");
#define _ei_x_encode_string(buf, string) { ei_x_encode_binary(buf, string, strlen(string)); }

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
