#define VERSION "0.0.5.0"

/* includes //{ */

#include <ros/ros.h>

#include <mrs_uav_manager/Tracker.h>

#include <mrs_lib/ParamLoader.h>
#include <mrs_lib/Profiler.h>
#include <mrs_lib/mutex.h>
#include <mrs_lib/geometry_utils.h>
#include <mrs_lib/attitude_converter.h>
#include <mrs_lib/Utils.h>

//}

#define STOP_THR 1e-3

namespace mrs_trackers
{

namespace line_tracker
{

/* //{ class LineTracker */

// state machine
typedef enum
{

  IDLE_STATE,
  STOP_MOTION_STATE,
  ACCELERATING_STATE,
  DECELERATING_STATE,
  STOPPING_STATE,

} States_t;

const char *state_names[5] = {

    "IDLING", "STOPPING_MOTION", "ACCELERATING", "DECELERATING", "STOPPING"};

class LineTracker : public mrs_uav_manager::Tracker {
public:
  void initialize(const ros::NodeHandle &parent_nh, const std::string uav_name, std::shared_ptr<mrs_uav_manager::CommonHandlers_t> common_handlers);
  bool activate(const mrs_msgs::PositionCommand::ConstPtr &last_position_cmd);
  void deactivate(void);
  bool resetStatic(void);

  const mrs_msgs::PositionCommand::ConstPtr update(const mrs_msgs::UavState::ConstPtr &uav_state, const mrs_msgs::AttitudeCommand::ConstPtr &last_attitude_cmd);
  const mrs_msgs::TrackerStatus             getStatus();
  const std_srvs::SetBoolResponse::ConstPtr enableCallbacks(const std_srvs::SetBoolRequest::ConstPtr &cmd);
  void                                      switchOdometrySource(const mrs_msgs::UavState::ConstPtr &new_uav_state);

  const mrs_msgs::ReferenceSrvResponse::ConstPtr           setReference(const mrs_msgs::ReferenceSrvRequest::ConstPtr &cmd);
  const mrs_msgs::TrajectoryReferenceSrvResponse::ConstPtr setTrajectoryReference(const mrs_msgs::TrajectoryReferenceSrvRequest::ConstPtr &cmd);

  const mrs_msgs::TrackerConstraintsSrvResponse::ConstPtr setConstraints(const mrs_msgs::TrackerConstraintsSrvRequest::ConstPtr &cmd);

  const std_srvs::TriggerResponse::ConstPtr hover(const std_srvs::TriggerRequest::ConstPtr &cmd);
  const std_srvs::TriggerResponse::ConstPtr startTrajectoryTracking(const std_srvs::TriggerRequest::ConstPtr &cmd);
  const std_srvs::TriggerResponse::ConstPtr stopTrajectoryTracking(const std_srvs::TriggerRequest::ConstPtr &cmd);
  const std_srvs::TriggerResponse::ConstPtr resumeTrajectoryTracking(const std_srvs::TriggerRequest::ConstPtr &cmd);
  const std_srvs::TriggerResponse::ConstPtr gotoTrajectoryStart(const std_srvs::TriggerRequest::ConstPtr &cmd);

private:
  std::shared_ptr<mrs_uav_manager::CommonHandlers_t> common_handlers_;

  bool callbacks_enabled_ = true;

  std::string _version_;
  std::string _uav_name_;

  void       mainTimer(const ros::TimerEvent &event);
  ros::Timer main_timer_;

  // | ------------------------ uav state ----------------------- |

  mrs_msgs::UavState uav_state_;
  bool               got_uav_state_ = false;
  std::mutex         mutex_uav_state_;

  double uav_x_;
  double uav_y_;
  double uav_z_;
  double uav_yaw_;

  // tracker's inner states
  double _tracker_loop_rate_;
  double _tracker_dt_;
  bool   is_initialized_ = false;
  bool   is_active_      = false;
  bool   first_iter_     = false;

  // | ----------------- internal state mmachine ---------------- |

  States_t current_state_vertical_    = IDLE_STATE;
  States_t previous_state_vertical_   = IDLE_STATE;
  States_t current_state_horizontal_  = IDLE_STATE;
  States_t previous_state_horizontal_ = IDLE_STATE;

  void changeStateHorizontal(States_t new_state);
  void changeStateVertical(States_t new_state);
  void changeState(States_t new_state);

  void stopHorizontalMotion(void);
  void stopVerticalMotion(void);
  void accelerateHorizontal(void);
  void accelerateVertical(void);
  void decelerateHorizontal(void);
  void decelerateVertical(void);
  void stopHorizontal(void);
  void stopVertical(void);

  // | ------------------ dynamics constraints ------------------ |

  double     _horizontal_speed_;
  double     _vertical_speed_;
  double     _horizontal_acceleration_;
  double     _vertical_acceleration_;
  double     _yaw_rate_;
  double     _yaw_gain_;
  std::mutex mutex_constraints_;

  // | ---------------------- desired goal ---------------------- |

  double     goal_x_;
  double     goal_y_;
  double     goal_z_;
  double     goal_yaw_;
  double     have_goal_ = false;
  std::mutex mutex_goal_;

  // | ------------------- the state variables ------------------ |
  double state_x_;
  double state_y_;
  double state_z_;
  double state_yaw_;

  double speed_x_;
  double speed_y_;
  double speed_yaw_;

  double current_heading_;
  double current_vertical_direction_;

  double current_vertical_speed_;
  double current_horizontal_speed_;

  double current_horizontal_acceleration_;
  double current_vertical_acceleration_;

  std::mutex mutex_state_;

  // | ------------------------ profiler ------------------------ |

  mrs_lib::Profiler profiler_;
  bool              _profiler_enabled_ = false;
};

//}

// | -------------- tracker's interface routines -------------- |

/* //{ initialize() */

void LineTracker::initialize(const ros::NodeHandle &parent_nh, [[maybe_unused]] const std::string uav_name,
                             [[maybe_unused]] std::shared_ptr<mrs_uav_manager::CommonHandlers_t> common_handlers) {

  _uav_name_             = uav_name;
  this->common_handlers_ = common_handlers;

  ros::NodeHandle nh_(parent_nh, "line_tracker");

  ros::Time::waitForValid();

  // --------------------------------------------------------------
  // |                       load parameters                      |
  // --------------------------------------------------------------


  mrs_lib::ParamLoader param_loader(nh_, "LineTracker");

  param_loader.load_param("version", _version_);

  if (_version_ != VERSION) {

    ROS_ERROR("[LineTracker]: the version of the binary (%s) does not match the config file (%s), please build me!", VERSION, _version_.c_str());
    ros::shutdown();
  }

  param_loader.load_param("enable_profiler", _profiler_enabled_);

  param_loader.load_param("horizontal_tracker/horizontal_speed", _horizontal_speed_);
  param_loader.load_param("horizontal_tracker/horizontal_acceleration", _horizontal_acceleration_);

  param_loader.load_param("vertical_tracker/vertical_speed", _vertical_speed_);
  param_loader.load_param("vertical_tracker/vertical_acceleration", _vertical_acceleration_);

  param_loader.load_param("yaw_tracker/yaw_rate", _yaw_rate_);
  param_loader.load_param("yaw_tracker/yaw_gain", _yaw_gain_);

  param_loader.load_param("tracker_loop_rate", _tracker_loop_rate_);

  _tracker_dt_ = 1.0 / double(_tracker_loop_rate_);

  ROS_INFO("[LineTracker]: tracker_dt: %.2f", _tracker_dt_);

  state_x_   = 0;
  state_y_   = 0;
  state_z_   = 0;
  state_yaw_ = 0;

  speed_x_   = 0;
  speed_y_   = 0;
  speed_yaw_ = 0;

  current_horizontal_speed_ = 0;
  current_vertical_speed_   = 0;

  current_horizontal_acceleration_ = 0;
  current_vertical_acceleration_   = 0;

  current_vertical_direction_ = 0;

  current_state_vertical_  = IDLE_STATE;
  previous_state_vertical_ = IDLE_STATE;

  current_state_horizontal_  = IDLE_STATE;
  previous_state_horizontal_ = IDLE_STATE;

  // --------------------------------------------------------------
  // |                          profiler_                          |
  // --------------------------------------------------------------

  profiler_ = mrs_lib::Profiler(nh_, "LineTracker", _profiler_enabled_);

  // --------------------------------------------------------------
  // |                           timers                           |
  // --------------------------------------------------------------

  main_timer_ = nh_.createTimer(ros::Rate(_tracker_loop_rate_), &LineTracker::mainTimer, this);

  if (!param_loader.loaded_successfully()) {
    ROS_ERROR("[LineTracker]: could not load all parameters!");
    ros::shutdown();
  }

  is_initialized_ = true;

  ROS_INFO("[LineTracker]: initialized, version %s", VERSION);
}

//}

/* //{ activate() */

bool LineTracker::activate(const mrs_msgs::PositionCommand::ConstPtr &last_position_cmd) {

  if (!got_uav_state_) {

    ROS_ERROR("[LineTracker]: can not activate, odometry not set");
    return false;
  }

  // copy member variables
  auto [uav_state, uav_yaw] = mrs_lib::get_mutexed(mutex_uav_state_, uav_state_, uav_yaw_);

  {
    std::scoped_lock lock(mutex_goal_, mutex_state_);

    if (mrs_msgs::PositionCommand::Ptr() != last_position_cmd) {

      // the last command is usable
      state_x_   = last_position_cmd->position.x;
      state_y_   = last_position_cmd->position.y;
      state_z_   = last_position_cmd->position.z;
      state_yaw_ = last_position_cmd->yaw;

      speed_x_                  = last_position_cmd->velocity.x;
      speed_y_                  = last_position_cmd->velocity.y;
      current_heading_          = atan2(speed_y_, speed_x_);
      current_horizontal_speed_ = sqrt(pow(speed_x_, 2) + pow(speed_y_, 2));

      current_vertical_speed_     = fabs(last_position_cmd->velocity.z);
      current_vertical_direction_ = last_position_cmd->velocity.z > 0 ? +1 : -1;

      current_horizontal_acceleration_ = 0;
      current_vertical_acceleration_   = 0;

      goal_yaw_ = last_position_cmd->yaw;

      ROS_INFO("[LineTracker]: initial condition: x=%.2f, y=%.2f, z=%.2f, yaw=%.2f", last_position_cmd->position.x, last_position_cmd->position.y,
               last_position_cmd->position.z, last_position_cmd->yaw);
      ROS_INFO("[LineTracker]: initial condition: x_dot=%.2f, y_dot=%.2f, z_dot=%.2f", speed_x_, speed_y_, current_vertical_speed_);

    } else {

      state_x_   = uav_state.pose.position.x;
      state_y_   = uav_state.pose.position.y;
      state_z_   = uav_state.pose.position.z;
      state_yaw_ = uav_yaw;

      speed_x_                  = uav_state.velocity.linear.x;
      speed_y_                  = uav_state.velocity.linear.y;
      current_heading_          = atan2(speed_y_, speed_x_);
      current_horizontal_speed_ = sqrt(pow(speed_x_, 2) + pow(speed_y_, 2));

      current_vertical_speed_     = fabs(uav_state.velocity.linear.z);
      current_vertical_direction_ = uav_state.velocity.linear.z > 0 ? +1 : -1;

      current_horizontal_acceleration_ = 0;
      current_vertical_acceleration_   = 0;

      goal_yaw_ = uav_yaw;

      ROS_WARN("[LineTracker]: the previous command is not usable for activation, using Odometry instead");
    }
  }

  // --------------------------------------------------------------
  // |          horizontal initial conditions prediction          |
  // --------------------------------------------------------------

  double horizontal_t_stop, horizontal_stop_dist, stop_dist_x, stop_dist_y;

  {
    std::scoped_lock lock(mutex_state_);

    horizontal_t_stop    = current_horizontal_speed_ / _horizontal_acceleration_;
    horizontal_stop_dist = (horizontal_t_stop * current_horizontal_speed_) / 2.0;
    stop_dist_x          = cos(current_heading_) * horizontal_stop_dist;
    stop_dist_y          = sin(current_heading_) * horizontal_stop_dist;
  }

  // --------------------------------------------------------------
  // |           vertical initial conditions prediction           |
  // --------------------------------------------------------------

  double vertical_t_stop, vertical_stop_dist;

  {
    std::scoped_lock lock(mutex_state_);

    vertical_t_stop    = current_vertical_speed_ / _vertical_acceleration_;
    vertical_stop_dist = current_vertical_direction_ * (vertical_t_stop * current_vertical_speed_) / 2.0;
  }

  // --------------------------------------------------------------
  // |              yaw initial condition  prediction             |
  // --------------------------------------------------------------

  {
    std::scoped_lock lock(mutex_goal_, mutex_state_);

    goal_x_ = state_x_ + stop_dist_x;
    goal_y_ = state_y_ + stop_dist_y;
    goal_z_ = state_z_ + vertical_stop_dist;

    ROS_INFO("[LineTracker]: setting z goal to %.2f", goal_z_);
  }

  is_active_ = true;

  ROS_INFO("[LineTracker]: activated");

  changeState(STOP_MOTION_STATE);

  return true;
}

//}

/* //{ deactivate() */

void LineTracker::deactivate(void) {

  is_active_ = false;

  ROS_INFO("[LineTracker]: deactivated");
}

//}

/* //{ resetStatic() */

bool LineTracker::resetStatic(void) {

  if (!is_initialized_) {
    ROS_ERROR("[LineTracker]: can not reset, not initialized");
    return false;
  }

  if (!is_active_) {
    ROS_ERROR("[LineTracker]: can not reset, not active");
    return false;
  }

  ROS_INFO("[LineTracker]: reseting with no dynamics");

  {
    std::scoped_lock lock(mutex_goal_, mutex_state_, mutex_uav_state_);

    state_x_   = uav_state_.pose.position.x;
    state_y_   = uav_state_.pose.position.y;
    state_z_   = uav_state_.pose.position.z;
    state_yaw_ = uav_yaw_;

    speed_x_                  = 0;
    speed_y_                  = 0;
    current_heading_          = 0;
    current_horizontal_speed_ = 0;

    current_vertical_speed_     = 0;
    current_vertical_direction_ = 0;

    current_horizontal_acceleration_ = 0;
    current_vertical_acceleration_   = 0;

    goal_yaw_ = uav_yaw_;
  }

  changeState(IDLE_STATE);

  return true;
}

//}

/* //{ update() */

const mrs_msgs::PositionCommand::ConstPtr LineTracker::update(const mrs_msgs::UavState::ConstPtr &                        uav_state,
                                                              [[maybe_unused]] const mrs_msgs::AttitudeCommand::ConstPtr &last_attitude_cmd) {

  mrs_lib::Routine profiler_routine = profiler_.createRoutine("update");

  {
    std::scoped_lock lock(mutex_uav_state_);

    uav_state_ = *uav_state;
    uav_x_     = uav_state_.pose.position.x;
    uav_y_     = uav_state_.pose.position.y;
    uav_z_     = uav_state_.pose.position.z;

    uav_yaw_ = mrs_lib::AttitudeConverter(uav_state->pose.orientation).getYaw();

    got_uav_state_ = true;
  }

  // up to this part the update() method is evaluated even when the tracker is not active
  if (!is_active_) {
    return mrs_msgs::PositionCommand::Ptr();
  }

  mrs_msgs::PositionCommand position_cmd;

  position_cmd.header.stamp    = ros::Time::now();
  position_cmd.header.frame_id = uav_state->header.frame_id;

  {
    std::scoped_lock lock(mutex_state_);

    position_cmd.position.x = state_x_;
    position_cmd.position.y = state_y_;
    position_cmd.position.z = state_z_;
    position_cmd.yaw        = state_yaw_;

    position_cmd.velocity.x = cos(current_heading_) * current_horizontal_speed_;
    position_cmd.velocity.y = sin(current_heading_) * current_horizontal_speed_;
    position_cmd.velocity.z = current_vertical_direction_ * current_vertical_speed_;
    position_cmd.yaw_dot    = speed_yaw_;

    position_cmd.acceleration.x = 0;
    position_cmd.acceleration.y = 0;
    position_cmd.acceleration.z = current_vertical_direction_ * current_vertical_acceleration_;

    position_cmd.use_position_vertical   = 1;
    position_cmd.use_position_horizontal = 1;
    position_cmd.use_yaw                 = 1;
    position_cmd.use_yaw_dot             = 1;
    position_cmd.use_velocity_vertical   = 1;
    position_cmd.use_velocity_horizontal = 1;
    position_cmd.use_acceleration        = 1;
  }

  return mrs_msgs::PositionCommand::ConstPtr(new mrs_msgs::PositionCommand(position_cmd));
}

//}

/* //{ getStatus() */

const mrs_msgs::TrackerStatus LineTracker::getStatus() {

  mrs_msgs::TrackerStatus tracker_status;

  tracker_status.active            = is_active_;
  tracker_status.callbacks_enabled = callbacks_enabled_;

  bool idling = current_state_vertical_ == IDLE_STATE && current_state_horizontal_ == IDLE_STATE;

  tracker_status.have_goal = !idling;

  tracker_status.tracking_trajectory = false;

  return tracker_status;
}

//}

/* //{ enableCallbacks() */

const std_srvs::SetBoolResponse::ConstPtr LineTracker::enableCallbacks(const std_srvs::SetBoolRequest::ConstPtr &cmd) {

  std_srvs::SetBoolResponse res;
  std::stringstream         ss;

  if (cmd->data != callbacks_enabled_) {

    callbacks_enabled_ = cmd->data;

    ss << "callbacks " << (callbacks_enabled_ ? "enabled" : "disabled");
    ROS_INFO_STREAM_THROTTLE(1.0, "[LineTracker]: " << ss.str());

  } else {

    ss << "callbacks were already " << (callbacks_enabled_ ? "enabled" : "disabled");
    ROS_WARN_STREAM_THROTTLE(1.0, "[LineTracker]: " << ss.str());
  }

  res.message = ss.str();
  res.success = true;

  return std_srvs::SetBoolResponse::ConstPtr(new std_srvs::SetBoolResponse(res));
}

//}

/* switchOdometrySource() //{ */

void LineTracker::switchOdometrySource(const mrs_msgs::UavState::ConstPtr &new_uav_state) {

  std::scoped_lock lock(mutex_goal_, mutex_state_);

  auto uav_state = mrs_lib::get_mutexed(mutex_uav_state_, uav_state_);

  double old_yaw = mrs_lib::AttitudeConverter(uav_state.pose.orientation).getYaw();
  double new_yaw = mrs_lib::AttitudeConverter(new_uav_state->pose.orientation).getYaw();

  // | --------- recalculate the goal to new coordinates -------- |

  double dx   = new_uav_state->pose.position.x - uav_state.pose.position.x;
  double dy   = new_uav_state->pose.position.y - uav_state.pose.position.y;
  double dz   = new_uav_state->pose.position.z - uav_state.pose.position.z;
  double dyaw = new_yaw - old_yaw;

  goal_x_ += dx;
  goal_y_ += dy;
  goal_z_ += dz;
  goal_yaw_ += dyaw;

  // | -------------------- update the state -------------------- |

  state_x_ += dx;
  state_y_ += dy;
  state_z_ += dz;
  state_yaw_ += dyaw;

  current_heading_ = atan2(goal_y_ - state_y_, goal_x_ - state_x_);
}

//}

/* //{ hover() */

const std_srvs::TriggerResponse::ConstPtr LineTracker::hover([[maybe_unused]] const std_srvs::TriggerRequest::ConstPtr &cmd) {

  std_srvs::TriggerResponse res;

  // --------------------------------------------------------------
  // |          horizontal initial conditions prediction          |
  // --------------------------------------------------------------
  {
    std::scoped_lock lock(mutex_state_, mutex_uav_state_);

    current_horizontal_speed_ = sqrt(pow(uav_state_.velocity.linear.x, 2) + pow(uav_state_.velocity.linear.y, 2));
    current_vertical_speed_   = uav_state_.velocity.linear.z;
    current_heading_          = atan2(uav_state_.velocity.linear.y, uav_state_.velocity.linear.x);
  }

  double horizontal_t_stop, horizontal_stop_dist, stop_dist_x, stop_dist_y;

  {
    std::scoped_lock lock(mutex_state_);

    horizontal_t_stop    = current_horizontal_speed_ / _horizontal_acceleration_;
    horizontal_stop_dist = (horizontal_t_stop * current_horizontal_speed_) / 2.0;
    stop_dist_x          = cos(current_heading_) * horizontal_stop_dist;
    stop_dist_y          = sin(current_heading_) * horizontal_stop_dist;
  }

  // --------------------------------------------------------------
  // |           vertical initial conditions prediction           |
  // --------------------------------------------------------------

  double vertical_t_stop, vertical_stop_dist;

  {
    std::scoped_lock lock(mutex_state_);

    vertical_t_stop    = current_vertical_speed_ / _vertical_acceleration_;
    vertical_stop_dist = current_vertical_direction_ * (vertical_t_stop * current_vertical_speed_) / 2.0;
  }

  // --------------------------------------------------------------
  // |                        set the goal                        |
  // --------------------------------------------------------------

  {
    std::scoped_lock lock(mutex_goal_, mutex_state_);

    goal_x_ = state_x_ + stop_dist_x;
    goal_y_ = state_y_ + stop_dist_y;
    goal_z_ = state_z_ + vertical_stop_dist;
  }

  res.message = "hover initiated";
  res.success = true;

  changeState(STOP_MOTION_STATE);

  return std_srvs::TriggerResponse::ConstPtr(new std_srvs::TriggerResponse(res));
}

//}

/* //{ startTrajectoryTracking() */

const std_srvs::TriggerResponse::ConstPtr LineTracker::startTrajectoryTracking([[maybe_unused]] const std_srvs::TriggerRequest::ConstPtr &cmd) {
  return std_srvs::TriggerResponse::Ptr();
}

//}

/* //{ stopTrajectoryTracking() */

const std_srvs::TriggerResponse::ConstPtr LineTracker::stopTrajectoryTracking([[maybe_unused]] const std_srvs::TriggerRequest::ConstPtr &cmd) {
  return std_srvs::TriggerResponse::Ptr();
}

//}

/* //{ resumeTrajectoryTracking() */

const std_srvs::TriggerResponse::ConstPtr LineTracker::resumeTrajectoryTracking([[maybe_unused]] const std_srvs::TriggerRequest::ConstPtr &cmd) {
  return std_srvs::TriggerResponse::Ptr();
}

//}

/* //{ gotoTrajectoryStart() */

const std_srvs::TriggerResponse::ConstPtr LineTracker::gotoTrajectoryStart([[maybe_unused]] const std_srvs::TriggerRequest::ConstPtr &cmd) {
  return std_srvs::TriggerResponse::Ptr();
}

//}

/* //{ setConstraints() */

const mrs_msgs::TrackerConstraintsSrvResponse::ConstPtr LineTracker::setConstraints(const mrs_msgs::TrackerConstraintsSrvRequest::ConstPtr &cmd) {

  mrs_msgs::TrackerConstraintsSrvResponse res;

  // this is the place to copy the constraints
  {
    std::scoped_lock lock(mutex_constraints_);

    _horizontal_speed_        = cmd->constraints.horizontal_speed;
    _horizontal_acceleration_ = cmd->constraints.horizontal_acceleration;

    _vertical_speed_        = cmd->constraints.vertical_ascending_speed;
    _vertical_acceleration_ = cmd->constraints.vertical_ascending_acceleration;

    _yaw_rate_ = cmd->constraints.yaw_speed;
  }

  res.success = true;
  res.message = "constraints updated";

  return mrs_msgs::TrackerConstraintsSrvResponse::ConstPtr(new mrs_msgs::TrackerConstraintsSrvResponse(res));
}

//}

/* //{ setReference() */

const mrs_msgs::ReferenceSrvResponse::ConstPtr LineTracker::setReference(const mrs_msgs::ReferenceSrvRequest::ConstPtr &cmd) {

  mrs_msgs::ReferenceSrvResponse res;

  {
    std::scoped_lock lock(mutex_goal_);

    goal_x_   = cmd->reference.position.x;
    goal_y_   = cmd->reference.position.y;
    goal_z_   = cmd->reference.position.z;
    goal_yaw_ = mrs_lib::wrapAngle(cmd->reference.yaw);

    ROS_INFO("[LineTracker]: received new setpoint %.2f, %.2f, %.2f, %.2f", goal_x_, goal_y_, goal_z_, goal_yaw_);

    have_goal_ = true;
  }

  changeState(STOP_MOTION_STATE);

  res.success = true;
  res.message = "reference set";

  return mrs_msgs::ReferenceSrvResponse::ConstPtr(new mrs_msgs::ReferenceSrvResponse(res));
}

//}

/* //{ setTrajectoryReference() */

const mrs_msgs::TrajectoryReferenceSrvResponse::ConstPtr LineTracker::setTrajectoryReference([
    [maybe_unused]] const mrs_msgs::TrajectoryReferenceSrvRequest::ConstPtr &cmd) {
  return mrs_msgs::TrajectoryReferenceSrvResponse::Ptr();
}

//}

// | ----------------- state machine routines ----------------- |

/* //{ changeStateHorizontal() */

void LineTracker::changeStateHorizontal(States_t new_state) {

  previous_state_horizontal_ = current_state_horizontal_;
  current_state_horizontal_  = new_state;

  // just for ROS_INFO
  ROS_DEBUG("[LineTracker]: Switching horizontal state %s -> %s", state_names[previous_state_horizontal_], state_names[current_state_horizontal_]);
}

//}

/* //{ changeStateVertical() */

void LineTracker::changeStateVertical(States_t new_state) {

  previous_state_vertical_ = current_state_vertical_;
  current_state_vertical_  = new_state;

  // just for ROS_INFO
  ROS_DEBUG("[LineTracker]: Switching vertical state %s -> %s", state_names[previous_state_vertical_], state_names[current_state_vertical_]);
}

//}

/* //{ changeState() */

void LineTracker::changeState(States_t new_state) {

  changeStateVertical(new_state);
  changeStateHorizontal(new_state);
}

//}

// | --------------------- motion routines -------------------- |

/* //{ stopHorizontalMotion() */

void LineTracker::stopHorizontalMotion(void) {

  {
    std::scoped_lock lock(mutex_state_);

    current_horizontal_speed_ -= _horizontal_acceleration_ * _tracker_dt_;

    if (current_horizontal_speed_ < 0) {
      current_horizontal_speed_        = 0;
      current_horizontal_acceleration_ = 0;
    } else {
      current_horizontal_acceleration_ = -_horizontal_acceleration_;
    }
  }
}

//}

/* //{ stopVerticalMotion() */

void LineTracker::stopVerticalMotion(void) {

  {
    std::scoped_lock lock(mutex_state_);

    current_vertical_speed_ -= _vertical_acceleration_ * _tracker_dt_;

    if (current_vertical_speed_ < 0) {
      current_vertical_speed_        = 0;
      current_vertical_acceleration_ = 0;
    } else {
      current_vertical_acceleration_ = -_vertical_acceleration_;
    }
  }
}

//}

/* //{ accelerateHorizontal() */

void LineTracker::accelerateHorizontal(void) {

  // copy member variables
  auto [goal_x, goal_y]                             = mrs_lib::get_mutexed(mutex_goal_, goal_x_, goal_y_);
  auto [state_x, state_y, current_horizontal_speed] = mrs_lib::get_mutexed(mutex_state_, state_x_, state_y_, current_horizontal_speed_);

  {
    std::scoped_lock lock(mutex_state_);

    current_heading_ = atan2(goal_y - state_y, goal_x - state_x);
  }

  auto current_heading = mrs_lib::get_mutexed(mutex_state_, current_heading_);

  double horizontal_t_stop, horizontal_stop_dist, stop_dist_x, stop_dist_y;

  horizontal_t_stop    = current_horizontal_speed / _horizontal_acceleration_;
  horizontal_stop_dist = (horizontal_t_stop * current_horizontal_speed) / 2.0;
  stop_dist_x          = cos(current_heading) * horizontal_stop_dist;
  stop_dist_y          = sin(current_heading) * horizontal_stop_dist;

  {
    std::scoped_lock lock(mutex_state_);

    current_horizontal_speed_ += _horizontal_acceleration_ * _tracker_dt_;

    if (current_horizontal_speed_ >= _horizontal_speed_) {
      current_horizontal_speed_        = _horizontal_speed_;
      current_horizontal_acceleration_ = 0;
    } else {
      current_horizontal_acceleration_ = _horizontal_acceleration_;
    }
  }

  if (sqrt(pow(state_x + stop_dist_x - goal_x, 2) + pow(state_y + stop_dist_y - goal_y, 2)) < (2 * (_horizontal_speed_ * _tracker_dt_))) {

    {
      std::scoped_lock lock(mutex_state_);

      current_horizontal_acceleration_ = 0;
    }

    changeStateHorizontal(DECELERATING_STATE);
  }
}

//}

/* //{ accelerateVertical() */

void LineTracker::accelerateVertical(void) {

  auto goal_z                            = mrs_lib::get_mutexed(mutex_goal_, goal_z_);
  auto [state_z, current_vertical_speed] = mrs_lib::get_mutexed(mutex_state_, state_z_, current_vertical_speed_);

  // set the right heading
  double tar_z = goal_z - state_z;

  // set the right vertical direction
  {
    std::scoped_lock lock(mutex_state_);

    current_vertical_direction_ = mrs_lib::sign(tar_z);
  }

  auto current_vertical_direction = mrs_lib::get_mutexed(mutex_state_, current_vertical_direction_);

  // calculate the time to stop and the distance it will take to stop [vertical]
  double vertical_t_stop    = current_vertical_speed / _vertical_acceleration_;
  double vertical_stop_dist = (vertical_t_stop * current_vertical_speed) / 2.0;
  double stop_dist_z        = current_vertical_direction * vertical_stop_dist;

  {
    std::scoped_lock lock(mutex_state_);

    current_vertical_speed_ += _vertical_acceleration_ * _tracker_dt_;

    if (current_vertical_speed_ >= _vertical_speed_) {
      current_vertical_speed_        = _vertical_speed_;
      current_vertical_acceleration_ = 0;
    } else {
      current_vertical_acceleration_ = _vertical_acceleration_;
    }
  }

  if (fabs(state_z + stop_dist_z - goal_z) < (2 * (_vertical_speed_ * _tracker_dt_))) {

    {
      std::scoped_lock lock(mutex_state_);

      current_vertical_acceleration_ = 0;
    }

    changeStateVertical(DECELERATING_STATE);
  }
}

//}

/* //{ decelerateHorizontal() */

void LineTracker::decelerateHorizontal(void) {

  {
    std::scoped_lock lock(mutex_state_);

    current_horizontal_speed_ -= _horizontal_acceleration_ * _tracker_dt_;

    if (current_horizontal_speed_ < 0) {
      current_horizontal_speed_ = 0;
    } else {
      current_horizontal_acceleration_ = -_horizontal_acceleration_;
    }
  }

  auto current_horizontal_speed = mrs_lib::get_mutexed(mutex_state_, current_horizontal_speed_);

  if (current_horizontal_speed == 0) {

    {
      std::scoped_lock lock(mutex_state_);

      current_horizontal_acceleration_ = 0;
    }

    changeStateHorizontal(STOPPING_STATE);
  }
}

//}

/* //{ decelerateVertical() */

void LineTracker::decelerateVertical(void) {

  {
    std::scoped_lock lock(mutex_state_);

    current_vertical_speed_ -= _vertical_acceleration_ * _tracker_dt_;

    if (current_vertical_speed_ < 0) {
      current_vertical_speed_ = 0;
    } else {
      current_vertical_acceleration_ = -_vertical_acceleration_;
    }
  }

  auto current_vertical_speed = mrs_lib::get_mutexed(mutex_state_, current_vertical_speed_);

  if (current_vertical_speed == 0) {
    current_vertical_acceleration_ = 0;
    changeStateVertical(STOPPING_STATE);
  }
}

//}

/* //{ stopHorizontal() */

void LineTracker::stopHorizontal(void) {

  {
    std::scoped_lock lock(mutex_state_);

    state_x_                         = 0.95 * state_x_ + 0.05 * goal_x_;
    state_y_                         = 0.95 * state_y_ + 0.05 * goal_y_;
    current_horizontal_acceleration_ = 0;
  }
}

//}

/* //{ stopVertical() */

void LineTracker::stopVertical(void) {

  {
    std::scoped_lock lock(mutex_state_);

    state_z_                       = 0.95 * state_z_ + 0.05 * goal_z_;
    current_vertical_acceleration_ = 0;
  }
}

//}

// | ------------------------- timers ------------------------- |

/* //{ mainTimer() */

void LineTracker::mainTimer(const ros::TimerEvent &event) {

  if (!is_active_) {
    return;
  }

  mrs_lib::Routine profiler_routine = profiler_.createRoutine("main", _tracker_loop_rate_, 0.01, event);

  auto [goal_x, goal_y, goal_z]    = mrs_lib::get_mutexed(mutex_goal_, goal_x_, goal_y_, goal_z_);
  auto [state_x, state_y, state_z] = mrs_lib::get_mutexed(mutex_state_, state_x_, state_y_, state_z_);

  switch (current_state_horizontal_) {

    case IDLE_STATE:

      break;

    case STOP_MOTION_STATE:

      stopHorizontalMotion();

      break;

    case ACCELERATING_STATE:

      accelerateHorizontal();

      break;

    case DECELERATING_STATE:

      decelerateHorizontal();

      break;

    case STOPPING_STATE:

      stopHorizontal();

      break;
  }

  switch (current_state_vertical_) {

    case IDLE_STATE:

      break;

    case STOP_MOTION_STATE:

      stopVerticalMotion();

      break;

    case ACCELERATING_STATE:

      accelerateVertical();

      break;

    case DECELERATING_STATE:

      decelerateVertical();

      break;

    case STOPPING_STATE:

      stopVertical();

      break;
  }

  if (current_state_horizontal_ == STOP_MOTION_STATE && current_state_vertical_ == STOP_MOTION_STATE) {
    if (current_vertical_speed_ == 0 && current_horizontal_speed_ == 0) {
      if (have_goal_) {
        changeState(ACCELERATING_STATE);
      } else {
        changeState(STOPPING_STATE);
      }
    }
  }

  if (current_state_horizontal_ == STOPPING_STATE && current_state_vertical_ == STOPPING_STATE) {
    if (fabs(state_x - goal_x) < 1e-3 && fabs(state_y - goal_y) < 1e-3 && fabs(state_z - goal_z) < 1e-3) {

      {
        std::scoped_lock lock(mutex_state_);

        state_x_ = goal_x;
        state_y_ = goal_y;
        state_z_ = goal_z;
      }

      changeState(IDLE_STATE);

      have_goal_ = false;
    }
  }

  {
    std::scoped_lock lock(mutex_state_);

    state_x_ += cos(current_heading_) * current_horizontal_speed_ * _tracker_dt_;
    state_y_ += sin(current_heading_) * current_horizontal_speed_ * _tracker_dt_;
    state_z_ += current_vertical_direction_ * current_vertical_speed_ * _tracker_dt_;
  }

  // --------------------------------------------------------------
  // |                        yaw tracking                        |
  // --------------------------------------------------------------

  {
    std::scoped_lock lock(mutex_state_);

    // compute the desired yaw rate
    double current_yaw_rate;
    if (fabs(goal_yaw_ - state_yaw_) > M_PI)
      current_yaw_rate = -_yaw_gain_ * (goal_yaw_ - state_yaw_);
    else
      current_yaw_rate = _yaw_gain_ * (goal_yaw_ - state_yaw_);

    if (current_yaw_rate > _yaw_rate_) {
      current_yaw_rate = _yaw_rate_;
    } else if (current_yaw_rate < -_yaw_rate_) {
      current_yaw_rate = -_yaw_rate_;
    }

    // flap the resulted state_yaw_ aroud PI
    state_yaw_ += current_yaw_rate * _tracker_dt_;

    state_yaw_ = mrs_lib::wrapAngle(state_yaw_);

    if (fabs(state_yaw_ - goal_yaw_) < (2 * (_yaw_rate_ * _tracker_dt_))) {
      state_yaw_ = goal_yaw_;
    }
  }
}

//}

}  // namespace line_tracker

}  // namespace mrs_trackers

#include <pluginlib/class_list_macros.h>
PLUGINLIB_EXPORT_CLASS(mrs_trackers::line_tracker::LineTracker, mrs_uav_manager::Tracker)
