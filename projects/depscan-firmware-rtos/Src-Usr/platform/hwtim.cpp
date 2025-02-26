//! @file       name.x
//! @brief      File brief description
//!
//! @author     Seungwoo Kang (ki6080@gmail.com)
//! @copyright  Copyright (c) 2019. Seungwoo Kang. All rights reserved.
//!
//! @details
//!             File detailed description
//! @todo       Check for potential data corruption during overall timer usage
#include <FreeRTOS.h>

#include <main.h>
#include <task.h>
#include <uEmbedded-pp/timer_logic.hxx>
#include "../defs.h"

/////////////////////////////////////////////////////////////////////////////
// Imports
extern TIM_HandleTypeDef htim2;

/////////////////////////////////////////////////////////////////////////////
// Statics
// using hwtim_t = upp::linked_timer_logic<usec_t>;
using hwtim_t = upp::static_timer_logic<usec_t, uint8_t, NUM_MAX_HWTIMER_NODE>;
static hwtim_t s_tim;
static usec_t  s_total_us = std::numeric_limits<usec_t>::max() - (usec_t)1e9;
static TIM_HandleTypeDef& htim = htim2;
static TaskHandle_t       sTimerTask;
static StackType_t        sTimerStack[NUM_TIMER_TASK_STACK_WORDS];
static StaticTask_t       sTimerTaskStaticCb;

/////////////////////////////////////////////////////////////////////////////
// Decls
_Noreturn static void TimerUpdateTask( void* nouse__ );
extern "C" usec_t     API_GetTime_us();

/////////////////////////////////////////////////////////////////////////////
// Macro
#define GET_TICK() ( htim.Instance->CNT )
#define TICK_TIME ((int)1e9)
/////////////////////////////////////////////////////////////////////////////
// Utility class - timer lock
//

/////////////////////////////////////////////////////////////////////////////
// Defs
extern "C" void HW_TIMER_INIT()
{
    s_tim.tick_function( []() { return API_GetTime_us(); } );

    sTimerTask = xTaskCreateStatic(
      TimerUpdateTask,
      "TIMER",
      sizeof( sTimerStack ) / sizeof( *sTimerStack ),
      NULL,
      TaskPriorityRealtime,
      sTimerStack,
      &sTimerTaskStaticCb );

    // Configure timer
    __HAL_TIM_SET_PRESCALER( &htim, ( SystemCoreClock / 1000000 ) );
    __HAL_TIM_SET_AUTORELOAD( &htim, TICK_TIME - 1 );
    HAL_TIM_Base_Start_IT( &htim );
    TIM_CCxChannelCmd( htim.Instance, TIM_CHANNEL_1, TIM_CCx_ENABLE );
}

extern "C" void TIM2_IRQHandler( void )
{
    // If it's update interrupt, accumulates 1000 second to total time
    if ( __HAL_TIM_GET_FLAG( &htim, TIM_FLAG_UPDATE ) != RESET )
    {
        __HAL_TIM_CLEAR_FLAG( &htim, TIM_FLAG_UPDATE );
        s_total_us += TICK_TIME;
    }

    // If it's oc interrupt, process hwtimer event and switch to timer task
    if ( __HAL_TIM_GET_FLAG( &htim, TIM_FLAG_CC1 ) != RESET )
    {
        __HAL_TIM_CLEAR_FLAG( &htim, TIM_FLAG_CC1 );
        BaseType_t bHigherTaskPriorityWoken = pdFALSE;
        vTaskNotifyGiveFromISR( sTimerTask, &bHigherTaskPriorityWoken );
        portYIELD_FROM_ISR( bHigherTaskPriorityWoken );
    }
}

extern "C" usec_t API_GetTime_us()
{
    return s_total_us + GET_TICK();
}

extern "C" timer_handle_t
API_SetTimer( usec_t delay, void* obj, void ( *cb )( void* ) )
{
    uassert( s_tim.capacity() > 0 && cb );
    taskENTER_CRITICAL();
    auto r = s_tim.add( std::max( 11ull, delay ) - 10ull, obj, cb );
    // Wake update task
    xTaskNotifyGive( sTimerTask );
    taskEXIT_CRITICAL();
    return { r.id_, r.time_ };
}

extern "C" int print( char const* fmt, ... );

extern "C" void
API_SetTimerFromISR( usec_t delay, void* obj, void ( *cb )( void* ) )
{
    s_tim.add( std::max( 11ull, delay ) - 10ull, obj, cb );
    BaseType_t bHigherTaskPriorityWoken = pdFALSE;
    vTaskNotifyGiveFromISR( sTimerTask, &bHigherTaskPriorityWoken );
    portYIELD_FROM_ISR( bHigherTaskPriorityWoken );
}

extern "C" void API_AbortTimer( timer_handle_t h )
{
    taskENTER_CRITICAL();
    hwtim_t::handle_type t;
    t.id_   = h.data_[0];
    t.time_ = h.data_[1];
    s_tim.remove( t );
    taskEXIT_CRITICAL();
}

_Noreturn void TimerUpdateTask( void* nouse__ )
{
    for ( ;; )
    {
        ulTaskNotifyTake( pdTRUE, 100 /* For case if lost ... */ );
        __HAL_TIM_DISABLE_IT( &htim, TIM_FLAG_CC1 );

        auto next = s_tim.update_lock(
          []() { taskENTER_CRITICAL(); }, []() { taskEXIT_CRITICAL(); } );
        // auto next = s_tim.update();

        if ( next != (usec_t)-1 )
        {
            int delay = next - API_GetTime_us();
            int cnt   = htim.Instance->CNT;
            int arr   = htim.Instance->ARR;

            if ( cnt + delay > arr )
                htim.Instance->CCR1 = ( cnt + delay ) - arr;
            else
                htim.Instance->CCR1 = ( cnt + delay );

            __HAL_TIM_ENABLE_IT( &htim, TIM_FLAG_CC1 );
        }
    }
}

extern "C" bool API_CheckTimer( timer_handle_t h, usec_t* usLeft )
{
    hwtim_t::desc_type d;
    if ( s_tim.browse( { h.data_[0], h.data_[1] }, d ) == false )
    {
        return false;
    }

    if ( usLeft )
        *usLeft = d.trigger_at_ - API_GetTime_us();
    return true;
}

// This code is a dummy function to prevent link errors that occur when using
// the std :: function class.
namespace std {
void __throw_bad_function_call()
{
    uassert( false );
    for ( ;; )
    {
    }
}
} // namespace std
