/**
 * @file flightboard.cpp
 * @brief Controls the output to the flight board.
 * Calculates the correct PWM pulse width to be sent to the PWM drivers
 * for the aileron, elevator and rudder, as well as camera gimbal.
 * Uses wiringPi and a GPIO pin for driving the buzzer with software based PWM.
 */

#include "common.h"
#include "flightboard.h"
#include "flightboard-private.h"
#include "navigation.h"
#include "gps_mav.h"
#include "imu_feed.h"

using picopter::FlightBoard;
using picopter::FlightData;
using picopter::GPS;
using picopter::IMU;
using std::this_thread::sleep_for;
using std::chrono::milliseconds;
using std::chrono::seconds;
using std::chrono::steady_clock;
using std::chrono::duration_cast;

/**
 * Constructor; initiates a connection to the flight computer.
 * @param opts A pointer to options, if any (NULL for defaults)
 * @throws std::invalid_argument if it can't connect to the flight computer.
 */
FlightBoard::FlightBoard(Options *opts)
: m_heartbeat_timeout(HEARTBEAT_TIMEOUT_DEFAULT)
, m_last_heartbeat(999)
, m_currentData{}
, m_shutdown{false}
, m_waypoints_mode{false}
, m_system_id(0)
, m_component_id(0)
, m_flightboard_id(128) //Arbitrary value 0-255
, m_is_auto_mode{false}
, m_handler_table{}
{
    if (opts) {
        opts->SetFamily("FLIGHTBOARD");
        m_heartbeat_timeout = opts->GetInt("HEARTBEAT_TIMEOUT", HEARTBEAT_TIMEOUT_DEFAULT);
    }
    //Some increment past the timeout
    m_last_heartbeat = m_heartbeat_timeout + 10;
    //m_link = new MAVCommsSerial("/dev/ttyAMA0", 115200);
    try {
        m_link = new MAVCommsTCP("127.0.0.1", 5760);
    } catch (std::invalid_argument e) {
        m_link = new MAVCommsSerial("/dev/ttyAMA0", 115200);
    }
    
    m_gps = new GPSMAV(this, opts);
    m_imu = new IMU(this, opts);
    
    //m_link = new MAVCommsSerial("/dev/virtualcom0", 57600);
    m_input_thread = std::thread(&FlightBoard::InputLoop, this);
    m_output_thread = std::thread(&FlightBoard::OutputLoop, this);
}

/** 
 * Constructor. Constructs a new flight board with default settings.
 */
FlightBoard::FlightBoard() : FlightBoard(NULL) {}

/**
 * Destructor. Closes the connection to ServoBlaster.
 */
FlightBoard::~FlightBoard() {
    Stop();
    m_shutdown = true;
    m_input_thread.join();
    m_output_thread.join();
    delete m_gps;
    delete m_imu;
    delete m_link;
}

GPS* FlightBoard::GetGPSInstance() {
    return m_gps;
}

IMU* FlightBoard::GetIMUInstance() {
    return m_imu;
}

void FlightBoard::InputLoop() {
    auto last_heartbeat = steady_clock::now() - seconds(m_heartbeat_timeout);
    mavlink_message_t msg;
    mavlink_heartbeat_t heartbeat;
    
    while (!m_shutdown) {
        if (m_link->ReadMessage(&msg)) {
            switch (msg.msgid) {
                case MAVLINK_MSG_ID_HEARTBEAT: {
                    mavlink_msg_heartbeat_decode(&msg, &heartbeat);
                    
                    //Skip heartbeats that aren't from the copter
                    if (heartbeat.type != MAV_TYPE_GCS) {
                        m_is_auto_mode = static_cast<bool>(heartbeat.custom_mode == GUIDED);
                        LogSimple(LOG_DEBUG, "Heatbeat! Mode: %d, %d, %d, %d", heartbeat.type, heartbeat.base_mode, heartbeat.custom_mode, (int)m_is_auto_mode);
                        
                        if (m_last_heartbeat >= m_heartbeat_timeout) {
                            mavlink_message_t smsg;

                            m_system_id = msg.sysid;
                            m_component_id = msg.compid;
                            Log(LOG_INFO, "Initialisation: sysid: %d, compid: %d",
                                msg.sysid, msg.compid);
                            
                            //10 Hz update rate
                            mavlink_msg_request_data_stream_pack(
                                m_system_id, m_flightboard_id, &smsg,
                                msg.sysid, msg.compid, MAV_DATA_STREAM_ALL, 10, 1);
                            //Log(LOG_DEBUG, "Sending data request");
                            m_link->WriteMessage(&smsg);
                        }
                        last_heartbeat = steady_clock::now();
                    }
                } break;
                case MAVLINK_MSG_ID_SYS_STATUS: {
                    mavlink_sys_status_t status;
                    mavlink_msg_sys_status_decode(&msg, &status);
                    //LogSimple(LOG_DEBUG, "BATTERY: %.2fV, Draw: %.2fA, Remain: %3d%%",
                    //    status.voltage_battery*1e-3,
                    //    status.current_battery*1e-2,
                    //    status.battery_remaining);
                } break;
                //case MAVLINK_MSG_ID_MISSION_ITEM_REACHED: {
                //    Log(LOG_DEBUG, "MID REACHED!!!!!");
                //} break;
                //case MAVLINK_MSG_ID_GLOBAL_POSITION_INT: {
                //    mavlink_global_position_int_t pos;
                //    mavlink_msg_global_position_int_decode(&msg, &pos);
                //    LogSimple(LOG_DEBUG, "Lat: %.2f, Lon: %.2f, RelAlt: %.2f, Brng: %.2f, (%.1f,%.1f,%.1f)",
                //    pos.lat*1e-7, pos.lon*1e-7, pos.relative_alt*1e-3, pos.hdg*1e-2,
                //    pos.vx*1e-2,pos.vy*1e-2,pos.vz*1e-2);
                //} break;
                //case MAVLINK_MSG_ID_GPS_RAW_INT: {
                //    mavlink_gps_raw_int_t gps;
                //    printf("GPS!\n");
                //    mavlink_msg_gps_raw_int_decode(&msg, &gps);
                //    printf("Lat: %.2f, Lon: %.2f, Alt: %.2fm\n",
                //        gps.lat * 1e-7, gps.lon * 1e-7, gps.alt / 1000.0);
                //} break;
                //case MAVLINK_MSG_ID_ATTITUDE: {
                //    mavlink_attitude_t att;
                //    mavlink_msg_attitude_decode(&msg, &att);
                //    m_current_yaw = att.yaw;
                //    LogSimple(LOG_DEBUG, "YAW: %.2f", RAD2DEG(att.yaw));
                //    LogSimple(LOG_DEBUG, "Roll: %.2f, Pitch: %.2f, Yaw: %.2f",
                //        RAD2DEG(att.roll), RAD2DEG(att.pitch), RAD2DEG(att.yaw));
                //} break;
                //case MAVLINK_MSG_ID_PARAM_VALUE: {
                //    mavlink_param_value_t param;
                //    mavlink_msg_param_value_decode(&msg, &param);
                //    LogSimple(LOG_DEBUG, "%.16s", param.param_id);
                //} break;
                //default: {
                //    LogSimple(LOG_DEBUG, "MSGID: %d\n", msg.msgid);
                //} break;
            }
            
            //Call the event handler, if any.
            if (msg.msgid >= 0 && msg.msgid < 255) {
                EventHandler e = m_handler_table[msg.msgid];
                if (e) {
                    e(&msg);
                }
            }
        }
        m_last_heartbeat = duration_cast<seconds>(steady_clock::now()-last_heartbeat).count();
        if (m_is_auto_mode && m_last_heartbeat >= m_heartbeat_timeout) {
            Log(LOG_WARNING, "Heartbeat timeout (%d s); disabling auto mode!",
                m_last_heartbeat);
            m_is_auto_mode = false;
        }
    }
}

void FlightBoard::OutputLoop() {
    mavlink_message_t msg;
    //mavlink_set_attitude_target_t sp = {0};
    //auto now = steady_clock::now();
    mavlink_set_position_target_local_ned_t sp = {0};
    sp.type_mask = MAVLINK_MSG_SET_POSITION_TARGET_LOCAL_NED_VELOCITY;
    sp.coordinate_frame = MAV_FRAME_BODY_OFFSET_NED;
    //mavlink_set_actuator_control_target_t sp = {0};
    mavlink_command_long_t yaw_sp = {0};
    yaw_sp.command = MAV_CMD_CONDITION_YAW;

    while (!m_shutdown) {
        if (!m_waypoints_mode && m_is_auto_mode) {
            std::lock_guard<std::mutex> lock(m_output_mutex);
            //Log(LOG_DEBUG, "SENDING");
            float yaw = m_imu->GetLatestYaw();
            float px = std::cos(yaw);
            float py = std::sin(yaw);
            sp.vx = (8 * (m_currentData.elevator * px - m_currentData.aileron * py)) / 100.0; //Max speed is 8 m/s along one axis
            sp.vy = (8 * (m_currentData.aileron * px + m_currentData.elevator * py)) / 100.0;
            mavlink_msg_set_position_target_local_ned_encode(m_system_id, m_flightboard_id, &msg, &sp);
            //Log(LOG_DEBUG, "px: %.2f, py: %.2f, A: %d, E: %d, R: %d, vx: %.2f, vy: %.2f", px, py, m_currentData.aileron, m_currentData.elevator, m_currentData.rudder, sp.vx, sp.vy);
            m_link->WriteMessage(&msg);
            
            if (m_currentData.rudder != 0) {
                //Log(LOG_DEBUG, "MOVING RUDDER");
                yaw_sp.param1 = 0.5;//std::fabs(m_currentData.rudder * 15/100.0); //Yaw in deg (relative)
                yaw_sp.param2 = std::fabs(m_currentData.rudder * 30/100.0); //Yaw rate in deg/s
                yaw_sp.param3 = m_currentData.rudder < 0 ? -1 : 1; //Yaw direction (CCW or CW)
                yaw_sp.param4 = 1; //Relative
                mavlink_msg_command_long_encode(m_system_id, m_flightboard_id, &msg, &yaw_sp);
                m_link->WriteMessage(&msg);
            }
        } else {
            //Log(LOG_DEBUG, "SLEEPING");
        }
        sleep_for(milliseconds(100));
    }
}

bool FlightBoard::SetGuidedWaypoint(int seq, float radius, float wait, float lat, float lon, float alt, bool relative_alt) {
    mavlink_mission_item_t mi = {0};
    mavlink_message_t msg;
    std::lock_guard<std::mutex> lock(m_output_mutex);
    
    mi.seq = seq;
    mi.frame = MAV_FRAME_GLOBAL_RELATIVE_ALT;
    mi.command = MAV_CMD_NAV_WAYPOINT;
    mi.current = 2; //ArduCopter magic number for 'guided mode' waypoint
    mi.param1 = 0; //Hold time in s; we do this ourselves
    mi.param2 = radius; //Acceptance radius in m
    mi.param3 = 0; //Pass through the waypoint; ignored by ArduPilot
    mi.param4 = 0; //Desired yaw angle on completion; ignored by ArduPilot
    mi.x = lat;
    mi.y = lon;
    mi.z = (relative_alt ? (m_gps->GetLatestRelAlt() + alt) : alt);
    
    if (std::isnan(mi.z) || mi.z <= 0) {
        Log(LOG_WARNING, "Waypoint %d: Invalid altitude (%.2f m)", seq, mi.z);
        return false;        
    }
    
    m_waypoints_mode = true;
    mavlink_msg_mission_item_encode(m_system_id, m_flightboard_id, &msg, &mi);
    m_link->WriteMessage(&msg);
    return true;
}

/**
 * Determines if the system is in auto mode.
 * This means that the Pixhawk is in guided mode. Where we can send commands. 
 * @return true iff in auto mode. 
 */
bool FlightBoard::IsAutoMode() {
    return m_is_auto_mode;
}

/**
 * Stops the hexacopter.
 * Note: Stopping refers to making it hold its current position.
 * Sets all speeds and the gimbal angle to 0.
 */
void FlightBoard::Stop() {
    std::lock_guard<std::mutex> lock(m_output_mutex);
    FlightData fd_zero = {0};
    m_currentData = fd_zero;
    m_waypoints_mode = false;
}

/**
 * Registers an event handler, which will be called when a message with
 * the given message id is received. The current implementation will only allow
 * up to one function to respond to a specific message at any one time. If a
 * callback function already exists for the given msgid, it will be replaced.
 * A more versatile implementation would maintain some sort of list per msgid
 * to allow for more than one function to be called at any one time. Or, if
 * more than one function really must be called, then a callback function that
 * then itself delegates to these functions can be used instead.
 *  
 * @param [in] msgid The message id to respond to.
 * @param [in] handler The event handler to call.
 * @return -1 on error, or the unique handler id. At present this is just equal
 *         to msgid. 
 */
int FlightBoard::RegisterHandler(int msgid, EventHandler handler) {
    if (msgid < 0 || msgid > 255) {
        return -1;
    } else {
        m_handler_table[msgid] = handler;
        return msgid;
    }
}

/**
 * Deregisters a message handler.
 * @param [in] handlerid The unique handler id as returned from RegisterHandler.
 */
void FlightBoard::DeregisterHandler(int handlerid) {
    if (handlerid >= 0 && handlerid <= 255) {
        m_handler_table[handlerid] = nullptr;
    }
}

/**
 * Returns a copy of the current flight data.
 * @param d A pointer to the output location.
 */
void FlightBoard::GetData(FlightData *d) {
    std::lock_guard<std::mutex> lock(m_output_mutex);
    *d = m_currentData;
}

/**
 * Sets the flight data and actuates the hexacopter.
 * @param d Specifies the flight data for how the hexacopter should be actuated.
 */
void FlightBoard::SetData(FlightData *d) {
    std::lock_guard<std::mutex> lock(m_output_mutex);
    m_currentData.aileron = picopter::clamp(d->aileron, -100, 100);
    m_currentData.elevator = picopter::clamp(d->elevator, -100, 100);
    m_currentData.rudder = picopter::clamp(d->rudder, -100, 100);
    m_currentData.gimbal = picopter::clamp(d->gimbal, 0, 90);
}


/**
 * Sets the aileron speed.
 * @param speed The aileron speed, as a percentage (-100% to 100%)
 */
void FlightBoard::SetAileron(int speed) {
    std::lock_guard<std::mutex> lock(m_output_mutex);
    m_currentData.aileron = picopter::clamp(speed, -100, 100);
}

/**
 * Sets the elevator speed.
 * @param speed The elevator speed, as a percentage (-100% to 100%)
 */
void FlightBoard::SetElevator(int speed) {
    std::lock_guard<std::mutex> lock(m_output_mutex);
    //Elevator speed is inverted.
    m_currentData.elevator = picopter::clamp(speed, -100, 100);
}

/**
 * Sets the rudder speed.
 * @param speed The rudder speed, as a percentage (-100% to 100%)
 */
void FlightBoard::SetRudder(int speed) {
    std::lock_guard<std::mutex> lock(m_output_mutex);
    m_currentData.rudder = picopter::clamp(speed, -100, 100);
}

/**
 * Sets the gimbal angle.
 * @param pos The gimbal angle, in degrees (0 to 90)
 */
void FlightBoard::SetGimbal(int pos) {
    std::lock_guard<std::mutex> lock(m_output_mutex);
    m_currentData.gimbal = picopter::clamp(pos, 0, 90);
}