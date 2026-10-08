/**
  ******************************************************************************
  * @file    oximeter.h
  * @brief   DFRobot Heart Rate & Oximeter Sensor V2.0 (MAX30102 plus an
  *          on-board algorithm MCU). Reads back the heart rate and SpO2 the
  *          module has already computed, over I2C.
  *
  *          Register addresses and data layout were taken from DFRobot's
  *          Arduino library DFRobot_BloodOxygen_S (MIT licence) and ported
  *          to the STM32 HAL here.
  ******************************************************************************
  */

#ifndef INC_OXIMETER_H_
#define INC_OXIMETER_H_

/* These includes must come before every declaration below */
#include "stm32l4xx_hal.h"
#include <stdint.h>

/* 7-bit address 0x57, already shifted left by one for the HAL */
#define OXI_I2C_ADDR      (0x57 << 1)     /* = 0xAE */

/* Registers, from the vendor library */
#define OXI_REG_DATA        0x0Cu   /* read 8 bytes: SpO2 and heart rate */
#define OXI_REG_TEMP        0x14u   /* read 2 bytes: board temperature */
#define OXI_REG_COLLECT     0x20u   /* write 2 bytes: 0x0001 start, 0x0002 stop */

/* Plausible range; anything outside counts as invalid */
#define OXI_SPO2_MAX      100u    /* SpO2 can never exceed 100 % */
#define OXI_HR_MAX        250u    /* above this the algorithm has not converged */

typedef struct {
    int16_t spo2;           /* SpO2 in %, -1 when this reading is invalid */
    int32_t heartbeat;      /* heart rate in bpm, -1 when invalid */
} OxiReading;

/** Probe whether the module is on the bus: 1 present, 0 absent. */
uint8_t Oxi_IsPresent(I2C_HandleTypeDef *hi2c);

/** Store the handle and start acquisition. Must be called once before
 * any reading, otherwise the data registers stay at zero. */
void     Oxi_Start(I2C_HandleTypeDef *hi2c);

/** Stop acquisition to save power. Not needed in this lab. */
void      Oxi_Stop(void);

/** Read heart rate and SpO2 once. HAL_OK means the bus transfer worked,
 * which is NOT the same as the reading being valid - check whether
 * out->spo2 and out->heartbeat came back as -1. */
HAL_StatusTypeDef Oxi_Read(OxiReading *out);

/** Board temperature in degrees Celsius; -100.0f if the transfer failed. */
float     Oxi_ReadTemperature(void);

/** Scan the whole bus and return the first 8-bit address that answers,
 * or 0 if nothing does. */
uint8_t   Oxi_ScanBus(I2C_HandleTypeDef *hi2c);

#endif /* INC_OXIMETER_H_ */
