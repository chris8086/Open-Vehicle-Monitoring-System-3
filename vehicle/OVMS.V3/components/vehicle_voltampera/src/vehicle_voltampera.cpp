/*
;    Project:       Open Vehicle Monitor System
;    Date:          14th March 2017
;
;    Changes:
;    1.0  Initial release
;
;    (C) 2011       Michael Stegen / Stegen Electronics
;    (C) 2011-2017  Mark Webb-Johnson
;    (C) 2011        Sonny Chen @ EPRO/DX
;
; Permission is hereby granted, free of charge, to any person obtaining a copy
; of this software and associated documentation files (the "Software"), to deal
; in the Software without restriction, including without limitation the rights
; to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
; copies of the Software, and to permit persons to whom the Software is
; furnished to do so, subject to the following conditions:
;
; The above copyright notice and this permission notice shall be included in
; all copies or substantial portions of the Software.
;
; THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
; IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
; FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
; AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
; LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
; OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
; THE SOFTWARE.
*/

#include "ovms_log.h"
static const char *TAG = "v-voltampera";

#include <stdio.h>
#include "vehicle_voltampera.h"

// Use states:
// 0 = bus is idle, car sleeping
// 1 = car is on and ready to Drive
static const OvmsVehicle::poll_pid_t va_polls[]
  =
  {
    { 0x7e0, 0x7e8, VEHICLE_POLL_TYPE_OBDIIEXTENDED, 0x000d, {  0, 10,  0 } }, // Vehicle speed
    { 0x7e4, 0x7ec, VEHICLE_POLL_TYPE_OBDIIEXTENDED, 0x4369, {  0, 10,  0 } }, // On-board charger current
    { 0x7e4, 0x7ec, VEHICLE_POLL_TYPE_OBDIIEXTENDED, 0x4368, {  0, 10,  0 } }, // On-board charger voltage
    { 0x7e4, 0x7ec, VEHICLE_POLL_TYPE_OBDIIEXTENDED, 0x801f, {  0, 10,  0 } }, // Outside temperature (filtered)
    { 0x7e4, 0x7ec, VEHICLE_POLL_TYPE_OBDIIEXTENDED, 0x801e, {  0, 10,  0 } }, // Outside temperature (raw)
    { 0x7e4, 0x7ec, VEHICLE_POLL_TYPE_OBDIIEXTENDED, 0x434f, {  0, 10,  0 } }, // High-voltage Battery temperature
    { 0x7e4, 0x7ec, VEHICLE_POLL_TYPE_OBDIIEXTENDED, 0x1c43, {  0, 10,  0 } }, // PEM temperature
    { 0x7e4, 0x7ec, VEHICLE_POLL_TYPE_OBDIIEXTENDED, 0x8334, {  0, 10,  0 } }, // SOC
    { 0x7e1, 0x7e9, VEHICLE_POLL_TYPE_OBDIIEXTENDED, 0x2487, {  0,100,  0 } }, // Distance Traveled on Battery Energy This Drive Cycle
    { 0, 0, 0x00, 0x00, { 0, 0, 0 } }
  };

OvmsVehicleVoltAmpera::OvmsVehicleVoltAmpera()
  {
  ESP_LOGI(TAG, "Volt/Ampera vehicle module");

  memset(m_vin,0,sizeof(m_vin));
  m_type[0] = 'V';
  m_type[1] = 'A';
  m_type[2] = 0;
  m_type[3] = 0;
  m_charge_timer = 0;
  m_charge_wm = 0;
  m_candata_timer = 0;
  m_drive_distance_bat_max = (35*5)/8;

  // require GPS:
  MyEvents.SignalEvent("vehicle.require.gps", NULL);
  MyEvents.SignalEvent("vehicle.require.gpstime", NULL);

  // Register CAN bus and polling requests
  RegisterCanBus(1,CAN_MODE_ACTIVE,CAN_SPEED_500KBPS);
  PollSetPidList(m_can1,va_polls);
  PollSetState(0);
  }

OvmsVehicleVoltAmpera::~OvmsVehicleVoltAmpera()
  {
  ESP_LOGI(TAG, "Shutdown Volt/Ampera vehicle module");
  }

void OvmsVehicleVoltAmpera::IncomingFrameCan1(CAN_frame_t* p_frame)
  {
  uint8_t *d = p_frame->data.u8;
  int k;

  if ((p_frame->MsgID == 0x7e8)||
      (p_frame->MsgID == 0x7ec)||
      (p_frame->MsgID == 0x7e9))
    return; // Ignore poll responses

  // Activity on the bus, so resume polling
  if (m_poll_state != 1)
    {
    ESP_LOGI(TAG,"Car has woken (CAN bus activity)");
    PollSetState(1);
    }
  m_candata_timer = 60;

  // Process the incoming message
  switch (p_frame->MsgID)
    {
    case 0x4e1:
      {
      for (k=0;k<8;k++)
        m_vin[k+9] = d[k];
      break;
      }
    case 0x514:
      {
      for (k=0;k<8;k++)
        m_vin[k+1] = d[k];
      m_vin[0] = '1';
      m_vin[17] = 0;
      if (m_vin[9] != 0)
        {
        StandardMetrics.ms_v_vin->SetValue(m_vin);
        StandardMetrics.ms_v_type->SetValue(m_type);
        }
      break;
      }
    case 0x135:
      {
      if (d[0] == 0)
        {
        // Car is in PARK
        StandardMetrics.ms_v_env_gear->SetValue(0);
        StandardMetrics.ms_v_env_on->SetValue(false);
        StandardMetrics.ms_v_env_awake->SetValue(false);
        StandardMetrics.ms_v_env_handbrake->SetValue(true);
        }
      else
        {
        // Car is not in PARK
        StandardMetrics.ms_v_env_gear->SetValue(0);
        StandardMetrics.ms_v_env_on->SetValue(true);
        StandardMetrics.ms_v_env_awake->SetValue(true);
        StandardMetrics.ms_v_env_handbrake->SetValue(false);
        }
      break;
      }
    default:
      break;
    }
  }

void OvmsVehicleVoltAmpera::IncomingPollReply(canbus* bus, uint16_t type, uint16_t pid, uint8_t* data, uint8_t length, uint16_t mlremain)
  {
  uint8_t value = *data;

  switch (pid)
    {
    case 0x4369:  // On-board charger current
      StandardMetrics.ms_v_charge_current->SetValue((unsigned int)value / 5);
      break;
    case 0x4368:  // On-board charger voltage
      StandardMetrics.ms_v_charge_voltage->SetValue((unsigned int)value <<1);
      break;
    case 0x801f:  // Outside temperature (filtered) (aka ambient temperature)
      StandardMetrics.ms_v_env_temp->SetValue((int)value/2 - 0x28);
      break;
    case 0x801e:  // Outside temperature (raw)
      break;
    case 0x434f:  // High-voltage Battery temperature
      StandardMetrics.ms_v_bat_temp->SetValue((int)value - 0x28);
      break;
    case 0x1c43:  // PEM temperature
      StandardMetrics.ms_v_inv_temp->SetValue((int)value - 0x28);
      break;
    case 0x8334:  // SOC
      {
      int soc = ((int)value * 39)/99;
      StandardMetrics.ms_v_bat_soc->SetValue(soc);
      StandardMetrics.ms_v_bat_range_est->SetValue((soc * m_drive_distance_bat_max)/100);
      StandardMetrics.ms_v_bat_range_ideal->SetValue((soc * m_drive_distance_bat_max)/100);
      break;
      }
    case 0x000d:  // Vehicle speed
      StandardMetrics.ms_v_pos_speed->SetValue(value,Kilometers);
      break;
    case 0x2487:  //Distance Traveled on Battery Energy This Drive Cycle
       //edrive_distance = MiFromKm((can_databuffer[5] + ((unsigned int)can_databuffer[4] << 8)) / 100); // German Volt Report im KM
       //if ((edrive_distance > va_drive_distance_bat_max) && (car_chargestate == 4)) va_drive_distance_bat_max = edrive_distance;
       break;
    default:
      break;
    }
  }

void OvmsVehicleVoltAmpera::Ticker1(uint32_t ticker)
  {
  // Check if the car has gone to sleep
  if (m_candata_timer > 0)
    {
    if (--m_candata_timer == 0)
      {
      // Car has gone to sleep
      ESP_LOGI(TAG,"Car has gone to sleep (CAN bus timeout)");
      StandardMetrics.ms_v_env_gear->SetValue(0);
      StandardMetrics.ms_v_env_on->SetValue(false);
      StandardMetrics.ms_v_env_awake->SetValue(false);
      StandardMetrics.ms_v_env_handbrake->SetValue(true);
      PollSetState(0);
      }
    else
      {
      // Car is awake
      StandardMetrics.ms_v_env_on->SetValue(true);
      StandardMetrics.ms_v_env_awake->SetValue(true);
      StandardMetrics.ms_v_env_handbrake->SetValue(false);
      }
    }

  int cc = StandardMetrics.ms_v_charge_current->AsInt();
  int cv = StandardMetrics.ms_v_charge_voltage->AsInt();
  if ((cc != 0)&&(cv != 0))
    {
    // The car is charging
    StandardMetrics.ms_v_env_charging12v->SetValue(true);
    if (StandardMetrics.ms_v_charge_inprogress->AsBool() == false)
      {
      // A charge has started
      ESP_LOGI(TAG,"Car has started a charge");
      StandardMetrics.ms_v_charge_pilot->SetValue(true);
      StandardMetrics.ms_v_charge_inprogress->SetValue(true);
      StandardMetrics.ms_v_door_chargeport->SetValue(true);
      StandardMetrics.ms_v_charge_mode->SetValue("standard");
      StandardMetrics.ms_v_charge_state->SetValue("charging");
      StandardMetrics.ms_v_charge_substate->SetValue("onrequest");
      StandardMetrics.ms_v_charge_climit->SetValue(16);
      m_charge_timer = 0;
      m_charge_wm = 0;
      // TODO: May need to raise a notification here (charge started)
      }
    else
      {
      // A charge is ongoing
      m_charge_timer++;
      if (m_charge_timer >= 60)
        {
        m_charge_timer -= 60;
        m_charge_wm += (StandardMetrics.ms_v_charge_voltage->AsInt()
                      * StandardMetrics.ms_v_charge_current->AsInt());
        if (m_charge_wm > 60000)
          {
          StandardMetrics.ms_v_charge_kwh->SetValue(
            StandardMetrics.ms_v_charge_kwh->AsInt() + 10);
          m_charge_wm -= 60000;
          }
        }
      }
    }
  else if ((cc == 0)&&(cv == 0))
    {
    // The car is not charging
    if (StandardMetrics.ms_v_charge_inprogress->AsBool())
      {
      // The charge has completed/stopped
      StandardMetrics.ms_v_charge_pilot->SetValue(false);
      StandardMetrics.ms_v_charge_inprogress->SetValue(false);
      StandardMetrics.ms_v_door_chargeport->SetValue(false);
      StandardMetrics.ms_v_charge_mode->SetValue("standard");
      if (StandardMetrics.ms_v_bat_soc->AsInt() < 95)
        {
        // Assume the charge was interrupted
        ESP_LOGI(TAG,"Car charge session was interrupted");
        StandardMetrics.ms_v_charge_state->SetValue("stopped");
        StandardMetrics.ms_v_charge_substate->SetValue("interrupted");
        // TODO: May need to raise a notification here (charge interrupted)
        }
      else
        {
        // Assume the charge completed normally
        ESP_LOGI(TAG,"Car charge session completed");
        StandardMetrics.ms_v_charge_state->SetValue("done");
        StandardMetrics.ms_v_charge_substate->SetValue("onrequest");
        // TODO: Need to raise a notification here (charged )
        }
      m_charge_timer = 0;
      m_charge_wm = 0;
      }
    StandardMetrics.ms_v_env_charging12v->SetValue(false);
    }

  }

class OvmsVehicleVoltAmperaInit
  {
  public: OvmsVehicleVoltAmperaInit();
} MyOvmsVehicleVoltAmperaInit  __attribute__ ((init_priority (9000)));

OvmsVehicleVoltAmperaInit::OvmsVehicleVoltAmperaInit()
  {
  ESP_LOGI(TAG, "Registering Vehicle: Volt/Ampera (9000)");

  MyVehicleFactory.RegisterVehicle<OvmsVehicleVoltAmpera>("VA","Volt/Ampera");
  }