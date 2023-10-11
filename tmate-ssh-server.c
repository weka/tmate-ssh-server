#include <libssh/libssh.h>
#include <libssh/server.h>
#include <libssh/callbacks.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <sys/wait.h>
#include <ctype.h>
#include <stdio.h>
#include <signal.h>
#include <event.h>
#include <arpa/inet.h>
#ifndef IPPROTO_TCP
#include <netinet/in.h>
#endif

#include "tmate.h"

char *get_ssh_conn_string(const char *session_token)
{
	char port_arg[16] = {0};
	char *ret;

	int ssh_port_advertized = tmate_settings->ssh_port_advertized == -1 ?
		tmate_settings->ssh_port :
		tmate_settings->ssh_port_advertized;

	if (ssh_port_advertized != 22)
		sprintf(port_arg, " -p%d", ssh_port_advertized);
	xasprintf(&ret, "ssh%s %s@%s", port_arg, session_token, tmate_settings->tmate_host);
	return ret;
}

static int pty_request(__unused ssh_session session,
		       __unused ssh_channel channel,
		       __unused const char *term,
		       int width, int height,
		       __unused int pxwidth, __unused int pwheight,
		       void *userdata)
{
	struct tmate_ssh_client *client = userdata;

	client->winsize_pty.ws_col = width;
	client->winsize_pty.ws_row = height;

	return 0;
}

static int shell_request(__unused ssh_session session,
			 __unused ssh_channel channel,
			 void *userdata)
{
	struct tmate_ssh_client *client = userdata;

	if (client->role)
		return 1;

	client->role = TMATE_ROLE_PTY_CLIENT;

	return 0;
}

static int subsystem_request(__unused ssh_session session,
			     __unused ssh_channel channel,
			     const char *subsystem, void *userdata)
{
	struct tmate_ssh_client *client = userdata;

	if (client->role)
		return 1;

	if (!strcmp(subsystem, "tmate"))
		client->role = TMATE_ROLE_DAEMON;

	return 0;
}

static int exec_request(__unused ssh_session session,
			__unused ssh_channel channel,
			const char *command, void *userdata)
{
	struct tmate_ssh_client *client = userdata;

	if (client->role)
		return 1;

	if (!tmate_has_websocket())
		return 1;

	client->role = TMATE_ROLE_EXEC;
	client->exec_command = xstrdup(command);

	return 0;
}

static ssh_channel channel_open_request_cb(ssh_session session, void *userdata)
{
	struct tmate_ssh_client *client = userdata;

	if (!client->username) {
		/* The authentication did not go through yet */
		return NULL;
	}

	if (client->channel) {
		/*
		 * We already have a channel, and we don't support multi
		 * channels yet. Returning NULL means the channel request will
		 * be denied.
		 */
		return NULL;
	}

	client->channel = ssh_channel_new(session);
	if (!client->channel)
		tmate_fatal("Error getting channel");

	memset(&client->channel_cb, 0, sizeof(client->channel_cb));
	ssh_callbacks_init(&client->channel_cb);
	client->channel_cb.userdata = client;
	client->channel_cb.channel_pty_request_function = pty_request;
	client->channel_cb.channel_shell_request_function = shell_request;
	client->channel_cb.channel_subsystem_request_function = subsystem_request;
	client->channel_cb.channel_exec_request_function = exec_request;
	ssh_set_channel_callbacks(client->channel, &client->channel_cb);

	return client->channel;
}

static int auth_pubkey_cb(__unused ssh_session session,
			  const char *user,
			  ssh_key pubkey,
			  char signature_state, void *userdata)
{
	struct tmate_ssh_client *client = userdata;

	switch (signature_state) {
	case SSH_PUBLICKEY_STATE_VALID:
		client->username = xstrdup(user);

		const char *key_type = ssh_key_type_to_char(ssh_key_type(pubkey));

		char *b64_key;
		if (ssh_pki_export_pubkey_base64(pubkey, &b64_key) != SSH_OK)
			tmate_fatal("error getting public key");

		char *pubkey64;
		xasprintf(&pubkey64, "%s %s", key_type, b64_key);
		free(b64_key);

		if (!would_tmate_session_allow_auth(user, pubkey64)) {
			free(pubkey64);
			return SSH_AUTH_DENIED;
		}

		client->pubkey = pubkey64;

		return SSH_AUTH_SUCCESS;
	case SSH_PUBLICKEY_STATE_NONE:
		return SSH_AUTH_SUCCESS;
	default:
		return SSH_AUTH_DENIED;
	}
}

static int auth_none_cb(__unused ssh_session session, const char *user, void *userdata)
{
	struct tmate_ssh_client *client = userdata;

	if (!would_tmate_session_allow_auth(user, NULL))
		return SSH_AUTH_DENIED;

	client->username = xstrdup(user);
	client->pubkey = NULL;

	return SSH_AUTH_SUCCESS;
}

static struct ssh_server_callbacks_struct ssh_server_cb = {
	.auth_pubkey_function = auth_pubkey_cb,
	.auth_none_function = auth_none_cb,
	.channel_open_request_session_function = channel_open_request_cb,
};

static void on_ssh_read(__unused evutil_socket_t fd, __unused short what, void *arg)
{
	struct tmate_ssh_client *client = arg;
	ssh_execute_message_callbacks(client->session);

	if (!ssh_is_connected(client->session)) {
		tmate_debug("ssh disconnected");

		event_del(&client->ev_ssh);

		/* For graceful tmux client termination */
		request_server_termination();
	}
}

static void register_on_ssh_read(struct tmate_ssh_client *client)
{
	event_set(&client->ev_ssh, ssh_get_fd(client->session),
		  EV_READ | EV_PERSIST, on_ssh_read, client);
	event_add(&client->ev_ssh, NULL);
}

static void handle_sigalrm(__unused int sig)
{
	tmate_fatal_quiet("Connection grace period (%ds) passed", TMATE_SSH_GRACE_PERIOD);
}

static void client_bootstrap(struct tmate_session *_session)
{
	struct tmate_ssh_client *client = &_session->ssh_client;
	long grace_period = TMATE_SSH_GRACE_PERIOD;
	ssh_event mainloop;
	ssh_session session = client->session;

	tmate_debug("Bootstrapping ssh client ip=%s", client->ip_address);

	_session->ev_base = osdep_event_init();

	/* new process group, we don't want to die with our parent (upstart) */
	setpgid(0, 0);

	{
	int flag = 1;
	int fd = ssh_get_fd(session);
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
	flag = 0x10;  /* IPTOS_LOWDELAY */
	setsockopt(fd, IPPROTO_IP, IP_TOS, &flag, sizeof(flag));
	}

	/*
	 * We should die early if we can't connect to websocket server. This
	 * way the tmate daemon will pick another server to work on.
	 */
	_session->websocket_fd = -1;
	if (tmate_has_websocket())
		_session->websocket_fd = tmate_connect_to_websocket();

	ssh_server_cb.userdata = client;
	ssh_callbacks_init(&ssh_server_cb);
	ssh_set_server_callbacks(client->session, &ssh_server_cb);

	ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &grace_period);
	ssh_options_set(session, SSH_OPTIONS_COMPRESSION, "yes");

	ssh_set_auth_methods(client->session, SSH_AUTH_METHOD_NONE |
					      SSH_AUTH_METHOD_PUBLICKEY);

	tmate_debug("Exchanging DH keys");
	if (ssh_handle_key_exchange(session) < 0)
		tmate_fatal_quiet("Error doing the key exchange: %s", ssh_get_error(session));

	mainloop = ssh_event_new();
	ssh_event_add_session(mainloop, session);

	while (!client->role) {
		if (ssh_event_dopoll(mainloop, -1) == SSH_ERROR)
			tmate_fatal_quiet("Error polling ssh socket: %s", ssh_get_error(session));
	}

	alarm(0);

	/* The latency callback is set later */
	start_keepalive_timer(client, TMATE_SSH_KEEPALIVE_SEC * 1000);
	register_on_ssh_read(client);

	switch (client->role) {
	case TMATE_ROLE_DAEMON:		tmate_spawn_daemon(_session);		break;
	case TMATE_ROLE_PTY_CLIENT:	tmate_spawn_pty_client(_session);	break;
	case TMATE_ROLE_EXEC:		tmate_spawn_exec(_session);		break;
	}
	/* never reached */
}

static int get_client_ip_socket(int fd, char *dst, size_t len)
{
	struct sockaddr sa;
	socklen_t sa_len = sizeof(sa);

	if (getpeername(fd, &sa, &sa_len) < 0)
		return -1;


	switch (sa.sa_family) {
	case AF_INET:
		if (!inet_ntop(AF_INET, &((struct sockaddr_in *)&sa)->sin_addr,
			       dst, len))
			return -1;
		break;
	case AF_INET6:
		if (!inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&sa)->sin6_addr,
			       dst, len))
			return -1;
		break;
	default:
		return -1;
	}

	return 0;
}

static int read_single_line(int fd, char *dst, size_t len)
{
	/*
	 * This reads exactly one line from fd.
	 * We cannot read bytes after the new line.
	 * We could use recv() with MSG_PEEK to do this more efficiently.
	 */
	for (size_t i = 0; i < len; i++) {
		if (read(fd, &dst[i], 1) <= 0)
			break;

		if (dst[i] == '\r')
			i--;

		if (dst[i] == '\n') {
			dst[i] = '\0';
			return i;
		}
	}

	return -1;
}

static int get_client_ip_proxy_protocol(int fd, char *dst, size_t len)
{
	char header[110];
	int tok_num;

#define SIGNATURE "PROXY "
	ssize_t ret = read(fd, header, sizeof(SIGNATURE)-1);
	if (ret <= 0)
		tmate_fatal_quiet("Disconnected, health checker?");
	if (ret != sizeof(SIGNATURE)-1)
		return -1;
	if (memcmp(header, SIGNATURE, sizeof(SIGNATURE)-1))
		return -1;
#undef SIGNATURE

	if (read_single_line(fd, header, sizeof(header)) < 0)
		return -1;

	tmate_debug("proxy header: %s", header);

	tok_num = 0;
	for (char *tok = strtok(header, " "); tok; tok = strtok(NULL, " "), tok_num++) {
		if (tok_num == 1)
			strlcpy(dst, tok, len);
	}

	if (tok_num != 5)
		return -1;

	return 0;
}

static int get_client_ip(int fd, char *dst, size_t len)
{
	if (tmate_settings->use_proxy_protocol)
		return get_client_ip_proxy_protocol(fd, dst, len);
	else
		return get_client_ip_socket(fd, dst, len);
}

static void ssh_log_function(int priority, const char *function,
			     const char *buffer, __unused void *userdata)
{
	/* loglevel already applied */
	log_emit(LOG_DEBUG, "[%s] %s", function, buffer);
}

static inline int max(int a, int b)
{
	if (a < b)
		return b;
	return a;
}

static void ssh_import_key(ssh_bind bind, const char *keys_dir, const char *name)
{
	char path[PATH_MAX];
	ssh_key key = NULL;

	snprintf(path, sizeof(path), "%s/%s", keys_dir, name);

	if (access(path, F_OK) < 0)
		return;

	tmate_info("Loading key %s", path);

	ssh_pki_import_privkey_file(path, NULL, NULL, NULL, &key);
	ssh_bind_options_set(bind, SSH_BIND_OPTIONS_IMPORT_KEY, key);
}

static ssh_bind prepare_ssh(const char *keys_dir, const char *bind_addr, int port)
{
	ssh_bind bind;
	int ssh_log_level;

	ssh_log_level = SSH_LOG_WARNING + max(log_get_level() - LOG_INFO, 0);

	ssh_set_log_callback(ssh_log_function);

	bind = ssh_bind_new();
	if (!bind)
		tmate_fatal("Cannot initialize ssh");

#if LIBSSH_VERSION_INT >= SSH_VERSION_INT(0,9,0)
	/* Explicitly parse configuration to avoid automatic configuration file
	 * loading which could override options */
	ssh_bind_options_parse_config(bind, NULL);
#endif

	if (bind_addr)
		ssh_bind_options_set(bind, SSH_BIND_OPTIONS_BINDADDR, bind_addr);
	ssh_bind_options_set(bind, SSH_BIND_OPTIONS_BINDPORT, &port);
	ssh_bind_options_set(bind, SSH_BIND_OPTIONS_BANNER, TMATE_SSH_BANNER);
	ssh_bind_options_set(bind, SSH_BIND_OPTIONS_LOG_VERBOSITY, &ssh_log_level);

	ssh_import_key(bind, keys_dir, "ssh_host_rsa_key");
	ssh_import_key(bind, keys_dir, "ssh_host_ed25519_key");

	if (ssh_bind_listen(bind) < 0)
		tmate_fatal("Error listening to socket: %s", ssh_get_error(bind));

	tmate_info("Accepting connections on %s:%d", bind_addr ?: "", port);

	return bind;
}

static void handle_sigchld(__unused int sig)
{
	int status;
	pid_t pid;

	while ((pid = waitpid(WAIT_ANY, &status, WNOHANG)) > 0) {
		/*
		 * It's not safe to call indirectly malloc() here, because
		 * of potential deadlocks with other malloc() calls.
		 */
#if 0
		if (WIFEXITED(status))
			tmate_debug("Child %d exited (%d)", pid, WEXITSTATUS(status));
		if (WIFSIGNALED(status))
			tmate_debug("Child %d killed (%d)", pid, WTERMSIG(status));
		if (WIFSTOPPED(status))
			tmate_debug("Child %d stopped (%d)", pid, WSTOPSIG(status));
#endif
	}
}

void tmate_ssh_server_main(struct tmate_session *session, const char *keys_dir,
			   const char *bind_addr, int port)
{
	struct tmate_ssh_client *client = &session->ssh_client;
	ssh_bind bind;
	pid_t pid;
	int fd;

	tmate_catch_sigsegv();
	signal(SIGCHLD, handle_sigchld);

	bind = prepare_ssh(keys_dir, bind_addr, port);

	client->session = ssh_new();
	client->channel = NULL;
	client->winsize_pty.ws_col = 80;
	client->winsize_pty.ws_row = 24;
	session->session_token = "init";

	if (!client->session)
		tmate_fatal("Cannot initialize session");

	for (;;) {
		fd = accept(ssh_bind_get_fd(bind), NULL, NULL);
		if (fd < 0)
			tmate_fatal("Error accepting connection");

		if ((pid = fork()) < 0)
			tmate_fatal("Can't fork");

		if (pid) {
			/* Parent process */
			close(fd);
			continue;
		}

		/* Child process */

		signal(SIGALRM, handle_sigalrm);
		alarm(TMATE_SSH_GRACE_PERIOD);

		if (get_client_ip(fd, client->ip_address, sizeof(client->ip_address)) < 0) {
			if (tmate_settings->use_proxy_protocol)
				tmate_fatal("Proxy header invalid. Load balancer may be misconfigured");
			else
				tmate_fatal("Error getting client IP from connection");
		}

		tmate_debug("Connection accepted ip=%s", client->ip_address);

		if (ssh_bind_accept_fd(bind, client->session, fd) < 0)
			tmate_fatal("Error accepting connection: %s", ssh_get_error(bind));

		ssh_bind_free(bind);

		client_bootstrap(session);
		/* never reached */
	}
}
