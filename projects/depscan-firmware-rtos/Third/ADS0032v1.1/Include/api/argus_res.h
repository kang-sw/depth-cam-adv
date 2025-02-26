/*************************************************************************//**
 * @file
 * @brief    	This file is part of the AFBR-S50 API.
 * @details		Defines the generic measurement results data structure.
 * 
 * @copyright	Copyright c 2016-2019, Avago Technologies GmbH.
 * 				All rights reserved.
 *****************************************************************************/

#ifndef ARGUS_RES_H
#define ARGUS_RES_H

/*!***************************************************************************
 * @defgroup	argusres Measurement Data
 * @ingroup		argusapi
 *
 * @brief		Measurement results data structures.
 *
 * @details		The interface defines all data structures that correspond to
 * 				the AFBR-S50 measurement results, e.g.
 * 					- 1D distance and amplitude values,
 * 					- 3D distance and amplitude values (i.e. per pixel),
 * 					- Auxiliary channel measurement results (VDD, IAPD, temperature, ...)
 * 					- Device and result status
 * 					- ...
 * 					.
 *
 * @addtogroup 	argusres
 * @{
 *****************************************************************************/

#include "argus_def.h"
#include "argus_px.h"
#include "argus_meas.h"

/*!***************************************************************************
 * @brief	The 1d measurement results data structure.
 * @details The 1d measurement results obtained by the pixel binning algorithm.
 *****************************************************************************/
typedef struct
{
	/*! Raw 1D range value in meter (Q9.22 format). The distance obtained by
	 *  the pixel binning algorithm from the current measurement frame. */
	q9_22_t Range;

	/*! The 1D amplitude in LSB (Q12.4 format). The (maximum) amplitude obtained
	 *  by the pixel binning algorithm from the current measurement frame.
	 *  Special value: 0 == No/Invalid Result. */
	uq12_4_t Amplitude;

} argus_results_bin_t;

/*!***************************************************************************
 * @brief	The auxiliary measurement results data structure.
 * @details The auxiliary measurement results obtained by the auxiliary task.
 * 			Special values, i.e. 0xFFFFU, indicate no readout value available.
 *****************************************************************************/
typedef struct
{
	/*! VDD ADC channel readout value.
	 *  Special Value if no value has been measured:
	 *  Invalid/NotAvailable = 0xFFFFU (UQ12_4_MAX) */
	uq12_4_t VDD;

	/*! Temperature sensor ADC channel readout value.
	 *  Special Value if no value has been measured:
	 *  Invalid/NotAvailable = 0x7FFFU (Q11_4_MAX) */
	q11_4_t TEMP;

	/*! Substrate Voltage ADC Channel readout value.
	 *  Special Value if no value has been measured:
	 *  Invalid/NotAvailable = 0xFFFFU (UQ12_4_MAX) */
	uq12_4_t VSUB;

	/*! VDD VCSEL ADC channel readout value.
	 *  Special Value if no value has been measured:
	 *  Invalid/NotAvailable = 0xFFFFU (UQ12_4_MAX) */
	uq12_4_t VDDL;

	/*! APD current ADC Channel readout value.
	 *  Special Value if no value has been measured:
	 *  Invalid/NotAvailable = 0xFFFFU (UQ12_4_MAX) */
	uq12_4_t IAPD;

} argus_results_aux_t;

/*!***************************************************************************
 * @brief	The measurement results data structure.
 * @details Measurement data from the device.
 * @code
 * 			// Pixel Field: Pixel[x][y]
 * 			//
 * 			// 0  -----------> x
 * 			// |  O O O O O O O O
 * 			// |   O O O O O O O O
 * 			// |  O O O O O O O O          O (ref. Px)
 * 			// y   O O O O O O O O
 * @endcode
 *****************************************************************************/
typedef struct
{
	/*! The \link #status_t status\endlink of the current measurement frame.
	 *   - 0 (i.e. #STATUS_OK) for a good measurement signal.
	 *   - > 0 for warnings and weak measurement signal.
	 *   - < 0 for errors and invalid measurement signal. */
	status_t Status;

	/*! Time in milliseconds (measured since the last MCU startup/reset)
	 *  when the measurement was triggered. */
	ltc_t TimeStamp;

	/*! The configuration for the current measurement frame. */
	argus_meas_frame_t Frame;

	/*! Raw unmapped ADC results from the device. */
	uint8_t Data[ARGUS_RAW_DATA_SIZE];

	/*! Raw Range Values from the device in meter.
	 *  It is the actual distance before software adjustments/calibrations. */
	argus_pixel_t PixelRef;

	/*! Raw Range Values from the device in meter.
	 *  It is the actual distance before software adjustments/calibrations. */
	argus_pixel_t Pixel[ARGUS_PIXELS_X][ARGUS_PIXELS_Y];

	/*! Pixel binned results. */
	argus_results_bin_t Bin;

	/*! The auxiliary ADC channel data. */
	argus_results_aux_t Auxiliary;

} argus_results_t;


/*! @} */
#endif /* ARGUS_RES_H */
