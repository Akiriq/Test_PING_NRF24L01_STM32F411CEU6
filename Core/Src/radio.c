/*
 * radio.c
 *
 *  Created on: February 21, 2025
 *      Author: rguilletlhomat
 */
#include <memory.h>
#include "cmsis_os2.h"
#include "support.h"
#include "nrf24.h"
#include "usb_device.h"


#define HEX_CHARS      "0123456789ABCDEF"

extern UART_HandleTypeDef huart2;
extern uint8_t CDC_Transmit_FS(uint8_t* Buf, uint16_t Len);


void UART_SendChar(char b)
{
    //HAL_UART_Transmit(&huart2, (uint8_t *) &b, 1, 200);
    CDC_Transmit_FS((uint8_t *) &b, 1);
}

void UART_SendStr(char *string)
{
    //HAL_UART_Transmit(&huart2, (uint8_t *) string, (uint16_t) strlen(string), 200);

	CDC_Transmit_FS((uint8_t *) string,sizeof(string));
}

void Toggle_LED()
{
    HAL_GPIO_TogglePin(LD2_GPIO_Port,LD2_Pin);
}

void UART_SendBufHex(char *buf, uint16_t bufsize) {
    uint16_t i;
    char ch;
    for (i = 0; i < bufsize; i++) {
        ch = *buf++;
        UART_SendChar(HEX_CHARS[(ch >> 4) % 0x10]);
        UART_SendChar(HEX_CHARS[(ch & 0x0f) % 0x10]);
    }
}

void UART_SendHex8(uint16_t num) {
    UART_SendChar(HEX_CHARS[(num >> 4) % 0x10]);
    UART_SendChar(HEX_CHARS[(num & 0x0f) % 0x10]);
}

void UART_SendInt(int32_t num) {
    char str[10]; // 10 chars max for INT32_MAX
    int i = 0;
    if (num < 0) {
        UART_SendChar('-');
        num *= -1;
    }
    do str[i++] = (char) (num % 10 + '0'); while ((num /= 10) > 0);
    for (i--; i >= 0; i--) UART_SendChar(str[i]);
}

uint8_t nRF24_payload[32];

// Pipe number
nRF24_RXResult pipe;

uint32_t i, j, k;

// Length of received pay_load
uint8_t payload_length;

// Helpers for transmit mode demo

// Timeout counter (depends on the CPU speed)
// Used for not stuck waiting for IRQ
#define nRF24_WAIT_TIMEOUT         (uint32_t)0x000FFFFF

// Result of packet transmission
typedef enum {
	nRF24_TX_ERROR  = (uint8_t)0x00, // Unknown error
	nRF24_TX_SUCCESS,                // Packet has been transmitted successfully
	nRF24_TX_TIMEOUT,                // It was timeout during packet transmit
	nRF24_TX_MAXRT                   // Transmit failed with maximum auto retransmit count
} nRF24_TXResult;

nRF24_TXResult tx_res;

// Function to transmit data packet
// input:
//   pBuf - pointer to the buffer with data to transmit
//   length - length of the data buffer in bytes
// return: one of nRF24_TX_xx values
nRF24_TXResult nRF24_TransmitPacket(uint8_t *pBuf, uint8_t length)
{
	volatile uint32_t wait = nRF24_WAIT_TIMEOUT;
	uint8_t status;

	// Deassert the CE pin (in case if it still high)
	nRF24_CE_L();

	// Transfer a data from the specified buffer to the TX FIFO
	nRF24_WritePayload(pBuf, length);

	// Start a transmission by asserting CE pin (must be held at least 10us)
	nRF24_CE_H();

	// Poll the transceiver status register until one of the following flags will be set:
	//   TX_DS  - means the packet has been transmitted
	//   MAX_RT - means the maximum number of TX retransmits happened
	// note: this solution is far from perfect, better to use IRQ instead of polling the status
	do {
		status = nRF24_GetStatus();
		if (status & (nRF24_FLAG_TX_DS | nRF24_FLAG_MAX_RT)) {
			break;
		}
	} while (wait--);

	// Deassert the CE pin (Standby-II --> Standby-I)
	nRF24_CE_L();

	if (!wait) {
		// Timeout
		return nRF24_TX_TIMEOUT;
	}

	// Check the flags in STATUS register
	UART_SendStr("[");
	UART_SendHex8(status);
	UART_SendStr("] ");

	// Clear pending IRQ flags
    nRF24_ClearIRQFlags();

	if (status & nRF24_FLAG_MAX_RT) {
		// Auto retransmit counter exceeds the programmed maximum limit (FIFO is not removed)
		return nRF24_TX_MAXRT;
	}

	if (status & nRF24_FLAG_TX_DS) {
		// Successful transmission
		return nRF24_TX_SUCCESS;
	}

	// Some banana happens, a payload remains in the TX FIFO, flush it
	nRF24_FlushTX();

	return nRF24_TX_ERROR;
}

void send_payload(uint8_t* payload, uint8_t length)
{

    // Set operational mode (PTX == transmitter)
    nRF24_SetOperationalMode(nRF24_MODE_TX);

    // Clear any pending IRQ flags
    nRF24_ClearIRQFlags();

	UART_SendStr("PAYLOAD:>");
	UART_SendBufHex((char *)payload, length);
	UART_SendStr("< ... TX: ");

	// Transmit a packet
	tx_res = nRF24_TransmitPacket(payload, length);
	switch (tx_res) {
		case nRF24_TX_SUCCESS:
			UART_SendStr("OK");
			break;
		case nRF24_TX_TIMEOUT:
			UART_SendStr("TIMEOUT");
			break;
		case nRF24_TX_MAXRT:
			UART_SendStr("MAX RETRANSMIT");
			break;
		default:
			UART_SendStr("ERROR");
			break;
	}
	UART_SendStr("\r\n");
	osDelay(5);
    // Set operational mode (PRX == receiver)
    nRF24_SetOperationalMode(nRF24_MODE_RX);


    // Put the transceiver to the RX mode
    nRF24_CE_H();
}




void runRadio(void)
{


	UART_SendStr("\r\nSTM32F411CEU6 is online.\r\n");
	osDelay(1000);
	// RX/TX disabled
	nRF24_CE_L();

	// Configure the nRF24L01+
	UART_SendStr("nRF24L01+ check: ");
	osDelay(1000);
	if (!nRF24_Check())
	{
		UART_SendStr("FAIL\r\n");
		while (1)
		{
			Toggle_LED();
			osDelay(100);
			if (nRF24_Check()) break;
		}
	}

	UART_SendStr("OK\r\n");


	// Initialize the nRF24L01 to its default state
	nRF24_Init();




	// This is simple receiver/transmitter :
	//   - pipe#1 address	: '0xE7 0x1C 0xE4'
	//   - TX address		: '0xE7 0x1C 0xE3'
	//   - payload			: 5 bytes
	//   - RF channel		: 115 (2515MHz)
	//   - data rate		: 250kbps (minimum possible, to increase reception reliability)
	//   - CRC scheme		: 2 byte

    // The transmitter sends a 5-byte packets to the address '0xE7 0x1C 0xE3' without Auto-ACK (ShockBurst disabled)

    // Disable ShockBurst for all RX pipes
    nRF24_DisableAA(0xFF);

    // Set RF channel
    nRF24_SetRFChannel(115);

    // Set data rate
    nRF24_SetDataRate(nRF24_DR_250kbps);

    // Set CRC scheme
    nRF24_SetCRCScheme(nRF24_CRC_2byte);

    // Set address width, its common for all pipes (RX and TX)
    nRF24_SetAddrWidth(3);

    // Configure RX PIPE#1
    static const uint8_t nRF24_ADDR_Rx[] = { 0xE7, 0x1C, 0xE4 };
    nRF24_SetAddr(nRF24_PIPE1, nRF24_ADDR_Rx); // program address for RX pipe #1
    nRF24_SetRXPipe(nRF24_PIPE1, nRF24_AA_OFF, 5); // Auto-ACK: disabled, payload length: 5 bytes

    // Configure TX PIPE
    static const uint8_t nRF24_ADDR_Tx[] = { 0xE7, 0x1C, 0xE3 };
    nRF24_SetAddr(nRF24_PIPETX, nRF24_ADDR_Tx); // program TX address

    // Set TX power (maximum)
    nRF24_SetTXPower(nRF24_TXPWR_0dBm);

    // Set operational mode (PRX == receiver)
    nRF24_SetOperationalMode(nRF24_MODE_RX);

    // Wake the transceiver
    nRF24_SetPowerMode(nRF24_PWR_UP);

    // Put the transceiver to the RX mode
    nRF24_CE_H();

    uint8_t BP_released = 1;
    // The main loop
    while (1) {
    	//
    	// Constantly poll the status of the RX FIFO and get a payload if FIFO is not empty
    	//
    	// This is far from best solution, but it's ok for testing purposes
    	// More smart way is to use the IRQ pin :)
    	//
    	if (nRF24_GetStatus_RXFIFO() != nRF24_STATUS_RXFIFO_EMPTY)
    	{
    		// Get a payload from the transceiver
    		pipe = nRF24_ReadPayload(nRF24_payload, &payload_length);

    		// Clear all pending IRQ flags
			nRF24_ClearIRQFlags();

			// Print a payload contents to UART
			UART_SendStr("RCV PIPE#");
			UART_SendInt(pipe);
			UART_SendStr(" PAYLOAD:>");
			UART_SendBufHex((char *)nRF24_payload, payload_length);
			UART_SendStr("<\r\n");
			// send back the payload
			send_payload(nRF24_payload, payload_length);
    	}
    	if(!HAL_GPIO_ReadPin(BP_GPIO_Port, BP_Pin)&& BP_released)
    	{
    		BP_released = 0;

    		uint8_t payload[32] = {0x01,0x23,0x45,0x67,0x89};
//        	for (i = 0; i < 5; i++) {
//        		payload[i] = j++;
//        		if (j > 0x000000FF) j = 0;
//        	}


    		send_payload(payload, 5);
    		osDelay(10);
    	}
    	if(HAL_GPIO_ReadPin(BP_GPIO_Port, BP_Pin) && !BP_released)
    	{

    		BP_released = 1;
    		osDelay(10);

    	}

    }
}
