/* includes //{ */

#include <ros/ros.h>

#include <geometry_msgs/PoseStamped.h>
#include <nav_msgs/Odometry.h>

#include <mrs_msgs/TrackerPointStamped.h>
#include <mrs_uav_manager/Tracker.h>

#include <tf/transform_datatypes.h>
#include <mutex>

#include <commons.h>

#include <mrs_lib/ParamLoader.h>
#include <mrs_lib/Profiler.h>

//}

#define STOP_THR 1e-3

namespace mrs_trackers
{

namespace matlab_tracker
{

/* //{ class MatlabTracker */

class MatlabTracker : public mrs_uav_manager::Tracker {
public:
  virtual void initialize(const ros::NodeHandle &parent_nh, mrs_uav_manager::SafetyArea_t const *safety_area, mrs_uav_manager::Transformer_t const *transformer);
  virtual bool activate(const mrs_msgs::PositionCommand::ConstPtr &cmd);
  virtual void deactivate(void);

  virtual const mrs_msgs::PositionCommand::ConstPtr update(const mrs_msgs::UavState::ConstPtr &msg);
  virtual const mrs_msgs::TrackerStatus             getStatus();
  virtual const std_srvs::SetBoolResponse::ConstPtr enableCallbacks(const std_srvs::SetBoolRequest::ConstPtr &cmd);
  virtual void                                      switchOdometrySource(const mrs_msgs::UavState::ConstPtr &msg);

  virtual const mrs_msgs::Vec4Response::ConstPtr goTo(const mrs_msgs::Vec4Request::ConstPtr &cmd);
  virtual const mrs_msgs::Vec4Response::ConstPtr goToRelative(const mrs_msgs::Vec4Request::ConstPtr &cmd);
  virtual const mrs_msgs::Vec1Response::ConstPtr goToAltitude(const mrs_msgs::Vec1Request::ConstPtr &cmd);
  virtual const mrs_msgs::Vec1Response::ConstPtr setYaw(const mrs_msgs::Vec1Request::ConstPtr &cmd);
  virtual const mrs_msgs::Vec1Response::ConstPtr setYawRelative(const mrs_msgs::Vec1Request::ConstPtr &cmd);

  virtual bool goTo(const mrs_msgs::TrackerPointStampedConstPtr &msg);
  virtual bool goToRelative(const mrs_msgs::TrackerPointStampedConstPtr &msg);
  virtual bool goToAltitude(const std_msgs::Float64ConstPtr &msg);
  virtual bool setYaw(const std_msgs::Float64ConstPtr &msg);
  virtual bool setYawRelative(const std_msgs::Float64ConstPtr &msg);

  virtual const mrs_msgs::TrackerConstraintsResponse::ConstPtr setConstraints(const mrs_msgs::TrackerConstraintsRequest::ConstPtr &cmd);

  virtual const std_srvs::TriggerResponse::ConstPtr hover(const std_srvs::TriggerRequest::ConstPtr &cmd);

private:
  bool callbacks_enabled = true;

  std::string uav_name_;
  std::string local_origin_frame_id_;

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
  // dynamical constraints
  double     yaw_rate_;
  std::mutex mutex_constraints;

private:
  // desired goal
  double             have_goal = false;
  std::mutex         mutex_goal;
  nav_msgs::Odometry matlab_goal;

  mrs_msgs::PositionCommand position_output;

private:
  ros::Subscriber subscriber_matlab;
  void            callbackMatlab(const nav_msgs::Odometry &msg);

private:
  mrs_lib::Profiler *profiler;
  bool               profiler_enabled_ = false;
  bool               position_mode_    = false;
  bool               tilt_mode_        = false;
};

//}

// | -------------- tracker's interface routines -------------- |

/* //{ initialize() */

void MatlabTracker::initialize(const ros::NodeHandle &parent_nh, [[maybe_unused]] mrs_uav_manager::SafetyArea_t const *safety_area, [[maybe_unused]] mrs_uav_manager::Transformer_t const *transformer) {

  ros::NodeHandle nh_(parent_nh, "matlab_tracker");

  ros::Time::waitForValid();

  // --------------------------------------------------------------
  // |                       load parameters                      |
  // --------------------------------------------------------------

  mrs_lib::ParamLoader param_loader(nh_, "MatlabTracker");

  param_loader.load_param("uav_name", uav_name_);
  local_origin_frame_id_ = uav_name_ + "/local_origin";

  param_loader.load_param("enable_profiler", profiler_enabled_);
  param_loader.load_param("position_mode", profiler_enabled_);
  param_loader.load_param("tilt_mode", tilt_mode_);

  // --------------------------------------------------------------
  // |                          profiler                          |
  // --------------------------------------------------------------

  profiler = new mrs_lib::Profiler(nh_, "matlabtracker", profiler_enabled_);

  // --------------------------------------------------------------
  // |                         subscribers                        |
  // --------------------------------------------------------------

  subscriber_matlab = nh_.subscribe("goal_in", 1, &MatlabTracker::callbackMatlab, this, ros::TransportHints().tcpNoDelay());

  if (!param_loader.loaded_successfully()) {
    ROS_ERROR("[MatlabTracker]: Could not load all parameters!");
    ros::shutdown();
  }

  is_initialized = true;

  ROS_INFO("[MatlabTracker]: initialized");
}

//}

/* //{ activate() */

bool MatlabTracker::activate([[maybe_unused]] const mrs_msgs::PositionCommand::ConstPtr &cmd) {

  if (!got_uav_state) {
    ROS_ERROR("[MatlabTracker]: can't activate(), odometry not set");
    return false;
  }

  std::scoped_lock lock(mutex_goal);

  if (!have_goal) {

    ROS_ERROR("[MatlabTracker]: cannot activate, missing Matlab command");
    return false;
  }

  // --------------------------------------------------------------
  // |              yaw initial condition  prediction             |
  // --------------------------------------------------------------

  is_active = true;

  ROS_INFO("[MatlabTracker]: activated");

  return true;
}

//}

/* //{ deactivate() */

void MatlabTracker::deactivate(void) {

  is_active = false;
  have_goal = false;

  ROS_INFO("[MatlabTracker]: deactivated");
}

//}

/* //{ update() */

const mrs_msgs::PositionCommand::ConstPtr MatlabTracker::update(const mrs_msgs::UavState::ConstPtr &msg) {

  mrs_lib::Routine profiler_routine = profiler->createRoutine("update");

  {
    std::scoped_lock lock(mutex_uav_state);

    uav_state  = *msg;
    uav_x = uav_state.pose.position.x;
    uav_y = uav_state.pose.position.y;
    uav_z = uav_state.pose.position.z;

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

  position_output.header.stamp    = ros::Time::now();
  position_output.header.frame_id = local_origin_frame_id_;

  {
    std::scoped_lock lock(mutex_uav_state, mutex_goal);

    if (position_mode_) {

      position_output.position.x = matlab_goal.pose.pose.position.x;
      position_output.position.y = matlab_goal.pose.pose.position.y;
      position_output.position.z = matlab_goal.pose.pose.position.z;

      position_output.velocity.x = matlab_goal.twist.twist.linear.x;
      position_output.velocity.y = matlab_goal.twist.twist.linear.y;
      position_output.velocity.z = matlab_goal.twist.twist.linear.z;

      position_output.acceleration.x = matlab_goal.twist.twist.angular.x;
      position_output.acceleration.y = matlab_goal.twist.twist.angular.y;
      position_output.acceleration.z = matlab_goal.twist.twist.angular.z;

      position_output.yaw     = matlab_goal.pose.pose.orientation.x;
      position_output.yaw_dot = matlab_goal.pose.pose.orientation.y;

      position_output.use_euler_attitude = 1;
      position_output.use_position       = 1;
      position_output.use_velocity       = 1;
      position_output.use_acceleration   = 1;
    }

    if (tilt_mode_) {

      position_output.position.x = uav_x;
      position_output.position.y = uav_y;
      position_output.position.z = matlab_goal.pose.pose.position.z;

      position_output.velocity.x = uav_state.velocity.linear.x;
      position_output.velocity.y = uav_state.velocity.linear.y;
      position_output.velocity.z = uav_state.velocity.linear.z;

      position_output.acceleration.x = 0;
      position_output.acceleration.y = 0;
      position_output.acceleration.z = 0;

      position_output.use_quat_attitude = 1;
      position_output.use_position      = 1;
    }
  }

  return mrs_msgs::PositionCommand::ConstPtr(new mrs_msgs::PositionCommand(position_output));
}

//}

/* //{ getStatus() */

const mrs_msgs::TrackerStatus MatlabTracker::getStatus() {

  mrs_msgs::TrackerStatus tracker_status;

  tracker_status.active            = is_active;
  tracker_status.callbacks_enabled = callbacks_enabled;

  return tracker_status;
}

//}

/* //{ enableCallbacks() */

const std_srvs::SetBoolResponse::ConstPtr MatlabTracker::enableCallbacks(const std_srvs::SetBoolRequest::ConstPtr &cmd) {

  char                      message[100];
  std_srvs::SetBoolResponse res;

  if (cmd->data != callbacks_enabled) {

    callbacks_enabled = cmd->data;

    sprintf((char *)&message, "Callbacks %s", callbacks_enabled ? "enabled" : "disabled");

    ROS_INFO("[MatlabTracker]: %s", message);

  } else {

    sprintf((char *)&message, "Callbacks were already %s", callbacks_enabled ? "enabled" : "disabled");
  }

  res.message = message;
  res.success = true;

  return std_srvs::SetBoolResponse::ConstPtr(new std_srvs::SetBoolResponse(res));
}

//}

/* switchOdometrySource() //{ */

void MatlabTracker::switchOdometrySource([[maybe_unused]] const mrs_msgs::UavState::ConstPtr &msg) {
}

//}

// | -------------- setpoint topics and services -------------- |

/* //{ goTo() service */

const mrs_msgs::Vec4Response::ConstPtr MatlabTracker::goTo([[maybe_unused]] const mrs_msgs::Vec4Request::ConstPtr &cmd) {

  return mrs_msgs::Vec4Response::Ptr();
}

//}

/* //{ goTo() topic */

bool MatlabTracker::goTo([[maybe_unused]] const mrs_msgs::TrackerPointStampedConstPtr &msg) {

  return false;
}

//}

/* //{ goToRelative() service */

const mrs_msgs::Vec4Response::ConstPtr MatlabTracker::goToRelative([[maybe_unused]] const mrs_msgs::Vec4Request::ConstPtr &cmd) {

  return mrs_msgs::Vec4Response::Ptr();
}

//}

/* //{ goToRelative() topic */

bool MatlabTracker::goToRelative([[maybe_unused]] const mrs_msgs::TrackerPointStampedConstPtr &msg) {

  return false;
}

//}

/* //{ goToAltitude() service */

const mrs_msgs::Vec1Response::ConstPtr MatlabTracker::goToAltitude([[maybe_unused]] const mrs_msgs::Vec1Request::ConstPtr &cmd) {

  return mrs_msgs::Vec1Response::Ptr();
}

//}

/* //{ goToAltitude() topic */

bool MatlabTracker::goToAltitude([[maybe_unused]] const std_msgs::Float64ConstPtr &msg) {

  return false;
}

//}

/* //{ setYaw() service */

const mrs_msgs::Vec1Response::ConstPtr MatlabTracker::setYaw([[maybe_unused]] const mrs_msgs::Vec1Request::ConstPtr &cmd) {

  return mrs_msgs::Vec1Response::Ptr();
}

//}

/* //{ setYaw() topic */

bool MatlabTracker::setYaw([[maybe_unused]] const std_msgs::Float64ConstPtr &msg) {

  return false;
}

//}

/* //{ setYawRelative() service */

const mrs_msgs::Vec1Response::ConstPtr MatlabTracker::setYawRelative([[maybe_unused]] const mrs_msgs::Vec1Request::ConstPtr &cmd) {

  return mrs_msgs::Vec1Response::Ptr();
}

//}

/* //{ setYawRelative() topic */

bool MatlabTracker::setYawRelative([[maybe_unused]] const std_msgs::Float64ConstPtr &msg) {

  return false;
}

//}

/* //{ hover() service */

const std_srvs::TriggerResponse::ConstPtr MatlabTracker::hover([[maybe_unused]] const std_srvs::TriggerRequest::ConstPtr &cmd) {

  return std_srvs::TriggerResponse::Ptr();
}

//}

/* //{ setConstraints() service */

const mrs_msgs::TrackerConstraintsResponse::ConstPtr MatlabTracker::setConstraints([[maybe_unused]] const mrs_msgs::TrackerConstraintsRequest::ConstPtr &cmd) {

  return mrs_msgs::TrackerConstraintsResponse::Ptr();
}

//}

// | --------------------- custom methods --------------------- |

/* callbackMatlab() //{ */

void MatlabTracker::callbackMatlab(const nav_msgs::Odometry &msg) {

  if (!is_initialized)
    return;

  std::scoped_lock lock(mutex_goal);

  mrs_lib::Routine profiler_routine = profiler->createRoutine("callbackMatlab");

  matlab_goal = msg;

  have_goal = true;
}

//}

}  // namespace matlab_tracker

}  // namespace mrs_trackers

#include <pluginlib/class_list_macros.h>
PLUGINLIB_EXPORT_CLASS(mrs_trackers::matlab_tracker::MatlabTracker, mrs_uav_manager::Tracker)
