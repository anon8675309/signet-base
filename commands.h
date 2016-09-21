#ifndef COMMANDS_H
#define COMMANDS_H
#include "types.h"
#include "common.h"

void get_progress_cmd(u8 *data, int data_len);

void finish_command(enum command_responses resp, const u8 *payload, int payload_len);
void finish_command_resp(enum command_responses resp);

extern u8 cmd_resp[];

extern enum device_state device_state;

union state_data_u {
	struct {
		int prev_state;
	} backup;
};

union cmd_data_u {
	struct {
		u16 index;
		u16 num_pages;
		u8 pages[CMD_PACKET_PAYLOAD_SIZE];
	} erase_flash_pages;
	struct {
		u8 chars[CMD_PACKET_PAYLOAD_SIZE];
	} type_data;
	struct {
		int id;
	} open_id;
	struct {
		int started;
		int rand_avail_init;
		int random_data_gathered;
		int root_block_finalized;
		int blocks_written;
		u8 passwd[AES_BLK_SIZE];
		u8 hashfn[AES_BLK_SIZE];
		u8 salt[AES_BLK_SIZE];
		u8 rand[INIT_RAND_DATA_SZ];
		u8 userdata[BLK_SIZE];
	} init_data;
	struct {
		int block;
	} wipe_data;
	struct {
		u8 iv[AES_BLK_SIZE];
		u8 block[BLK_SIZE];
	} set_data;
	struct {
		u8 iv[AES_BLK_SIZE];
		u8 block[BLK_SIZE];
	} get_data;
	struct {
		u8 new_key[AES_BLK_SIZE];
		u8 cyphertext[AES_BLK_SIZE];
		u8 hashfn[AES_BLK_SIZE];
		u8 salt[AES_BLK_SIZE];
	} change_master_password;
	struct {
		u8 password[AES_BLK_SIZE];
		u8 cyphertext[AES_BLK_SIZE];
	} login;
};

extern union cmd_data_u cmd_data;

int cmd_packet_recv();
void cmd_packet_send(const u8 *data);
void cmd_event_send(int event_num, const u8 *data, int data_len);

extern u8 cmd_packet_buf[];

void enter_state(enum device_state state);
void enter_progresssing_state(enum device_state state, int _n_progress_components, int *_progress_maximums);

void cmd_rand_update();

#endif
