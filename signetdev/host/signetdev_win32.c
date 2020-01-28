#include "rawhid/hid.h"
#include "../common/signetdev_common.h"
#include <windows.h>
#include <dbt.h>
#include <Usbiodef.h>
#include <hidsdi.h>
#include <hidclass.h>
#include "signetdev.h"
#include "signetdev_priv.h"

static int g_open_request_pending = 0;
static HWND g_window = INVALID_HANDLE_VALUE;
static HANDLE g_device_handle = INVALID_HANDLE_VALUE;
static int g_is_hc_device = 0;

static HANDLE g_msg_thread = INVALID_HANDLE_VALUE;
static HANDLE g_msg_read_event = INVALID_HANDLE_VALUE;
static HANDLE g_msg_write_event = INVALID_HANDLE_VALUE;
static HANDLE g_command_event = INVALID_HANDLE_VALUE;
static HANDLE g_command_resp_event = INVALID_HANDLE_VALUE;
static HANDLE g_command_mutex = INVALID_HANDLE_VALUE;
static int g_emulating = 0;

static OVERLAPPED g_read_overlapped;
static OVERLAPPED g_write_overlapped;

static int g_quitting = 0;

static u8 g_rx_packet[RAW_HID_PACKET_SIZE + 1];

struct tx_message_state g_tx_state;
struct rx_message_state g_rx_state;
struct send_message_req *g_tail_message;
struct send_message_req *g_head_message;
struct send_message_req *g_tail_cancel_message;
struct send_message_req *g_head_cancel_message;

static struct send_message_req **pending_message()
{
	struct send_message_req **msg = NULL;
	if (g_rx_state.message) {
		msg = &g_rx_state.message;
	} else if (g_tx_state.message && !g_tx_state.message->interrupt) {
		msg = &g_tx_state.message;
	}
	return msg;
}

void signetdev_priv_handle_error()
{
	rawhid_close(0);
	g_device_handle = INVALID_HANDLE_VALUE;

	struct send_message_req **msg = pending_message();
	if (msg) {
		signetdev_priv_finalize_message(msg, SIGNET_ERROR_DISCONNECT);
	}
	g_tx_state.message = NULL;
	g_rx_state.message = NULL;
	if (g_error_handler) {
		g_error_handler(g_error_handler_param);
	}
}

static int send_next_packet()
{
	signetdev_priv_advance_message_state(&g_tx_state);
	memset(&g_write_overlapped, 0, sizeof(OVERLAPPED));
	g_write_overlapped.hEvent = g_msg_write_event;
	if (!WriteFile(g_device_handle, &g_tx_state.packet_buf, RAW_HID_PACKET_SIZE + 1, NULL, &g_write_overlapped)) {
		if (GetLastError() != ERROR_IO_PENDING)	{
			return -1;
		}
	}
	return 0;
}

static int request_next_packet()
{
	memset(&g_read_overlapped, 0, sizeof(OVERLAPPED));
	g_read_overlapped.hEvent = g_msg_read_event;
	if (!ReadFile(g_device_handle, g_rx_packet, RAW_HID_PACKET_SIZE + 1, NULL, &g_read_overlapped)) {
		if (GetLastError() != ERROR_IO_PENDING)	{
			return -1;
		}
	}
	return 0;
}

static void state_iter()
{
	if (!g_tx_state.message && (g_head_message || g_head_cancel_message)) {
		if (g_head_cancel_message) {
			g_tx_state.message = g_head_cancel_message;
			g_head_cancel_message = g_head_cancel_message->next;
			if (!g_head_cancel_message) {
				g_tail_cancel_message = NULL;
			}
		} else if (!g_rx_state.message) {
			g_tx_state.message = g_head_message;
			g_rx_state.message = g_head_message;
			g_head_message = g_head_message->next;
			if (!g_head_message) {
				g_tail_message = NULL;
			}
		}
		if (g_tx_state.message) {
			signetdev_priv_prepare_message_state(&g_tx_state,
					 g_tx_state.message->dev_cmd,
					 g_tx_state.message->messages_remaining,
					 g_tx_state.message->payload,
					 g_tx_state.message->payload_size);

			if (send_next_packet()) {
				signetdev_priv_handle_error();
				return;
			}
		}
	}
}

static void process_send_message_req(struct send_message_req *req)
{
	struct send_message_req *msg = (struct send_message_req *)req;
	msg->next = NULL;
	if (!g_head_message) {
		g_head_message = msg;
	}
	if (g_tail_message)
		g_tail_message->next = msg;
	g_tail_message = msg;

	state_iter();
}

static void process_cancel_message_req(struct send_message_req *msg)
{
	msg->next = NULL;
	if (!g_head_cancel_message) {
		g_head_cancel_message = msg;
	}
	if (g_tail_cancel_message)
		g_tail_cancel_message->next = msg;
	g_tail_cancel_message = msg;

	state_iter();
}

struct command {
	int cmd;
	void *data;
};

struct command g_command_queue[128];
int g_command_queue_head = 0;
int g_command_queue_tail = 0;
int g_command_resp;

void signetdev_priv_issue_command_no_resp(int cmd, void *data)
{
	struct command *c;
	WaitForSingleObject(g_command_mutex, -1);
	c = g_command_queue + g_command_queue_head;
	c->cmd = cmd;
	c->data = data;
	g_command_queue_head = (g_command_queue_head + 1) % 128;
	SetEvent(g_command_event);
	ReleaseMutex(g_command_mutex);
}

int signetdev_priv_issue_command(int cmd, void *data)
{
	signetdev_priv_issue_command_no_resp(cmd, data);
	WaitForSingleObject(g_command_resp_event, -1);
	return g_command_resp;
}

static void handle_command(int command, void *p)
{
	switch (command) {
	case SIGNETDEV_CMD_EMULATE_BEGIN:
		if (g_device_handle == INVALID_HANDLE_VALUE && !g_open_request_pending) {
			g_emulating = 1;
			g_command_resp = 1;
		} else {
			g_command_resp = 0;
		}
		SetEvent(g_command_resp_event);
		break;
	case SIGNETDEV_CMD_EMULATE_END:
		g_emulating = 0;
		break;
	case SIGNETDEV_CMD_OPEN:
		break;
	case SIGNETDEV_CMD_CLOSE:
		break;
	case SIGNETDEV_CMD_MESSAGE: {
		struct send_message_req *msg = (struct send_message_req *)p;
		if (g_emulating) {
			signetdev_emulate_handle_message_priv(msg);
		} else {
			process_send_message_req(msg);
		}
		} break;
	case SIGNETDEV_CMD_QUIT: {
		g_quitting = 1;
		} break;
	case SIGNETDEV_CMD_CANCEL_MESSAGE: {
		struct send_message_req *msg = (struct send_message_req *)p;
		if (g_emulating) {
			signetdev_emulate_handle_message_priv(msg);
		} else {
			process_cancel_message_req(msg);
		}
		} break;
	}
}

static DWORD WINAPI transaction_thread(LPVOID lpParameter)
{
	(void)lpParameter;
	HANDLE wait_handles[8];
	wait_handles[0] = g_msg_read_event;
	wait_handles[1] = g_msg_write_event;
	wait_handles[2] = g_command_event;
	while (!g_quitting) {
		DWORD rc = WaitForMultipleObjects(3, wait_handles, FALSE, INFINITE);
		int index = rc - WAIT_OBJECT_0;
		ResetEvent(wait_handles[index]);
		switch(index) {
		case 0: {
			DWORD n;
			if (!GetOverlappedResult(g_device_handle, &g_read_overlapped, &n, FALSE)) {
				signetdev_priv_handle_error();
				break;
			}
			if (n != (RAW_HID_PACKET_SIZE + 1)) {
				signetdev_priv_handle_error();
				break;
			}
			signetdev_priv_process_rx_packet(&g_rx_state, g_rx_packet + 1);
			if (request_next_packet()) {
				signetdev_priv_handle_error();
				break;
			}
			state_iter();
		} break;
		case 1: {
			DWORD n;
			if (!GetOverlappedResult(g_device_handle, &g_write_overlapped, &n, FALSE)) {
				signetdev_priv_handle_error();
				break;
			}
			if (n != (RAW_HID_PACKET_SIZE + 1)) {
				signetdev_priv_handle_error();
				break;
			}
			if (g_tx_state.msg_packet_seq == g_tx_state.msg_packet_count) {
				//Finalize or move to reading phase
				if (!g_tx_state.message->resp) {
					signetdev_priv_finalize_message(&g_tx_state.message, g_tx_state.msg_size);
					g_rx_state.message = NULL;
				} else {
					g_tx_state.message = NULL;
				}

			} else {
				if (send_next_packet()) {
					signetdev_priv_handle_error();
					break;
				}
			}
			} break;
		case 2: {
			WaitForSingleObject(g_command_mutex, -1);
			int entries = g_command_queue_head - g_command_queue_tail;
			if (entries < 0) entries += 128;
			for (int i = 0; (i < entries) && !g_quitting; i++) {
				int j = (g_command_queue_tail + i) % 128;
				handle_command(g_command_queue[j].cmd, g_command_queue[j].data);
			}
			g_command_queue_head = 0;
			g_command_queue_tail = 0;
			ReleaseMutex(g_command_mutex);
		}
		}
	}
	return 0;
}

void signetdev_priv_platform_init()
{
	g_msg_read_event = CreateEvent(NULL, TRUE, FALSE, NULL);
	g_msg_write_event = CreateEvent(NULL, TRUE, FALSE, NULL);
	g_command_event = CreateEvent(NULL, FALSE, FALSE, NULL);
	g_command_resp_event = CreateEvent(NULL, FALSE, FALSE, NULL);
	g_command_mutex = CreateMutex(NULL, FALSE, NULL);
	g_msg_thread = CreateThread(NULL, 4096*16, transaction_thread, NULL, 0, NULL);
}

void signetdev_priv_platform_deinit()
{
	signetdev_priv_issue_command_no_resp(SIGNETDEV_CMD_QUIT, NULL);
	WaitForSingleObject(g_msg_thread, 2000);
	CloseHandle(g_msg_thread);
	g_msg_thread = INVALID_HANDLE_VALUE;
	CloseHandle(g_msg_read_event);
	g_msg_read_event = INVALID_HANDLE_VALUE;
	CloseHandle(g_msg_write_event);
	g_msg_write_event = INVALID_HANDLE_VALUE;
	CloseHandle(g_command_event);
	g_command_event = INVALID_HANDLE_VALUE;
	CloseHandle(g_command_resp_event);
	g_command_resp_event = INVALID_HANDLE_VALUE;
}

enum signetdev_device_type signetdev_open_connection()
{
	int ct = rawhid_open(1, USB_SIGNET_HC_VENDOR_ID, USB_SIGNET_HC_PRODUCT_ID, USB_RAW_HID_USAGE_PAGE, USB_RAW_HID_USAGE);
	g_device_type = SIGNETDEV_DEVICE_NONE;
	if (ct != 1) {
		ct = rawhid_open(1, USB_SIGNET_VENDOR_ID, USB_SIGNET_PRODUCT_ID, USB_RAW_HID_USAGE_PAGE, USB_RAW_HID_USAGE);
		if (ct != 1) {
			g_open_request_pending = 1;
			return -1;
		} else {
			g_device_type = SIGNETDEV_DEVICE_ORIGINAL;
		}
	} else {
		g_device_type = SIGNETDEV_DEVICE_HC;
	}
	g_device_handle = rawhid_win32_get_handle(0);
	g_open_request_pending = 0;
	if (request_next_packet()) {
		g_device_type = SIGNETDEV_DEVICE_NONE;
		//TODO: Shouldn't we cleanup the device here?
		return g_device_type;
	}

	DEV_BROADCAST_HANDLE dbh;
	dbh.dbch_size = sizeof(dbh);
	dbh.dbch_handle = g_device_handle;
	dbh.dbch_devicetype = DBT_DEVTYP_HANDLE;
	dbh.dbch_hdevnotify = NULL;
	RegisterDeviceNotification((HANDLE)g_window, &dbh, DEVICE_NOTIFY_WINDOW_HANDLE);
	return g_device_type;
}

void signetdev_win32_set_window_handle(HANDLE recp)
{
	GUID guid;
	HidD_GetHidGuid(&guid);
	DEV_BROADCAST_DEVICEINTERFACE bdi;
	bdi.dbcc_size = sizeof(&bdi);
	bdi.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
	bdi.dbcc_classguid = guid;
	bdi.dbcc_reserved = 0;
	bdi.dbcc_name[0] = 0;
	g_window = (HWND)recp;
	RegisterDeviceNotification(recp, &bdi, DEVICE_NOTIFY_WINDOW_HANDLE);
}

int signetdev_filter_window_messasage(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	PDEV_BROADCAST_HDR	pDBHdr;
	PDEV_BROADCAST_HANDLE pDBHandle;
	int rc;
	switch (uMsg) {
	case WM_DEVICECHANGE:
		switch (wParam) {
		case DBT_DEVTYP_HANDLE:
			break;
		case DBT_DEVNODES_CHANGED:
			if (g_open_request_pending) {
				enum signetdev_device_type dev_type = signetdev_open_connection();
				if (rc != SIGNETDEV_DEVICE_NONE) {
					g_open_request_pending = 0;
				}
				if (!rc && g_device_opened_cb) {
					g_device_opened_cb(dev_type, g_device_opened_cb_param);
				}
			}
			break;
		case DBT_DEVICEREMOVECOMPLETE:
			pDBHdr = (PDEV_BROADCAST_HDR) lParam;
			switch (pDBHdr->dbch_devicetype)
			{
				case DBT_DEVTYP_HANDLE:
					pDBHandle = (PDEV_BROADCAST_HANDLE) pDBHdr;
					UnregisterDeviceNotification(pDBHandle->dbch_hdevnotify);
					rawhid_close(0);
					g_device_handle = INVALID_HANDLE_VALUE;
					if (g_device_closed_cb)
						g_device_closed_cb(g_device_closed_cb_param);
					break;
				default:
					break;

			}
			break;
		}
		return 1;
		break;
	default:
		return 0;
	}
}

void signetdev_close_connection()
{
	rawhid_close(0);
	g_device_handle = INVALID_HANDLE_VALUE;
	g_open_request_pending = 0;
}
