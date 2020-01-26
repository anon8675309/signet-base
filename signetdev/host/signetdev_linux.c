#include "signetdev_priv.h"
#include "signetdev_unix.h"
#include "signetdev.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "../common/signetdev_common.h"

extern signetdev_conn_err_t g_error_handler;
extern void *g_error_handler_param;

static int g_opening_connection;
static int g_emulating;
static int g_poll_fd = -1;
static int g_inotify_fd = -1;

struct signetdev_connection {
	int fd;
	struct send_message_req *tail_message;
	struct send_message_req *head_message;
	struct send_message_req *tail_cancel_message;
	struct send_message_req *head_cancel_message;

	struct tx_message_state tx_state;
	struct rx_message_state rx_state;
};

struct signetdev_connection g_connection;

static struct send_message_req **pending_message()
{
	struct signetdev_connection *conn = &g_connection;
	struct send_message_req **msg = NULL;
	if (conn->rx_state.message) {
		msg = &conn->rx_state.message;
	} else if (conn->tx_state.message && !conn->tx_state.message->interrupt) {
		msg = &conn->tx_state.message;
	}
	return msg;
}

void signetdev_priv_handle_error()
{
	struct signetdev_connection *conn = &g_connection;
	if (conn->fd != -1) {
		close(conn->fd);
		conn->fd  = -1;
	}
	if (g_error_handler) {
		g_error_handler(g_error_handler_param);
	}
}

static void command_response(int rc)
{
	char resp = rc;
	write(g_command_resp_pipe[1], &resp, 1);
}

static void handle_exit(void *arg)
{
	struct signetdev_connection *conn = &g_connection;
	(void)arg;
	struct send_message_req **msg = pending_message();
	if (msg) {
		signetdev_priv_finalize_message(msg, SIGNET_ERROR_QUIT);
	}
	if (conn->fd != -1)
		close(conn->fd);
	if (g_poll_fd != -1)
		close(g_poll_fd);
	if (g_inotify_fd != -1)
		close(g_inotify_fd);
	g_poll_fd = -1;
	g_inotify_fd = -1;
}

static void handle_error()
{
	struct signetdev_connection *conn = &g_connection;
	if (conn->fd >= 0) {
		close(conn->fd);
		conn->fd = -1;
	}
	if (g_error_handler) {
		g_error_handler(g_error_handler_param);
	}
	struct send_message_req **msg = pending_message();
	if (msg) {
		signetdev_priv_finalize_message(msg, SIGNET_ERROR_DISCONNECT);
	}
	conn->tx_state.message = NULL;
	conn->rx_state.message = NULL;
}

static int attempt_raw_hid_write()
{
	struct signetdev_connection *conn = &g_connection;
	if (!conn->tx_state.message)
		return 1;

	if (conn->tx_state.msg_packet_seq == conn->tx_state.msg_packet_count) {
		if (!conn->tx_state.message->resp) {
			signetdev_priv_finalize_message(&conn->tx_state.message, conn->tx_state.msg_size);
		} else {
			conn->tx_state.message = NULL;
		}
		return 0;
	}
	signetdev_priv_advance_message_state(&conn->tx_state);
	int rc = write(conn->fd, conn->tx_state.packet_buf, RAW_HID_PACKET_SIZE + 1);
	if (rc == -1 && errno == EAGAIN) {
		return 1;
	} else if (rc == -1) {
		handle_error();
	}
	return 0;
}

static int attempt_raw_hid_read()
{
	struct signetdev_connection *conn = &g_connection;
	u8 rx_packet_buf[RAW_HID_PACKET_SIZE];
	int rc = read(conn->fd, rx_packet_buf, RAW_HID_PACKET_SIZE);
	if (rc == -1 && errno == EAGAIN) {
		return 1;
	} else if (rc != RAW_HID_PACKET_SIZE) {
		handle_error();
		return 1;
	} else {
		signetdev_priv_process_rx_packet(&conn->rx_state, rx_packet_buf);
	}
	return 0;
}

static int attempt_open_connection(const char *path)
{
	struct signetdev_connection *conn = &g_connection;
	if (conn->fd >= 0) {
		g_opening_connection = 0;
		return 0;
	}
	int fd = open(path, O_RDWR | O_NONBLOCK);
	if (fd >= 0) {
		memset(conn, 0, sizeof(g_connection));
		conn->fd = fd;
		struct epoll_event ev;
		ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
		ev.data.fd = conn->fd;
		int rc = epoll_ctl(g_poll_fd, EPOLL_CTL_ADD, conn->fd, &ev);
		if (rc)
			pthread_exit(NULL);
		g_opening_connection = 0;
		return 0;
	} else {
		g_opening_connection = 1;
		return -1;
	}
}

static void handle_command(int command, void *p)
{
	int rc;
	struct signetdev_connection *conn = &g_connection;
	switch (command) {
	case SIGNETDEV_CMD_EMULATE_BEGIN:
		if (!g_opening_connection && conn->fd < 0) {
			g_emulating = 1;
			command_response(1);
		} else {
			command_response(0);
		}
		break;
	case SIGNETDEV_CMD_EMULATE_END:
		g_emulating = 0;
		break;
	case SIGNETDEV_CMD_OPEN:
		//TODO: This seems sloppy. Need a list somewhere of all possible node names
		rc = attempt_open_connection("/dev/signet-hc");
		if (rc != 0) {
			rc = attempt_open_connection("/dev/signet");
		}
		command_response(rc);
		break;
	case SIGNETDEV_CMD_CLOSE:
		g_opening_connection = 0;
		if (conn->fd >= 0) {
			epoll_ctl(g_poll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
			close(conn->fd);
			conn->fd = -1;
		}
		break;
	case SIGNETDEV_CMD_MESSAGE: {
		struct signetdev_connection *conn = &g_connection;
		struct send_message_req *msg = (struct send_message_req *)p;
		if (g_emulating) {
			signetdev_emulate_handle_message_priv(msg);
		} else {
			msg->next = NULL;
			if (!conn->head_message) {
				conn->head_message = msg;
			}
			if (conn->tail_message)
				conn->tail_message->next = msg;
			conn->tail_message = msg;
		}
	} break;
	case SIGNETDEV_CMD_QUIT: {
		pthread_exit(NULL);
	} break;
	case SIGNETDEV_CMD_CANCEL_MESSAGE: {
		struct send_message_req *msg = (struct send_message_req *)p;
		if (g_emulating) {
			signetdev_emulate_handle_message_priv(msg);
		} else {
			msg->next = NULL;
			if (!conn->head_cancel_message) {
				conn->head_cancel_message = msg;
			}
			if (conn->tail_cancel_message)
				conn->tail_cancel_message->next = msg;
			conn->tail_cancel_message = msg;
		}
	} break;
	}
}

static void command_pipe_io_iter()
{
	int done = 0;
	while (!done) {
		intptr_t v[2];
		int rc = read(g_command_pipe[0], v, sizeof(intptr_t) * 2);
		if (rc == sizeof(intptr_t) * 2) {
			handle_command(v[0], (void *)v[1]);
		} else if (rc == -1 && errno == EAGAIN) {
			done = 1;
		} else {
			handle_error();
		}
	}
}

static int raw_hid_io(struct signetdev_connection *conn)
{
	if (!conn->tx_state.message && (conn->head_message || conn->head_cancel_message)) {
		if (conn->head_cancel_message) {
			conn->tx_state.message = conn->head_cancel_message;
			conn->head_cancel_message = conn->head_cancel_message->next;
			if (!conn->head_cancel_message) {
				conn->tail_cancel_message = NULL;
			}
		} else if (!conn->rx_state.message) {
			conn->tx_state.message = conn->head_message;
			if (conn->head_message->resp) {
				conn->rx_state.message = conn->head_message;
			}
			conn->head_message = conn->head_message->next;
			if (!conn->head_message) {
				conn->tail_message = NULL;
			}
		}
		if (conn->tx_state.message) {
			signetdev_priv_prepare_message_state(&conn->tx_state,
					 conn->tx_state.message->dev_cmd,
					 conn->tx_state.message->messages_remaining,
					 conn->tx_state.message->payload,
					 conn->tx_state.message->payload_size);
		}
	}

	return attempt_raw_hid_read() && attempt_raw_hid_write();
}

static void raw_hid_io_iter()
{
	//Read and/or write non-blocking until EAGAIN is returned
	struct signetdev_connection *conn = &g_connection;
	while (!raw_hid_io(conn));
}

static void inotify_fd_readable()
{
	while (g_opening_connection) {
		u8 buf[4096];
		char path[128] = "/dev/";
		int rc = read(g_inotify_fd, buf, 4096);
		if (rc == -1 && errno == EINTR)
			break;
		if (rc == -1)
			break;
		int idx = 0;
		while (idx < rc) {
			struct inotify_event *ev = (struct inotify_event *)(buf + idx);
			int attempt_open = 0;
			if (!strcmp(ev->name, "signet")) {
				attempt_open = 1;
			} else if (!strcmp(ev->name, "signet-hc")) {
				attempt_open = 1;
			}
			strncat(path, ev->name, 16);
			if (attempt_open) {
				rc = attempt_open_connection(path);
				if (rc == 0) {
					g_device_opened_cb(g_device_opened_cb_param);
					break;
				}
			}
			idx += sizeof(struct inotify_event) + ev->len;
		}
		break;
	}
}

void *transaction_thread(void *arg)
{
	struct signetdev_connection *conn = &g_connection;
	g_opening_connection = 0;
	conn->fd = -1;

	pthread_cleanup_push(handle_exit, NULL);

	g_poll_fd = epoll_create1(0);

	struct epoll_event ev_pipe;
	struct epoll_event ev_inotify;
	int rc;

	(void)arg;

	ev_pipe.events = EPOLLIN | EPOLLET;
	ev_pipe.data.fd = g_command_pipe[0];
	rc = epoll_ctl(g_poll_fd, EPOLL_CTL_ADD, g_command_pipe[0], &ev_pipe);
	if (rc)
		pthread_exit(NULL);

	struct epoll_event events[8];

	g_inotify_fd = inotify_init1(O_NONBLOCK);
	inotify_add_watch(g_inotify_fd, "/dev", IN_CREATE | IN_DELETE);
	ev_inotify.events = EPOLLIN | EPOLLET;
	ev_inotify.data.fd = g_inotify_fd;
	rc = epoll_ctl(g_poll_fd, EPOLL_CTL_ADD, g_inotify_fd, &ev_inotify);
	if (rc)
		pthread_exit(NULL);

	while (1) {
		command_pipe_io_iter();
		if (conn->fd != -1) {
			raw_hid_io_iter();
		}
		int nfds = epoll_wait(g_poll_fd, events, 8, -1);
		int i;
		for (i = 0; i < nfds; i++) {
			if (events[i].data.fd == g_inotify_fd) {
				inotify_fd_readable();
			}
			if (events[i].data.fd == conn->fd && (events[i].events & EPOLLERR)) {
				handle_error();
			}
		}
	}
	pthread_cleanup_pop(1);
	pthread_exit(NULL);
	return NULL;
}

int signetdev_priv_issue_command(int command, void *p)
{
	intptr_t v[2] = {command, (intptr_t)p};
	write(g_command_pipe[1], v, sizeof(intptr_t) * 2);
	char cmd_resp;
	read(g_command_resp_pipe[0], &cmd_resp, 1);
	return cmd_resp;
}

void signetdev_priv_issue_command_no_resp(int command, void *p)
{
	intptr_t v[2] = {command, (intptr_t)p};
	write(g_command_pipe[1], v, sizeof(intptr_t) * 2);
}
