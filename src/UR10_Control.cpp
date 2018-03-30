/**
 * @file UR10_Control.cpp
 * @author     Ravi Bhadeshiya
 * @version    2.0
 * @brief      Class for controlling ur10 arm
 *
 * @copyright  BSD 3-Clause License (c) 2018 Ravi Bhadeshiya
 *
 * Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice, this
list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

* Neither the name of the copyright holder nor the names of its contributors may
be used to endorse or promote products derived from this software without
specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#include "project_ariac/UR10_Control.hpp"

UR10_Control::UR10_Control(const ros::NodeHandle& server)
    : ur10_("manipulator"), gripperPickCheck(false), gripperPlaceCheck(false) {
  // init the move group interface
  // Set the planning param
  int planning_attempt;
  double planning_time;
  std::string planner, end_link, base_link, scene_file;
  home_joint_angle_.resize(7);

  server.param("elbow_joint", home_joint_angle_[0], 0.0);
  server.param("linear_arm_actuator_joint", home_joint_angle_[1], 3.0);
  server.param("shoulder_lift_joint", home_joint_angle_[2], -1.0);
  server.param("shoulder_pan_joint", home_joint_angle_[3], 1.9);
  server.param("wrist_1_joint", home_joint_angle_[4], 4.0);
  server.param("wrist_2_joint", home_joint_angle_[5], 4.7);
  server.param("wrist_3_joint", home_joint_angle_[6], 0.0);
  server.param("z_offSet_", z_offSet_, 0.030);
  server.param("planning_time", planning_time, 2.5);
  server.param("planning_attempt", planning_attempt, 20);
  server.param<std::string>("planner", planner, "RRTConnectkConfigDefault");
  server.param<std::string>("end_link", end_link, "/ee_link");
  server.param<std::string>("base_link", base_link, "/world");
  server.param<std::string>("scene", scene_file, " ");

  ur10_.setPlannerId(planner);
  ur10_.setPlanningTime(planning_time);
  ur10_.setNumPlanningAttempts(planning_attempt);
  ur10_.allowReplanning(true);

  move(home_joint_angle_);  // Home condition

  // Find pose of home position
  auto transform = this->getTransfrom(base_link, end_link);
  home_.position.x = transform.getOrigin().x();
  home_.position.y = transform.getOrigin().y();
  home_.position.z = transform.getOrigin().z() - z_offSet_;
  home_.orientation.x = transform.getRotation().x();
  home_.orientation.y = transform.getRotation().y();
  home_.orientation.z = transform.getRotation().z();
  home_.orientation.w = transform.getRotation().w();
  target_ = home_;

  transform = this->getTransfrom("/world", "/agv1_load_point_frame");
  agv_.position.x = transform.getOrigin().x();
  agv_.position.y = transform.getOrigin().y();
  agv_.position.z = transform.getOrigin().z();
  agv_.orientation = home_.orientation;

  // Init the gripper control and feedback
  gripper_ = nh_.serviceClient<osrf_gear::VacuumGripperControl>(
      "/ariac/gripper/control");
  gripper_sensor_ = nh_.subscribe("/ariac/gripper/state/", 10,
                                  &UR10_Control::gripperStatusCallback, this);
  // Pick and place srv init
  // But not possible due to async spinner
  // pickupServer_ =
  //     nh_.advertiseService("ur10_pickup", &UR10_Control::pickupSrvCB, this);
  // placeServer_ =
  //     nh_.advertiseService("ur10_place", &UR10_Control::placeSrvCB, this);

  ros::Duration(0.5).sleep();
}

UR10_Control::~UR10_Control() { ur10_.stop(); }

tf::StampedTransform UR10_Control::getTransfrom(const std::string& src,
                                                const std::string& target) {
  tf::StampedTransform transform;
  tf::TransformListener listener;

  listener.waitForTransform(src, target, ros::Time(0), ros::Duration(20));
  try {
    listener.lookupTransform(src, target, ros::Time(0), transform);
  } catch (tf::TransformException& ex) {
    ROS_ERROR("%s", ex.what());
    ros::Duration(0.5).sleep();
  }

  return transform;
}

void UR10_Control::move(const geometry_msgs::Pose& target) {
  ros::AsyncSpinner spinner(4);
  spinner.start();

  ur10_.setPoseTarget(target);

  ROS_INFO("Planning start");

  bool success = (ur10_.plan(planner_) ==
                  moveit::planning_interface::MoveItErrorCode::SUCCESS);
  if (success) {
    ROS_INFO("Planned success.");
    ur10_.move();
  } else
    ROS_WARN("Planned unsucess!");
}

void UR10_Control::move(const std::vector<double>& target_joint) {
  ros::AsyncSpinner spinner(4);
  spinner.start();
  // std::vector<double> target = {0, 3 , -1, 1.9, 4, 4.7, 0};
  // ur10_.setJointValueTarget(target);
  ur10_.setJointValueTarget(home_joint_angle_);

  ROS_INFO("Planning start");

  bool success = (ur10_.plan(planner_) ==
                  moveit::planning_interface::MoveItErrorCode::SUCCESS);
  if (success) {
    ROS_INFO("Planned success.");
    ur10_.move();
  } else
    ROS_WARN("Planned unsucess!");
}

void UR10_Control::move(const std::vector<geometry_msgs::Pose>& waypoints,
                        double velocity_factor, double eef_step,
                        double jump_threshold) {
  ros::AsyncSpinner spinner(4);
  spinner.start();

  moveit_msgs::RobotTrajectory trajectory;
  ur10_.setMaxAccelerationScalingFactor(velocity_factor);
  ur10_.setMaxVelocityScalingFactor(velocity_factor);

  double fraction = ur10_.computeCartesianPath(waypoints, eef_step,
                                               jump_threshold, trajectory);
  planner_.trajectory_ = trajectory;

  ROS_INFO("UR10 control Move %.2f%% acheived..", fraction * 100.0);

  if (fraction > 0.9) ur10_.execute(planner_);
}

void UR10_Control::gripperAction(const bool action) {
  osrf_gear::VacuumGripperControl srv;

  srv.request.enable = action ? gripper::CLOSE : gripper::OPEN;

  gripper_.call(srv);

  ros::Duration(1).sleep();

  if (srv.response.success)
    ROS_INFO_STREAM("Gripper Action Successfully Exicuted!");
  else
    ROS_ERROR("Gripper Action Failed!");
}

void UR10_Control::gripperStatusCallback(const GripperState& gripper_status) {
  gripper_state_ = gripper_status;
  // ROS_WARN_STREAM("Gripper state:" << gripper_state_.attached);
  // if ((gripperPickCheck && gripper_state_.attached)) ur10_.stop();
}

// TODO: Feedback is not correct
// if part is dropped or picked, then motion will stop
bool UR10_Control::pickup(const geometry_msgs::Pose& target) {
  // Lock the orientation
  target_.position = target.position;
  target_.position.z += z_offSet_;

  move(target_);
  gripperAction(gripper::CLOSE);
  // should stop after part is being picked
  ros::Duration(0.5).sleep();
  gripperPickCheck = true;
  target_.position.z -= 0.8 * z_offSet_;
  move({target_}, 0.1, 0.001);  // Grasp move
  gripperPickCheck = false;
  // Reset speed
  ur10_.setMaxAccelerationScalingFactor(1.0);
  ur10_.setMaxVelocityScalingFactor(1.0);
  // should attach after motion
  // it means, robot pick up part
  ros::spinOnce();
  return gripper_state_.attached;
  // return res.result;
}

bool UR10_Control::place(geometry_msgs::Pose target) {
  // Lock the orientation

  target_.position = target.position;
  target_.position.z += (2.0 * z_offSet_);
  target.position.z += (15.0 * z_offSet_);

  auto waypoints = {home_, target, target_};
  // it will stop the motion,
  // if robot drop part
  gripperPlaceCheck = true;
  // move(home_);
  // move(target);
  move(waypoints);
  gripperPlaceCheck = false;
  // should attach before openning
  // robot didn't drop part
  ros::spinOnce();
  bool result = gripper_state_.attached;
  gripperAction(gripper::OPEN);
  move(home_);
  return result;
}
