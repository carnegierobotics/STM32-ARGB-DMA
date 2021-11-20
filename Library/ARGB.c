/**
 *******************************************
 * @file    ARGB.c
 * @author  Dmitriy Semenov / Crazy_Geeks
 * @version 1.3
 * @date	01-November-2021
 * @brief   Source file for ARGB (Adreassable RGB)
 *******************************************
 *
 * @note Repo: https://github.com/Crazy-Geeks/STM32-ARGB-DMA
 * @note RU article: https://crazygeeks.ru/stm32-argb-lib
 */

/* WS2811 Timings
 * Tolerance: +/- 150ns <-> +/- 0.15us
 * RES: >50us
 *
 * Slow mode:
 * Period: 2.5us <-> 400 KHz
 * T0H: 0.5us
 * T1H: 1.2us
 * T0L: 2.0us
 * T1L: 1.3us
 *
 * Fast mode:
 * Period: 1.25us <-> 800 KHz
 * T0H: 0.25us - 20%
 * T1H: 0.6us  - 48%
 * T0L: 1.0us
 * T1H: 0.65us
 *
 */

/* WS2811 Timings
 * Tolerance: +/- 150ns <-> +/- 0.15us
 * RES: >50us

 * Period: 1.25us <-> 800 KHz
 * T0H: 0.35us - 20%
 * T1H: 0.7us  - 48%
 * T0L: 0.8us
 * T1H: 0.6us
 *
 */

#include "ARGB.h"  // include header file
#include "stm32g4xx_hal_tim.h" // Set your file

/**
 * @addtogroup ARGB_Driver
 * @{
 */

/**
 * @addtogroup Private_entities
 * @brief Private methods and variables
 * @{
*/

/// Timer handler
#if TIM_NUM == 1
#define TIM_HANDLE  htim1
#elif TIM_NUM == 2
#define TIM_HANDLE  htim2
#elif TIM_NUM == 3
#define TIM_HANDLE  htim3
#elif TIM_NUM == 4
#define TIM_HANDLE  htim4
#elif TIM_NUM == 5
#define TIM_HANDLE  htim5
#elif TIM_NUM == 8
#define TIM_HANDLE  htim8
#else
#error Wrong timer! Fix it in ARGB.h string 41
#warning If you shure, set TIM_HANDLE and APB ring by yourself
#endif

/// Timer's RCC Bus
#if TIM_NUM == 1 || (TIM_NUM >= 8 && TIM_NUM <= 11)
#define APB1
#else
#define APB2
#endif

extern TIM_HandleTypeDef (TIM_HANDLE);  ///< Timer handler
extern DMA_HandleTypeDef (DMA_HANDLE);  ///< DMA handler

volatile uint8_t PWM_HI;    ///< PWM Code HI Log.1 period
volatile uint8_t PWM_LO;    ///< PWM Code LO Log.1 period

#ifdef SK6812
#define NUM_BYTES (4 * NUM_PIXELS) ///< Strip size in bytes
#define PWM_BUF_LEN (4 * 8 * 2)    ///< Pack len * 8 bit * 2 LEDs
#else
#define NUM_BYTES (3 * NUM_PIXELS) ///< Strip size in bytes
#define PWM_BUF_LEN (3 * 8 * 2)    ///< Pack len * 8 bit * 2 LEDs
#endif

/// Static LED buffer
volatile u8_t RGB_BUF[NUM_BYTES] = {0,};

/// Timer PWM value buffer
volatile u8_t PWM_BUF[PWM_BUF_LEN] = {0,};
/// PWM buffer iterator
volatile u16_t BUF_COUNTER = 0;

volatile u8_t ARGB_BR = 255;     ///< LED Global brightness
volatile ARGB_STATE ARGB_LOC_ST; ///< Buffer send status

static inline u8_t scale8(u8_t x, u8_t scale); // Gamma correction
static void HSV2RGB(u8_t hue, u8_t sat, u8_t val, u8_t *_r, u8_t *_g, u8_t *_b);
// Callbacks
static void ARGB_TIM_DMADelayPulseCplt(DMA_HandleTypeDef *hdma);
static void ARGB_TIM_DMADelayPulseHalfCplt(DMA_HandleTypeDef *hdma);
/// @} //Private



/**
 * @brief Init timer & prescalers
 * @param none
 * @return #ARGB_STATE enum
 */
ARGB_STATE ARGB_Init(void) {
    /* Auto-calculation! */
    uint32_t APBfq;                                 // clock frequencies
    RCC_ClkInitTypeDef clk_obj;                 // clocks structure
    uint32_t latency = FLASH_ACR_LATENCY_4WS;     // dummy hardcode
    HAL_RCC_GetClockConfig(&clk_obj, &latency);  // get clocks table

#ifdef APB2
    APBfq = HAL_RCC_GetPCLK2Freq(); // get fq
    if (clk_obj.APB2CLKDivider != RCC_HCLK_DIV1)  // if not divided to 1
        APBfq *= 2;                                  // multiple to 2
#endif
#ifdef APB1
    APBfq = HAL_RCC_GetPCLK1Freq(); // get fq
    if (clk_obj.APB1CLKDivider != RCC_HCLK_DIV1)  // if not divided to 1
        APBfq *= 2;                                  // multiple to 2
#endif

#ifdef WS2811S
    APBfq /= (uint32_t) (400 * 1000);  // 400 KHz - 2.5us
#else
    APBfq /= (uint32_t) (800 * 1000);  // 800 KHz - 1.25us
#endif

    TIM_HANDLE.Instance->PSC = 0;                        // dummy hardcode now
    TIM_HANDLE.Instance->ARR = (uint16_t) (APBfq - 1);    // set timer prescaler
    TIM_HANDLE.Instance->EGR = 1;                        // update timer registers

#ifdef WS2811F
    PWM_HI = (u8_t) (APBfq * 0.48) - 1;     // Log.1 - 60% - 1.3us
    PWM_LO = (u8_t) (APBfq * 0.20) - 1;     // Log.0 - 20% - 0.5us
#else
    PWM_HI = (u8_t) (APBfq * 0.85) - 1;	 // Log.1 - 85%
    PWM_LO = (u8_t) (APBfq * 0.25) - 1;   // Log.0 - 25%
#endif


//#if INV_SIGNAL
//    TIM_POINTER->CCER |= TIM_CCER_CC2P; // set inv ch bit
//#else
//    TIM_POINTER->CCER &= ~TIM_CCER_CC2P;
//#endif
    ARGB_LOC_ST = ARGB_READY;
    TIM_CCxChannelCmd(TIM_HANDLE.Instance, TIM_CH, TIM_CCx_ENABLE);
    HAL_Delay(1);
    return ARGB_OK;
}

/**
 * @brief Fill ALL LEDs with (0,0,0)
 * @param none
 * @note Update strip after that
 * @return #ARGB_STATE enum
 */
ARGB_STATE ARGB_Clear(void) {
    ARGB_FillRGB(0, 0, 0);
#ifdef SK6812
    ARGB_FillWhite(0);
#endif
    return ARGB_OK;
}

/**
 * @brief Set GLOBAL LED brightness
 * @param[in] br Brightness [0..255]
 * @return #ARGB_STATE enum
 */
ARGB_STATE ARGB_SetBrightness(u8_t br) {
    ARGB_BR = br;
    return ARGB_OK;
}

/**
 * @brief Set LED with RGB color by index
 * @param[in] i LED position
 * @param[in] r Red component   [0..255]
 * @param[in] g Green component [0..255]
 * @param[in] b Blue component  [0..255]
 * @return #ARGB_STATE enum
 */
ARGB_STATE ARGB_SetRGB(u16_t i, u8_t r, u8_t g, u8_t b) {
    // overflow protection
    if (i >= NUM_PIXELS) {
        u16_t _i = i / NUM_PIXELS;
        i -= _i * NUM_PIXELS;
    }

    // set brightness
    r /= 256 / ((u16_t) ARGB_BR + 1);
    g /= 256 / ((u16_t) ARGB_BR + 1);
    b /= 256 / ((u16_t) ARGB_BR + 1);

#if USE_GAMMA_CORRECTION
    g = scale8(g, 0xB0);
    b = scale8(b, 0xF0);
#endif

    // Subpixel chain order
#if defined(SK6812) || defined(WS2811F) || defined(WS2811S)
    const u8_t subp1 = r;
    const u8_t subp2 = g;
    const u8_t subp3 = b;
#else
    const u8_t subp1 = g;
    const u8_t subp2 = r;
    const u8_t subp3 = b;
#endif
    // RGB or RGBW
#ifdef SK6812
    RGB_BUF[4 * i] = subp1;     // subpixel 1
    RGB_BUF[4 * i + 1] = subp2; // subpixel 2
    RGB_BUF[4 * i + 2] = subp3; // subpixel 3
#else
    RGB_BUF[3 * i] = subp1;     // subpixel 1
    RGB_BUF[3 * i + 1] = subp2; // subpixel 2
    RGB_BUF[3 * i + 2] = subp3; // subpixel 3
#endif
    return ARGB_OK;
}

/**
 * @brief Set LED with HSV color by index
 * @param[in] i LED position
 * @param[in] hue HUE (color) [0..255]
 * @param[in] sat Saturation  [0..255]
 * @param[in] val Value (brightness) [0..255]
 * @return #ARGB_STATE enum
 */
ARGB_STATE ARGB_SetHSV(u16_t i, u8_t hue, u8_t sat, u8_t val) {
    uint8_t _r, _g, _b;                    // init buffer color
    HSV2RGB(hue, sat, val, &_r, &_g, &_b); // get RGB color
    return ARGB_SetRGB(i, _r, _g, _b);     // set color
}

/**
 * @brief Set White component in strip by index
 * @param[in] i LED position
 * @param[in] w White component [0..255]
 * @return #ARGB_STATE enum
 */
ARGB_STATE ARGB_SetWhite(u16_t i, u8_t w) {
#ifdef RGB
    return ARGB_PARAM_ERR;
#endif
    w /= 256 / ((u16_t) ARGB_BR + 1); // set brightness
    RGB_BUF[4 * i + 3] = w;                // set white part
    return ARGB_OK;
}

/**
 * @brief Fill ALL LEDs with RGB color
 * @param[in] r Red component   [0..255]
 * @param[in] g Green component [0..255]
 * @param[in] b Blue component  [0..255]
 * @return #ARGB_STATE enum
 */
ARGB_STATE ARGB_FillRGB(u8_t r, u8_t g, u8_t b) {
    for (volatile u16_t i = 0; i < NUM_PIXELS; i++)
        ARGB_SetRGB(i, r, g, b);
    return ARGB_OK;
}

/**
 * @brief Fill ALL LEDs with HSV color
 * @param[in] hue HUE (color) [0..255]
 * @param[in] sat Saturation  [0..255]
 * @param[in] val Value (brightness) [0..255]
 * @return #ARGB_STATE enum
 */
ARGB_STATE ARGB_FillHSV(u8_t hue, u8_t sat, u8_t val) {
    uint8_t _r, _g, _b;                    // init buffer color
    HSV2RGB(hue, sat, val, &_r, &_g, &_b); // get color once (!)
    return ARGB_FillRGB(_r, _g, _b);       // set color
}

/**
 * @brief Set ALL White components in strip
 * @param[in] w White component [0..255]
 * @return #ARGB_STATE enum
 */
ARGB_STATE ARGB_FillWhite(u8_t w) {
    for (volatile u16_t i = 0; i < NUM_PIXELS; i++)
        ARGB_SetWhite(i, w);
    return ARGB_OK;
}

/**
 * @brief Get current DMA status
 * @param none
 * @return #ARGB_STATE enum
 */
ARGB_STATE ARGB_Ready(void) {
    return ARGB_LOC_ST;
}

/**
 * @brief Update strip
 * @param none
 * @return #ARGB_STATE enum
 */
ARGB_STATE ARGB_Show(void) {
    ARGB_LOC_ST = ARGB_BUSY;
    if (BUF_COUNTER != 0 || DMA_HANDLE.State != HAL_DMA_STATE_READY) {
        return ARGB_BUSY;
    } else {
        for (volatile u8_t i = 0; i < 8; i++) {
            // set first transfer from first values
            PWM_BUF[i] = (((RGB_BUF[0] << i) & 0x80) > 0) ? PWM_HI : PWM_LO;
            PWM_BUF[i + 8] = (((RGB_BUF[1] << i) & 0x80) > 0) ? PWM_HI : PWM_LO;
            PWM_BUF[i + 16] = (((RGB_BUF[2] << i) & 0x80) > 0) ? PWM_HI : PWM_LO;
            PWM_BUF[i + 24] = (((RGB_BUF[3] << i) & 0x80) > 0) ? PWM_HI : PWM_LO;
            PWM_BUF[i + 32] = (((RGB_BUF[4] << i) & 0x80) > 0) ? PWM_HI : PWM_LO;
            PWM_BUF[i + 40] = (((RGB_BUF[5] << i) & 0x80) > 0) ? PWM_HI : PWM_LO;
#ifdef SK6812
            PWM_BUF[i + 48] = (((RGB_BUF[6] << i) & 0x80) > 0) ? PWM_HI : PWM_LO;
            PWM_BUF[i + 56] = (((RGB_BUF[7] << i) & 0x80) > 0) ? PWM_HI : PWM_LO;
#endif
        }

        HAL_StatusTypeDef DMA_Send_Stat = HAL_ERROR;
        while (DMA_Send_Stat != HAL_OK) {

            if (TIM_CHANNEL_STATE_GET(&TIM_HANDLE, TIM_CH) == HAL_TIM_CHANNEL_STATE_BUSY) {
                DMA_Send_Stat = HAL_BUSY;
                continue;
            } else if (TIM_CHANNEL_STATE_GET(&TIM_HANDLE, TIM_CH) == HAL_TIM_CHANNEL_STATE_READY) {
                TIM_CHANNEL_STATE_SET(&TIM_HANDLE, TIM_CH, HAL_TIM_CHANNEL_STATE_BUSY);
            } else {
                DMA_Send_Stat = HAL_ERROR;
                continue;
            }

#if TIM_CH == TIM_CHANNEL_1
#define ARGB_TIM_DMA_ID TIM_DMA_ID_CC1
#define ARGB_TIM_DMA_CC TIM_DMA_CC1
#define ARGB_TIM_CCR CCR1
#elif TIM_CH == TIM_CHANNEL_2
#define ARGB_TIM_DMA_ID TIM_DMA_ID_CC2
#define ARGB_TIM_DMA_CC TIM_DMA_CC2
#define ARGB_TIM_CCR CCR2
#elif TIM_CH == TIM_CHANNEL_3
#define ARGB_TIM_DMA_ID TIM_DMA_ID_CC3
#define ARGB_TIM_DMA_CC TIM_DMA_CC3
#define ARGB_TIM_CCR CCR3
#elif TIM_CH == TIM_CHANNEL_4
#define ARGB_TIM_DMA_ID TIM_DMA_ID_CC4
#define ARGB_TIM_DMA_CC TIM_DMA_CC4
#define ARGB_TIM_CCR CCR4
#endif
            TIM_HANDLE.hdma[ARGB_TIM_DMA_ID]->XferCpltCallback = ARGB_TIM_DMADelayPulseCplt;
            TIM_HANDLE.hdma[ARGB_TIM_DMA_ID]->XferHalfCpltCallback = ARGB_TIM_DMADelayPulseHalfCplt;
            TIM_HANDLE.hdma[ARGB_TIM_DMA_ID]->XferErrorCallback = TIM_DMAError;
            if (HAL_DMA_Start_IT(TIM_HANDLE.hdma[ARGB_TIM_DMA_ID], (u32_t) PWM_BUF,
                                 (u32_t) &TIM_HANDLE.Instance->ARGB_TIM_CCR,
                                 (u16_t) PWM_BUF_LEN) != HAL_OK) {
                DMA_Send_Stat = HAL_ERROR;
                continue;
            }
            __HAL_TIM_ENABLE_DMA(&TIM_HANDLE, ARGB_TIM_DMA_CC);
            if (IS_TIM_BREAK_INSTANCE(TIM_HANDLE.Instance) != RESET)
                __HAL_TIM_MOE_ENABLE(&TIM_HANDLE);
            if (IS_TIM_SLAVE_INSTANCE(TIM_HANDLE.Instance)) {
                u32_t tmpsmcr = TIM_HANDLE.Instance->SMCR & TIM_SMCR_SMS;
                if (!IS_TIM_SLAVEMODE_TRIGGER_ENABLED(tmpsmcr))
                    __HAL_TIM_ENABLE(&TIM_HANDLE);
            } else
                __HAL_TIM_ENABLE(&TIM_HANDLE);
            DMA_Send_Stat = HAL_OK;
        }
        BUF_COUNTER = 2;

        return ARGB_OK;
    }
}

/* DMA CALLBACKS */

/**
 * @addtogroup Private_entities
 * @{ */

/**
 * @brief DMA Finished half buffer transmission callback
 * @param[in] htim Pointer to timer instance
*/
void HAL_TIM_PWM_PulseFinishedHalfCpltCallback(TIM_HandleTypeDef *htim) {
    // if wrong DMA channel
    if (htim->Instance != TIM_HANDLE.Instance) return;

    // if data transfer
    if (BUF_COUNTER < NUM_PIXELS) {
        // fill first part of buffer
        for (volatile u8_t i = 0; i < 8; i++) {
#ifdef SK6812
            PWM_BUF[i] = (((RGB_BUF[4 * BUF_COUNTER] << i) & 0x80) > 0) ? PWM_HI : PWM_LO;
            PWM_BUF[i + 8] = (((RGB_BUF[4 * BUF_COUNTER + 1] << i) & 0x80) > 0) ? PWM_HI : PWM_LO;
            PWM_BUF[i + 16] = (((RGB_BUF[4 * BUF_COUNTER + 2] << i) & 0x80) > 0) ? PWM_HI : PWM_LO;
            PWM_BUF[i + 24] = (((RGB_BUF[4 * BUF_COUNTER + 3] << i) & 0x80) > 0)? PWM_HI : PWM_LO;
#else
            PWM_BUF[i] = (((RGB_BUF[3 * BUF_COUNTER] << i) & 0x80) > 0) ? PWM_HI : PWM_LO;
            PWM_BUF[i + 8] = (((RGB_BUF[3 * BUF_COUNTER + 1] << i) & 0x80) > 0) ? PWM_HI : PWM_LO;
            PWM_BUF[i + 16] = (((RGB_BUF[3 * BUF_COUNTER + 2] << i) & 0x80) > 0) ? PWM_HI : PWM_LO;
#endif
        }
        BUF_COUNTER++;
    } else if (BUF_COUNTER < NUM_PIXELS + 2) { // if RET transfer
        memset((u8_t *) &PWM_BUF[0], 0, PWM_BUF_LEN / 2); // first part
        BUF_COUNTER++;
    }
}

/**
 * @brief DMA Finished full buffer transmission callback
 * @param[in] htim Pointer to timer instance
 */
void HAL_TIM_PWM_PulseFinishedCallback(TIM_HandleTypeDef *htim) {
    // if wrong DMA channel
    if (htim->Instance != TIM_HANDLE.Instance) return;

    // if data transfer
    if (BUF_COUNTER < NUM_PIXELS) {
        // fill second part of buffer
        for (volatile u8_t i = 0; i < 8; i++) {
#ifdef SK6812
            PWM_BUF[i + 32] = (((RGB_BUF[4 * BUF_COUNTER] << i) & 0x80) > 0) ? PWM_HI : PWM_LO;
            PWM_BUF[i + 40] = (((RGB_BUF[4 * BUF_COUNTER + 1] << i) & 0x80) > 0) ? PWM_HI : PWM_LO;
            PWM_BUF[i + 48] = (((RGB_BUF[4 * BUF_COUNTER + 2] << i) & 0x80) > 0) ? PWM_HI : PWM_LO;
            PWM_BUF[i + 56] = (((RGB_BUF[4 * BUF_COUNTER + 3] << i) & 0x80) > 0) ? PWM_HI : PWM_LO;
#else
            PWM_BUF[i + 24] = (((RGB_BUF[3 * BUF_COUNTER] << i) & 0x80) > 0) ? PWM_HI : PWM_LO;
            PWM_BUF[i + 32] = (((RGB_BUF[3 * BUF_COUNTER + 1] << i) & 0x80) > 0) ? PWM_HI : PWM_LO;
            PWM_BUF[i + 40] = (((RGB_BUF[3 * BUF_COUNTER + 2] << i) & 0x80) > 0) ? PWM_HI : PWM_LO;
#endif
        }
        BUF_COUNTER++;
    } else if (BUF_COUNTER < NUM_PIXELS + 2) { // if RET transfer
        memset((u8_t *) &PWM_BUF[PWM_BUF_LEN / 2], 0, PWM_BUF_LEN / 2); // second part
        BUF_COUNTER++;
    } else { // if END of transfer
        BUF_COUNTER = 0;

        // STOP DMA:
#if TIM_CH == TIM_CHANNEL_1
        __HAL_TIM_DISABLE_DMA(htim, TIM_DMA_CC1);
        (void) HAL_DMA_Abort_IT(htim->hdma[TIM_DMA_ID_CC1]);
#endif
#if TIM_CH == TIM_CHANNEL_2
        __HAL_TIM_DISABLE_DMA(htim, TIM_DMA_CC2);
        (void) HAL_DMA_Abort_IT(htim->hdma[TIM_DMA_ID_CC2]);
#endif
#if TIM_CH == TIM_CHANNEL_3
        __HAL_TIM_DISABLE_DMA(htim, TIM_DMA_CC3);
        (void) HAL_DMA_Abort_IT(htim->hdma[TIM_DMA_ID_CC3]);
#endif
#if TIM_CH == TIM_CHANNEL_4
        __HAL_TIM_DISABLE_DMA(htim, TIM_DMA_CC4);
        (void) HAL_DMA_Abort_IT(htim->hdma[TIM_DMA_ID_CC4]);
#endif
        if (IS_TIM_BREAK_INSTANCE(htim->Instance) != RESET) {
            /* Disable the Main Output */
            __HAL_TIM_MOE_DISABLE(htim);
        }

        /* Disable the Peripheral */
        __HAL_TIM_DISABLE(htim);

        /* Set the TIM channel state */
        TIM_CHANNEL_STATE_SET(htim, TIM_CH, HAL_TIM_CHANNEL_STATE_READY);
        ARGB_LOC_ST = ARGB_READY;
    }
}

/**
 * @brief Private method for gamma correction
 * @param[in] x Param to scale
 * @param[in] scale Scale coefficient
 * @return Scaled value
 */
static inline u8_t scale8(u8_t x, u8_t scale) {
    return ((uint16_t) x * scale) >> 8;
}

/**
 * @brief Convert color in HSV to RGB
 * @param[in] hue HUE (color) [0..255]
 * @param[in] sat Saturation  [0..255]
 * @param[in] val Value (brightness) [0..255]
 * @param[out] _r Pointer to RED component value
 * @param[out] _g Pointer to GREEN component value
 * @param[out] _b Pointer to BLUE component value
 */
static void HSV2RGB(u8_t hue, u8_t sat, u8_t val, u8_t *_r, u8_t *_g, u8_t *_b) {
    /* Source:
     * https://stackoverflow.com/questions/3018313/algorithm-to-convert-rgb-to-hsv-and-hsv-to-rgb-in-range-0-255-for-both
     */

    if (sat == 0) { // if white color
        _r[0] = _g[0] = _b[0] = val;
        return;
    }

#if USE_HSV_FLOAT  /* DOESN'T WORK NOW */
    // Float is smoother but check for FPU (Floating point unit) in your MCU
    // Otherwise it will take longer time in the code
    // FPU is in: F3/L3 and greater
    float _hue = (float) hue * (360.0f / 255.0f);
    float _sat = (float) sat * (1.0f / 255.0f);
    float _val = (float) val * (1.0f / 255.0f);

    //float s = sat / 100;
    //float v = val / 100;
    float C = _sat * _val;
    float X = C * (1 - abs(fmod(_hue / 60.0, 2) - 1));
    float m = _val - C;
    float r, g, b;
    if (_hue >= 0 && _hue < 60) {
        r = C, g = X, b = 0;
    } else if (_hue >= 60 && _hue < 120) {
        r = X, g = C, b = 0;
    } else if (_hue >= 120 && _hue < 180) {
        r = 0, g = C, b = X;
    } else if (_hue >= 180 && _hue < 240) {
        r = 0, g = X, b = C;
    } else if (_hue >= 240 && _hue < 300) {
        r = X, g = 0, b = C;
    } else {
        r = C, g = 0, b = X;
    }
    _r[0] = (r + m) * 255;
    _g[0] = (g + m) * 255;
    _b[0] = (b + m) * 255;
#endif

//#if !USE_HSV_FLOAT
    uint8_t reg = hue / 43;
    uint8_t rem = (hue - (reg * 43)) * 6;

    uint8_t p = (val * (255 - sat)) >> 8;
    uint8_t q = (val * (255 - ((sat * rem) >> 8))) >> 8;
    uint8_t t = (val * (255 - ((sat * (255 - rem)) >> 8))) >> 8;

    switch (reg) {
        case 0:
            _r[0] = val;
            _g[0] = t;
            _b[0] = p;
            break;
        case 1:
            _r[0] = q;
            _g[0] = val;
            _b[0] = p;
            break;
        case 2:
            _r[0] = p;
            _g[0] = val;
            _b[0] = t;
            break;
        case 3:
            _r[0] = p;
            _g[0] = q;
            _b[0] = val;
            break;
        case 4:
            _r[0] = t;
            _g[0] = p;
            _b[0] = val;
            break;
        default:
            _r[0] = val;
            _g[0] = p;
            _b[0] = q;
            break;
    }
//#endif

}

/**
  * @brief  TIM DMA Delay Pulse complete callback.
  * @param  hdma pointer to DMA handle.
  * @retval None
  */
static void ARGB_TIM_DMADelayPulseCplt(DMA_HandleTypeDef *hdma) {
    TIM_HandleTypeDef *htim = (TIM_HandleTypeDef *) ((DMA_HandleTypeDef *) hdma)->Parent;

    if (hdma == htim->hdma[TIM_DMA_ID_CC1]) {
        htim->Channel = HAL_TIM_ACTIVE_CHANNEL_1;

        if (hdma->Init.Mode == DMA_NORMAL) {
            TIM_CHANNEL_STATE_SET(htim, TIM_CHANNEL_1, HAL_TIM_CHANNEL_STATE_READY);
        }
    } else if (hdma == htim->hdma[TIM_DMA_ID_CC2]) {
        htim->Channel = HAL_TIM_ACTIVE_CHANNEL_2;

        if (hdma->Init.Mode == DMA_NORMAL) {
            TIM_CHANNEL_STATE_SET(htim, TIM_CHANNEL_2, HAL_TIM_CHANNEL_STATE_READY);
        }
    } else if (hdma == htim->hdma[TIM_DMA_ID_CC3]) {
        htim->Channel = HAL_TIM_ACTIVE_CHANNEL_3;

        if (hdma->Init.Mode == DMA_NORMAL) {
            TIM_CHANNEL_STATE_SET(htim, TIM_CHANNEL_3, HAL_TIM_CHANNEL_STATE_READY);
        }
    } else if (hdma == htim->hdma[TIM_DMA_ID_CC4]) {
        htim->Channel = HAL_TIM_ACTIVE_CHANNEL_4;

        if (hdma->Init.Mode == DMA_NORMAL) {
            TIM_CHANNEL_STATE_SET(htim, TIM_CHANNEL_4, HAL_TIM_CHANNEL_STATE_READY);
        }
    } else {
        /* nothing to do */
    }

#if (USE_HAL_TIM_REGISTER_CALLBACKS == 1)
    htim->PWM_PulseFinishedCallback(htim);
#else
    HAL_TIM_PWM_PulseFinishedCallback(htim);
#endif /* USE_HAL_TIM_REGISTER_CALLBACKS */

    htim->Channel = HAL_TIM_ACTIVE_CHANNEL_CLEARED;
}


/**
  * @brief  TIM DMA Delay Pulse half complete callback.
  * @param  hdma pointer to DMA handle.
  * @retval None
  */
static void ARGB_TIM_DMADelayPulseHalfCplt(DMA_HandleTypeDef *hdma) {
    TIM_HandleTypeDef *htim = (TIM_HandleTypeDef *) ((DMA_HandleTypeDef *) hdma)->Parent;

    if (hdma == htim->hdma[TIM_DMA_ID_CC1]) {
        htim->Channel = HAL_TIM_ACTIVE_CHANNEL_1;
    } else if (hdma == htim->hdma[TIM_DMA_ID_CC2]) {
        htim->Channel = HAL_TIM_ACTIVE_CHANNEL_2;
    } else if (hdma == htim->hdma[TIM_DMA_ID_CC3]) {
        htim->Channel = HAL_TIM_ACTIVE_CHANNEL_3;
    } else if (hdma == htim->hdma[TIM_DMA_ID_CC4]) {
        htim->Channel = HAL_TIM_ACTIVE_CHANNEL_4;
    } else {
        /* nothing to do */
    }

#if (USE_HAL_TIM_REGISTER_CALLBACKS == 1)
    htim->PWM_PulseFinishedHalfCpltCallback(htim);
#else
    HAL_TIM_PWM_PulseFinishedHalfCpltCallback(htim);
#endif /* USE_HAL_TIM_REGISTER_CALLBACKS */

    htim->Channel = HAL_TIM_ACTIVE_CHANNEL_CLEARED;
}

/** @} */ // Private

/** @} */ // Driver

#if !(defined(SK6812) || defined(WS2811F) || defined(WS2811S) || defined(WS2812))
#error INCORRECT LED TYPE
#warning Set it from list in ARGB.h string 28
#endif

#if !(TIM_CH == TIM_CHANNEL_1 || TIM_CH == TIM_CHANNEL_2 || TIM_CH == TIM_CHANNEL_3 || TIM_CH == TIM_CHANNEL_4)
#error Wrong channel! Fix it in ARGB.h string 42
#warning If you shure, search and set TIM_CHANNEL by yourself
#endif

#if USE_HSV_FLOAT == 1
#error SET USE_HSV_FLOAT to 0! ARGB.h string 39
#endif
