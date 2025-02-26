/// @brief
///
/// @file scanner_protocol.h
/// @author Seungwoo Kang (ki6080@gmail.com)
/// @date 2019-12-26
/// @copyright Copyright (c) 2019. Seungwoo Kang. All rights reserved.
///
/// @details
#pragma once
#include <stdint.h>

#pragma pack(push, 2)

typedef uint16_t SCANNER_COMMAND_TYPE;

struct FDeviceStat {
    uint16_t StepPerPxlX;
    uint16_t StepPerPxlY;
    uint32_t SizeX;
    uint32_t SizeY;
    uint32_t OfstX;
    uint32_t OfstY;
    uint32_t DelayPerCapture;

    float DegreePerStepX;
    float DegreePerStepY;

    int32_t CurMotorStepX;
    int32_t CurMotorStepY;

    int64_t TimeAfterLaunch_us;

    bool    bIsPrecisionMode;
    bool    bIsIdle;
    bool    bIsSensorInitialized;
    bool    bIsPaused;
};

enum ECaptureModeBit {
    ECMB_DISTANCE_AUTO = 1,
    ECMB_DISTANCE_FAR  = 2,
    ECMB_LOW_PRECISION = 4,
};

typedef int32_t  q9_22_t;
typedef uint16_t uq12_4_t;
#define Q9_22_ONE_INT  (1 << 22)
#define UQ12_4_ONE_INT (1u << 4)

struct FPxlData {
    q9_22_t  Distance;
    uq12_4_t AMP;
};

struct FLineData {
    uint32_t LineIdx;
    uint32_t OfstX;
    uint32_t NumPxls;
};

#ifdef __cplusplus
namespace ECommand {
#endif
enum ECommand // REQ = HOST -> DEVICE, RSP = DEVICE -> HOST
{
    RSP_STAT_REPORT,

    RSP_PING,

    RSP_LINE_DATA,
    RSP_PIXEL_DATA,
    RSP_DONE
};
#ifdef __cplusplus
}
#endif
#pragma pack(pop)
