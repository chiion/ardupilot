#include "Copter.h"

#if MODE_GUIDED_ENABLED == ENABLED

/*
 * Init and run calls for guided flight mode
 */

#ifndef GUIDED_LOOK_AT_TARGET_MIN_DISTANCE_CM
 # define GUIDED_LOOK_AT_TARGET_MIN_DISTANCE_CM     500     // point nose at target if it is more than 5m away
#endif

#define GUIDED_POSVEL_TIMEOUT_MS    3000    // guided mode's position-velocity controller times out after 3seconds with no new updates
#define GUIDED_ATTITUDE_TIMEOUT_MS  1000    // guided mode's attitude controller times out after 1 second with no new updates

static Vector3f guided_pos_target_cm;       // position target (used by posvel controller only)
static Vector3f guided_vel_target_cms;      // velocity target (used by velocity controller and posvel controller)
static uint32_t posvel_update_time_ms;      // system time of last target update to posvel controller (i.e. position and velocity update)
static uint32_t vel_update_time_ms;         // system time of last target update to velocity controller

struct {
    uint32_t update_time_ms;
    float roll_cd;
    float pitch_cd;
    float yaw_cd;
    float yaw_rate_cds;
    float climb_rate_cms;   // climb rate in cms.  Used if use_thrust is false
    float thrust;           // thrust from -1 to 1.  Used if use_thrust is true
    bool use_yaw_rate;
    bool use_thrust;
} static guided_angle_state;

struct Guided_Limit {
    uint32_t timeout_ms;  // timeout (in seconds) from the time that guided is invoked
    float alt_min_cm;   // lower altitude limit in cm above home (0 = no limit)
    float alt_max_cm;   // upper altitude limit in cm above home (0 = no limit)
    float horiz_max_cm; // horizontal position limit in cm from where guided mode was initiated (0 = no limit)
    uint32_t start_time;// system time in milliseconds that control was handed to the external computer
    Vector3f start_pos; // start position as a distance from home in cm.  used for checking horiz_max limit
} guided_limit;

// guided_init - initialise guided controller
bool ModeGuided::init(bool ignore_checks)
{
    // start in position control mode
    pos_control_start();
    return true;
}

// guided_run - runs the guided controller
// should be called at 100hz or more
void ModeGuided::run()
{
    // call the correct auto controller
    switch (guided_mode) {

    case Guided_TakeOff:
        // run takeoff controller
        takeoff_run();
        break;

    case Guided_WP:
    case Guided_CircleMoveToEdge:
        // run position controller
        pos_control_run();
        break;

    case Guided_Velocity:
        // run velocity controller
        vel_control_run();
        break;

    case Guided_PosVel:
        // run position-velocity controller
        posvel_control_run();
        break;

    case Guided_Angle:
        // run angle controller
        angle_control_run();
        break;

    case Guided_Circle:
        // run circle controller
        circle_run();
        break;
    }
 }

bool ModeGuided::allows_arming(bool from_gcs) const
{
    // always allow arming from the ground station
    if (from_gcs) {
        return true;
    }

    // optionally allow arming from the transmitter
    return (copter.g2.guided_options & (uint32_t)Options::AllowArmingFromTX) != 0;
};

// do_user_takeoff_start - initialises waypoint controller to implement take-off
bool ModeGuided::do_user_takeoff_start(float takeoff_alt_cm)
{
    guided_mode = Guided_TakeOff;

    // initialise wpnav destination
    Location target_loc = copter.current_loc;
    Location::AltFrame frame = Location::AltFrame::ABOVE_HOME;
    if (wp_nav->rangefinder_used_and_healthy() &&
            wp_nav->get_terrain_source() == AC_WPNav::TerrainSource::TERRAIN_FROM_RANGEFINDER &&
            takeoff_alt_cm < copter.rangefinder.max_distance_cm_orient(ROTATION_PITCH_270)) {
        // can't takeoff downwards
        if (takeoff_alt_cm <= copter.rangefinder_state.alt_cm) {
            return false;
        }
        frame = Location::AltFrame::ABOVE_TERRAIN;
    }
    target_loc.set_alt_cm(takeoff_alt_cm, frame);

    if (!wp_nav->set_wp_destination(target_loc)) {
        // failure to set destination can only be because of missing terrain data
        AP::logger().Write_Error(LogErrorSubsystem::NAVIGATION, LogErrorCode::FAILED_TO_SET_DESTINATION);
        // failure is propagated to GCS with NAK
        return false;
    }

    // initialise yaw
    auto_yaw.set_mode(AUTO_YAW_HOLD);

    // clear i term when we're taking off
    set_throttle_takeoff();

    // get initial alt for WP_NAVALT_MIN
    auto_takeoff_set_start_alt();

    return true;
}

// initialise guided mode's position controller
void ModeGuided::pos_control_start()
{
    // set to position control mode
    guided_mode = Guided_WP;

    // initialise waypoint and spline controller
    wp_nav->wp_and_spline_init();

    // initialise wpnav to stopping point
    Vector3f stopping_point;
    wp_nav->get_wp_stopping_point(stopping_point);

    // no need to check return status because terrain data is not used
    wp_nav->set_wp_destination(stopping_point, false);

    // initialise yaw
    auto_yaw.set_mode_to_default(false);
}

// initialise guided mode's velocity controller
void ModeGuided::vel_control_start()
{
    // set guided_mode to velocity controller
    guided_mode = Guided_Velocity;

    // initialise horizontal speed, acceleration
    pos_control->set_max_speed_xy(wp_nav->get_default_speed_xy());
    pos_control->set_max_accel_xy(wp_nav->get_wp_acceleration());

    // initialize vertical speeds and acceleration
    pos_control->set_max_speed_z(-get_pilot_speed_dn(), g.pilot_speed_up);
    pos_control->set_max_accel_z(g.pilot_accel_z);

    // initialise velocity controller
    pos_control->init_vel_controller_xyz();
}

// initialise guided mode's posvel controller
void ModeGuided::posvel_control_start()
{
    // set guided_mode to velocity controller
    guided_mode = Guided_PosVel;

    pos_control->init_xy_controller();

    // set speed and acceleration from wpnav's speed and acceleration
    pos_control->set_max_speed_xy(wp_nav->get_default_speed_xy());
    pos_control->set_max_accel_xy(wp_nav->get_wp_acceleration());

    const Vector3f& curr_pos = inertial_nav.get_position();
    const Vector3f& curr_vel = inertial_nav.get_velocity();

    // set target position and velocity to current position and velocity
    pos_control->set_xy_target(curr_pos.x, curr_pos.y);
    pos_control->set_desired_velocity_xy(curr_vel.x, curr_vel.y);

    // set vertical speed and acceleration
    pos_control->set_max_speed_z(wp_nav->get_default_speed_down(), wp_nav->get_default_speed_up());
    pos_control->set_max_accel_z(wp_nav->get_accel_z());

    // pilot always controls yaw
    auto_yaw.set_mode(AUTO_YAW_HOLD);
}

bool ModeGuided::is_taking_off() const
{
    return guided_mode == Guided_TakeOff;
}

// initialise guided mode's angle controller
void ModeGuided::angle_control_start()
{
    // set guided_mode to velocity controller
    guided_mode = Guided_Angle;

    // set vertical speed and acceleration
    pos_control->set_max_speed_z(wp_nav->get_default_speed_down(), wp_nav->get_default_speed_up());
    pos_control->set_max_accel_z(wp_nav->get_accel_z());

    // initialise position and desired velocity
    if (!pos_control->is_active_z()) {
        pos_control->set_alt_target_to_current_alt();
        pos_control->set_desired_velocity_z(inertial_nav.get_velocity_z());
    }

    // initialise targets
    guided_angle_state.update_time_ms = millis();
    guided_angle_state.roll_cd = ahrs.roll_sensor;
    guided_angle_state.pitch_cd = ahrs.pitch_sensor;
    guided_angle_state.yaw_cd = ahrs.yaw_sensor;
    guided_angle_state.climb_rate_cms = 0.0f;
    guided_angle_state.yaw_rate_cds = 0.0f;
    guided_angle_state.use_yaw_rate = false;

    // pilot always controls yaw
    auto_yaw.set_mode(AUTO_YAW_HOLD);
}

// guided_set_destination - sets guided mode's target destination
// Returns true if the fence is enabled and guided waypoint is within the fence
// else return false if the waypoint is outside the fence
bool ModeGuided::set_destination(const Vector3f& destination, bool use_yaw, float yaw_cd, bool use_yaw_rate, float yaw_rate_cds, bool relative_yaw, bool terrain_alt)
{
#if AC_FENCE == ENABLED
    // reject destination if outside the fence
    const Location dest_loc(destination);
    if (!copter.fence.check_destination_within_fence(dest_loc)) {
        AP::logger().Write_Error(LogErrorSubsystem::NAVIGATION, LogErrorCode::DEST_OUTSIDE_FENCE);
        // failure is propagated to GCS with NAK
        return false;
    }
#endif

    // ensure we are in position control mode
    if (guided_mode != Guided_WP) {
        pos_control_start();
    }

    // set yaw state
    set_yaw_state(use_yaw, yaw_cd, use_yaw_rate, yaw_rate_cds, relative_yaw);

    // no need to check return status because terrain data is not used
    wp_nav->set_wp_destination(destination, terrain_alt);

    // log target
    copter.Log_Write_GuidedTarget(guided_mode, destination, Vector3f());
    return true;
}

bool ModeGuided::get_wp(Location& destination)
{
    if (guided_mode != Guided_WP) {
        return false;
    }
    return wp_nav->get_oa_wp_destination(destination);
}

// sets guided mode's target from a Location object
// returns false if destination could not be set (probably caused by missing terrain data)
// or if the fence is enabled and guided waypoint is outside the fence
bool ModeGuided::set_destination(const Location& dest_loc, bool use_yaw, float yaw_cd, bool use_yaw_rate, float yaw_rate_cds, bool relative_yaw)
{
#if AC_FENCE == ENABLED
    // reject destination outside the fence.
    // Note: there is a danger that a target specified as a terrain altitude might not be checked if the conversion to alt-above-home fails
    if (!copter.fence.check_destination_within_fence(dest_loc)) {
        AP::logger().Write_Error(LogErrorSubsystem::NAVIGATION, LogErrorCode::DEST_OUTSIDE_FENCE);
        // failure is propagated to GCS with NAK
        return false;
    }
#endif

    // ensure we are in position control mode
    if (guided_mode != Guided_WP) {
        pos_control_start();
    }

    if (!wp_nav->set_wp_destination(dest_loc)) {
        // failure to set destination can only be because of missing terrain data
        AP::logger().Write_Error(LogErrorSubsystem::NAVIGATION, LogErrorCode::FAILED_TO_SET_DESTINATION);
        // failure is propagated to GCS with NAK
        return false;
    }

    // set yaw state
    set_yaw_state(use_yaw, yaw_cd, use_yaw_rate, yaw_rate_cds, relative_yaw);

    // log target
    copter.Log_Write_GuidedTarget(guided_mode, Vector3f(dest_loc.lat, dest_loc.lng, dest_loc.alt),Vector3f());
    return true;
}

// guided_set_velocity - sets guided mode's target velocity
void ModeGuided::set_velocity(const Vector3f& velocity, bool use_yaw, float yaw_cd, bool use_yaw_rate, float yaw_rate_cds, bool relative_yaw, bool log_request)
{
    // check we are in velocity control mode
    if (guided_mode != Guided_Velocity) {
        vel_control_start();
    }

    // set yaw state
    set_yaw_state(use_yaw, yaw_cd, use_yaw_rate, yaw_rate_cds, relative_yaw);

    // record velocity target
    guided_vel_target_cms = velocity;
    vel_update_time_ms = millis();

    // log target
    if (log_request) {
        copter.Log_Write_GuidedTarget(guided_mode, Vector3f(), velocity);
    }
}

// set guided mode posvel target
bool ModeGuided::set_destination_posvel(const Vector3f& destination, const Vector3f& velocity, bool use_yaw, float yaw_cd, bool use_yaw_rate, float yaw_rate_cds, bool relative_yaw)
{
#if AC_FENCE == ENABLED
    // reject destination if outside the fence
    const Location dest_loc(destination);
    if (!copter.fence.check_destination_within_fence(dest_loc)) {
        AP::logger().Write_Error(LogErrorSubsystem::NAVIGATION, LogErrorCode::DEST_OUTSIDE_FENCE);
        // failure is propagated to GCS with NAK
        return false;
    }
#endif

    // check we are in velocity control mode
    if (guided_mode != Guided_PosVel) {
        posvel_control_start();
    }

    // set yaw state
    set_yaw_state(use_yaw, yaw_cd, use_yaw_rate, yaw_rate_cds, relative_yaw);

    posvel_update_time_ms = millis();
    guided_pos_target_cm = destination;
    guided_vel_target_cms = velocity;

    copter.pos_control->set_pos_target(guided_pos_target_cm);

    // log target
    copter.Log_Write_GuidedTarget(guided_mode, destination, velocity);
    return true;
}

// set guided mode angle target and climbrate
void ModeGuided::set_angle(const Quaternion &q, float climb_rate_cms_or_thrust, bool use_yaw_rate, float yaw_rate_rads, bool use_thrust)
{
    // check we are in velocity control mode
    if (guided_mode != Guided_Angle) {
        angle_control_start();
    }

    // convert quaternion to euler angles
    q.to_euler(guided_angle_state.roll_cd, guided_angle_state.pitch_cd, guided_angle_state.yaw_cd);
    guided_angle_state.roll_cd = ToDeg(guided_angle_state.roll_cd) * 100.0f;
    guided_angle_state.pitch_cd = ToDeg(guided_angle_state.pitch_cd) * 100.0f;
    guided_angle_state.yaw_cd = wrap_180_cd(ToDeg(guided_angle_state.yaw_cd) * 100.0f);
    guided_angle_state.yaw_rate_cds = ToDeg(yaw_rate_rads) * 100.0f;
    guided_angle_state.use_yaw_rate = use_yaw_rate;

    guided_angle_state.use_thrust = use_thrust;
    if (use_thrust) {
        guided_angle_state.thrust = climb_rate_cms_or_thrust;
        guided_angle_state.climb_rate_cms = 0.0f;
    } else {
        guided_angle_state.thrust = 0.0f;
        guided_angle_state.climb_rate_cms = climb_rate_cms_or_thrust;
    }

    guided_angle_state.update_time_ms = millis();

    // log target
    copter.Log_Write_GuidedTarget(guided_mode,
                           Vector3f(guided_angle_state.roll_cd, guided_angle_state.pitch_cd, guided_angle_state.yaw_cd),
                           Vector3f(0.0f, 0.0f, climb_rate_cms_or_thrust));
}

// guided_takeoff_run - takeoff in guided mode
//      called by guided_run at 100hz or more
void ModeGuided::takeoff_run()
{
    auto_takeoff_run();
    if (wp_nav->reached_wp_destination()) {
        // optionally retract landing gear
        copter.landinggear.retract_after_takeoff();

        // switch to position control mode but maintain current target
        const Vector3f target = wp_nav->get_wp_destination();
        set_destination(target, false, 0, false, 0, false, wp_nav->origin_and_destination_are_terrain_alt());
    }
}

// guided_pos_control_run - runs the guided position controller
// called from guided_run
void ModeGuided::pos_control_run()
{
    // process pilot's yaw input
    float target_yaw_rate = 0;
    if (!copter.failsafe.radio && use_pilot_yaw()) {
        // get pilot's desired yaw rate
        target_yaw_rate = get_pilot_desired_yaw_rate(channel_yaw->get_control_in());
        if (!is_zero(target_yaw_rate)) {
            auto_yaw.set_mode(AUTO_YAW_HOLD);
        }
    }

    // if not armed set throttle to zero and exit immediately
    if (is_disarmed_or_landed()) {
        make_safe_spool_down();
        return;
    }

    // set motors to full range
    motors->set_desired_spool_state(AP_Motors::DesiredSpoolState::THROTTLE_UNLIMITED);

    // run waypoint controller
    copter.failsafe_terrain_set_status(wp_nav->update_wpnav());

    // call z-axis position controller (wpnav should have already updated it's alt target)
    pos_control->update_z_controller();

    // call attitude controller
    if (auto_yaw.mode() == AUTO_YAW_HOLD) {
        // roll & pitch from waypoint controller, yaw rate from pilot
        attitude_control->input_euler_angle_roll_pitch_euler_rate_yaw(wp_nav->get_roll(), wp_nav->get_pitch(), target_yaw_rate);
    } else if (auto_yaw.mode() == AUTO_YAW_RATE) {
        // roll & pitch from waypoint controller, yaw rate from mavlink command or mission item
        attitude_control->input_euler_angle_roll_pitch_euler_rate_yaw(wp_nav->get_roll(), wp_nav->get_pitch(), auto_yaw.rate_cds());
    } else {
        // roll, pitch from waypoint controller, yaw heading from GCS or auto_heading()
        attitude_control->input_euler_angle_roll_pitch_yaw(wp_nav->get_roll(), wp_nav->get_pitch(), auto_yaw.yaw(), true);
    }
}

// guided_vel_control_run - runs the guided velocity controller
// called from guided_run
void ModeGuided::vel_control_run()
{
    // process pilot's yaw input
    float target_yaw_rate = 0;
    if (!copter.failsafe.radio && use_pilot_yaw()) {
        // get pilot's desired yaw rate
        target_yaw_rate = get_pilot_desired_yaw_rate(channel_yaw->get_control_in());
        if (!is_zero(target_yaw_rate)) {
            auto_yaw.set_mode(AUTO_YAW_HOLD);
        }
    }

    // landed with positive desired climb rate, initiate takeoff
    if (motors->armed() && copter.ap.auto_armed && copter.ap.land_complete && is_positive(guided_vel_target_cms.z)) {
        zero_throttle_and_relax_ac();
        motors->set_desired_spool_state(AP_Motors::DesiredSpoolState::THROTTLE_UNLIMITED);
        if (motors->get_spool_state() == AP_Motors::SpoolState::THROTTLE_UNLIMITED) {
            set_land_complete(false);
            set_throttle_takeoff();
        }
        return;
    }

    // if not armed set throttle to zero and exit immediately
    if (is_disarmed_or_landed()) {
        make_safe_spool_down();
        return;
    }

    // set motors to full range
    motors->set_desired_spool_state(AP_Motors::DesiredSpoolState::THROTTLE_UNLIMITED);

    // set velocity to zero and stop rotating if no updates received for 3 seconds
    uint32_t tnow = millis();
    if (tnow - vel_update_time_ms > GUIDED_POSVEL_TIMEOUT_MS) {
        if (!pos_control->get_desired_velocity().is_zero()) {
            set_desired_velocity_with_accel_and_fence_limits(Vector3f(0.0f, 0.0f, 0.0f));
        }
        if (auto_yaw.mode() == AUTO_YAW_RATE) {
            auto_yaw.set_rate(0.0f);
        }
    } else {
        set_desired_velocity_with_accel_and_fence_limits(guided_vel_target_cms);
    }

    // call velocity controller which includes z axis controller
    pos_control->update_vel_controller_xyz();

    // call attitude controller
    if (auto_yaw.mode() == AUTO_YAW_HOLD) {
        // roll & pitch from waypoint controller, yaw rate from pilot
        attitude_control->input_euler_angle_roll_pitch_euler_rate_yaw(pos_control->get_roll(), pos_control->get_pitch(), target_yaw_rate);
    } else if (auto_yaw.mode() == AUTO_YAW_RATE) {
        // roll & pitch from velocity controller, yaw rate from mavlink command or mission item
        attitude_control->input_euler_angle_roll_pitch_euler_rate_yaw(pos_control->get_roll(), pos_control->get_pitch(), auto_yaw.rate_cds());
    } else {
        // roll, pitch from waypoint controller, yaw heading from GCS or auto_heading()
        attitude_control->input_euler_angle_roll_pitch_yaw(pos_control->get_roll(), pos_control->get_pitch(), auto_yaw.yaw(), true);
    }
}

// guided_posvel_control_run - runs the guided spline controller
// called from guided_run
void ModeGuided::posvel_control_run()
{
    // process pilot's yaw input
    float target_yaw_rate = 0;

    if (!copter.failsafe.radio && use_pilot_yaw()) {
        // get pilot's desired yaw rate
        target_yaw_rate = get_pilot_desired_yaw_rate(channel_yaw->get_control_in());
        if (!is_zero(target_yaw_rate)) {
            auto_yaw.set_mode(AUTO_YAW_HOLD);
        }
    }

    // if not armed set throttle to zero and exit immediately
    if (is_disarmed_or_landed()) {
        make_safe_spool_down();
        return;
    }

    // set motors to full range
    motors->set_desired_spool_state(AP_Motors::DesiredSpoolState::THROTTLE_UNLIMITED);

    // set velocity to zero and stop rotating if no updates received for 3 seconds
    uint32_t tnow = millis();
    if (tnow - posvel_update_time_ms > GUIDED_POSVEL_TIMEOUT_MS) {
        guided_vel_target_cms.zero();
        if (auto_yaw.mode() == AUTO_YAW_RATE) {
            auto_yaw.set_rate(0.0f);
        }
    }

    // calculate dt
    float dt = pos_control->time_since_last_xy_update();

    // sanity check dt
    if (dt >= 0.2f) {
        dt = 0.0f;
    }

    // advance position target using velocity target
    guided_pos_target_cm += guided_vel_target_cms * dt;

    // send position and velocity targets to position controller
    pos_control->set_pos_target(guided_pos_target_cm);
    pos_control->set_desired_velocity_xy(guided_vel_target_cms.x, guided_vel_target_cms.y);

    // run position controllers
    pos_control->update_xy_controller();
    pos_control->update_z_controller();

    // call attitude controller
    if (auto_yaw.mode() == AUTO_YAW_HOLD) {
        // roll & pitch from waypoint controller, yaw rate from pilot
        attitude_control->input_euler_angle_roll_pitch_euler_rate_yaw(pos_control->get_roll(), pos_control->get_pitch(), target_yaw_rate);
    } else if (auto_yaw.mode() == AUTO_YAW_RATE) {
        // roll & pitch from position-velocity controller, yaw rate from mavlink command or mission item
        attitude_control->input_euler_angle_roll_pitch_euler_rate_yaw(pos_control->get_roll(), pos_control->get_pitch(), auto_yaw.rate_cds());
    } else {
        // roll, pitch from waypoint controller, yaw heading from GCS or auto_heading()
        attitude_control->input_euler_angle_roll_pitch_yaw(pos_control->get_roll(), pos_control->get_pitch(), auto_yaw.yaw(), true);
    }
}

// guided_angle_control_run - runs the guided angle controller
// called from guided_run
void ModeGuided::angle_control_run()
{
    // constrain desired lean angles
    float roll_in = guided_angle_state.roll_cd;
    float pitch_in = guided_angle_state.pitch_cd;
    float total_in = norm(roll_in, pitch_in);
    float angle_max = MIN(attitude_control->get_althold_lean_angle_max(), copter.aparm.angle_max);
    if (total_in > angle_max) {
        float ratio = angle_max / total_in;
        roll_in *= ratio;
        pitch_in *= ratio;
    }

    // wrap yaw request
    float yaw_in = wrap_180_cd(guided_angle_state.yaw_cd);
    float yaw_rate_in = wrap_180_cd(guided_angle_state.yaw_rate_cds);

    float climb_rate_cms = 0.0f;
    if (!guided_angle_state.use_thrust) {
        // constrain climb rate
        climb_rate_cms = constrain_float(guided_angle_state.climb_rate_cms, -fabsf(wp_nav->get_default_speed_down()), wp_nav->get_default_speed_up());

        // get avoidance adjusted climb rate
        climb_rate_cms = get_avoidance_adjusted_climbrate(climb_rate_cms);
    }

    // check for timeout - set lean angles and climb rate to zero if no updates received for 3 seconds
    uint32_t tnow = millis();
    if (tnow - guided_angle_state.update_time_ms > GUIDED_ATTITUDE_TIMEOUT_MS) {
        roll_in = 0.0f;
        pitch_in = 0.0f;
        climb_rate_cms = 0.0f;
        yaw_rate_in = 0.0f;
        guided_angle_state.use_thrust = false;
    }

    // interpret positive climb rate or thrust as triggering take-off
    const bool positive_thrust_or_climbrate = is_positive(guided_angle_state.use_thrust ? guided_angle_state.thrust : climb_rate_cms);
    if (motors->armed() && positive_thrust_or_climbrate) {
        copter.set_auto_armed(true);
    }

    // if not armed set throttle to zero and exit immediately
    if (!motors->armed() || !copter.ap.auto_armed || (copter.ap.land_complete && !positive_thrust_or_climbrate)) {
        make_safe_spool_down();
        return;
    }

    // TODO: use get_alt_hold_state
    // landed with positive desired climb rate, takeoff
    if (copter.ap.land_complete && (guided_angle_state.climb_rate_cms > 0.0f)) {
        zero_throttle_and_relax_ac();
        motors->set_desired_spool_state(AP_Motors::DesiredSpoolState::THROTTLE_UNLIMITED);
        if (motors->get_spool_state() == AP_Motors::SpoolState::THROTTLE_UNLIMITED) {
            set_land_complete(false);
            set_throttle_takeoff();
        }
        return;
    }

    // set motors to full range
    motors->set_desired_spool_state(AP_Motors::DesiredSpoolState::THROTTLE_UNLIMITED);

    // call attitude controller
    if (guided_angle_state.use_yaw_rate) {
        attitude_control->input_euler_angle_roll_pitch_euler_rate_yaw(roll_in, pitch_in, yaw_rate_in);
    } else {
        attitude_control->input_euler_angle_roll_pitch_yaw(roll_in, pitch_in, yaw_in, true);
    }

    // call position controller
    if (guided_angle_state.use_thrust) {
        attitude_control->set_throttle_out(guided_angle_state.thrust, true, copter.g.throttle_filt);
    } else {
        pos_control->set_alt_target_from_climb_rate_ff(climb_rate_cms, G_Dt, false);
        pos_control->update_z_controller();
    }
}

// helper function to update position controller's desired velocity while respecting acceleration limits
void ModeGuided::set_desired_velocity_with_accel_and_fence_limits(const Vector3f& vel_des)
{
    // get current desired velocity
    Vector3f curr_vel_des = pos_control->get_desired_velocity();

    // get change in desired velocity
    Vector3f vel_delta = vel_des - curr_vel_des;

    // limit xy change
    float vel_delta_xy = safe_sqrt(sq(vel_delta.x)+sq(vel_delta.y));
    float vel_delta_xy_max = G_Dt * pos_control->get_max_accel_xy();
    float ratio_xy = 1.0f;
    if (!is_zero(vel_delta_xy) && (vel_delta_xy > vel_delta_xy_max)) {
        ratio_xy = vel_delta_xy_max / vel_delta_xy;
    }
    curr_vel_des.x += (vel_delta.x * ratio_xy);
    curr_vel_des.y += (vel_delta.y * ratio_xy);

    // limit z change
    float vel_delta_z_max = G_Dt * pos_control->get_max_accel_z();
    curr_vel_des.z += constrain_float(vel_delta.z, -vel_delta_z_max, vel_delta_z_max);

#if AC_AVOID_ENABLED
    // limit the velocity to prevent fence violations
    copter.avoid.adjust_velocity(pos_control->get_pos_xy_p().kP(), pos_control->get_max_accel_xy(), curr_vel_des, G_Dt);
    // get avoidance adjusted climb rate
    curr_vel_des.z = get_avoidance_adjusted_climbrate(curr_vel_des.z);
#endif

    // update position controller with new target
    pos_control->set_desired_velocity(curr_vel_des);
}

// helper function to set yaw state and targets
void ModeGuided::set_yaw_state(bool use_yaw, float yaw_cd, bool use_yaw_rate, float yaw_rate_cds, bool relative_angle)
{
    if (use_yaw) {
        auto_yaw.set_fixed_yaw(yaw_cd * 0.01f, 0.0f, 0, relative_angle);
    } else if (use_yaw_rate) {
        auto_yaw.set_rate(yaw_rate_cds);
    }
}

// returns true if pilot's yaw input should be used to adjust vehicle's heading
bool ModeGuided::use_pilot_yaw(void) const
{
    return (copter.g2.guided_options.get() & uint32_t(Options::IgnorePilotYaw)) == 0;
}

// Guided Limit code

// guided_limit_clear - clear/turn off guided limits
void ModeGuided::limit_clear()
{
    guided_limit.timeout_ms = 0;
    guided_limit.alt_min_cm = 0.0f;
    guided_limit.alt_max_cm = 0.0f;
    guided_limit.horiz_max_cm = 0.0f;
}

// guided_limit_set - set guided timeout and movement limits
void ModeGuided::limit_set(uint32_t timeout_ms, float alt_min_cm, float alt_max_cm, float horiz_max_cm)
{
    guided_limit.timeout_ms = timeout_ms;
    guided_limit.alt_min_cm = alt_min_cm;
    guided_limit.alt_max_cm = alt_max_cm;
    guided_limit.horiz_max_cm = horiz_max_cm;
}

// guided_limit_init_time_and_pos - initialise guided start time and position as reference for limit checking
//  only called from AUTO mode's auto_nav_guided_start function
void ModeGuided::limit_init_time_and_pos()
{
    // initialise start time
    guided_limit.start_time = AP_HAL::millis();

    // initialise start position from current position
    guided_limit.start_pos = inertial_nav.get_position();
}

// guided_limit_check - returns true if guided mode has breached a limit
//  used when guided is invoked from the NAV_GUIDED_ENABLE mission command
bool ModeGuided::limit_check()
{
    // check if we have passed the timeout
    if ((guided_limit.timeout_ms > 0) && (millis() - guided_limit.start_time >= guided_limit.timeout_ms)) {
        return true;
    }

    // get current location
    const Vector3f& curr_pos = inertial_nav.get_position();

    // check if we have gone below min alt
    if (!is_zero(guided_limit.alt_min_cm) && (curr_pos.z < guided_limit.alt_min_cm)) {
        return true;
    }

    // check if we have gone above max alt
    if (!is_zero(guided_limit.alt_max_cm) && (curr_pos.z > guided_limit.alt_max_cm)) {
        return true;
    }

    // check if we have gone beyond horizontal limit
    if (guided_limit.horiz_max_cm > 0.0f) {
        float horiz_move = get_horizontal_distance_cm(guided_limit.start_pos, curr_pos);
        if (horiz_move > guided_limit.horiz_max_cm) {
            return true;
        }
    }

    // if we got this far we must be within limits
    return false;
}


uint32_t ModeGuided::wp_distance() const
{
    switch(mode()) {
    case Guided_WP:
        return wp_nav->get_wp_distance_to_destination();
        break;
    case Guided_PosVel:
        return pos_control->get_distance_to_target();
        break;
    default:
        return 0;
    }
}

int32_t ModeGuided::wp_bearing() const
{
    switch(mode()) {
    case Guided_WP:
        return wp_nav->get_wp_bearing_to_destination();
        break;
    case Guided_PosVel:
        return pos_control->get_bearing_to_target();
        break;
    default:
        return 0;
    }
}

float ModeGuided::crosstrack_error() const
{
    if (mode() == Guided_WP) {
        return wp_nav->crosstrack_error();
    } else {
        return 0;
    }
}

// guided_circle_run - circle in GUIDED flight mode
//      called by guided_run at 100hz or more
void ModeGuided::circle_run()
{
    // call circle controller
    copter.circle_nav->update();

    // call z-axis position controller
    pos_control->update_z_controller();

    if (auto_yaw.mode() == AUTO_YAW_HOLD) {
        // roll & pitch from waypoint controller, yaw rate from pilot
        attitude_control->input_euler_angle_roll_pitch_yaw(copter.circle_nav->get_roll(), copter.circle_nav->get_pitch(), copter.circle_nav->get_yaw(), true);
    } else {
        // roll, pitch from waypoint controller, yaw heading from auto_heading()
        attitude_control->input_euler_angle_roll_pitch_yaw(copter.circle_nav->get_roll(), copter.circle_nav->get_pitch(), auto_yaw.yaw(), true);
    }
}



Location ModeGuided::loc_from_cmd(const AP_Mission::Mission_Command& cmd) const
{
    Location ret(cmd.content.location);

    // use current lat, lon if zero
    if (ret.lat == 0 && ret.lng == 0) {
        ret.lat = copter.current_loc.lat;
        ret.lng = copter.current_loc.lng;
    }
    // use current altitude if not provided
    if (ret.alt == 0) {
        // set to current altitude but in command's alt frame
        int32_t curr_alt;
        if (copter.current_loc.get_alt_cm(ret.get_alt_frame(),curr_alt)) {
            ret.set_alt_cm(curr_alt, ret.get_alt_frame());
        } else {
            // default to current altitude as alt-above-home
            ret.set_alt_cm(copter.current_loc.alt,
                           copter.current_loc.get_alt_frame());
        }
    }
    return ret;
}



// do_circle - initiate moving in a circle
void ModeGuided::do_circle(const AP_Mission::Mission_Command& cmd)
{
    const Location circle_center = loc_from_cmd(cmd);

    // calculate radius
    uint8_t circle_radius_m = HIGHBYTE(cmd.p1); // circle radius held in high byte of p1

    // move to edge of circle (verify_circle) will ensure we begin circling once we reach the edge
    circle_movetoedge_start(circle_center, circle_radius_m);
}

// update mission
void ModeGuided::run_autopilot()
{
    mission.update();
}

// auto_circle_start - initialises controller to fly a circle in Guided flight mode
//   assumes that circle_nav object has already been initialised with circle center and radius
void ModeGuided::circle_start()
{
    guided_mode = Guided_Circle;

    // initialise circle controller
    copter.circle_nav->init(copter.circle_nav->get_center());

    if (auto_yaw.mode() != AUTO_YAW_ROI) {
        auto_yaw.set_mode(AUTO_YAW_HOLD);
    }
}

// guided_circle_movetoedge_start - initialise waypoint controller to move to edge of a circle with it's center at the specified location
//  we assume the caller has performed all required GPS_ok checks
void ModeGuided::circle_movetoedge_start(const Location &circle_center, float radius_m)
{
    // convert location to vector from ekf origin
    Vector3f circle_center_neu;
    if (!circle_center.get_vector_from_origin_NEU(circle_center_neu)) {
        // default to current position and log error
        circle_center_neu = inertial_nav.get_position();
        AP::logger().Write_Error(LogErrorSubsystem::NAVIGATION, LogErrorCode::FAILED_CIRCLE_INIT);
    }
    copter.circle_nav->set_center(circle_center_neu);

    // set circle radius
    if (!is_zero(radius_m)) {
        copter.circle_nav->set_radius(radius_m * 100.0f);
    }

    // check our distance from edge of circle
    Vector3f circle_edge_neu;
    copter.circle_nav->get_closest_point_on_circle(circle_edge_neu);
    float dist_to_edge = (inertial_nav.get_position() - circle_edge_neu).length();

    // if more than 3m then fly to edge
    if (dist_to_edge > 300.0f) {
        // set the state to move to the edge of the circle
        guided_mode = Guided_CircleMoveToEdge;

        // convert circle_edge_neu to Location
        Location circle_edge(circle_edge_neu);

        // convert altitude to same as command
        circle_edge.set_alt_cm(circle_center.alt, circle_center.get_alt_frame());

        // initialise wpnav to move to edge of circle
        if (!wp_nav->set_wp_destination(circle_edge)) {
            // failure to set destination can only be because of missing terrain data
            copter.failsafe_terrain_on_event();
        }

        // if we are outside the circle, point at the edge, otherwise hold yaw
        const Vector3f &curr_pos = inertial_nav.get_position();
        float dist_to_center = norm(circle_center_neu.x - curr_pos.x, circle_center_neu.y - curr_pos.y);
        // initialise yaw
        // To-Do: reset the yaw only when the previous navigation command is not a WP.  this would allow removing the special check for ROI
        if (auto_yaw.mode() != AUTO_YAW_ROI) {
            if (dist_to_center > copter.circle_nav->get_radius() && dist_to_center > 500) {
                auto_yaw.set_mode_to_default(false);
            } else {
                // vehicle is within circle so hold yaw to avoid spinning as we move to edge of circle
                auto_yaw.set_mode(AUTO_YAW_HOLD);
            }
        }
    } else {
        circle_start();
    }
}

// start_command - this function will be called when the ap_mission lib wishes to start a new command
bool ModeGuided::start_command(const AP_Mission::Mission_Command& cmd)
{
    /*
    // To-Do: logging when new commands start/end
    if (copter.should_log(MASK_LOG_CMD)) {
        copter.logger.Write_Mission_Cmd(mission, cmd);
    }

    switch(cmd.id) {

    ///
    /// navigation commands
    ///
    case MAV_CMD_NAV_LOITER_TURNS:              //18 Loiter N Times
        do_circle(cmd);
        break;

    default:
        // unable to use the command, allow the vehicle to try the next command
        return false;
    }

    */

    // always return success
    return true;
}

// verify_command - callback function called from ap-mission at 10hz or higher when a command is being run
//      we double check that the flight mode is GUIDED to avoid the possibility of ap-mission triggering actions while we're not in GUIDED mode
bool ModeGuided::verify_command(const AP_Mission::Mission_Command& cmd)
{
    if (copter.flightmode != &copter.mode_guided) {
        return false;
    }

    bool cmd_complete = false;

    switch (cmd.id) {
    //
    // navigation commands
    //
    case MAV_CMD_NAV_LOITER_TURNS:
        cmd_complete = verify_circle(cmd);
        break;

    default:
        // error message
        gcs().send_text(MAV_SEVERITY_WARNING,"Skipping invalid cmd #%i",cmd.id);
        // return true if we do not recognize the command so that we move on to the next command
        cmd_complete = true;
        break;
    }


    // send message to GCS
    if (cmd_complete) {
        gcs().send_mission_item_reached_message(cmd.index);
    }

    return cmd_complete;
}

// verify_circle - check if we have circled the point enough
bool ModeGuided::verify_circle(const AP_Mission::Mission_Command& cmd)
{
    // check if we've reached the edge
    if (mode() == Guided_CircleMoveToEdge) {
        if (copter.wp_nav->reached_wp_destination()) {
            Vector3f circle_center;
            if (!cmd.content.location.get_vector_from_origin_NEU(circle_center)) {
                // should never happen
                return true;
            }
            const Vector3f curr_pos = copter.inertial_nav.get_position();
            // set target altitude if not provided
            if (is_zero(circle_center.z)) {
                circle_center.z = curr_pos.z;
            }

            // set lat/lon position if not provided
            if (cmd.content.location.lat == 0 && cmd.content.location.lng == 0) {
                circle_center.x = curr_pos.x;
                circle_center.y = curr_pos.y;
            }

            // start circling
            circle_start();
        }
        return false;
    }

    // check if we have completed circling
    return fabsf(copter.circle_nav->get_angle_total()/M_2PI) >= LOWBYTE(cmd.p1);
}

// exit_mission - function that is called once the mission completes
void ModeGuided::exit_mission()
{
    // play a tone
    AP_Notify::events.mission_complete = 1;
}



#endif
