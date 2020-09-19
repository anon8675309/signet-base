#include "flash.h"
#include "regmap.h"
#include "signetdev/common/signetdev_common.h"
#include "signetdev/common/signetdev_common_priv.h"

enum flash_state {
	FLASH_IDLE,
	FLASH_ERASING,
	FLASH_WRITING
};

enum flash_state flash_state = FLASH_IDLE;
u32 *flash_write_data = NULL;
u32 *flash_write_addr = NULL;
u16 flash_write_count = 0;

static u32 addr_to_pg(void *addr)
{
	u32 addr_i = (u32)addr;
	u32 pgnum = (addr_i - FLASH_MEM_BASE_ADDR)/FLASH_PAGE_SIZE;
	return pgnum;
}

void flash_unlock()
{
	FLASH_KEYR = FLASH_KEYR_KEY1;
	FLASH_KEYR = FLASH_KEYR_KEY2;
}

int flash_writing()
{
	return flash_state == FLASH_WRITING;
}

void flash_erase(void *addr)
{
	u32 pgnum = addr_to_pg(addr);
	FLASH_CR = (FLASH_CR & ~FLASH_CR_PNB_MASK) | FLASH_CR_PER |
		(FLASH_CR_PNB_FACTOR * pgnum);
	FLASH_CR |= FLASH_CR_STRT;
	flash_state = FLASH_ERASING;
}

void flash_write_complete();
void flash_write_failed();

void flash_write_state()
{
	for (int i = 0; i < 32 && flash_write_count > 0; i++) {
		*(flash_write_addr++) = *(flash_write_data++);
		*(flash_write_addr++) = *(flash_write_data++);
		while (FLASH_SR & FLASH_SR_BUSY);
		if (FLASH_SR & 0xfffe) {
			FLASH_CR &= ~FLASH_CR_PG;
			FLASH_CR |= FLASH_CR_EOPIE;
			flash_state = FLASH_IDLE;
			flash_write_failed();
			return;
		}
		flash_write_count--;
	}
	if (!flash_write_count) {
		FLASH_CR &= ~FLASH_CR_PG;
		FLASH_CR |= FLASH_CR_EOPIE;
		flash_state = FLASH_IDLE;
		flash_write_complete();
	}
}

void flash_handler()
{
	switch(flash_state) {
	case FLASH_ERASING:
		FLASH_CR &= ~(FLASH_CR_PER);
		if (FLASH_SR & FLASH_SR_EOP) {
			FLASH_SR |= FLASH_SR_EOP;
			if (flash_write_data) {
				FLASH_CR |= FLASH_CR_PG;
				FLASH_CR &= ~FLASH_CR_EOPIE;
				flash_state = FLASH_WRITING;
			} else {
				flash_state = FLASH_IDLE;
				flash_write_complete();
			}
		} else {
			flash_state = FLASH_IDLE;
			flash_write_failed();
		}
		break;
	default:
		break;
	}
}

void flash_idle()
{
	switch (flash_state) {
	case FLASH_WRITING:
		flash_write_state();
	default:
		break;
	}
}

void flash_write_page(u8 *dest, u8 *src, int count)
{
	if (flash_state == FLASH_IDLE) {
		flash_write_data = (u32 *)src;
		flash_write_addr = (u32 *)dest;
		flash_write_count = (count + 7)/8;
		if (FLASH_CR & FLASH_CR_LOCK)
			flash_unlock();
		FLASH_CR |= FLASH_CR_PER;
		u32 pgnum = addr_to_pg(dest);
		FLASH_CR = (FLASH_CR & ~FLASH_CR_PNB_MASK) | FLASH_CR_PER |
			(FLASH_CR_PNB_FACTOR * pgnum);
		FLASH_CR |= FLASH_CR_STRT;
		flash_state = FLASH_ERASING;
	}
}

void flash_write(u8 *dest, u8 *src, int count)
{
	if (flash_state == FLASH_IDLE) {
		flash_write_data = (u32 *)src;
		flash_write_addr = (u32 *)dest;
		flash_write_count = (count + 7)/8;
		if (FLASH_CR & FLASH_CR_LOCK)
			flash_unlock();
		FLASH_CR |= FLASH_CR_PG;
		FLASH_CR &= ~FLASH_CR_EOPIE;
		flash_state = FLASH_WRITING;
	}
}
