/*
 ;    Project:       Open Vehicle Monitor System
 ;    Date:          1th October 2018
 ;
 ;    Changes:
 ;    1.0  Initial release
 ;
 ;    (C) 2018       Martin Graml
 ;    (C) 2019       Thomas Heuer
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
 ;
 ; Most of the CAN Messages are based on https://github.com/MyLab-odyssey/ED_BMSdiag
 ; http://ed.no-limit.de/wiki/index.php/Hauptseite
 */

#include "ovms_log.h"
static const char *TAG = "v-smarteq";

#define VERSION "1.0.0"

#include <stdio.h>
#include <string>
#include <iomanip>
#include "pcp.h"
#include "ovms_metrics.h"
#include "ovms_events.h"
#include "ovms_config.h"
#include "ovms_command.h"
#include "metrics_standard.h"
#include "ovms_notify.h"
#include "ovms_peripherals.h"

#include "vehicle_smarteq.h"

static const OvmsPoller::poll_pid_t obdii_polls[] =
{
  // { tx, rx, type, pid, {OFF,AWAKE,ON,CHARGING}, bus, protocol }
  // { 0x792, 0x793, VEHICLE_POLL_TYPE_OBDIIGROUP, 0x80, {  0,300,999,999 }, 0, ISOTP_STD }, // rqIDpart OBL_7KW_Installed
  { 0x79B, 0x7BB, VEHICLE_POLL_TYPE_OBDIIGROUP, 0x07, {  0,300,3,3 }, 0, ISOTP_STD }, // rqBattState
  { 0x79B, 0x7BB, VEHICLE_POLL_TYPE_OBDIIGROUP, 0x04, {  0,300,300,300 }, 0, ISOTP_STD }, // rqBattTemperatures
  { 0x79B, 0x7BB, VEHICLE_POLL_TYPE_OBDIIGROUP, 0x41, {  0,300,300,60 }, 0, ISOTP_STD }, // rqBattVoltages_P1
  { 0x79B, 0x7BB, VEHICLE_POLL_TYPE_OBDIIGROUP, 0x42, {  0,300,300,60 }, 0, ISOTP_STD }, // rqBattVoltages_P2
  { 0x743, 0x763, VEHICLE_POLL_TYPE_OBDIIEXTENDED, 0x200c, {  0,300,10,300 }, 0, ISOTP_STD }, // extern temp byte 2+3
  { 0x7E4, 0x7EC, VEHICLE_POLL_TYPE_OBDIIEXTENDED, 0x320c, {  0,300,60,60 }, 0, ISOTP_STD }, // rqHV_Energy
  { 0x7E4, 0x7EC, VEHICLE_POLL_TYPE_OBDIIEXTENDED, 0x302A, {  0,300,60,60 }, 0, ISOTP_STD }, // rqDCDC_State
  { 0x7E4, 0x7EC, VEHICLE_POLL_TYPE_OBDIIEXTENDED, 0x3495, {  0,300,60,60 }, 0, ISOTP_STD }, // rqDCDC_Load
  { 0x7E4, 0x7EC, VEHICLE_POLL_TYPE_OBDIIEXTENDED, 0x3025, {  0,300,60,60 }, 0, ISOTP_STD }, // rqDCDC_Amps
  { 0x7E4, 0x7EC, VEHICLE_POLL_TYPE_OBDIIEXTENDED, 0x3494, {  0,300,60,60 }, 0, ISOTP_STD }, // rqDCDC_Power
  { 0x745, 0x765, VEHICLE_POLL_TYPE_OBDIIGROUP, 0x81, {  0,3600,3600,3600 }, 0, ISOTP_STD }, // req.VIN
  // { 0x744, 0x764, VEHICLE_POLL_TYPE_OBDIIGROUP, 0x52, {  0,300,10,999 }, 0, ISOTP_STD }, // ,764,36,45,.1,400,1,°C,2152,6152,ff,IH_InCarTemp
  POLL_LIST_END
};

/**
 * Constructor & destructor
 */

OvmsVehicleSmartEQ::OvmsVehicleSmartEQ() {
  ESP_LOGI(TAG, "Start Smart EQ vehicle module");
  
  m_booster_start = false;
  m_led_state = 0;

  // BMS configuration:
  BmsSetCellArrangementVoltage(96, 3);
  BmsSetCellArrangementTemperature(28, 1);
  BmsSetCellLimitsVoltage(2.0, 5.0);
  BmsSetCellLimitsTemperature(-39, 200);
  BmsSetCellDefaultThresholdsVoltage(0.020, 0.030);
  BmsSetCellDefaultThresholdsTemperature(2.0, 3.0);
  
  mt_bms_temps = new OvmsMetricVector<float>("xsq.v.bms.temps", SM_STALE_HIGH, Celcius);
  mt_bus_awake = MyMetrics.InitBool("xsq.v.bus.awake", SM_STALE_MIN, false);
  mt_evc_hv_energy = MyMetrics.InitFloat("xsq.evc.hv.energy", SM_STALE_MID, 0, kWh);
  mt_evc_LV_DCDC_amps = MyMetrics.InitFloat("xsq.evc.lv.dcdc.amps", SM_STALE_MID, 0, Amps);
  mt_evc_LV_DCDC_load = MyMetrics.InitFloat("xsq.evc.lv.dcdc.load", SM_STALE_MID, 0, Percentage);
  mt_evc_LV_DCDC_power = MyMetrics.InitFloat("xsq.evc.lv.dcdc.power", SM_STALE_MID, 0, Watts);
  mt_evc_LV_DCDC_state = MyMetrics.InitInt("xsq.evc.lv.dcdc.state", SM_STALE_MID, 0, Other);
  
  mt_bms_CV_Range_min = MyMetrics.InitFloat("xsq.bms.cv.range.min", SM_STALE_MID, 0, Volts);
  mt_bms_CV_Range_max = MyMetrics.InitFloat("xsq.bms.cv.range.max", SM_STALE_MID, 0, Volts);
  mt_bms_CV_Range_mean = MyMetrics.InitFloat("xsq.bms.cv.range.mean", SM_STALE_MID, 0, Volts);
  mt_bms_BattLinkVoltage = MyMetrics.InitFloat("xsq.bms.batt.link.voltage", SM_STALE_MID, 0, Volts);
  mt_bms_BattCV_Sum = MyMetrics.InitFloat("xsq.bms.batt.cv.sum", SM_STALE_MID, 0, Volts);
  mt_bms_BattPower_voltage = MyMetrics.InitFloat("xsq.bms.batt.voltage", SM_STALE_MID, 0, Volts);
  mt_bms_BattPower_current = MyMetrics.InitFloat("xsq.bms.batt.current", SM_STALE_MID, 0, Amps);
  mt_bms_BattPower_power = MyMetrics.InitFloat("xsq.bms.batt.power", SM_STALE_MID, 0, kW);
  mt_bms_HVcontactState = MyMetrics.InitInt("xsq.bms.hv.contact.state", SM_STALE_MID, 0, Other);
  mt_bms_HV = MyMetrics.InitFloat("xsq.bms.hv", SM_STALE_MID, 0, Volts);
  mt_bms_EVmode = MyMetrics.InitInt("xsq.bms.ev.mode", SM_STALE_MID, 0, Other);
  mt_bms_LV = MyMetrics.InitFloat("xsq.bms.lv", SM_STALE_MID, 0, Volts);
  mt_bms_Amps = MyMetrics.InitFloat("xsq.bms.amps", SM_STALE_MID, 0, Amps);
  mt_bms_Amps2 = MyMetrics.InitFloat("xsq.bms.amp2", SM_STALE_MID, 0, Amps);
  mt_bms_Power = MyMetrics.InitFloat("xsq.bms.power", SM_STALE_MID, 0, kW);
  
  RegisterCanBus(1, CAN_MODE_ACTIVE, CAN_SPEED_500KBPS);
  PollSetPidList(m_can1, obdii_polls);
  PollSetState(0);
  
  PollSetThrottling(5);
  PollSetResponseSeparationTime(20);

  MyConfig.RegisterParam("xsq", "Smart EQ", true, true);
  ConfigChanged(NULL);

#ifdef CONFIG_OVMS_COMP_WEBSERVER
  WebInit();
#endif
}

OvmsVehicleSmartEQ::~OvmsVehicleSmartEQ() {
  ESP_LOGI(TAG, "Stop Smart EQ vehicle module");

#ifdef CONFIG_OVMS_COMP_WEBSERVER
  WebDeInit();
#endif
}

/**
 * ConfigChanged: reload single/all configuration variables
 */
void OvmsVehicleSmartEQ::ConfigChanged(OvmsConfigParam* param) {
  if (param && param->GetName() != "xsq")
    return;

  ESP_LOGI(TAG, "Smart EQ reload configuration");
  
  m_enable_write = MyConfig.GetParamValueBool("xsq", "canwrite", false);
  m_enable_LED_state = MyConfig.GetParamValueBool("xsq", "led", false);
}

uint64_t OvmsVehicleSmartEQ::swap_uint64(uint64_t val) {
  val = ((val << 8) & 0xFF00FF00FF00FF00ull) | ((val >> 8) & 0x00FF00FF00FF00FFull);
  val = ((val << 16) & 0xFFFF0000FFFF0000ull) | ((val >> 16) & 0x0000FFFF0000FFFFull);
  return (val << 32) | (val >> 32);
}

void OvmsVehicleSmartEQ::IncomingFrameCan1(CAN_frame_t* p_frame) {
  uint8_t *data = p_frame->data.u8;
  uint64_t c = swap_uint64(p_frame->data.u64);
  
  static bool isCharging = false;
  static bool lastCharging = false;
  float _range_est;

  if (m_candata_poll != 1 && StandardMetrics.ms_v_bat_voltage->AsFloat(0, Volts) > 100) {
    ESP_LOGI(TAG,"Car has woken (CAN bus activity)");
    mt_bus_awake->SetValue(true);
    m_candata_poll = 1;
  }
  m_candata_timer = SQ_CANDATA_TIMEOUT;
  
  switch (p_frame->MsgID) {
    case 0x392:
      StandardMetrics.ms_v_env_hvac->SetValue((CAN_BYTE(1) & 0x40) > 0);
      StandardMetrics.ms_v_env_cabintemp->SetValue(CAN_BYTE(5) - 40);
      break;
    case 0x42E: // HV Voltage
      StandardMetrics.ms_v_bat_voltage->SetValue((float) ((CAN_UINT(3)>>5)&0x3ff) / 2); // HV Voltage
      StandardMetrics.ms_v_bat_temp->SetValue(((c >> 13) & 0x7Fu) - 40); // HVBatteryTemp
      StandardMetrics.ms_v_charge_climit->SetValue((c >> 20) & 0x3Fu); // MaxChargingNegotiatedCurrent
      break;
    case 0x4F8:
      StandardMetrics.ms_v_env_handbrake->SetValue((CAN_BYTE(0) & 0x08) > 0);
      StandardMetrics.ms_v_env_awake->SetValue((CAN_BYTE(0) & 0x40) > 0); // Ignition on
      break;
    case 0x5D7: // Speed, ODO
      StandardMetrics.ms_v_pos_speed->SetValue((float) CAN_UINT(0) / 100);
      StandardMetrics.ms_v_pos_odometer->SetValue((float) (CAN_UINT32(2)>>4) / 100);
      break;
    case 0x5de:
      StandardMetrics.ms_v_env_headlights->SetValue((CAN_BYTE(0) & 0x04) > 0);
      StandardMetrics.ms_v_door_fl->SetValue((CAN_BYTE(1) & 0x08) > 0);
      StandardMetrics.ms_v_door_fr->SetValue((CAN_BYTE(1) & 0x02) > 0);
      StandardMetrics.ms_v_door_rl->SetValue((CAN_BYTE(2) & 0x40) > 0);
      StandardMetrics.ms_v_door_rr->SetValue((CAN_BYTE(2) & 0x10) > 0);
      StandardMetrics.ms_v_door_trunk->SetValue((CAN_BYTE(7) & 0x10) > 0);
      break;
    case 0x654: // SOC(b)
      StandardMetrics.ms_v_bat_soc->SetValue(CAN_BYTE(3));
      StandardMetrics.ms_v_door_chargeport->SetValue((CAN_BYTE(0) & 0x20)); // ChargingPlugConnected
      StandardMetrics.ms_v_charge_duration_full->SetValue((((c >> 22) & 0x3ffu) < 0x3ff) ? (c >> 22) & 0x3ffu : 0);
      _range_est = ((c >> 12) & 0x3FFu); // VehicleAutonomy
      if ( _range_est != 1023.0 )
        StandardMetrics.ms_v_bat_range_est->SetValue(_range_est); // VehicleAutonomy
      break;
    case 0x65C: // ExternalTemp
      StandardMetrics.ms_v_env_temp->SetValue((CAN_BYTE(0) >> 1) - 40); // ExternalTemp ?
      break;
    case 0x658: //
      StandardMetrics.ms_v_bat_soh->SetValue(CAN_BYTE(4) & 0x7Fu); // SOH ?
      isCharging = (CAN_BYTE(5) & 0x20); // ChargeInProgress
      if (isCharging) { // STATE charge in progress
        //StandardMetrics.ms_v_charge_inprogress->SetValue(isCharging);
      }
      if (isCharging != lastCharging) { // EVENT charge state changed
        if (isCharging) { // EVENT started charging
          StandardMetrics.ms_v_charge_pilot->SetValue(true);
          StandardMetrics.ms_v_charge_inprogress->SetValue(isCharging);
          StandardMetrics.ms_v_charge_mode->SetValue("standard");
          StandardMetrics.ms_v_charge_type->SetValue("type2");
          StandardMetrics.ms_v_charge_state->SetValue("charging");
          StandardMetrics.ms_v_charge_substate->SetValue("onrequest");
        } else { // EVENT stopped charging
          StandardMetrics.ms_v_charge_pilot->SetValue(false);
          StandardMetrics.ms_v_charge_inprogress->SetValue(isCharging);
          StandardMetrics.ms_v_charge_mode->SetValue("standard");
          StandardMetrics.ms_v_charge_type->SetValue("type2");
          if (StandardMetrics.ms_v_bat_soc->AsInt() < 95) {
            // Assume the charge was interrupted
            ESP_LOGI(TAG,"Car charge session was interrupted");
            StandardMetrics.ms_v_charge_state->SetValue("stopped");
            StandardMetrics.ms_v_charge_substate->SetValue("interrupted");
          } else {
            // Assume the charge completed normally
            ESP_LOGI(TAG,"Car charge session completed");
            StandardMetrics.ms_v_charge_state->SetValue("done");
            StandardMetrics.ms_v_charge_substate->SetValue("onrequest");
          }
        }
      }
      lastCharging = isCharging;
      break;
    case 0x668:
      StandardMetrics.ms_v_env_on->SetValue((CAN_BYTE(0) & 0x40) > 0); // Drive Ready
      break;
    case 0x673:
      if (CAN_BYTE(2) != 0xff)
        StandardMetrics.ms_v_tpms_pressure->SetElemValue(MS_V_TPMS_IDX_RR, (float) CAN_BYTE(2)*3.1);
      if (CAN_BYTE(3) != 0xff)
        StandardMetrics.ms_v_tpms_pressure->SetElemValue(MS_V_TPMS_IDX_RL, (float) CAN_BYTE(3)*3.1);
      if (CAN_BYTE(4) != 0xff)
        StandardMetrics.ms_v_tpms_pressure->SetElemValue(MS_V_TPMS_IDX_FR, (float) CAN_BYTE(4)*3.1);
      if (CAN_BYTE(5) != 0xff)
        StandardMetrics.ms_v_tpms_pressure->SetElemValue(MS_V_TPMS_IDX_FL, (float) CAN_BYTE(5)*3.1);
      break;

    default:
      //ESP_LOGD(TAG, "IFC %03x 8 %02x %02x %02x %02x %02x %02x %02x %02x", p_frame->MsgID, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
      break;
  }
}

/**
 * Update derived energy metrics while driving
 * Called once per second from Ticker1
 */
void OvmsVehicleSmartEQ::HandleEnergy() {
  float voltage  = StandardMetrics.ms_v_bat_voltage->AsFloat(0, Volts);
  float current  = StandardMetrics.ms_v_bat_current->AsFloat(0, Amps);

  // Power (in kw) resulting from voltage and current
  float power = voltage * current / 1000.0;

  // Are we driving?
  if (power != 0.0 && StandardMetrics.ms_v_env_on->AsBool()) {
    // Update energy used and recovered
    float energy = power / 3600.0;    // 1 second worth of energy in kwh's
    if (energy < 0.0f)
      StandardMetrics.ms_v_bat_energy_used->SetValue( StandardMetrics.ms_v_bat_energy_used->AsFloat() - energy);
    else // (energy > 0.0f)
      StandardMetrics.ms_v_bat_energy_recd->SetValue( StandardMetrics.ms_v_bat_energy_recd->AsFloat() + energy);
  }
}

void OvmsVehicleSmartEQ::HandlePollState() {
  if ( StandardMetrics.ms_v_charge_pilot->AsBool() && m_poll_state != 3 && m_enable_write ) {
    PollSetState(3);
    ESP_LOGI(TAG,"Pollstate Charging");
  }
  else if ( !StandardMetrics.ms_v_charge_pilot->AsBool() && StandardMetrics.ms_v_env_awake->AsBool() && m_poll_state != 2 && m_enable_write ) {
    PollSetState(2);
    ESP_LOGI(TAG,"Pollstate Running");
  }
  else if ( !StandardMetrics.ms_v_charge_pilot->AsBool() && !StandardMetrics.ms_v_env_awake->AsBool() && mt_bus_awake->AsBool() && m_poll_state != 1 && m_enable_write ) {
    PollSetState(1);
    ESP_LOGI(TAG,"Pollstate Awake");
  }
  else if ( !mt_bus_awake->AsBool() && m_poll_state != 0 ) {
    PollSetState(0);
    ESP_LOGI(TAG,"Pollstate Off");
  }
}

void OvmsVehicleSmartEQ::OnlineState() {
#ifdef CONFIG_OVMS_COMP_MAX7317
  if (StandardMetrics.ms_m_net_ip->AsBool()) {
    // connected:
    if (StandardMetrics.ms_s_v2_connected->AsBool()) {
      if (m_led_state != 1) {
        MyPeripherals->m_max7317->Output(9, 1);
        MyPeripherals->m_max7317->Output(8, 0);
        MyPeripherals->m_max7317->Output(7, 1);
        m_led_state = 1;
        ESP_LOGI(TAG,"LED GREEN");
      }
    } else if (StandardMetrics.ms_m_net_connected->AsBool()) {
      if (m_led_state != 2) {
        MyPeripherals->m_max7317->Output(9, 1);
        MyPeripherals->m_max7317->Output(8, 1);
        MyPeripherals->m_max7317->Output(7, 0);
        m_led_state = 2;
        ESP_LOGI(TAG,"LED BLUE");
      }
    } else {
      if (m_led_state != 3) {
        MyPeripherals->m_max7317->Output(9, 0);
        MyPeripherals->m_max7317->Output(8, 1);
        MyPeripherals->m_max7317->Output(7, 1);
        m_led_state = 3;
        ESP_LOGI(TAG,"LED RED");
      }
    }
  }
  else if (m_led_state != 0) {
    // not connected:
    MyPeripherals->m_max7317->Output(9, 1);
    MyPeripherals->m_max7317->Output(8, 1);
    MyPeripherals->m_max7317->Output(7, 1);
    m_led_state = 0;
    ESP_LOGI(TAG,"LED Off");
  }
#endif
}

void OvmsVehicleSmartEQ::Ticker1(uint32_t ticker) {
  if (m_candata_timer > 0) {
    if (--m_candata_timer == 0) {
      // Car has gone to sleep
      ESP_LOGI(TAG,"Car has gone to sleep (CAN bus timeout)");
      mt_bus_awake->SetValue(false);
      m_candata_poll = 0;
      // PollSetState(0);
    }
  }

  if (m_booster_start && StandardMetrics.ms_v_env_hvac->AsBool()) {
    m_booster_start = false;
    MyNotify.NotifyString("info", "hvac.enabled", "Booster on");
  }
  if (m_enable_LED_state) OnlineState();
}

/**
 * PollerStateTicker: check for state changes
 *  This is called by VehicleTicker1() just before the next PollerSend().
 */
void OvmsVehicleSmartEQ::PollerStateTicker(canbus *bus) {
  bool car_online = mt_bus_awake->AsBool();
  int lv_pwrstate = mt_evc_LV_DCDC_amps->AsInt();
  
  // - base system is awake if we've got a fresh lv_pwrstate:
  StandardMetrics.ms_v_env_aux12v->SetValue(car_online);

  // - charging / trickle charging 12V battery is active when lv_pwrstate is not zero:
  StandardMetrics.ms_v_env_charging12v->SetValue(car_online && lv_pwrstate > 0);
  
  HandlePollState();
}

// can can1 tx st 634 40 01 72 00
OvmsVehicle::vehicle_command_t OvmsVehicleSmartEQ::CommandClimateControl(bool enable) {
  if(!m_enable_write) {
    ESP_LOGE(TAG, "CommandClimateControl failed / no write access");
    return Fail;
  }
  ESP_LOGI(TAG, "CommandClimateControl %s", enable ? "ON" : "OFF");

  OvmsVehicle::vehicle_command_t res;

  if (enable) {
    uint8_t data[4] = {0x40, 0x01, 0x00, 0x00};
    canbus *obd;
    obd = m_can1;

    res = CommandWakeup();
    if (res == Success) {
      vTaskDelay(2000 / portTICK_PERIOD_MS);
      for (int i = 0; i < 10; i++) {
        obd->WriteStandard(0x634, 4, data);
        vTaskDelay(100 / portTICK_PERIOD_MS);
      }
      m_booster_start = true;
      res = Success;
    } else {
      res = Fail;
    }
  } else {
    res = NotImplemented;
  }

  // fallback to default implementation?
  if (res == NotImplemented) {
    res = OvmsVehicle::CommandClimateControl(enable);
  }
  return res;
}

OvmsVehicle::vehicle_command_t OvmsVehicleSmartEQ::CommandHomelink(int button, int durationms) {
  // This is needed to enable climate control via Homelink for the iOS app
  ESP_LOGI(TAG, "CommandHomelink button=%d durationms=%d", button, durationms);
  
  OvmsVehicle::vehicle_command_t res = NotImplemented;
  if (button == 0) {
    res = CommandClimateControl(true);
  }
  else if (button == 1) {
    res = CommandClimateControl(false);
  }

  // fallback to default implementation?
  if (res == NotImplemented) {
    res = OvmsVehicle::CommandHomelink(button, durationms);
  }
  return res;
}

OvmsVehicle::vehicle_command_t OvmsVehicleSmartEQ::CommandWakeup() {
  if(!m_enable_write) {
    ESP_LOGE(TAG, "CommandWakeup failed: no write access!");
    return Fail;
  }

  OvmsVehicle::vehicle_command_t res;

  ESP_LOGI(TAG, "Send Wakeup Command");
  res = Fail;
  if(!mt_bus_awake->AsBool()) {
    uint8_t data[4] = {0x40, 0x00, 0x00, 0x00};
    canbus *obd;
    obd = m_can1;

    for (int i = 0; i < 20; i++) {
      obd->WriteStandard(0x634, 4, data);
      vTaskDelay(200 / portTICK_PERIOD_MS);
      if (mt_bus_awake->AsBool()) {
        res = Success;
        ESP_LOGI(TAG, "Vehicle is now awake");
        break;
      }
    }
  } else {
    res = Success;
    ESP_LOGI(TAG, "Vehicle is awake");
  }

  return res;
}

OvmsVehicle::vehicle_command_t OvmsVehicleSmartEQ::CommandStat(int verbosity, OvmsWriter* writer) {

  bool chargeport_open = StdMetrics.ms_v_door_chargeport->AsBool();
  std::string charge_state = StdMetrics.ms_v_charge_state->AsString();
  if (chargeport_open && charge_state != "")
    {
    std::string charge_mode = StdMetrics.ms_v_charge_mode->AsString();
    bool show_details = !(charge_state == "done" || charge_state == "stopped");

    // Translate mode codes:
    if (charge_mode == "standard")
      charge_mode = "Standard";
    else if (charge_mode == "storage")
      charge_mode = "Storage";
    else if (charge_mode == "range")
      charge_mode = "Range";
    else if (charge_mode == "performance")
      charge_mode = "Performance";

    // Translate state codes:
    if (charge_state == "charging")
      charge_state = "Charging";
    else if (charge_state == "topoff")
      charge_state = "Topping off";
    else if (charge_state == "done")
      charge_state = "Charge Done";
    else if (charge_state == "preparing")
      charge_state = "Preparing";
    else if (charge_state == "heating")
      charge_state = "Charging, Heating";
    else if (charge_state == "stopped")
      charge_state = "Charge Stopped";
    else if (charge_state == "timerwait")
      charge_state = "Charge Stopped, Timer On";

    if (charge_mode != "")
      writer->printf("%s - ", charge_mode.c_str());
    writer->printf("%s\n", charge_state.c_str());

    if (show_details)
      {
      // Voltage & current:
      bool show_vc = (StdMetrics.ms_v_charge_voltage->AsFloat() > 0 || StdMetrics.ms_v_charge_current->AsFloat() > 0);
      if (show_vc)
        {
        writer->printf("%s/%s ",
          (char*) StdMetrics.ms_v_charge_voltage->AsUnitString("-", Native, 1).c_str(),
          (char*) StdMetrics.ms_v_charge_current->AsUnitString("-", Native, 1).c_str());
        }

      // Charge speed:
      if (StdMetrics.ms_v_bat_range_speed->IsDefined() && StdMetrics.ms_v_bat_range_speed->AsFloat() != 0)
        {
        writer->printf("%s\n", StdMetrics.ms_v_bat_range_speed->AsUnitString("-", ToUser, 1).c_str());
        }
      else if (show_vc)
        {
        writer->puts("");
        }

      // Estimated time(s) remaining:
      int duration_full = StdMetrics.ms_v_charge_duration_full->AsInt();
      if (duration_full > 0)
        writer->printf("Full: %d:%02dh\n", duration_full / 60, duration_full % 60);

      int duration_soc = StdMetrics.ms_v_charge_duration_soc->AsInt();
      if (duration_soc > 0)
        writer->printf("%s: %d:%02dh\n",
          (char*) StdMetrics.ms_v_charge_limit_soc->AsUnitString("SOC", ToUser, 0).c_str(),
          duration_soc / 60, duration_soc % 60);

      int duration_range = StdMetrics.ms_v_charge_duration_range->AsInt();
      if (duration_range > 0)
        writer->printf("%s: %d:%02dh\n",
          (char*) StdMetrics.ms_v_charge_limit_range->AsUnitString("Range", ToUser, 0).c_str(),
          duration_range / 60, duration_range % 60);
      }

    // Energy sums:
    if (StdMetrics.ms_v_charge_kwh_grid->IsDefined())
      {
      writer->printf("Drawn: %s\n",
        StdMetrics.ms_v_charge_kwh_grid->AsUnitString("-", ToUser, 1).c_str());
      }
    if (StdMetrics.ms_v_charge_kwh->IsDefined())
      {
      writer->printf("Charged: %s\n",
        StdMetrics.ms_v_charge_kwh->AsUnitString("-", ToUser, 1).c_str());
      }
    }
  else
    {
    writer->puts("Not charging");
    }

  writer->printf("SOC: %s\n", (char*) StdMetrics.ms_v_bat_soc->AsUnitString("-", ToUser, 1).c_str());

  if (StdMetrics.ms_v_bat_range_ideal->IsDefined())
    {
    const std::string& range_ideal = StdMetrics.ms_v_bat_range_ideal->AsUnitString("-", ToUser, 0);
    writer->printf("Ideal range: %s\n", range_ideal.c_str());
    }

  if (StdMetrics.ms_v_bat_range_est->IsDefined())
    {
    const std::string& range_est = StdMetrics.ms_v_bat_range_est->AsUnitString("-", ToUser, 0);
    writer->printf("Est. range: %s\n", range_est.c_str());
    }

  if (StdMetrics.ms_v_pos_odometer->IsDefined())
    {
    const std::string& odometer = StdMetrics.ms_v_pos_odometer->AsUnitString("-", ToUser, 1);
    writer->printf("ODO: %s\n", odometer.c_str());
    }

  if (StdMetrics.ms_v_bat_cac->IsDefined())
    {
    const std::string& cac = StdMetrics.ms_v_bat_cac->AsUnitString("-", ToUser, 1);
    writer->printf("CAC: %s\n", cac.c_str());
    }

  if (StdMetrics.ms_v_bat_soh->IsDefined())
    {
    const std::string& soh = StdMetrics.ms_v_bat_soh->AsUnitString("-", ToUser, 0);
    writer->printf("SOH: %s\n", soh.c_str());
    }

  if (mt_evc_hv_energy->IsDefined())
    {
    const std::string& hv_energy = mt_evc_hv_energy->AsUnitString("-", ToUser, 3);
    writer->printf("usable Energy: %s\n", hv_energy.c_str());
    }

  return Success;
}

/**
 * SetFeature: V2 compatibility config wrapper
 *  Note: V2 only supported integer values, V3 values may be text
 */
bool OvmsVehicleSmartEQ::SetFeature(int key, const char *value)
{
  switch (key)
  {
    case 10:
      MyConfig.SetParamValue("xsq", "suffsoc", value);
      return true;
    case 11:
      MyConfig.SetParamValue("xsq", "suffrange", value);
      return true;
    case 15:
    {
      int bits = atoi(value);
      MyConfig.SetParamValueBool("xsq", "canwrite",  (bits& 1)!=0);
      return true;
    }
    default:
      return OvmsVehicle::SetFeature(key, value);
  }
}

/**
 * GetFeature: V2 compatibility config wrapper
 *  Note: V2 only supported integer values, V3 values may be text
 */
const std::string OvmsVehicleSmartEQ::GetFeature(int key)
{
  switch (key)
  {
    case 10:
      return MyConfig.GetParamValue("xsq", "suffsoc", STR(0));
    case 11:
      return MyConfig.GetParamValue("xsq", "suffrange", STR(0));
    case 15:
    {
      int bits = ( MyConfig.GetParamValueBool("xsq", "canwrite",  false) ?  1 : 0);
      char buf[4];
      sprintf(buf, "%d", bits);
      return std::string(buf);
    }
    default:
      return OvmsVehicle::GetFeature(key);
  }
}

class OvmsVehicleSmartEQInit {
  public:
  OvmsVehicleSmartEQInit();
} MyOvmsVehicleSmartEQInit __attribute__ ((init_priority (9000)));

OvmsVehicleSmartEQInit::OvmsVehicleSmartEQInit() {
  ESP_LOGI(TAG, "Registering Vehicle: SMART EQ (9000)");
  MyVehicleFactory.RegisterVehicle<OvmsVehicleSmartEQ>("SQ", "Smart ED/EQ 4.Gen");
}

