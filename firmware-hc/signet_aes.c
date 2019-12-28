#include <memory.h>
#include "signet_aes.h"

#include "nettle/aes.h"
#include "signetdev_common.h"

void signet_aes_init()
{
}

void signet_aes_128_encrypt(const u8 *key, const u8 *din, u8 *dout)
{
	struct aes128_ctx ctx;
	aes128_set_encrypt_key(&ctx, key);
	aes128_encrypt(&ctx, AES_BLK_SIZE, dout, din);
}

void signet_aes_256_encrypt(const u8 *key, const u8 *din, u8 *dout)
{
	struct aes256_ctx ctx;
	aes256_set_encrypt_key(&ctx, key);
	aes256_encrypt(&ctx, AES_BLK_SIZE, dout, din);
}

void xor_block(const u8 *src_block, const u8 *mask, u8 *dst_block)
{
	int i;
	if (mask) {
		for (i = 0; i < AES_BLK_SIZE; i++) {
			dst_block[i] = src_block[i] ^ mask[i];
		}
	} else {
		memcpy(dst_block, src_block, AES_BLK_SIZE);
	}
}

void signet_aes_128_encrypt_cbc(const u8 *key, int n_blocks, const u8 *iv, const u8 *din, u8 *dout)
{
	int i;
	for (i = 0; i < n_blocks; i++) {
		u8 temp[AES_BLK_SIZE];
		xor_block(din, iv, temp);
		signet_aes_128_encrypt(key, temp, dout);
		iv = dout;
		din += AES_BLK_SIZE;
		dout += AES_BLK_SIZE;
	}
}

void signet_aes_256_encrypt_cbc(const u8 *key, int n_blocks, const u8 *iv, const u8 *din, u8 *dout)
{
	int i;
	for (i = 0; i < n_blocks; i++) {
		u8 temp[AES_BLK_SIZE];
		xor_block(din, iv, temp);
		signet_aes_256_encrypt(key, temp, dout);
		iv = dout;
		din += AES_BLK_SIZE;
		dout += AES_BLK_SIZE;
	}
}

void signet_aes_128_decrypt_cbc(const u8 *key, int n_blocks, const u8 *iv, const u8 *din, u8 *dout)
{
	int i;
	for (i = 0; i < n_blocks; i++) {
		u8 temp[AES_BLK_SIZE];
		signet_aes_128_decrypt(key, din, temp);
		xor_block(temp, iv, dout);
		iv = din;
		din += AES_BLK_SIZE;
		dout += AES_BLK_SIZE;
	}
}

void signet_aes_256_decrypt_cbc(const u8 *key, int n_blocks, const u8 *iv, const u8 *din, u8 *dout)
{
	int i;
	for (i = 0; i < n_blocks; i++) {
		u8 temp[AES_BLK_SIZE];
		signet_aes_256_decrypt(key, din, temp);
		xor_block(temp, iv, dout);
		iv = din;
		din += AES_BLK_SIZE;
		dout += AES_BLK_SIZE;
	}
}

void signet_aes_128_decrypt(const u8 *key, const u8 *din, u8 *dout)
{
	struct aes128_ctx ctx;
	aes128_set_decrypt_key(&ctx, key);
	aes128_decrypt(&ctx, AES_BLK_SIZE,  dout, din);
}

void signet_aes_256_decrypt(const u8 *key, const u8 *din, u8 *dout)
{
	struct aes256_ctx ctx;
	aes256_set_decrypt_key(&ctx, key);
	aes256_decrypt(&ctx, AES_BLK_SIZE,  dout, din);
}
