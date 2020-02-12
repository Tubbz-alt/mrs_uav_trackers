#define VERSION "0.0.3.0"

/* includes //{ */

#include <ros/ros.h>

#include <geometry_msgs/PoseStamped.h>
#include <nav_msgs/Odometry.h>
#include <sensor_msgs/Joy.h>

#include <mrs_msgs/ObstacleSectors.h>
#include <mrs_uav_manager/Tracker.h>
#include <mrs_msgs/UavState.h>

#include <tf/transform_datatypes.h>
#include <mutex>

#include <commons.h>

#include <mrs_lib/ParamLoader.h>
#include <mrs_lib/Profiler.h>

#include <mrs_controllers/cvx_wrapper.h>

//}

#define STOP_THR 1e-3

namespace mrs_trackers
{

namespace joy_bumper_tracker
{

/* //{ class JoyBumperTracker */

class JoyBumperTracker : public mrs_uav_manager::Tracker {
public:
  virtual void initialize(const ros::NodeHandle &parent_nh, const std::string uav_name, std::shared_ptr<mrs_uav_manager::CommonHandlers_t> common_handlers);
  virtual bool activate(const mrs_msgs::PositionCommand::ConstPtr &cmd);
  virtual void deactivate(void);
  virtual bool resetStatic(void);

  virtual const mrs_msgs::PositionCommand::ConstPtr update(const mrs_msgs::UavState::ConstPtr &msg, const mrs_msgs::AttitudeCommand::ConstPtr &cmd);
  virtual const mrs_msgs::TrackerStatus             getStatus();
  virtual const std_srvs::SetBoolResponse::ConstPtr enableCallbacks(const std_srvs::SetBoolRequest::ConstPtr &cmd);
  virtual void                                      switchOdometrySource(const mrs_msgs::UavState::ConstPtr &msg);

  virtual const mrs_msgs::ReferenceSrvResponse::ConstPtr goTo(const mrs_msgs::ReferenceSrvRequest::ConstPtr &cmd);
  virtual const mrs_msgs::ReferenceSrvResponse::ConstPtr goToRelative(const mrs_msgs::ReferenceSrvRequest::ConstPtr &cmd);
  virtual const mrs_msgs::Float64SrvResponse::ConstPtr   goToAltitude(const mrs_msgs::Float64SrvRequest::ConstPtr &cmd);
  virtual const mrs_msgs::Float64SrvResponse::ConstPtr   setYaw(const mrs_msgs::Float64SrvRequest::ConstPtr &cmd);
  virtual const mrs_msgs::Float64SrvResponse::ConstPtr   setYawRelative(const mrs_msgs::Float64SrvRequest::ConstPtr &cmd);

  virtual bool goTo(const mrs_msgs::ReferenceConstPtr &msg);

  virtual const mrs_msgs::TrackerConstraintsSrvResponse::ConstPtr setConstraints(const mrs_msgs::TrackerConstraintsSrvRequest::ConstPtr &cmd);

  virtual const std_srvs::TriggerResponse::ConstPtr hover(const std_srvs::TriggerRequest::ConstPtr &cmd);

private:
  std::shared_ptr<mrs_uav_manager::CommonHandlers_t> common_handlers;

  bool callbacks_enabled = true;

private:
  mrs_msgs::UavState uav_state;
  bool               got_uav_state = false;
  std::mutex         mutex_uav_state;

  double uav_x;
  double uav_y;
  double uav_z;
  double uav_yaw;
  double uav_roll;
  double uav_pitch;

private:
  // tracker's inner states
  int    tracker_loop_rate_;
  double tracker_dt_;
  bool   is_initialized = false;
  bool   is_active      = false;
  bool   first_iter     = false;

private:
  void       mainTimer(const ros::TimerEvent &event);
  ros::Timer main_timer;

private:
  ros::Timer bumper_timer;
  void       bumperTimer(const ros::TimerEvent &event);

private:
  // dynamical constraints
  double     yaw_rate_;
  std::mutex mutex_constraints;

private:
  // desired goal
  double     got_goal = false;
  std::mutex mutex_goal;

  // my current state
  double     state_z, state_yaw;
  double     speed_yaw;
  double     current_heading;
  double     current_vertical_speed = 0;
  double     current_yaw_rate       = 0;
  double     desired_pitch          = 0;
  double     desired_roll           = 0;
  double     attitude_coeff         = 1;
  double     current_horizontal_acceleration, current_vertical_acceleration;
  std::mutex mutex_state;

  mrs_msgs::PositionCommand position_output;

private:
  ros::Subscriber subscriber_joystick;
  ros::Subscriber subscriber_bumper;

  void callbackJoystic(const sensor_msgs::Joy &msg);
  void callbackBumper(const mrs_msgs::ObstacleSectorsConstPtr &msg);

  double max_tilt_;
  double vertical_speed_;
  double bumper_horizontal_distance_;
  double bumper_vertical_distance_;
  double bumper_repulsion_horizontal_distance_;
  double bumper_repulsion_vertical_distance_;
  double filter_coeff_;
  bool   got_bumper_         = false;
  bool   filter_initialized_ = false;
  bool   bumper_enabled_     = false;
  int    bumper_timer_rate_;

  mrs_msgs::ObstacleSectors bumper_data_;
  mrs_msgs::ObstacleSectors bumper_previous_data_;

  std::vector<double> diff_filter_;
  std::vector<double> reg_error_;
  std::vector<double> reg_error_diff_;
  std::mutex          mutex_bumper;

  bool bumperPushFromObstacle();
  int  bumperGetSectorId(const double x, const double y, const double z);

private:
  mrs_lib::Profiler profiler;
  bool              profiler_enabled_ = false;

  std::string _version_;
  std::string uav_name_;

  // indices of joystick buttons
  int start_button_idx_;
  int thrust_idx_;
  int yaw_idx_;
  int pitch_idx_;
  int roll_idx_;

  // --------------------------------------------------------------
  // |                       MPC controller                       |
  // --------------------------------------------------------------

private:
  int n;  // number of states

  double dt1, dt2;

  double cvx_x_u = 0;

  int horizon_length_;

  double max_speed_horizontal_, max_acceleration_horizontal_, max_jerk_;

  std::vector<double> Q, S;
  std::vector<double> Q_z, S_z;

  mrs_controllers::cvx_wrapper::CvxWrapper *cvx_x;

  bool cvx_verbose_ = false;
  int  cvx_max_iterations_;
};

//}

// | -------------- tracker's interface routines -------------- |

/* //{ initialize() */

void JoyBumperTracker::initialize(const ros::NodeHandle &parent_nh, [[maybe_unused]] const std::string uav_name,
                                  [[maybe_unused]] std::shared_ptr<mrs_uav_manager::CommonHandlers_t> common_handlers) {

  uav_name_             = uav_name;
  this->common_handlers = common_handlers;

  ros::NodeHandle nh_(parent_nh, "joy_bumper_tracker");

  ros::Time::waitForValid();

  // --------------------------------------------------------------
  // |                       load parameters                      |
  // --------------------------------------------------------------

  mrs_lib::ParamLoader param_loader(nh_, "JoyBumperTracker");

  param_loader.load_param("version", _version_);

  if (_version_ != VERSION) {

    ROS_ERROR("[JoyBumperTracker]: the version of the binary (%s) does not match the config file (%s), please build me!", VERSION, _version_.c_str());
    ros::shutdown();
  }

  param_loader.load_param("enable_profiler", profiler_enabled_);

  param_loader.load_param("vertical_tracker/vertical_speed", vertical_speed_);

  param_loader.load_param("tracker_loop_rate", tracker_loop_rate_);
  param_loader.load_param("bumper_timer_rate", bumper_timer_rate_);
  param_loader.load_param("max_tilt", max_tilt_);
  param_loader.load_param("filter_coefficient", filter_coeff_);
  param_loader.load_param("bumper_enabled", bumper_enabled_);
  param_loader.load_param("bumper_horizontal_repulsion_distance", bumper_repulsion_horizontal_distance_);
  param_loader.load_param("bumper_vertical_repulsion_distance", bumper_repulsion_vertical_distance_);

  param_loader.load_param("yaw_tracker/yaw_rate", yaw_rate_);

  param_loader.load_param("buttons_indices/thrust", thrust_idx_);
  param_loader.load_param("buttons_indices/yaw", yaw_idx_);
  param_loader.load_param("buttons_indices/pitch", pitch_idx_);
  param_loader.load_param("buttons_indices/roll", roll_idx_);
  param_loader.load_param("buttons_indices/start", start_button_idx_);

  // | --------------------- mpc controller --------------------- |

  // load the dynamicall model parameters
  param_loader.load_param("mpc_model/number_of_states", n);
  param_loader.load_param("mpc_model/dt1", dt1);
  param_loader.load_param("mpc_model/dt2", dt2);

  param_loader.load_param("mpc_parameters/horizon_length", horizon_length_);

  param_loader.load_param("mpc_parameters/horizontal/max_speed", max_speed_horizontal_);
  param_loader.load_param("mpc_parameters/horizontal/max_acceleration", max_acceleration_horizontal_);
  param_loader.load_param("mpc_parameters/horizontal/max_jerk", max_jerk_);

  param_loader.load_param("mpc_parameters/horizontal/Q", Q);
  param_loader.load_param("mpc_parameters/horizontal/S", S);

  param_loader.load_param("cvx_parameters/verbose", cvx_verbose_);
  param_loader.load_param("cvx_parameters/max_iterations", cvx_max_iterations_);

  /* //{ check loaded indices */
  if (start_button_idx_ < 0 || start_button_idx_ > 10) {
    ROS_ERROR("[JoyBumperTracker]: Invalid index of start button. Setting default value.");
    start_button_idx_ = 7;
  }
  if (thrust_idx_ < 0 || thrust_idx_ > 7) {
    ROS_ERROR("[JoyBumperTracker]: Invalid index of thrust button. Setting default value.");
    thrust_idx_ = 4;
  }
  if (yaw_idx_ < 0 || yaw_idx_ > 7) {
    ROS_ERROR("[JoyBumperTracker]: Invalid index of yaw button. Setting default value.");
    yaw_idx_ = 0;
  }
  if (pitch_idx_ < 0 || pitch_idx_ > 7) {
    ROS_ERROR("[JoyBumperTracker]: Invalid index of pitch button. Setting default value.");
    pitch_idx_ = 1;
  }
  if (roll_idx_ < 0 || roll_idx_ > 7) {
    ROS_ERROR("[JoyBumperTracker]: Invalid index of roll button. Setting default value.");
    roll_idx_ = 3;
  }
  /*//}*/

  tracker_dt_ = 1.0 / double(tracker_loop_rate_);

  ROS_INFO("[JoyBumperTracker]: tracker_dt: %f", tracker_dt_);

  // --------------------------------------------------------------
  // |                       prepare cvxgen                       |
  // --------------------------------------------------------------

  cvx_x = new mrs_controllers::cvx_wrapper::CvxWrapper(cvx_verbose_, cvx_max_iterations_, Q, S, dt1, dt2, 0, 1.0);

  // --------------------------------------------------------------
  // |                          profiler                          |
  // --------------------------------------------------------------

  profiler = mrs_lib::Profiler(nh_, "joytracker", profiler_enabled_);

  // --------------------------------------------------------------
  // |                         subscribers                        |
  // --------------------------------------------------------------

  subscriber_joystick = nh_.subscribe("joystick_in", 1, &JoyBumperTracker::callbackJoystic, this, ros::TransportHints().tcpNoDelay());
  subscriber_bumper   = nh_.subscribe("bumper_in", 1, &JoyBumperTracker::callbackBumper, this, ros::TransportHints().tcpNoDelay());

  // --------------------------------------------------------------
  // |                           timers                           |
  // --------------------------------------------------------------

  main_timer   = nh_.createTimer(ros::Rate(tracker_loop_rate_), &JoyBumperTracker::mainTimer, this, false, false);
  bumper_timer = nh_.createTimer(ros::Rate(bumper_timer_rate_), &JoyBumperTracker::bumperTimer, this, false, false);

  if (!param_loader.loaded_successfully()) {
    ROS_ERROR("[JoyBumperTracker]: Could not load all parameters!");
    ros::shutdown();
  }

  is_initialized = true;

  ROS_INFO("[JoyBumperTracker]: initialized, version %s", VERSION);
}

//}

/* //{ activate() */

bool JoyBumperTracker::activate(const mrs_msgs::PositionCommand::ConstPtr &cmd) {

  if (!got_uav_state) {

    ROS_ERROR("[JoyBumperTracker]: can't activate(), odometry not set");
    return false;
  }

  if (!got_goal) {

    ROS_ERROR("[JoyBumperTracker]: can't activate(), missing joystic goal");
    return false;
  }

  {
    std::scoped_lock lock(mutex_goal, mutex_state, mutex_uav_state);

    if (mrs_msgs::PositionCommand::Ptr() != cmd) {

      // the last command is usable
      state_z   = cmd->position.z;
      state_yaw = cmd->yaw;

    } else {

      state_z   = uav_state.pose.position.z;
      state_yaw = uav_yaw;

      ROS_WARN("[JoyBumperTracker]: the previous command is not usable for activation, using Odometry instead.");
    }
  }

  // --------------------------------------------------------------
  // |              yaw initial condition  prediction             |
  // --------------------------------------------------------------

  main_timer.start();
  bumper_timer.start();

  is_active = true;

  ROS_INFO("[JoyBumperTracker]: activated");

  return true;
}

//}

/* //{ deactivate() */

void JoyBumperTracker::deactivate(void) {

  main_timer.stop();
  bumper_timer.stop();

  is_active = false;

  ROS_INFO("[JoyBumperTracker]: deactivated");
}

//}

/* //{ resetStatic() */

bool JoyBumperTracker::resetStatic(void) {

  return false;
}

//}

/* //{ update() */

const mrs_msgs::PositionCommand::ConstPtr JoyBumperTracker::update(const mrs_msgs::UavState::ConstPtr &                        msg,
                                                                   [[maybe_unused]] const mrs_msgs::AttitudeCommand::ConstPtr &cmd) {

  mrs_lib::Routine profiler_routine = profiler.createRoutine("update");

  {
    std::scoped_lock lock(mutex_uav_state);

    uav_state = *msg;
    uav_x     = uav_state.pose.position.x;
    uav_y     = uav_state.pose.position.y;
    uav_z     = uav_state.pose.position.z;

    // calculate the euler angles
    tf::Quaternion quaternion_odometry;
    quaternionMsgToTF(uav_state.pose.orientation, quaternion_odometry);
    tf::Matrix3x3 m(quaternion_odometry);
    m.getRPY(uav_roll, uav_pitch, uav_yaw);

    got_uav_state = true;
  }

  // up to this part the update() method is evaluated even when the tracker is not active
  if (!is_active) {
    return mrs_msgs::PositionCommand::Ptr();
  }

  bumperPushFromObstacle();

  position_output.header.stamp    = ros::Time::now();
  position_output.header.frame_id = uav_state.header.frame_id;

  {
    std::scoped_lock lock(mutex_state, mutex_uav_state);

    position_output.position.x = uav_state.pose.position.x;
    position_output.position.y = uav_state.pose.position.y;
    position_output.position.z = state_z;
    position_output.yaw        = state_yaw;
    /* position_output.use_position = 1; */

    position_output.velocity.x              = uav_state.velocity.linear.x;
    position_output.velocity.y              = uav_state.velocity.linear.y;
    position_output.velocity.z              = current_vertical_speed;
    position_output.use_velocity_horizontal = 1;
    position_output.use_velocity_vertical   = 1;
    position_output.yaw_dot                 = current_yaw_rate;

    position_output.acceleration.x = 0;
    position_output.acceleration.y = 0;
    position_output.acceleration.z = 0;

    position_output.use_quat_attitude = 1;

    tf::Quaternion desired_orientation;

    double affine_coef = 0.99;
    if (fabs(uav_state.velocity.linear.x) > 5 || fabs(uav_state.velocity.linear.y) > 5) {
      attitude_coeff = affine_coef * attitude_coeff;
    } else {
      attitude_coeff = affine_coef * attitude_coeff + (1 - affine_coef);
    }

    desired_orientation = tf::createQuaternionFromRPY(-desired_roll * attitude_coeff, desired_pitch * attitude_coeff, state_yaw);

    position_output.attitude.w = desired_orientation.getW();
    position_output.attitude.x = desired_orientation.getX();
    position_output.attitude.y = desired_orientation.getY();
    position_output.attitude.z = desired_orientation.getZ();
  }

  return mrs_msgs::PositionCommand::ConstPtr(new mrs_msgs::PositionCommand(position_output));
}

//}

/* //{ getStatus() */

const mrs_msgs::TrackerStatus JoyBumperTracker::getStatus() {

  mrs_msgs::TrackerStatus tracker_status;

  tracker_status.active            = is_active;
  tracker_status.callbacks_enabled = callbacks_enabled;

  return tracker_status;
}

//}

/* //{ enableCallbacks() */

const std_srvs::SetBoolResponse::ConstPtr JoyBumperTracker::enableCallbacks(const std_srvs::SetBoolRequest::ConstPtr &cmd) {

  char                      message[100];
  std_srvs::SetBoolResponse res;

  if (cmd->data != callbacks_enabled) {

    callbacks_enabled = cmd->data;

    sprintf((char *)&message, "Callbacks %s", callbacks_enabled ? "enabled" : "disabled");

    ROS_INFO("[JoyBumperTracker]: %s", message);

  } else {

    sprintf((char *)&message, "Callbacks were already %s", callbacks_enabled ? "enabled" : "disabled");
  }

  res.message = message;
  res.success = true;

  return std_srvs::SetBoolResponse::ConstPtr(new std_srvs::SetBoolResponse(res));
}

//}

/* switchOdometrySource() //{ */

void JoyBumperTracker::switchOdometrySource([[maybe_unused]] const mrs_msgs::UavState::ConstPtr &msg) {
}

//}

/* //{ hover() service */

const std_srvs::TriggerResponse::ConstPtr JoyBumperTracker::hover([[maybe_unused]] const std_srvs::TriggerRequest::ConstPtr &cmd) {

  return std_srvs::TriggerResponse::Ptr();
}

//}

/* //{ setConstraints() service */

const mrs_msgs::TrackerConstraintsSrvResponse::ConstPtr JoyBumperTracker::setConstraints([
    [maybe_unused]] const mrs_msgs::TrackerConstraintsSrvRequest::ConstPtr &cmd) {

  return mrs_msgs::TrackerConstraintsSrvResponse::Ptr();
}

//}

// | -------------- setpoint topics and services -------------- |

/* //{ goTo() service */

const mrs_msgs::ReferenceSrvResponse::ConstPtr JoyBumperTracker::goTo([[maybe_unused]] const mrs_msgs::ReferenceSrvRequest::ConstPtr &cmd) {

  return mrs_msgs::ReferenceSrvResponse::Ptr();
}

//}

/* //{ goTo() topic */

bool JoyBumperTracker::goTo([[maybe_unused]] const mrs_msgs::ReferenceConstPtr &msg) {

  return false;
}

//}

/* //{ goToRelative() service */

const mrs_msgs::ReferenceSrvResponse::ConstPtr JoyBumperTracker::goToRelative([[maybe_unused]] const mrs_msgs::ReferenceSrvRequest::ConstPtr &cmd) {

  return mrs_msgs::ReferenceSrvResponse::Ptr();
}

//}

/* //{ goToAltitude() service */

const mrs_msgs::Float64SrvResponse::ConstPtr JoyBumperTracker::goToAltitude([[maybe_unused]] const mrs_msgs::Float64SrvRequest::ConstPtr &cmd) {

  return mrs_msgs::Float64SrvResponse::Ptr();
}

//}

/* //{ setYaw() service */

const mrs_msgs::Float64SrvResponse::ConstPtr JoyBumperTracker::setYaw([[maybe_unused]] const mrs_msgs::Float64SrvRequest::ConstPtr &cmd) {

  return mrs_msgs::Float64SrvResponse::Ptr();
}

//}

/* //{ setYawRelative() service */

const mrs_msgs::Float64SrvResponse::ConstPtr JoyBumperTracker::setYawRelative([[maybe_unused]] const mrs_msgs::Float64SrvRequest::ConstPtr &cmd) {

  return mrs_msgs::Float64SrvResponse::Ptr();
}

//}

// | ------------------------- timers ------------------------- |

/* //{ mainTimer() */

void JoyBumperTracker::mainTimer(const ros::TimerEvent &event) {

  if (!is_active) {
    return;
  }

  mrs_lib::Routine profiler_routine = profiler.createRoutine("main", tracker_loop_rate_, 0.002, event);

  // --------------------------------------------------------------
  // |                       height tracking                      |
  // --------------------------------------------------------------

  state_z += current_vertical_speed * tracker_dt_;

  // --------------------------------------------------------------
  // |                        yaw tracking                        |
  // --------------------------------------------------------------

  // flap the resulted state_yaw aroud PI
  state_yaw += current_yaw_rate * tracker_dt_;

  if (state_yaw > PI) {
    state_yaw -= 2 * PI;
  } else if (state_yaw < -PI) {
    state_yaw += 2 * PI;
  }
}

//}

// | --------------------- custom methods --------------------- |

/* callbackJoystick() //{ */

void JoyBumperTracker::callbackJoystic(const sensor_msgs::Joy &msg) {

  if (!is_initialized)
    return;

  mrs_lib::Routine profiler_routine = profiler.createRoutine("callbackJoy");

  current_vertical_speed = msg.axes[thrust_idx_] * vertical_speed_;
  current_yaw_rate       = msg.axes[yaw_idx_] * yaw_rate_;
  desired_pitch          = msg.axes[pitch_idx_] * max_tilt_;
  desired_roll           = msg.axes[roll_idx_] * max_tilt_;

  got_goal = true;

  ROS_INFO_THROTTLE(1.0, "[JoyBumperTracker]: th: %f, yaw: %f, pitch: %f, roll: %f", current_vertical_speed, current_yaw_rate, desired_pitch, desired_roll);
}

//}

/* callbackBumper() //{ */

void JoyBumperTracker::callbackBumper(const mrs_msgs::ObstacleSectorsConstPtr &msg) {

  if (!is_initialized)
    return;

  if (!is_active) {
    return;
  }

  ROS_INFO_ONCE("[JoyBumperTracker]: getting bumper data");
  ROS_INFO_THROTTLE(0.5, "[JoyBumperTracker]: getting bumper data");

  std::scoped_lock lock(mutex_bumper);

  got_bumper_           = true;
  bumper_previous_data_ = bumper_data_;
  bumper_data_          = *msg;
}

//}

/* bumperGetSectorId() //{ */

int JoyBumperTracker::bumperGetSectorId(const double x, const double y, [[maybe_unused]] const double z) {

  // heading of the point in drone frame
  double point_heading_horizontal = atan2(y, x);

  point_heading_horizontal += 2 * M_PI;

  // if point_heading_horizontal is greater then 2*M_PI mod it
  if (fabs(point_heading_horizontal) >= 2 * M_PI) {
    point_heading_horizontal = fmod(point_heading_horizontal, 2 * M_PI);
  }

  // heading of the right edge of the first sector
  double sector_size = 2 * M_PI / double(bumper_data_.n_horizontal_sectors);

  // calculate the idx
  int idx = floor((point_heading_horizontal + (sector_size / 2.0)) / sector_size);

  if (uint(idx) > bumper_data_.n_horizontal_sectors - 1) {
    idx -= bumper_data_.n_horizontal_sectors;
  }

  return idx;
}
//}

/* bumperPushFromObstacle() //{ */

bool JoyBumperTracker::bumperPushFromObstacle(void) {

  ROS_INFO_THROTTLE(1.0, "[JoyBumperTracker]: Entering bumper push form obstacle.");

  if (!bumper_enabled_) {
    return true;
  }

  if (!got_bumper_) {
    return true;
  }

  got_bumper_ = false;

  if (!filter_initialized_) {
    for (uint i = 0; i < bumper_data_.n_horizontal_sectors + 2; i++) {
      diff_filter_.push_back(0.0);
      reg_error_.push_back(0.0);
      reg_error_diff_.push_back(0.0);
    }
    bumper_previous_data_ = bumper_data_;
    filter_initialized_   = true;
  }

  std::scoped_lock lock(mutex_bumper);

  double sector_size                   = 2 * M_PI / double(bumper_data_.n_horizontal_sectors);
  double current_reg_error             = 0;
  double min_distance                  = 10e9;
  int    min_dist_index                = 0;
  bool   horizontal_collision_detected = false;
  double time_step                     = (bumper_data_.header.stamp - bumper_previous_data_.header.stamp).toSec();

  for (uint i = 0; i < bumper_data_.n_horizontal_sectors; i++) {

    if (bumper_data_.sectors[i] < 0 || bumper_data_.sectors[i] > bumper_repulsion_horizontal_distance_) {
      current_reg_error = 0;
    } else {

      // if the sector is under critical distance
      if (bumper_data_.sectors[i] <= bumper_repulsion_horizontal_distance_) {

        // get the desired direction of motion
        double oposite_direction  = double(i) * sector_size + M_PI;
        int    oposite_sector_idx = bumperGetSectorId(cos(oposite_direction), sin(oposite_direction), 0);

        if (bumper_data_.sectors[oposite_sector_idx] > 0 && bumper_data_.sectors[oposite_sector_idx] <= bumper_repulsion_horizontal_distance_) {
          current_reg_error = (bumper_data_.sectors[i] + bumper_data_.sectors[oposite_sector_idx]) / 2 - bumper_data_.sectors[i];
        } else {
          current_reg_error = bumper_repulsion_horizontal_distance_ - bumper_data_.sectors[i];
        }

        if (bumper_data_.sectors[i] < min_distance) {
          min_distance   = bumper_data_.sectors[i];
          min_dist_index = i;
        }
        horizontal_collision_detected = true;
      }

      reg_error_diff_[i] = (current_reg_error - reg_error_[i]) / time_step;
      reg_error_[i]      = current_reg_error;
      diff_filter_[i]    = (1 - filter_coeff_) * reg_error_diff_[i] + filter_coeff_ * diff_filter_[i];
    }
  }

  bool collision_above = false;
  bool collision_below = false;

  // check for vertical collision down
  if (bumper_data_.sectors[bumper_data_.n_horizontal_sectors] > 0 &&
      bumper_data_.sectors[bumper_data_.n_horizontal_sectors] <= bumper_repulsion_vertical_distance_) {
    ROS_INFO_THROTTLE(1.0, "[JoyBumperTracker]: bumper: potential collision below");
    collision_above   = true;
    current_reg_error = bumper_data_.sectors[bumper_data_.n_horizontal_sectors] - bumper_repulsion_vertical_distance_;
  }

  // check for vertical collision up
  if (bumper_data_.sectors[bumper_data_.n_horizontal_sectors + 1] > 0 &&
      bumper_data_.sectors[bumper_data_.n_horizontal_sectors + 1] <= bumper_repulsion_vertical_distance_) {
    ROS_INFO_THROTTLE(1.0, "[JoyBumperTracker]: bumper: potential collision above");
    collision_below   = true;
    current_reg_error = bumper_repulsion_vertical_distance_ - bumper_data_.sectors[bumper_data_.n_horizontal_sectors + 1];
  }

  // compute regulation error and its diff for vertical direction
  if (collision_above && collision_below) {
    current_reg_error = (bumper_data_.sectors[bumper_data_.n_horizontal_sectors] + bumper_data_.sectors[bumper_data_.n_horizontal_sectors + 1]) / 2 -
                        bumper_data_.sectors[bumper_data_.n_horizontal_sectors + 1];
  }

  reg_error_diff_[bumper_data_.n_horizontal_sectors] = (current_reg_error - reg_error_[bumper_data_.n_horizontal_sectors]) / time_step;
  reg_error_[bumper_data_.n_horizontal_sectors]      = current_reg_error;
  diff_filter_[bumper_data_.n_horizontal_sectors] =
      (1 - filter_coeff_) * reg_error_diff_[bumper_data_.n_horizontal_sectors] + filter_coeff_ * diff_filter_[bumper_data_.n_horizontal_sectors];

  if (horizontal_collision_detected) {

    // TODO generate modified x y acceleration for position command
    // min_dist_index contains the index of the problem sector
    // reg_error_[i]
    // reg_error_diff_[i]
    // diff_filter_[i]

    // --------------------------------------------------------------
    // |                     MPC lateral control                    |
    // --------------------------------------------------------------

    // | ------------------- initial conditions ------------------- |

    Eigen::MatrixXd initial_x = Eigen::MatrixXd::Zero(3, 1);
    initial_x << reg_error_[min_dist_index], diff_filter_[min_dist_index], 0;

    // | ---------------------- set reference --------------------- |

    Eigen::MatrixXd mpc_reference_x = Eigen::MatrixXd::Zero(horizon_length_ * n, 1);

    // prepare the full reference vector
    for (int i = 0; i < horizon_length_; i++) {

      mpc_reference_x((i * n) + 0, 0) = 1.5;  // TODO this is the reference
      mpc_reference_x((i * n) + 1, 0) = 0;
      mpc_reference_x((i * n) + 2, 0) = 0;
    }

    // | ------------------------ optimize ------------------------ |

    cvx_x->lock();
    cvx_x->setQ(Q);
    cvx_x->setS(S);
    cvx_x->setParams();
    cvx_x->setLastInput(cvx_x_u);
    cvx_x->loadReference(mpc_reference_x);
    cvx_x->setLimits(max_speed_horizontal_, 999, max_acceleration_horizontal_, max_jerk_, dt1, dt2);
    cvx_x->setInitialState(initial_x);
    [[maybe_unused]] int iters_x = cvx_x->solveCvx();
    cvx_x_u                      = cvx_x->getFirstControlInput();
    cvx_x->unlock();
  }

  if (collision_above || collision_below) {

    if (collision_above) {
      current_vertical_speed = fmin(current_vertical_speed, 0);
      ROS_INFO_COND(abs(current_vertical_speed) < 1e-10, "[JoyBumperTracker]: ################# Limiting the vertical speed.");
    }

    // TODO generate modified z velocity/position for position command
  }
  return false;
}

//}

/* bumperTimer() //{ */

void JoyBumperTracker::bumperTimer(const ros::TimerEvent &event) {

  if (!is_initialized)
    return;

  mrs_lib::Routine profiler_routine = profiler.createRoutine("bumperTimer", bumper_timer_rate_, 0.01, event);

  if (!bumper_enabled_) {
    return;
  }

  if ((ros::Time::now() - bumper_data_.header.stamp).toSec() > 1.0) {
    return;
  }

  {
    std::scoped_lock lock(mutex_uav_state);

    if (uav_z < 0.5) {
      ROS_WARN_THROTTLE(0.5, "[JoyBumperTracker]: not using bumper repulsion, height < 0.5 m");
      return;
    }
  }

  // --------------------------------------------------------------
  // |                      bumper repulsion                      |
  // --------------------------------------------------------------

  /* ROS_INFO_THROTTLE(1.0, "[JoyBumperTracker]: bumperTimer spinning"); */

  /* bumperPushFromObstacle(); */
}

//}

}  // namespace joy_bumper_tracker

}  // namespace mrs_trackers

#include <pluginlib/class_list_macros.h>
PLUGINLIB_EXPORT_CLASS(mrs_trackers::joy_bumper_tracker::JoyBumperTracker, mrs_uav_manager::Tracker)
