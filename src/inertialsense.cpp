#include <functional>
#include "inertialsense.h"

using namespace std;
using namespace std::placeholders;

#define IS_WARN(...)		{GPS_WARN(__VA_ARGS__);}
#define IS_DEBUG(...)		{/*GPS_WARN(__VA_ARGS__);*/}

//static GPSDriverInertialSense* hacky_gps_driver_global_pointer_;
//static CMHANDLE g_com_manager_;

GPSDriverInertialSense::GPSDriverInertialSense(Interface gpsInterface, GPSCallbackPtr callback, void *callback_user,
																							 struct vehicle_gps_position_s *gps_position,
																							 struct satellite_info_s *satellite_info)
	: GPSHelper(callback, callback_user)
	, _gps_position(gps_position)
	, _satellite_info(satellite_info)
{
	start_time = gps_absolute_time();
	initComManager(1, 1, 10, 1, IS_read_wrapper, IS_write_wrapper, NULL, IS_post_rx_wrapper, NULL, NULL);
	comManagerAssignUserPointer(getGlobalComManager(), this);
}

GPSDriverInertialSense::~GPSDriverInertialSense()
{
	comManagerAssignUserPointer(getGlobalComManager(), NULL);
}

/**
 * configure the device
 * @param baud will be set to the baudrate (output parameter)
 * @return 0 on success, <0 otherwise
 * TODO:: Needs to return properly
 */
int GPSDriverInertialSense::configure(unsigned &baud, OutputMode output_mode)
{
  // Set the output_mode
  uint32_t uINS_sys_config = SYS_CFG_BITS_RTK_ROVER;
  if (output_mode == OutputMode::RTCM)
  {
    uINS_sys_config = SYS_CFG_BITS_RTK_BASE_STATION;
    setComManagerPassThrough(IS_RTCM_rx_wrapper);
  }


  // Configure the Appropriate sys_config bits for RTCM handling
  int res = sendDataComManager(0, DID_FLASH_CONFIG, &uINS_sys_config, sizeof(uint32_t), OFFSETOF(nvm_flash_cfg_t, sysCfgBits));
  PX4_ERR("sys config bits = %b", uINS_sys_config);
  PX4_ERR("IS error %d", res);
  // Configure uINS to output GPS Info at 100 ms
  int res2 = getDataComManager(0, DID_GPS, 0, 0, 100);
  PX4_ERR("IS error2 %d", res2);

  if (receive(10000) != 0)
  {
    PX4_ERR("found IS");
    return 0;
  }
  else
  {
    PX4_ERR("missing IS");
    return -1;
  }

}

void GPSDriverInertialSense::GPS_callback(gps_t* data)
{
  PX4_ERR("IS got GPS message");
  _got_sat_info = true;
  _gps_position->lat = (int32_t)(data->pos.lla[0]*1e7);
  _gps_position->lon = (int32_t)(data->pos.lla[1]*1e7);
  _gps_position->alt = (int32_t)(data->pos.lla[2]*1e3);
  _gps_position->alt_ellipsoid = (int32_t)(data->pos.hMSL * 1e3f);

  _gps_position->s_variance_m_s = data->vel.sAcc;
  _gps_position->c_variance_rad = data->vel.cAcc;

  _gps_position->eph = data->pos.hAcc;
  _gps_position->epv = data->pos.vAcc;

  _gps_position->hdop = data->pos.pDop;
  _gps_position->hdop = data->pos.pDop * 1.2f; // This is a hack because the inertialsense doesn't output this directly

  _gps_position->noise_per_ms = 0.0f;
  _gps_position->jamming_indicator = 0.0f;

  _gps_position->vel_m_s = data->vel.s2D;
  _gps_position->vel_n_m_s = data->vel.ned[0];
  _gps_position->vel_e_m_s = data->vel.ned[1];
  _gps_position->vel_d_m_s = data->vel.ned[2];
  _gps_position->cog_rad = data->vel.course;
  _gps_position->vel_ned_valid = true;

  _gps_position->timestamp_time_relative = gps_absolute_time() - start_time;
  if (data->pos.week > 0)
  {
    _gps_position->time_utc_usec = GPS_UTC_OFFSET + (uint64_t)(data->pos.week*7*24*3600*1e9f) + (uint64_t)(data->towOffset*1e9);
  }
  _satellite_info->count = _gps_position->satellites_used = data->pos.status & (0x00FF);

  // For now, because we aren't getting a true survey status, let's just use the reported accuracy of the GPS position
  SurveyInStatus status;
  status.duration = _gps_position->timestamp_time_relative/1e6f;
  status.flags = (_gps_position->eph < 2.0f);
  status.mean_accuracy = _gps_position->eph*1000;
  surveyInStatus(status);
}

int GPSDriverInertialSense::IS_write_wrapper(CMHANDLE cmHandle, int pHandle, buffer_t* bufferToSend)
{
  PX4_ERR("sending IS data");
  (void)cmHandle;
  (void)pHandle;
  GPSDriverInertialSense* instance = (GPSDriverInertialSense*)comManagerGetUserPointer(getGlobalComManager());
  return instance->write(bufferToSend->buf, bufferToSend->size);
}

int GPSDriverInertialSense::IS_read_wrapper(CMHANDLE cmHandle, int pHandle, unsigned char* readIntoBytes, int numberOfBytes)
{
//  PX4_ERR("read");
  (void)cmHandle;
  (void)pHandle;
  GPSDriverInertialSense* instance = (GPSDriverInertialSense*)comManagerGetUserPointer(getGlobalComManager());
  int ret = instance->read(readIntoBytes, numberOfBytes, 2);
  if (ret != 0)
  {
//    PX4_ERR("read %d bytes", ret);
  }
  return ret;
}

void GPSDriverInertialSense::IS_post_rx_wrapper(CMHANDLE cmHandle, int pHandle, p_data_t *dataRead)
{
  PX4_ERR("got message");
  (void)cmHandle;
  (void)pHandle;
  GPSDriverInertialSense* instance = (GPSDriverInertialSense*)comManagerGetUserPointer(getGlobalComManager());

  // Call the appropriate callback
  switch (dataRead->hdr.id)
  {
  case DID_GPS:
    instance->GPS_callback((gps_t*)dataRead->buf);
    break;
  default:
    PX4_ERR("got message unmanaged");
    break;
  }
}

void GPSDriverInertialSense::IS_RTCM_rx_wrapper(CMHANDLE cmHandle, com_manager_pass_through_t passThroughType, int pHandle, const unsigned char *data, int dataLength)
{
  (void)cmHandle;
  (void)pHandle;
  GPSDriverInertialSense* instance = (GPSDriverInertialSense*)comManagerGetUserPointer(getGlobalComManager());

  if (passThroughType == COM_MANAGER_PASS_THROUGH_RTCM3)
  {
    instance->gotRTCMMessage((uint8_t*)data, dataLength);
  }
}


int GPSDriverInertialSense::receive(unsigned timeout)
{
  _got_sat_info = false;
  _got_gps = false;

  gps_abstime time_started = gps_absolute_time();

  // Wait for a callback to occur
  while ((gps_absolute_time() < time_started + timeout*1000) && !(_got_gps || _got_sat_info))
  {
    stepComManager();
  }
  uint8_t ret = 0;

  if (_got_gps)
  {
    ret |= 0b1;
  }

  if(_got_sat_info)
  {
    ret |= 0b10;
  }

  return ret;
}

void GPSDriverInertialSense::setSurveyInSpecs(uint32_t survey_in_acc_limit, uint32_t survey_in_min_dur)
{
  // TODO: We still  have to enable this on the InertialSense
}

/**
 * Start or restart the survey-in procees. This is only used in RTCM ouput mode.
 * It will be called automatically after configuring.
 * @return 0 on success, <0 on error
 */
int GPSDriverInertialSense::restartSurveyIn(){ return 0; }
