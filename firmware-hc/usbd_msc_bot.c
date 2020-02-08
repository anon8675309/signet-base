
#include "usbd_msc_bot.h"
#include "usbd_msc.h"
#include "usbd_msc_scsi.h"
#include "usbd_ioreq.h"
#include "usbd_multi.h"

/** @defgroup MSC_BOT_Private_FunctionPrototypes
  * @{
  */
static void MSC_BOT_CBW_Decode (USBD_HandleTypeDef  *pdev);

static void MSC_BOT_SendData (USBD_HandleTypeDef *pdev, uint8_t* pbuf,
                              uint16_t len);

static void MSC_BOT_Abort(USBD_HandleTypeDef  *pdev);


/**
* @brief  MSC_BOT_Init
*         Initialize the BOT Process
* @param  pdev: device instance
* @retval None
*/
void MSC_BOT_Init (USBD_HandleTypeDef  *pdev)
{
	USBD_MSC_BOT_HandleTypeDef  *hmsc = (USBD_MSC_BOT_HandleTypeDef*)pdev->pClassData[INTERFACE_MSC];

	hmsc->bot_state = USBD_BOT_IDLE;
	hmsc->bot_status = USBD_BOT_STATUS_NORMAL;

	hmsc->scsi_sense_tail = 0U;
	hmsc->scsi_sense_head = 0U;

	((USBD_StorageTypeDef *)pdev->pUserData)->Init(0U);

	USBD_LL_FlushEP(pdev, MSC_EPOUT_ADDR);
	USBD_LL_FlushEP(pdev, MSC_EPIN_ADDR);

	/* Prapare EP to Receive First BOT Cmd */
	USBD_LL_PrepareReceive (pdev, MSC_EPOUT_ADDR, (uint8_t *)(void *)&hmsc->cbw,
	                        USBD_BOT_CBW_LENGTH);
}

/**
* @brief  MSC_BOT_Reset
*         Reset the BOT Machine
* @param  pdev: device instance
* @retval  None
*/
void MSC_BOT_Reset (USBD_HandleTypeDef  *pdev)
{
	USBD_MSC_BOT_HandleTypeDef  *hmsc = (USBD_MSC_BOT_HandleTypeDef*)pdev->pClassData[INTERFACE_MSC];

	hmsc->bot_state  = USBD_BOT_IDLE;
	hmsc->bot_status = USBD_BOT_STATUS_RECOVERY;

	/* Prapare EP to Receive First BOT Cmd */
	USBD_LL_PrepareReceive (pdev, MSC_EPOUT_ADDR, (uint8_t *)(void *)&hmsc->cbw,
	                        USBD_BOT_CBW_LENGTH);
}

/**
* @brief  MSC_BOT_DeInit
*         Deinitialize the BOT Machine
* @param  pdev: device instance
* @retval None
*/
void MSC_BOT_DeInit (USBD_HandleTypeDef  *pdev)
{
	USBD_MSC_BOT_HandleTypeDef  *hmsc = (USBD_MSC_BOT_HandleTypeDef*)pdev->pClassData[INTERFACE_MSC];
	hmsc->bot_state  = USBD_BOT_IDLE;
}

/**
* @brief  MSC_BOT_DataIn
*         Handle BOT IN data stage
* @param  pdev: device instance
* @param  epnum: endpoint index
* @retval None
*/
void MSC_BOT_DataIn (USBD_HandleTypeDef  *pdev,
                     uint8_t epnum)
{
	USBD_MSC_BOT_HandleTypeDef  *hmsc = (USBD_MSC_BOT_HandleTypeDef*)pdev->pClassData[INTERFACE_MSC];

	switch (hmsc->bot_state) {
	case USBD_BOT_DATA_IN:
		if(SCSI_ProcessCmd(pdev,
		                   hmsc->cbw.bLUN,
		                   &hmsc->cbw.CB[0]) < 0) {
			USBD_LL_StallEP(pdev, MSC_EPIN_ADDR);
			MSC_BOT_SendCSW (pdev, USBD_CSW_CMD_FAILED);
		}
		break;

	case USBD_BOT_SEND_DATA:
		MSC_BOT_SendCSW (pdev, USBD_CSW_CMD_PASSED);
		break;
	case USBD_BOT_LAST_DATA_IN:
		MSC_BOT_SendCSW (pdev, USBD_CSW_CMD_PASSED);
		break;
	case USBD_BOT_NO_DATA:
		MSC_BOT_SendCSW (pdev, USBD_CSW_CMD_PASSED);
		break;
	default:
		break;
	}
}
/**
* @brief  MSC_BOT_DataOut
*         Process MSC OUT data
* @param  pdev: device instance
* @param  epnum: endpoint index
* @retval None
*/
void MSC_BOT_DataOut (USBD_HandleTypeDef  *pdev,
                      uint8_t epnum)
{
	USBD_MSC_BOT_HandleTypeDef  *hmsc = (USBD_MSC_BOT_HandleTypeDef*)pdev->pClassData[INTERFACE_MSC];

	switch (hmsc->bot_state) {
	case USBD_BOT_IDLE:
		MSC_BOT_CBW_Decode(pdev);
		break;

	case USBD_BOT_DATA_OUT:
		if(SCSI_ProcessCmd(pdev,
		                   hmsc->cbw.bLUN,
		                   &hmsc->cbw.CB[0]) < 0) {
			USBD_LL_StallEP(pdev, MSC_EPIN_ADDR);
			MSC_BOT_SendCSW (pdev, USBD_CSW_CMD_FAILED);
		}
		break;

	default:
		break;
	}
}

/**
* @brief  MSC_BOT_CBW_Decode
*         Decode the CBW command and set the BOT state machine accordingly
* @param  pdev: device instance
* @retval None
*/
static void  MSC_BOT_CBW_Decode (USBD_HandleTypeDef  *pdev)
{
	USBD_MSC_BOT_HandleTypeDef  *hmsc = (USBD_MSC_BOT_HandleTypeDef*)pdev->pClassData[INTERFACE_MSC];

	hmsc->csw.dTag = hmsc->cbw.dTag;
	hmsc->csw.dDataResidue = hmsc->cbw.dDataLength;

	if ((USBD_LL_GetRxDataSize (pdev,MSC_EPOUT_ADDR) != USBD_BOT_CBW_LENGTH) ||
	    (hmsc->cbw.dSignature != USBD_BOT_CBW_SIGNATURE) ||
	    (hmsc->cbw.bLUN >= MAX_SCSI_VOLUMES) ||
	    (hmsc->cbw.bCBLength < 1U) || (hmsc->cbw.bCBLength > 16U)) {

		SCSI_SenseCode(pdev, hmsc->cbw.bLUN, ILLEGAL_REQUEST, INVALID_CDB, 0);

		hmsc->bot_status = USBD_BOT_STATUS_ERROR;
		MSC_BOT_Abort(pdev);
	} else {
		if(SCSI_ProcessCmd(pdev, hmsc->cbw.bLUN, &hmsc->cbw.CB[0]) < 0) {
			if (hmsc->bot_state == USBD_BOT_NO_DATA) {
				USBD_LL_StallEP(pdev, MSC_EPIN_ADDR);
				MSC_BOT_SendCSW (pdev, USBD_CSW_CMD_FAILED);
			} else {
				MSC_BOT_Abort(pdev);
			}
		}
		/*Burst xfer handled internally*/
		else if ((hmsc->bot_state != USBD_BOT_DATA_IN) &&
		         (hmsc->bot_state != USBD_BOT_DATA_OUT) &&
		         (hmsc->bot_state != USBD_BOT_LAST_DATA_IN) &&
			 (hmsc->bot_state != USBD_BOT_SEND_DATA)) {
			if (hmsc->bot_data_length > 0U) {
				MSC_BOT_SendData(pdev, hmsc->bot_data, hmsc->bot_data_length);
			} else if (hmsc->bot_data_length == 0U) {
				MSC_BOT_SendCSW (pdev, USBD_CSW_CMD_PASSED);
			} else {
				MSC_BOT_Abort(pdev);
			}
		} else {
			return;
		}
	}
}

/**
* @brief  MSC_BOT_SendData
*         Send the requested data
* @param  pdev: device instance
* @param  buf: pointer to data buffer
* @param  len: Data Length
* @retval None
*/
static void  MSC_BOT_SendData(USBD_HandleTypeDef *pdev, uint8_t* pbuf,
                              uint16_t len)
{
	USBD_MSC_BOT_HandleTypeDef  *hmsc = (USBD_MSC_BOT_HandleTypeDef*)pdev->pClassData[INTERFACE_MSC];

	uint16_t length = (uint16_t)MIN(hmsc->cbw.dDataLength, len);

	hmsc->csw.dDataResidue -= len;
	hmsc->csw.bStatus = USBD_CSW_CMD_PASSED;
	hmsc->bot_state = USBD_BOT_SEND_DATA;

	USBD_LL_Transmit(pdev, MSC_EPIN_ADDR, pbuf, length);
}

/**
* @brief  MSC_BOT_SendCSW
*         Send the Command Status Wrapper
* @param  pdev: device instance
* @param  status : CSW status
* @retval None
*/
void  MSC_BOT_SendCSW (USBD_HandleTypeDef  *pdev,
                       uint8_t CSW_Status)
{
	USBD_MSC_BOT_HandleTypeDef  *hmsc = (USBD_MSC_BOT_HandleTypeDef*)pdev->pClassData[INTERFACE_MSC];

	hmsc->csw.dSignature = USBD_BOT_CSW_SIGNATURE;
	hmsc->csw.bStatus = CSW_Status;
	hmsc->bot_state = USBD_BOT_IDLE;

	USBD_LL_Transmit (pdev, MSC_EPIN_ADDR, (uint8_t *)(void *)&hmsc->csw,
	                  USBD_BOT_CSW_LENGTH);

	/* Prepare EP to Receive next Cmd */
	USBD_LL_PrepareReceive (pdev, MSC_EPOUT_ADDR, (uint8_t *)(void *)&hmsc->cbw,
	                        USBD_BOT_CBW_LENGTH);
}

/**
* @brief  MSC_BOT_Abort
*         Abort the current transfer
* @param  pdev: device instance
* @retval status
*/

static void  MSC_BOT_Abort (USBD_HandleTypeDef  *pdev)
{
	USBD_MSC_BOT_HandleTypeDef  *hmsc = (USBD_MSC_BOT_HandleTypeDef*)pdev->pClassData[INTERFACE_MSC];

	if ((hmsc->cbw.bmFlags == 0U) &&
	    (hmsc->cbw.dDataLength != 0U) &&
	    (hmsc->bot_status == USBD_BOT_STATUS_NORMAL)) {
		USBD_LL_StallEP(pdev, MSC_EPOUT_ADDR );
	}

	USBD_LL_StallEP(pdev, MSC_EPIN_ADDR);

	if(hmsc->bot_status == USBD_BOT_STATUS_ERROR) {
		USBD_LL_PrepareReceive (pdev, MSC_EPOUT_ADDR, (uint8_t *)(void *)&hmsc->cbw,
		                        USBD_BOT_CBW_LENGTH);
	}
}

/**
* @brief  MSC_BOT_CplClrFeature
*         Complete the clear feature request
* @param  pdev: device instance
* @param  epnum: endpoint index
* @retval None
*/

void  MSC_BOT_CplClrFeature (USBD_HandleTypeDef  *pdev, uint8_t epnum)
{
	USBD_MSC_BOT_HandleTypeDef  *hmsc = (USBD_MSC_BOT_HandleTypeDef*)pdev->pClassData[INTERFACE_MSC];

	if(hmsc->bot_status == USBD_BOT_STATUS_ERROR) { /* Bad CBW Signature */
		USBD_LL_StallEP(pdev, MSC_EPIN_ADDR);
		hmsc->bot_status = USBD_BOT_STATUS_NORMAL;
	} else if(((epnum & 0x80U) == 0x80U) && (hmsc->bot_status != USBD_BOT_STATUS_RECOVERY)) {
		MSC_BOT_SendCSW (pdev, USBD_CSW_CMD_FAILED);
	} else {
		return;
	}
}
