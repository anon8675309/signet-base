/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "stm32f7xx_hal.h"
#include "usbd_core.h"
#include "usbd_desc.h"
#include "usbd_multi.h"
#include "flash.h"
#include "rng_rand.h"
#include "rtc_rand.h"
#include "rand.h"
#include "commands.h"
#include "usb_keyboard.h"
#include "crc.h"
#include "usbd_msc_scsi.h"
#include "fido2/crypto.h"
#include "fido2/ctaphid.h"
#include "memory_layout.h"

void ctaphid_press();
void ctaphid_idle();
void ctaphid_blink_timeout();

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);


#include "usbd_core.h"
#include "commands.h"

/* Private variables ---------------------------------------------------------*/
CRYP_HandleTypeDef g_hcryp;
__ALIGN_BEGIN static const uint32_t pKeyAES[4] __ALIGN_END = {
	0x00000000,0x00000000,0x00000000,0x00000000
};

MMC_HandleTypeDef g_hmmc1;

UART_HandleTypeDef g_huart1;

PCD_HandleTypeDef hpcd_USB_OTG_HS __attribute__((aligned(16)));

/* Private function prototypes -----------------------------------------------*/
static void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_AES_Init(void);
static void MX_RNG_Init(void);
static void MX_SDMMC1_MMC_Init(void);
#ifdef USE_UART
static void MX_USART1_UART_Init(void);
#endif
USBD_HandleTypeDef USBD_Device;

#ifdef BOARD_REV_2

#define LED1_PORT GPIOG
#define LED1_PIN GPIO_PIN_0

#define LED2_PORT GPIOE
#define LED2_PIN GPIO_PIN_12

#define BUTTON_PORT GPIOC
#define BUTTON_PORT_NUM (2)
#define BUTTON_PIN GPIO_PIN_5
#define BUTTON_PIN_NUM (5)
#define BUTTON_IRQ (EXTI9_5_IRQn)
#define BUTTON_HANDLER (EXTI9_5_IRQHandler)

#else
#define LED1_PORT GPIOI
#define LED1_PIN GPIO_PIN_11

#define LED2_PORT GPIOD
#define LED2_PIN GPIO_PIN_13

#define BUTTON_PORT GPIOD
#define BUTTON_PORT_NUM (3)
#define BUTTON_PIN GPIO_PIN_0
#define BUTTON_PIN_NUM (0)
#define BUTTON_IRQ (EXTI0_IRQn)
#define BUTTON_HANDLER (EXTI0_IRQHandler)
#endif

void setLED1(int x)
{
	HAL_GPIO_WritePin(LED1_PORT, LED1_PIN, x ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void setLED2(int x)
{
	HAL_GPIO_WritePin(LED2_PORT, LED2_PIN, x ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static int buttonState()
{
	return HAL_GPIO_ReadPin(BUTTON_PORT, BUTTON_PIN);
}

//Multi-purpose transfer buffer. Used for different purposes in different modes
#define MMC_TEST_BULK_BUFFER_SIZE_REQ (HC_BLOCK_SZ*2)
#define BULK_BUFFER_SIZE MAX(MMC_TEST_BULK_BUFFER_SIZE_REQ, USB_BULK_BUFFER_SIZE_REQ)
uint8_t g_bulkBuffer[BULK_BUFFER_SIZE] __attribute__((aligned(16)));

static void MX_DMA_Init(void);

#include "buffer_manager.h"

static int g_ms_last_pressed = 0;

static struct {
	int blink_state;
	int blink_start;
	int blink_period;
	int blink_duration;
	int blink_paused;
	int pause_start;
	uint8_t timeout_event_secs;
} s_blink_state;

static int s_button_state;
static int s_timer_target;

__weak void blink_timeout()
{
}
__weak void button_press()
{
}

__weak void long_button_press()
{
}

__weak void button_release()
{
}

static void fail(int l1, int l2)
{
	setLED1(l1);
	setLED2(l2);
	Error_Handler();
}

#ifdef assert
#undef assert
void assert(int cond)
{
	assert_lit(cond, 1, 1);
}
#endif

void assert_lit(int cond, int l1, int l2)
{
	if (!cond) {
		fail(l1,l2);
	}
}

void timer_start(int ms)
{
	int ms_count = HAL_GetTick();
	s_timer_target = ms_count + ms;
	BEGIN_WORK(TIMER_WORK);
}

void timer_stop()
{
	s_timer_target = 0;
	END_WORK(TIMER_WORK);
}

__weak void timer_timeout()
{
}

void led_off()
{
	setLED1(0);
	setLED2(0);
}

void led_on()
{
	setLED1(1);
	setLED2(1);
}

static void busy_blink_once(int ms)
{
	led_on();
	HAL_Delay(ms);
	led_off();
}

static void busy_blink(int ms_off, int ms_on)
{
	while (1) {
		HAL_Delay(ms_off);
		busy_blink_once(ms_on);
	}
}

void blink_idle()
{
	int ms_count = HAL_GetTick();
	if (s_blink_state.blink_period > 0 && !s_blink_state.blink_paused) {
		int next_blink_state = ((ms_count - s_blink_state.blink_start)/(s_blink_state.blink_period/2)) % 2;
		if (next_blink_state != s_blink_state.blink_state) {
			if (next_blink_state) {
				led_off();
			} else {
				led_on();
			}
		}
		s_blink_state.blink_state = next_blink_state;
		if (s_blink_state.blink_duration) {
			int timeout_event_msecs = s_blink_state.blink_duration - (ms_count - s_blink_state.blink_start);
			int next_timeout_event_secs = (timeout_event_msecs+999)/1000;
			if ((timeout_event_msecs <= 0) && (s_blink_state.blink_duration > 0)) {
				s_blink_state.blink_period = 0;
				END_WORK(BLINK_WORK);
				led_off();
#ifdef ENABLE_FIDO2
				ctaphid_blink_timeout();
#endif
				blink_timeout();
			}
			if (next_timeout_event_secs != s_blink_state.timeout_event_secs) {
				s_blink_state.timeout_event_secs = next_timeout_event_secs;
				if (g_cmd_state.device_state != DS_DISCONNECTED && g_cmd_state.device_state != DS_RESET) {
					cmd_event_send(2, &s_blink_state.timeout_event_secs, sizeof(s_blink_state.timeout_event_secs));
				}
			}
		}
	}
}

void start_blinking(int period, int duration)
{
	int ms_count = HAL_GetTick();
	s_blink_state.blink_start = ms_count;
	s_blink_state.blink_period = period;
	s_blink_state.blink_duration = duration;
	s_blink_state.timeout_event_secs = (s_blink_state.blink_duration + 999)/1000;
	led_on();
	BEGIN_WORK(BLINK_WORK);
}

void pause_blinking()
{
	int ms_count = HAL_GetTick();
	led_off();
	s_blink_state.blink_paused = 1;
	s_blink_state.pause_start = ms_count;
}

void resume_blinking()
{
	int ms_count = HAL_GetTick();
	if (s_blink_state.blink_paused) {
		s_blink_state.blink_start += (ms_count - s_blink_state.pause_start);
		s_blink_state.blink_paused = 0;
		blink_idle();
	}
}

void stop_blinking()
{
	s_blink_state.blink_period = 0;
	s_blink_state.blink_paused = 0;
	led_off();
	END_WORK(BLINK_WORK);
}

int is_blinking()
{
	return s_blink_state.blink_period > 0;
}


#ifdef ENABLE_FIDO2
#include "mini-gmp.h"
#include "fido2/crypto.h"
#include "fido2/ctaphid.h"

void *mp_alloc(size_t sz)
{
	return malloc(sz);
}

void mp_dealloc(void *p, size_t sz)
{
	free(p);
}

void *mp_realloc(void *p, size_t before, size_t after)
{
	return realloc(p, after);
}

#endif

void BUTTON_HANDLER()
{
	int ms_count = HAL_GetTick();
	g_ms_last_pressed = ms_count;
	BEGIN_WORK(BUTTON_PRESS_WORK);
	EXTI->PR = (1 << BUTTON_PIN_NUM);
}

void authenticator_initialize();

static void wait_for_rand(int rand_needed)
{
	while(1) {
		__disable_irq();
		if (rand_avail() >= rand_needed) {
			__enable_irq();
			return;
		} else {
			__asm__("wfi");
			__enable_irq();
		}
	}
}

static int g_ctap_initialized = 0;

int is_ctap_initialized()
{
	return g_ctap_initialized;
}

volatile int g_work_to_do = 0;

extern struct hc_device_data *_root_page;

static int is_erased_root_page()
{
	const u32 *p = (const u32 *)_root_page;
	for (int i = 0; i < (BLK_SIZE/4); i++) {
		if (p[i] != 0xffffffff) {
			return 0;
		}
	}
	return 1;
}

static int is_memtest_root_page()
{
	const u8 *p = (const u8 *)_root_page;
	for (int i = (offsetof(struct hc_device_data, data_iteration) + 4) /* skip CRC and data iteration. HC_TODO */; i < BLK_SIZE; i++) {
		if (p[i] != 0x80) {
			return 0;
		}
	}
	return 1;
}

static int emmc_compare_test(const u8 *write_block, u8 *read_block, int blink_off, int blink_on)
{
	int blink_total = blink_off + blink_on;
	int nr_sub_blocks = g_hmmc1.MmcCard.BlockNbr;
	int nr_blocks = nr_sub_blocks/(HC_BLOCK_SZ/EMMC_SUB_BLOCK_SZ);
	for (int i = 0; i < nr_blocks; i++) {
		unsigned int cardState;
		do {
			cardState = HAL_MMC_GetCardState(&g_hmmc1);
		} while (cardState != HAL_MMC_CARD_TRANSFER);
		int ms_start = HAL_GetTick();
		HAL_MMC_WriteBlocks_DMA_Initial(&g_hmmc1,
						write_block,
						BLK_SIZE,
						i*(BLK_SIZE/MSC_MEDIA_PACKET),
						BLK_SIZE/MSC_MEDIA_PACKET);
		while (1) {
			int ms = HAL_GetTick();
			if ((ms % blink_total) > blink_off) {
				led_on();
			} else {
				led_off();
			}
			if (ms > (ms_start + 1000)) {
				return 2;
			}
			command_idle();
			if (HAS_WORK(WRITE_TEST_TX_WORK)) {
				END_WORK(WRITE_TEST_TX_WORK);
				break;
			}
		}
	}
	for (int i = 0; i < nr_blocks; i++) {
		unsigned int cardState;
		do {
			cardState = HAL_MMC_GetCardState(&g_hmmc1);
		} while (cardState != HAL_MMC_CARD_TRANSFER);
		int ms_start = HAL_GetTick();
		HAL_MMC_ReadBlocks_DMA(&g_hmmc1,
					read_block,
					i*(BLK_SIZE/MSC_MEDIA_PACKET),
					BLK_SIZE/MSC_MEDIA_PACKET);
		while (1) {
			int ms = HAL_GetTick();
			if ((ms % blink_total) > blink_off) {
				led_on();
			} else {
				led_off();
			}
			if (ms > (ms_start + 1000)) {
				return 4;
			}
			command_idle();
			if (HAS_WORK(READ_TEST_TX_WORK)) {
				END_WORK(READ_TEST_TX_WORK);
				break;
			}
		}
		if (memcmp(write_block, read_block, BLK_SIZE)) {
			return 8;
		}
	}
	led_off();
	return 0;
}

#define CLOCK_RATE_MEASUREMENT 0

int main (void)
{
	SCB_EnableICache();
	//We don't enable DCACHE tp ensure coherency for DMA transfers
	//SCB_EnableDCache();
	HAL_Init();
	SystemClock_Config();
	MX_GPIO_Init();

#if !CLOCK_RATE_MEASUREMENT
	int blink_duration = 250;
	led_on();
	HAL_Delay(blink_duration);
	led_off();

	MX_DMA_Init();
	MX_AES_Init();
#if ENABLE_MMC
	MX_SDMMC1_MMC_Init();
#endif
#endif

	//TODO: move this elsewhere
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

#if CLOCK_RATE_MEASUREMENT
	u64 clk = 0;
	u32 clk_prev;
	clk_prev = DWT->CYCCNT;
	while(1) {
		u32 clk_cur = DWT->CYCCNT;
		clk += (clk_cur - clk_prev);
		clk_prev = clk_cur;

		u32 sec = clk / SystemCoreClock;

		if ((sec % 10) < 5) {
			led_off();
		} else {
			led_on();
		}
	}
#endif

	__HAL_RCC_CRC_CLK_ENABLE();
	crc_init();

#ifdef BOOT_MODE_B
	rand_init();

	HAL_NVIC_SetPriority(RNG_IRQn, DEFAULT_INT_PRIORITY, 0);
	HAL_NVIC_EnableIRQ(RNG_IRQn);
	__HAL_RCC_RNG_CLK_ENABLE();
	rng_init();

	HAL_NVIC_SetPriority(RTC_WKUP_IRQn, DEFAULT_INT_PRIORITY, 0);
	HAL_NVIC_EnableIRQ(RTC_WKUP_IRQn);
	EXTI->IMR |= (1<<RTC_EXTI_LINE);
	EXTI->RTSR |= (1<<RTC_EXTI_LINE);
	__HAL_RCC_RTC_CLK_ENABLE();
	__HAL_RCC_RTC_ENABLE();
	rtc_rand_init(8);
#endif

#ifdef USE_UART
	MX_USART1_UART_Init();
#endif
#ifdef ENABLE_FIDO2
	mp_set_memory_functions(mp_alloc, mp_realloc, mp_dealloc);
#endif

	SYSCFG->EXTICR[BUTTON_PIN_NUM / 4] = (BUTTON_PORT_NUM << (4 * (BUTTON_PIN_NUM % 4)));
	EXTI->FTSR |= (1 << BUTTON_PIN_NUM);
	EXTI->IMR |= (1 << BUTTON_PIN_NUM);
	HAL_NVIC_SetPriority(BUTTON_IRQ, HIGH_INT_PRIORITY, 0);
	HAL_NVIC_EnableIRQ(BUTTON_IRQ);

	HAL_Delay(5);

#ifdef BOOT_MODE_B
	db3_init();
#endif
	cmd_init();
#if ENABLE_MEMORY_TEST
	int memory_test_mode = 0;
#endif
	if (is_erased_root_page()) {
		int press_count = 0;
		while(1) {
			if (HAS_WORK(BUTTON_PRESS_WORK)) {
				END_WORK(BUTTON_PRESS_WORK);
				if (!s_button_state) {
					s_button_state = 1;
					press_count++;
					led_on();
					BEGIN_WORK(BUTTON_PRESSING_WORK);
				}
			}
			int current_button_state = buttonState() ? 0 : 1;
			int ms_count = HAL_GetTick();
			if (!current_button_state && s_button_state && (ms_count - g_ms_last_pressed) > 100) {
				s_button_state = 0;
				led_off();
				END_WORK(BUTTON_PRESSING_WORK);
				if (press_count == 5) {
					press_count = 0;
					HAL_Delay(400);
					busy_blink_once(100);
				}
			}
			if (s_button_state && ((ms_count - g_ms_last_pressed) > 2000)) {
				s_button_state = 0;
				END_WORK(BUTTON_PRESSING_WORK);
				break;
			}
		}
		memset(g_bulkBuffer, 0x80, BLK_SIZE);
		write_root_block(g_bulkBuffer, BLK_SIZE);
		do {
			flash_idle();
		} while(!is_flash_idle());
		busy_blink(300,300);
	}
#if ENABLE_MEMORY_TEST
	else if (is_memtest_root_page()) {
		emmc_user_queue(EMMC_USER_TEST);

		for (int i = 0; i < HC_BLOCK_SZ; i += 2) {
			g_bulkBuffer[i + 0] = 0xaa;
			g_bulkBuffer[i + 1] = 0x55;
		}
		int rc = emmc_compare_test(g_bulkBuffer,
				g_bulkBuffer + HC_BLOCK_SZ,
				4900, 100);
	        if (rc)	{
			busy_blink(rc * 1000, rc * 1000);
		}
		memory_test_mode = 1;
	}
#endif

#ifdef ENABLE_FIDO2
    	crypto_ecc256_init();
	authenticator_initialize();
	ctaphid_init();

	ctap_init_begin();

	if (!ctap_is_state_initialized()) {
		led_on();
		rand_set_rewind_point();
		crypto_random_init();
    		if (!ctap_reset()) {
			int rand_needed = crypto_random_get_requested();
			crypto_random_init();
			rand_rewind();
			wait_for_rand(rand_needed);
			if (ctap_reset()) {
				sync_root_block();
			}
		} else {
			sync_root_block();
		}

		if (sync_root_block_pending()) {
			sync_root_block_immediate();
			while (flash_idle_ready()) {
				flash_idle();
			}
		}
		if (sync_root_block_error()) {
			//TODO: Handle error
		}
		led_off();
	}

	crypto_random_init();
	rand_clear_rewind_point();
	rand_set_rewind_point();
	ctap_init_finish();

	int ctap_init_rand_needed = crypto_random_get_requested();

	if (crypto_random_get_served() == ctap_init_rand_needed) {
		g_ctap_initialized = 1;
		rand_clear_rewind_point();
	} else {
		rand_rewind();
	}

#if ENABLE_MEMORY_TEST
	if (memory_test_mode)
		busy_blink(400, 100);
#endif
#endif

	USBD_Init(&USBD_Device, &Multi_Desc, 0);
	USBD_RegisterClass(&USBD_Device, USBD_MULTI_CLASS);
	USBD_Start(&USBD_Device);

	int work_led_off_ms = 0;
	int work_led_state = 0;

	while (1) {
		__disable_irq();
		int work_to_do = g_work_to_do;
#if ENABLE_FIDO2
		if (!g_ctap_initialized && (rand_avail() >= ctap_init_rand_needed) && device_subsystem_owner() == NO_SUBSYSTEM) {
			work_to_do = 1;
			__enable_irq();
			if (request_device(CTAP_STARTUP_SUBSYSTEM)) {
				ctap_init_finish();
				g_ctap_initialized = 1;
				release_device_request(CTAP_STARTUP_SUBSYSTEM);
			}
		} else if (!work_to_do) {
			HAL_SuspendTick();
			__asm__("wfi");
			HAL_ResumeTick();
			__enable_irq();
		} else {
			__enable_irq();
		}
#else
		if (!work_to_do) {
			HAL_SuspendTick();
			__asm__("wfi");
			HAL_ResumeTick();
		}
		__enable_irq();
#endif
		int ms_count = HAL_GetTick();
		const int work_to_light = KEYBOARD_WORK | SYNC_ROOT_BLOCK_WORK | FLASH_WORK;
		if ((g_work_to_do & work_to_light) || g_emmc_user == EMMC_USER_STORAGE || g_emmc_user == EMMC_USER_DB) {
			if (!work_led_state) {
				work_led_off_ms = ms_count + 40;
				BEGIN_WORK(WORK_STATUS_WORK);
			}
			work_led_state = 1;
		} else {
			if (work_led_state) {
				work_led_off_ms = ms_count + 10;
			}
			work_led_state = 0;
		}
		if (!HAS_WORK(BLINK_WORK)) {
			if (work_led_state || (!work_led_state && ms_count < work_led_off_ms)) {
				led_on();
			} else {
				END_WORK(WORK_STATUS_WORK);
				led_off();
			}
		}

		if (ms_count > s_timer_target && s_timer_target != 0) {
			timer_timeout();
			s_timer_target = 0;
			END_WORK(TIMER_WORK);
		}
#if ENABLE_MMC_STANDBY
		if (g_emmc_idle_ms != -1 && ms_count > (g_emmc_idle_ms + 1000)) {
			g_emmc_idle_ms = -1;
			END_WORK(MMC_IDLE_WORK);
			emmc_user_queue(EMMC_USER_STANDBY);
		}
#endif
		usb_keyboard_idle();
		blink_idle();
		command_idle();
		if (sync_root_block_pending() && is_flash_idle() && !sync_root_block_writing() && !sync_root_block_error()) {
			sync_root_block_immediate();
		}
		flash_idle();
		usbd_scsi_idle();
		int current_button_state = buttonState() ? 0 : 1;

		if (HAS_WORK(BUTTON_PRESS_WORK)) {
			END_WORK(BUTTON_PRESS_WORK);
			if (!s_button_state) {
				switch (device_subsystem_owner()) {
				case SIGNET_SUBSYSTEM:
					button_press();
					break;
#ifdef ENABLE_FIDO2
				case CTAP_SUBSYSTEM:
					ctaphid_press();
					break;
#endif
				case NO_SUBSYSTEM:
					button_press_unprompted();
					break;
				default:
					break;
				}
				s_button_state = 1;
				BEGIN_WORK(BUTTON_PRESSING_WORK);
			}
		}
		if (!current_button_state && s_button_state && (ms_count - g_ms_last_pressed) > 100) {
			button_release();
			s_button_state = 0;
			END_WORK(BUTTON_PRESSING_WORK);
		}
		if (s_button_state && ((ms_count - g_ms_last_pressed) > 2000)) {
			s_button_state = 0;
			END_WORK(BUTTON_PRESSING_WORK);
			switch (device_subsystem_owner()) {
			case SIGNET_SUBSYSTEM:
				long_button_press();
				break;
			default:
				break;
			}
		}
	}
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
static void SystemClock_Config(void)
{
	RCC_OscInitTypeDef RCC_OscInitStruct = {0};
	RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
	RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};

	/** Configure the main internal regulator output voltage
	*/
	__HAL_RCC_PWR_CLK_ENABLE();
	__HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
	/** Initializes the CPU, AHB and APB busses clocks
	*/
	RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
	RCC_OscInitStruct.HSEState = RCC_HSE_ON;
	RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
	RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
#ifdef VCC_1_8
	RCC_OscInitStruct.PLL.PLLM = 2*3;
	RCC_OscInitStruct.PLL.PLLN = 48*3;
	RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
	RCC_OscInitStruct.PLL.PLLQ = 2*3;
#else
	RCC_OscInitStruct.PLL.PLLM = 6;
	RCC_OscInitStruct.PLL.PLLN = 216;
	RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
	RCC_OscInitStruct.PLL.PLLQ = 9;
#endif
	if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
		Error_Handler();
	}
	/** Activate the Over-Drive mode
	*/
#ifndef VCC_1_8
	if (HAL_PWREx_EnableOverDrive() != HAL_OK) {
		Error_Handler();
	}
#endif

	RCC_OscInitStruct.OscillatorType =  RCC_OSCILLATORTYPE_LSI | RCC_OSCILLATORTYPE_LSE;
	RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
	RCC_OscInitStruct.LSIState = RCC_LSI_ON;
	RCC_OscInitStruct.LSEState = RCC_LSE_OFF;
	if(HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
	{
		Error_Handler();
	}

	RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
	RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
	RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
	RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
	RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

	if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_7) != HAL_OK) {
		Error_Handler();
	}
	PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_USART1|RCC_PERIPHCLK_SDMMC1 |RCC_PERIPHCLK_CLK48;
#ifdef BOOT_MODE_B
	PeriphClkInitStruct.PeriphClockSelection |= RCC_PERIPHCLK_RTC;
#endif
	PeriphClkInitStruct.Usart1ClockSelection = RCC_USART1CLKSOURCE_PCLK2;
	PeriphClkInitStruct.Clk48ClockSelection = RCC_CLK48SOURCE_PLL;
	PeriphClkInitStruct.Sdmmc1ClockSelection = RCC_SDMMC1CLKSOURCE_CLK48;
#ifdef BOOT_MODE_B
	PeriphClkInitStruct.RTCClockSelection = RCC_RTCCLKSOURCE_LSI;
#endif
	if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK) {
		Error_Handler();
	}
}

static void MX_AES_Init(void)
{
#ifdef BOOT_MODE_B
	g_hcryp.Instance = AES;
	g_hcryp.Init.DataType = CRYP_DATATYPE_32B;
	g_hcryp.Init.KeySize = CRYP_KEYSIZE_128B;
	g_hcryp.Init.pKey = (uint32_t *)pKeyAES;
	g_hcryp.Init.Algorithm = CRYP_AES_CBC;
	g_hcryp.Init.DataWidthUnit = CRYP_DATAWIDTHUNIT_WORD;
	if (HAL_CRYP_Init(&g_hcryp) != HAL_OK) {
		Error_Handler();
	}
#endif
}

/**
  * @brief SDMMC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SDMMC1_MMC_Init(void)
{
	g_hmmc1.Instance = SDMMC1;
	g_hmmc1.Init.ClockEdge = SDMMC_CLOCK_EDGE_RISING;
	g_hmmc1.Init.ClockBypass = SDMMC_CLOCK_BYPASS_ENABLE;
	g_hmmc1.Init.ClockPowerSave = SDMMC_CLOCK_POWER_SAVE_DISABLE;
	g_hmmc1.Init.BusWide = SDMMC_BUS_WIDE_1B;
	g_hmmc1.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_ENABLE;
	g_hmmc1.Init.ClockDiv = 0;
	if (HAL_MMC_Init(&g_hmmc1) != HAL_OK) {
		Error_Handler();
	}
	if (HAL_MMC_ConfigWideBusOperation(&g_hmmc1, SDMMC_BUS_WIDE_8B) != HAL_OK) {
		Error_Handler();
	}
}

static void MX_DMA_Init(void)
{
	__HAL_RCC_DMA2_CLK_ENABLE();
	HAL_NVIC_SetPriority(DMA2_Stream3_IRQn, LOW_INT_PRIORITY, 0);
	HAL_NVIC_EnableIRQ(DMA2_Stream3_IRQn);
	HAL_NVIC_SetPriority(DMA2_Stream5_IRQn, LOW_INT_PRIORITY, 0);
	HAL_NVIC_EnableIRQ(DMA2_Stream5_IRQn);
	HAL_NVIC_SetPriority(DMA2_Stream6_IRQn, LOW_INT_PRIORITY, 0);
	HAL_NVIC_EnableIRQ(DMA2_Stream6_IRQn);
}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */

#ifdef USE_UART
static void MX_USART1_UART_Init(void)
{
	g_huart1.Instance = USART1;
	g_huart1.Init.BaudRate = 115200;
	g_huart1.Init.WordLength = UART_WORDLENGTH_8B;
	g_huart1.Init.StopBits = UART_STOPBITS_1;
	g_huart1.Init.Parity = UART_PARITY_NONE;
	g_huart1.Init.Mode = UART_MODE_TX_RX;
	g_huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
	g_huart1.Init.OverSampling = UART_OVERSAMPLING_16;
	g_huart1.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
	g_huart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
	if (HAL_UART_Init(&g_huart1) != HAL_OK) {
		Error_Handler();
	}
}
#endif

static void MX_GPIO_Init(void)
{
	GPIO_InitTypeDef GPIO_InitStruct = {0};

	/* GPIO Ports Clock Enable */
	__HAL_RCC_GPIOB_CLK_ENABLE();
	__HAL_RCC_GPIOC_CLK_ENABLE();
	__HAL_RCC_GPIOD_CLK_ENABLE();
	__HAL_RCC_GPIOI_CLK_ENABLE();
	__HAL_RCC_GPIOH_CLK_ENABLE();
#ifdef BOARD_REV_2
	__HAL_RCC_GPIOG_CLK_ENABLE();
	__HAL_RCC_GPIOE_CLK_ENABLE();
#endif

	/*Configure GPIO pin Output Level */
	HAL_GPIO_WritePin(GPIOI, GPIO_PIN_11, GPIO_PIN_RESET);

	/*Configure GPIO pin Output Level */
	HAL_GPIO_WritePin(GPIOD, GPIO_PIN_13, GPIO_PIN_RESET);

	/*Configure GPIO pin : PD0 (switch) */
	GPIO_InitStruct.Pin = BUTTON_PIN;
	GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
	GPIO_InitStruct.Pull = GPIO_NOPULL;
	HAL_GPIO_Init(BUTTON_PORT, &GPIO_InitStruct);

	/*Configure GPIO pin : PI11 (Status LED 1)*/
	GPIO_InitStruct.Pin = LED1_PIN;
	GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
	GPIO_InitStruct.Pull = GPIO_NOPULL;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(LED1_PORT, &GPIO_InitStruct);

	/*Configure GPIO pin : PD13 (Status LED 2)*/
	GPIO_InitStruct.Pin = LED2_PIN;
	GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
	GPIO_InitStruct.Pull = GPIO_NOPULL;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(LED2_PORT, &GPIO_InitStruct);
}

void Error_Handler(void)
{
	/* User can add his own implementation to report the HAL error return state */
	__disable_irq();
	__set_BASEPRI(__get_BASEPRI() & 0xf0);
	while(1)
		__asm__("wfi");
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
	/* User can add his own implementation to report the file name and line number,
	   tex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
}
#endif /* USE_FULL_ASSERT */
