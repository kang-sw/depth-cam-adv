#include "scanner_protocol_handler.hpp"
//! @brief
//! @file
//! @author Seungwoo Kang (ki6080@gmail.com)
//! @copyright Copyright (c) 2019. Seungwoo Kang. All rights reserved.
//!
//! @details
#include <assert.h>
#include <future>
#include <stdarg.h>
#include <thread>
#include "../common/scanner_protocol.h"
#include "scanner_protocol_handler.hpp"

using namespace std;
using namespace std::chrono;

template <typename tptr_, typename sptr_>
tptr_*& ptr_cast( sptr_& src )
{
    return reinterpret_cast<tptr_*&>( src );
}

static void StoreLineData(
  vector<FPxlData>& arr,
  int               xl,
  int               yl,
  FLineDesc const&  desc,
  FPxlData const*   data );
static void GetImageInfo( FScanImageDesc* out, FDeviceStat const& stat );

FScannerProtocolHandler::~FScannerProtocolHandler()
{
    Shutdown();
}

FScannerProtocolHandler::ActivateResult FScannerProtocolHandler::Activate(
  PortOpenFunctionType                     ComOpener,
  FCommunicationProcedureInitStruct const& params,
  bool                                     bAsync ) noexcept
{
    if ( IsActive() )
        return ACTIVATE_ALREADY_RUNNING;

    // Clear shutdown flag if set.
    bShutdown = false;

    // Create new thread to run procedure
    mBackgroundProcess = async(
      launch::async,
      &FScannerProtocolHandler::procedureThread,
      this,
      move( ComOpener ),
      params );

    // Wait until connection done.
    print(
      "Requesting Activation. Is Active? %d, Is Connected? %d ... \n",
      IsActive(),
      IsConnected() );
    bool const bShouldWaitOpenningResult
      = bAsync == false && params.ConnectionRetryCount != -1;
    while ( IsActive()
            && ( bShouldWaitOpenningResult && IsConnected() == false ) )
        this_thread::sleep_for( 1ms );

    // If procedure stopped the execution, it means all retries to open COM port
    // was exhausted.
    if ( IsActive() == false )
        return ACTIVATE_INVALID_COM;

    // Request report for initial status load
    if ( bShouldWaitOpenningResult )
        requestReport( true, 500 );

    print(
      "Activation finished. Is Active? %d, Is Connected? %d ... \n",
      IsActive(),
      IsConnected() );
    assert( !bShouldWaitOpenningResult || IsConnected() );
    return ACTIVATE_OK;
}

void FScannerProtocolHandler::Shutdown() noexcept
{
    ClearConnection();

    if ( IsActive() == false )
    {
        assert( IsConnected() == false );
        return;
    }

    //! Set shutdown flag and wait until process joins.
    print( "Shutting down the process ... \n" );
    mPrevCaptureParam.reset();
    bShutdown = true;
    mBackgroundProcess.wait();
}

void FScannerProtocolHandler::procedureThread(
  PortOpenFunctionType              ComOpener,
  FCommunicationProcedureInitStruct params ) noexcept
{
    auto const ActualTimeout = params.TimeoutMs / 2;

    while ( bShutdown == false )
    {
        //! Try open COM port with given parameter
        for ( size_t Retry = params.ConnectionRetryCount;; Retry-- )
        {
            if ( Retry == 0 || bShutdown )
                goto RETRY_EXHAUSTED;

            auto ptr = ComOpener( *this );
            if ( ptr == nullptr )
            {
                print( "Connection failed. Retrying ... %lu\n", Retry );
                this_thread::sleep_for(
                  chrono::milliseconds( params.ConnectionRetryIntervalMs ) );
                continue;
            }

            // Connection successful.
            InitializeStream( move( ptr ), params.ReceiveBufferSize );
            print( "Connection successful\n" );
            bIsConnected = true;
            mPrevCaptureParam.reset();
            break;
        }

        //! Ping logic
        // Once timeout occurs, this wait ping flag set to up and request for
        // ping.
        bool bWaitPing = false;

        //! Process packets during connection is valid.
        for ( ; bShutdown == false; )
        {
            switch ( ProcessSinglePacket( ActualTimeout ) )
            {
            case EPacketProcessResult::PACKET_OK:
                bWaitPing = false;
                continue;
            case EPacketProcessResult::PACKET_ERROR_TIMEOUT:
            {
                // If timeout occurs when already waiting for ping result, this
                // timeout will be treated as error.
                if ( bWaitPing )
                {
                    print( "Timeout occrued. disconnecting ... \n" );
                    break;
                }
                bWaitPing = true;
                SendString( "ping" );
                continue;
            }
            default:
                print( "error: Unhandled data corruption! \n" );
                continue;
            }
            break;
        }

        //! On lost connection.
        bIsConnected = false;
    }

RETRY_EXHAUSTED:;
    bIsConnected = false;
}

static inline bool cmpflt( float a, float b, float tolerance = 1e-7f )
{
    return abs( a - b ) < tolerance;
}

void FScannerProtocolHandler::configureCapture(
  CaptureParam const& arg,
  bool const          bForce )
{
    print( "Configuring capture ... Forced: %d\n", bForce );
    //! Compares with previous value, then apply only dirty ones
    auto       Stat        = mStat.load();
    bool const bForceApply = bForce || mPrevCaptureParam.has_value() == false
                             || Stat.bIsSensorInitialized == false;
    bool        bShouldReinit = bForceApply;
    float const StepX         = Stat.DegreePerStepX;
    float const StepY         = Stat.DegreePerStepY;
    char        buf[1024];

    // Set desired offset
    if ( arg.DesiredOffset.has_value() )
    {
        auto [xv, yv] = arg.DesiredOffset.value();
        auto xd       = static_cast<int>( xv / StepX );
        auto yd       = static_cast<int>( yv / StepY );

        sprintf( buf, "capture config offset %d %d", xd, yd );
        SendString( buf );
    }

    if ( arg.bPrescisionMode )
    {
        sprintf(
          buf, "capture config precision %d", arg.bPrescisionMode.value() );
        SendString( buf );
    }

    // Set frame time
    if ( auto PrevCaptureDelay = mPrevCaptureParam.has_value()
                                   ? mPrevCaptureParam->CaptureDelayUs
                                   : -1;
         arg.CaptureDelayUs > 0 && arg.CaptureDelayUs != PrevCaptureDelay )
    {
        sprintf( buf, "capture config delay %d", arg.CaptureDelayUs );
        SendString( buf );
    }

    //! Calculate device dimensional image size / steps per pixels
    // -> Get Angle per pixel = AngleFOV / Resolution
    // -> Get steps per pixel = AnglesPerPixel / AnglesPerStep
    // -> Use image size itself.
    auto CalcDeviceDimension =
      []( int Resolution, float FOV, float AnglePerStep ) -> auto
    {
        float AngPerPxl  = FOV / Resolution;
        int   StepPerPxl = max(
          1, static_cast<int>( AngPerPxl / AnglePerStep + 0.5f ) ); // Rounds
        float AnglePerPxl   = AnglePerStep * StepPerPxl;
        int   NewResolution = max( 1, static_cast<int>( FOV / AnglePerPxl ) );

        return make_pair( NewResolution, StepPerPxl );
    };

#define has_or( a, b, opt )                                                    \
    ( ( a ).opt.has_value()                                                    \
        ? ( a ).opt                                                            \
        : ( b ).has_value() ? ( b )->opt : decltype( ( a ).opt ) {} )

    // Fill with current value when there's any unspecified options.
    auto Angle = has_or( arg, mPrevCaptureParam, DesiredAngle );
    auto Res   = has_or( arg, mPrevCaptureParam, DesiredResolution );
    if ( Angle && Res )
    {
        int ResolutionX = -1, SPPX = -1;
        int ResolutionY = -1, SPPY = -1;
        if ( Res->x > 0 && Angle->x > 0 )
        {
            auto [res, spxl] = CalcDeviceDimension( Res->x, Angle->x, StepX );
            ResolutionX      = res;
            SPPX             = spxl;
        }
        if ( Res->y > 0 && Angle->y > 0 )
        {
            auto [res, spxl] = CalcDeviceDimension( Res->y, Angle->y, StepY );
            ResolutionY      = res;
            SPPY             = spxl;
        }
        sprintf(
          buf, "capture config resolution %d %d", ResolutionX, ResolutionY );
        SendString( buf );
        sprintf( buf, "capture config step-per-pixel %d %d", SPPX, SPPY );
        SendString( buf );
    }

    // Reinitialize device
    if ( bShouldReinit )
    {
        SendString( "capture init-sensor" );
    }

    // Update previous capture params
    if ( mPrevCaptureParam.has_value() == false )
        mPrevCaptureParam.emplace();
    if ( arg.CaptureDelayUs > 0 )
        mPrevCaptureParam->CaptureDelayUs = arg.CaptureDelayUs;
    if ( arg.DesiredAngle.has_value() )
        mPrevCaptureParam->DesiredAngle = arg.DesiredAngle;
    if ( arg.DesiredOffset.has_value() )
        mPrevCaptureParam->DesiredOffset = arg.DesiredOffset;
    if ( arg.DesiredResolution.has_value() )
        mPrevCaptureParam->DesiredResolution = arg.DesiredResolution;
}

bool FScannerProtocolHandler::IsConnected() const noexcept
{
    return bIsConnected.load();
}

bool FScannerProtocolHandler::IsActive() const noexcept
{
    return mBackgroundProcess.valid()
           && mBackgroundProcess.wait_for( 0ms ) != future_status::ready;
}

FDeviceStat FScannerProtocolHandler::GetDeviceStatus() const noexcept
{
    return mStat.load();
}

bool FScannerProtocolHandler::GetCompleteImage(
  FScanImageDesc& out ) const noexcept
{
    if ( mCompleteImage.empty() )
        return false;

    out = FScanImageDesc( mCompleteImage.data() );
    GetImageInfo( &out, mCompleteImageStat );

    return true;
}

bool FScannerProtocolHandler::GetScanningImage(
  FScanImageDesc& out ) const noexcept
{
    if ( mImage.empty() )
        return false;

    out = FScanImageDesc( mImage.data() );
    GetImageInfo( &out, mStatCache );

    return true;
}

static void GetImageInfo( FScanImageDesc* out, FDeviceStat const& stat )
{
    out->Width       = stat.SizeX;
    out->Height      = stat.SizeY;
    out->AspectRatio = (double)( stat.SizeX * stat.StepPerPxlX )
                       / ( stat.SizeY * stat.StepPerPxlY );
}

bool FScannerProtocolHandler::BeginCapture(
  CaptureParam const* params,
  size_t              TimeoutMs )
{
    // To check whether device is running ...
    if ( IsDeviceRunning() )
        return false;

    // Configure options if 'params' is not nullptr
    if ( params )
        configureCapture( *params, false );
    else if ( mPrevCaptureParam.has_value() == false )
        configureCapture( { /* Default Arg */ }, true );

    // Request report in sync
    requestReport( true, TimeoutMs );

    // Cache status to prevent corruption during capture
    mStatCache = mStat.load();

    // Request capture
    bRequestingCapture = true;
    SendString( "capture scan-start" );

    return true;
}

bool FScannerProtocolHandler::requestReport( bool bSync, size_t TimeoutMs )
{
    // Request status update
    mReportWait.arg.store( false );

    // Wait until device return status if needed.
    if ( bSync )
    {
        // Try five times
        constexpr auto Retry = 5;
        auto           Wait  = std::chrono::milliseconds( TimeoutMs / Retry );

        for ( size_t i = 0; i < Retry; i++ )
        {
            unique_lock<mutex> lck( mReportWait.mtx );
            SendString( "capture report" );
            if ( mReportWait.cv.wait_for(
                   lck, Wait, [this]() { return mReportWait.arg.load(); } ) )
            {
                return true;
            }
        }

        return false;
    }

    return true;
}

void FScannerProtocolHandler::SetDegreesPerStep( float x, float y ) noexcept
{
    char buf[256];
    sprintf( buf, "capture config angle-per-step %d %d", *(int*)&x, *(int*)&y );
    SendString( buf );
}

void FScannerProtocolHandler::OnString( char const* str )

{
    if ( bSuppressDeviceLog )
        return;
    print( str );
}

void FScannerProtocolHandler::OnBinaryData( char const* data, size_t len )
{
    void const* p = data;

    auto cmd = *ptr_cast<const SCANNER_COMMAND_TYPE>( p )++;

    switch ( cmd )
    {
    case ECommand::RSP_STAT_REPORT:
    {
        // Writing to device status structure is only available here.
        auto Stat = *ptr_cast<const FDeviceStat>( p )++;
        mStat.store( Stat );
        mReportWait.arg.store( true );
        mReportWait.cv.notify_all();
        if ( OnReport )
        {
            OnReport( Stat );
        }
    }
    break;
    case ECommand::RSP_LINE_DATA:
    {
        if ( bRequestingCapture == false )
            break;
        auto desc = *ptr_cast<const FLineDesc>( p )++;
        auto ActualReceive
          = len - sizeof( SCANNER_COMMAND_TYPE ) - sizeof( FLineDesc );
        auto ExpectedReceive = desc.NumPxls * sizeof FPxlData();
        if ( ActualReceive != ExpectedReceive )
        {
            print(
              "warning: expected: %ul bytes, received: %ul bytes\n",
              uint32_t( ExpectedReceive ),
              uint32_t( ActualReceive ) );
        }
        mStatCache = mStat.load();
        StoreLineData(
          mImage,
          mStatCache.SizeX,
          mStatCache.SizeY,
          desc,
          reinterpret_cast<FPxlData const*>( p ) );
        if ( OnReceiveLine )
        {
            FScanImageDesc desc;
            GetScanningImage( desc );
            OnReceiveLine( desc );
        }
        SendString( "capture report" );
    }
    break;

    case ECommand::RSP_PIXEL_DATA:
    case ECommand::RSP_DONE:
        swap( mImage, mCompleteImage );
        mImage.clear();
        mCompleteImageStat = mStatCache;
        bRequestingCapture = false;
        // Callback call async
        if ( OnFinishScan )
        {
            FScanImageDesc desc;
            GetCompleteImage( desc );
            OnFinishScan( desc );
        }

        print( "Capturing process done.\n" );
        break;

    case ECommand::RSP_POINT:
        auto Data = *ptr_cast<const FPointData>( p )++;
        OnPointRecv ? OnPointRecv( Data ) : (void)0;
        mNumAvailablePointRequest++;
        break;

    default:
        break;
    }
}

void FScannerProtocolHandler::RequestMotorMovement(
  int xstep,
  int ystep ) noexcept
{
    char buf[256];

    sprintf( buf, "capture motor-move %d %d", xstep, ystep );
    SendString( buf );
    SendString( "report" );
}

void FScannerProtocolHandler::SetMotorDriveClockSpeed( int Hz ) noexcept
{
    SetMotorDriveClockSpeed( Hz, Hz );
}

void FScannerProtocolHandler::SetMotorDriveClockSpeed(
  int X_Hz,
  int Y_Hz ) noexcept
{
    char buf[256];
    sprintf( buf, "capture config motor-max-clk %d %d", X_Hz, Y_Hz );
    SendString( buf );
}

void FScannerProtocolHandler::SetMotorAcceleration( int Hz ) noexcept
{
    SetMotorAcceleration( Hz, Hz );
}

void FScannerProtocolHandler::SetMotorAcceleration(
  int X_Hz,
  int Y_Hz ) noexcept
{
    char buf[256];
    sprintf( buf, "capture config motor-accel %d %d", X_Hz, Y_Hz );
    SendString( buf );
}

void FScannerProtocolHandler::ResetMotorPosition() noexcept
{
    SendString( "capture motor-reset" );
}

void FScannerProtocolHandler::StopCapture() noexcept
{
    bRequestingCapture = false;
    SendString( "capture stop" );
}

bool FScannerProtocolHandler::Report( size_t TimeoutMs ) noexcept
{
    return requestReport( true, TimeoutMs );
}

void FScannerProtocolHandler::ConfigSensorDelay(
  uint32_t Microseconds ) noexcept
{
    char buf[128];
    sprintf( buf, "capture config delay %d", Microseconds );
    SendString( buf );
}

void FScannerProtocolHandler::ConfigSensorDistMode(
  bool bCloseDistMode ) noexcept
{
    SendString(
      bCloseDistMode ? "capture config precision 1"
                     : "capture config precision 0" );
}

bool FScannerProtocolHandler::IsDeviceRunning() const noexcept
{
    return !mStat.load().bIsIdle;
}

void FScannerProtocolHandler::print( char const* fmt, ... ) const noexcept
{
    if ( !Logger )
        return;

    va_list v;
    va_start( v, fmt );
    size_t bufsz = vsnprintf( NULL, 0, fmt, v );
    char*  buf   = (char*)alloca( bufsz + 1 );
    vsprintf( buf, fmt, v );
    Logger( buf );
    va_end( v );
}

void StoreLineData(
  vector<FPxlData>& arr,
  int const         xl,
  int const         yl,
  FLineDesc const&  desc,
  FPxlData const*   data )
{
    int const x = desc.OfstX;
    int const y = desc.LineIdx;
    int const w = desc.NumPxls;
    assert( y < yl );
    assert( x + w <= xl );

    // Reserve space before process
    if ( arr.size() != (size_t)xl * yl )
        arr.resize( (size_t)xl * yl );

    // Set line position
    FPxlData* buf = arr.data() + (size_t)y * xl + x;
    assert( buf + w <= arr.data() + arr.size() );

    // Copy data
    memcpy( buf, data, sizeof( FPxlData ) * w );
}

FScanImageDesc FScanImageDesc::Clone() const noexcept
{
    assert( mReadData );

    size_t sz = (size_t)Width * Height;
    assert( mReadData && sz );

    auto ret      = *this;
    ret.mData     = new FPxlData[sz];
    ret.mReadData = ret.mData;

    memcpy( (void*)ret.mData, mReadData, sz * sizeof( FPxlData ) );
    return ret;
}

FScanImageDesc::FScanImageDesc(
  size_t    w,
  size_t    h,
  float     aspect,
  FPxlData* RawPtr ) noexcept
    : Width( w )
    , Height( h )
    , mData( RawPtr )
    , AspectRatio( aspect )
{
    mReadData = mData;
}

FScanImageDesc::FScanImageDesc( size_t w, size_t h, float aspect ) noexcept
    : Width( w )
    , Height( h )
    , mData( new FPxlData[w * h] )
    , AspectRatio( aspect )
{
    mReadData = mData;
}

FScanImageDesc::FScanImageDesc( FScanImageDesc const& v ) noexcept
{
    *this = v;
}

FScanImageDesc::FScanImageDesc( FScanImageDesc&& v ) noexcept
{
    *this = move( v );
}

FScanImageDesc& FScanImageDesc::operator=( FScanImageDesc const& v ) noexcept
{
    if ( mData )
    {
        delete[] mData;
    }

    memcpy( this, &v, sizeof( v ) );
    mData = nullptr;
    return *this;
}

FScanImageDesc& FScanImageDesc::operator=( FScanImageDesc&& v ) noexcept
{
    memcpy( this, &v, sizeof( v ) );
    v.mData = nullptr;
    return *this;
}

FScanImageDesc::~FScanImageDesc() noexcept
{
    if ( mData )
    {
        delete[] mData;
    }
}

bool FScannerProtocolHandler::QueuePoint(
  uint32_t RequestID,
  int16_t  xs,
  int16_t  ys ) noexcept
{
    if ( mNumAvailablePointRequest == 0 )
    {
        return false;
    }

    mNumAvailablePointRequest--;
    char buf[256];
    sprintf( buf, "capture point-queue %d %d %d", RequestID, xs, ys );
    SendString( buf );
}

bool FScannerProtocolHandler::QueuePointAngular(
  uint32_t RequestID,
  float    AngleX,
  float    AngleY ) noexcept
{
    return QueuePoint(
      RequestID,
      int16_t( AngleX / mStatCache.DegreePerStepX ),
      int16_t( AngleY / mStatCache.DegreePerStepY ) );
}

size_t FScannerProtocolHandler::GetPendingPointRequestCount() const noexcept
{
    return mNumMaxPointRequest - mNumAvailablePointRequest;
}

bool FScannerProtocolHandler::InitPointMode() noexcept
{
    bool bWasIdle = !IsDeviceRunning();
    SendString( "capture point-start" );

    // Refresh number of available requests
    mStatCache = mStat.load();
    constexpr decltype( mStatCache.NumMaxPointRequest ) MAX_VALUE = 30;
    auto Max = std::min( MAX_VALUE, mStatCache.NumMaxPointRequest );
    mNumAvailablePointRequest = Max;
    mNumMaxPointRequest       = Max;

    print( "info: initialize point capture process; Max req %d\n", Max );
    return bWasIdle;
}