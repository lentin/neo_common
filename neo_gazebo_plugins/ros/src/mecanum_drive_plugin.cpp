/*
 *  Gazebo - Outdoor Multi-Robot Simulator
 *  Copyright (C) 2003
 *     Nate Koenig & Andrew Howard
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */
/*
 * Desc: ROS interface to a Position2d controller for a mecanum drive.
 * Author: Timo Hackel(adapted from  Daniel Hewlett)
 */

#include <algorithm>
#include <assert.h>

#include <neo_gazebo_plugins/mecanum_drive_plugin.h>

#include <gazebo/gazebo.h>
#include <gazebo/GazeboError.hh>
#include <gazebo/Quatern.hh>
#include <gazebo/Controller.hh>
#include <gazebo/Body.hh>
#include <gazebo/World.hh>
#include <gazebo/Simulator.hh>
#include <gazebo/Global.hh>
#include <gazebo/XMLConfig.hh>
#include <gazebo/ControllerFactory.hh>
#include <gazebo/PhysicsEngine.hh>

#include <ros/ros.h>
#include <tf/transform_broadcaster.h>
#include <tf/transform_listener.h>
#include <geometry_msgs/Twist.h>
#include <nav_msgs/Odometry.h>
#include <boost/bind.hpp>

using namespace gazebo;

GZ_REGISTER_DYNAMIC_CONTROLLER("mecanum_drive_plugin", MecanumDrivePlugin);

enum
{
  FRONT_RIGHT, FRONT_LEFT, BACK_RIGHT, BACK_LEFT
};

// Constructor
MecanumDrivePlugin::MecanumDrivePlugin(Entity *parent) :
  Controller(parent)
{
  parent_ = dynamic_cast<Model*> (parent);

  if (!parent_)
    gzthrow("Mecanum_Position2d controller requires a Model as its parent");

  enableMotors = true;

  wheelSpeed[FRONT_RIGHT] = 0;
  wheelSpeed[FRONT_LEFT] = 0;
  wheelSpeed[BACK_RIGHT] = 0;
  wheelSpeed[BACK_LEFT] = 0;

  prevUpdateTime = Simulator::Instance()->GetSimTime();

  Param::Begin(&parameters);
  frontLeftJointNameP = new ParamT<std::string> ("frontLeftJoint", "", 1);
  frontRightJointNameP = new ParamT<std::string> ("frontRightJoint", "", 1);
  backLeftJointNameP = new ParamT<std::string> ("backLeftJoint", "", 1);
  backRightJointNameP = new ParamT<std::string> ("backRightJoint", "", 1);
  robotLength = new ParamT<float> ("robotLength", 0.25, 1);
  robotWidth = new ParamT<float> ("robotLength", 0.27, 1);
  wheelDiamP = new ParamT<float> ("wheelDiameter", 0.15, 1);
  torqueP = new ParamT<float> ("torque", 10.0, 1);
  robotNamespaceP = new ParamT<std::string> ("robotNamespace", "", 0);
  topicNameP = new ParamT<std::string> ("topicName", "", 1);
  Param::End();

  x_ = 0;
  rot_ = 0;
  alive_ = true;
}

// Destructor
MecanumDrivePlugin::~MecanumDrivePlugin()
{
  delete frontLeftJointNameP;
  delete frontRightJointNameP;
  delete backLeftJointNameP;
  delete backRightJointNameP;
  delete robotLength;
  delete robotWidth;
  delete wheelDiamP;
  delete torqueP;
  delete robotNamespaceP;
  delete topicNameP;
  delete callback_queue_thread_;
  delete rosnode_;
  delete transform_broadcaster_;
}

// Load the controller
void MecanumDrivePlugin::LoadChild(XMLConfigNode *node)
{
  pos_iface_ = dynamic_cast<libgazebo::PositionIface*> (GetIface("position"));

  // the defaults are from pioneer2dx
  frontLeftJointNameP->Load(node);
  frontRightJointNameP->Load(node);
  backLeftJointNameP->Load(node);
  backRightJointNameP->Load(node);
  robotLength->Load(node);
  robotWidth->Load(node);
  wheelDiamP->Load(node);
  torqueP->Load(node);

  joints[FRONT_LEFT] = parent_->GetJoint(**frontLeftJointNameP);
  joints[FRONT_RIGHT] = parent_->GetJoint(**frontRightJointNameP);
  joints[BACK_LEFT] = parent_->GetJoint(**backLeftJointNameP);
  joints[BACK_RIGHT] = parent_->GetJoint(**backRightJointNameP);

  if (!joints[FRONT_LEFT])
    gzthrow("The controller couldn't get FRONT LEFT hinge joint");

  if (!joints[FRONT_RIGHT])
    gzthrow("The controller couldn't get FRONT RIGHT hinge joint");
  if (!joints[BACK_LEFT])
    gzthrow("The controller couldn't get BACK LEFT hinge joint");

  if (!joints[BACK_RIGHT])
    gzthrow("The controller couldn't get BACK RIGHT hinge joint");
  // Initialize the ROS node and subscribe to cmd_vel

  robotNamespaceP->Load(node);
  robotNamespace = robotNamespaceP->GetValue();

  int argc = 0;
  char** argv = NULL;
  ros::init(argc, argv, "mecanum_drive_plugin", ros::init_options::NoSigintHandler | ros::init_options::AnonymousName);
  rosnode_ = new ros::NodeHandle(robotNamespace);
  ROS_INFO("starting mecanumdrive plugin in ns: %s", this->robotNamespace.c_str());
  
  tf_prefix_ = tf::getPrefixParam(*rosnode_);
  transform_broadcaster_ = new tf::TransformBroadcaster();

  topicNameP->Load(node);
  topicName = topicNameP->GetValue();

  // ROS: Subscribe to the velocity command topic (usually "cmd_vel")
  ros::SubscribeOptions so =
      ros::SubscribeOptions::create<geometry_msgs::Twist>(topicName, 1,
                                                          boost::bind(&MecanumDrivePlugin::cmdVelCallback, this, _1),
                                                          ros::VoidPtr(), &queue_);
  sub_ = rosnode_->subscribe(so);
  pub_ = rosnode_->advertise<nav_msgs::Odometry>("odom", 1);
  pub_model_state = rosnode_->advertise<gazebo::ModelState>("/gazebo/set_model_state",1);
}

// Initialize the controller
void MecanumDrivePlugin::InitChild()
{
  // Reset odometric pose
  odomPose[0] = 0.0;
  odomPose[1] = 0.0;
  odomPose[2] = 0.0;

  odomVel[0] = 0.0;
  odomVel[1] = 0.0;
  odomVel[2] = 0.0;

  callback_queue_thread_ = new boost::thread(boost::bind(&MecanumDrivePlugin::QueueThread, this));
}

// Load the controller
void MecanumDrivePlugin::SaveChild(std::string &prefix, std::ostream &stream)
{
  stream << prefix << *(frontLeftJointNameP) << "\n";
  stream << prefix << *(frontRightJointNameP) << "\n";
  stream << prefix << *(backLeftJointNameP) << "\n";
  stream << prefix << *(backRightJointNameP) << "\n";
  stream << prefix << *(torqueP) << "\n";
  stream << prefix << *(wheelDiamP) << "\n";
  stream << prefix << *(robotLength) << "\n";
  stream << prefix << *(robotWidth) << "\n";
}

// Reset
void MecanumDrivePlugin::ResetChild()
{
  // Reset odometric pose
  odomPose[0] = 0.0;
  odomPose[1] = 0.0;
  odomPose[2] = 0.0;

  odomVel[0] = 0.0;
  odomVel[1] = 0.0;
  odomVel[2] = 0.0;
}

// Update the controller
void MecanumDrivePlugin::UpdateChild()
{
  // TODO: Step should be in a parameter of this function
  double wd, rl, rw;
  double d1, d2, d3, d4;
  double vx, vy,va;
  Time stepTime;

  //myIface->Lock(1);

  GetPositionCmd();
  rl = **(robotLength);
  rw = **(robotWidth);
  wd = **(wheelDiamP);

  //stepTime = World::Instance()->GetPhysicsEngine()->GetStepTime();
  stepTime = Simulator::Instance()->GetSimTime() - prevUpdateTime;
  prevUpdateTime = Simulator::Instance()->GetSimTime();

  // Distance travelled by wheel
  d1 = wd / 2 * joints[FRONT_LEFT]->GetVelocity(0);
  d2 = wd / 2 * joints[FRONT_RIGHT]->GetVelocity(0);
  d3 = wd / 2 * joints[BACK_LEFT]->GetVelocity(0);
  d4 = wd / 2 * joints[BACK_RIGHT]->GetVelocity(0);

  vx = (d1 + d2 + d3 + d4) / 4;
  vy = (d1 - d2 - d3 + d4) / 4;
  va = 1 / (rl + rw) * (-d1 + d2 - d3 + d4) / 4 ;

  // Compute odometric pose
  odomPose[0] += (vx * cos(odomPose[2]) - vy * sin(odomPose[2])) * stepTime.Double();
  odomPose[1] += (vx * sin(odomPose[2]) + vy * cos(odomPose[2])) * stepTime.Double();
  odomPose[2] += va * stepTime.Double();

  // Compute odometric instantaneous velocity
  odomVel[0] = vx;
  odomVel[1] = vy;
  odomVel[2] = va;

  if (enableMotors)
  {
    joints[FRONT_LEFT]->SetVelocity(0, wheelSpeed[FRONT_LEFT]);
    joints[FRONT_RIGHT]->SetVelocity(0, wheelSpeed[FRONT_RIGHT]);
    joints[BACK_LEFT]->SetVelocity(0, wheelSpeed[BACK_LEFT]);
    joints[BACK_RIGHT]->SetVelocity(0, wheelSpeed[BACK_RIGHT]);

    joints[FRONT_LEFT]->SetMaxForce(0, **(torqueP));
    joints[FRONT_RIGHT]->SetMaxForce(0, **(torqueP));
    joints[BACK_LEFT]->SetMaxForce(0, **(torqueP));
    joints[BACK_RIGHT]->SetMaxForce(0, **(torqueP));
  }

  write_position_data();
  publish_odometry();

  //myIface->Unlock();
}

// Finalize the controller
void MecanumDrivePlugin::FiniChild()
{
  //std::cout << "ENTERING FINALIZE\n";
  alive_ = false;
  // Custom Callback Queue
  queue_.clear();
  queue_.disable();
  rosnode_->shutdown();
  callback_queue_thread_->join();
  //std::cout << "EXITING FINALIZE\n";
}

// NEW: Now uses the target velocities from the ROS message, not the Iface 
void MecanumDrivePlugin::GetPositionCmd()
{
  lock.lock();
  double rl = **(robotLength);
  double rw = **(robotWidth);
  double wd = **(wheelDiamP);
  double vx, vy, va;
  double l_total = rl + rw;

  vx = x_; //myIface->data->cmdVelocity.pos.x;
  vy = y_;
  va = rot_; //myIface->data->cmdVelocity.yaw;

  //std::cout << "X: [" << x_ << "] ROT: [" << rot_ << "]" << std::endl;

  // Changed motors to be always on, which is probably what we want anyway
  enableMotors = true; //myIface->data->cmdEnableMotors > 0;

  //std::cout << enableMotors << std::endl;
  wheelSpeed[FRONT_LEFT] = 	(vx + vy - l_total * va) * 2 / wd;
  wheelSpeed[FRONT_RIGHT] = 	(vx - vy + l_total * va) * 2 / wd;
  wheelSpeed[BACK_LEFT] = 	(vx - vy - l_total * va) * 2 / wd;
  wheelSpeed[BACK_RIGHT] = 	(vx + vy + l_total * va) * 2 / wd;

  lock.unlock();
}

// NEW: Store the velocities from the ROS message
void MecanumDrivePlugin::cmdVelCallback(const geometry_msgs::Twist::ConstPtr& cmd_msg)
{
  //std::cout << "BEGIN CALLBACK\n";

  lock.lock();

  x_ = cmd_msg->linear.x;
  y_ = cmd_msg->linear.y;
  rot_ = cmd_msg->angular.z;

  lock.unlock();

  //std::cout << "END CALLBACK\n";
}

// NEW: custom callback queue thread
void MecanumDrivePlugin::QueueThread()
{
  static const double timeout = 0.01;

  while (alive_ && rosnode_->ok())
  {
    //    std::cout << "CALLING STUFF\n";
    queue_.callAvailable(ros::WallDuration(timeout));
  }
}

void MecanumDrivePlugin::publish_odometry()
{
  ros::Time current_time = ros::Time::now();
  std::string odom_frame = tf::resolve(tf_prefix_, "odom");
  std::string base_footprint_frame = tf::resolve(tf_prefix_, "base_link");
  
  // getting data for base_footprint to odom transform
  btQuaternion qt;
  qt.setRPY(pos_iface_->data->pose.roll,
            pos_iface_->data->pose.pitch,
            pos_iface_->data->pose.yaw);
  
  btVector3 vt(pos_iface_->data->pose.pos.x,
               pos_iface_->data->pose.pos.y,
               pos_iface_->data->pose.pos.z);
  
  tf::Transform base_footprint_to_odom(qt, vt);
  transform_broadcaster_->sendTransform(tf::StampedTransform(base_footprint_to_odom,
                                                             current_time,
                                                             odom_frame,
                                                             base_footprint_frame));

  // publish odom topic
  odom_.pose.pose.position.x = pos_iface_->data->pose.pos.x;
  odom_.pose.pose.position.y = pos_iface_->data->pose.pos.y;

  gazebo::Quatern rot;
  rot.SetFromEuler(gazebo::Vector3(pos_iface_->data->pose.roll,
                                   pos_iface_->data->pose.pitch,
                                   pos_iface_->data->pose.yaw));

  odom_.pose.pose.orientation.x = rot.x;
  odom_.pose.pose.orientation.y = rot.y;
  odom_.pose.pose.orientation.z = rot.z;
  odom_.pose.pose.orientation.w = rot.u;

  odom_.twist.twist.linear.x = pos_iface_->data->velocity.pos.x;
  odom_.twist.twist.linear.y = pos_iface_->data->velocity.pos.y;
  odom_.twist.twist.angular.z = pos_iface_->data->velocity.yaw;

  odom_.header.stamp = current_time;
  odom_.header.frame_id = odom_frame;
  odom_.child_frame_id = base_footprint_frame;

  gazebo::ModelState modelState;
  modelState.pose = odom_.pose.pose;
  modelState.twist = odom_.twist.twist;
  modelState.model_name = "robot_description"; //TODO: get "robot_description" as parameter
  pub_model_state.publish(modelState);
  pub_.publish(odom_);
}

// Update the data in the interface
void MecanumDrivePlugin::write_position_data()
{
  // TODO: Data timestamp
  pos_iface_->data->head.time = Simulator::Instance()->GetSimTime().Double();

  pos_iface_->data->pose.pos.x = odomPose[0];
  pos_iface_->data->pose.pos.y = odomPose[1];
  pos_iface_->data->pose.yaw = NORMALIZE(odomPose[2]);


  pos_iface_->data->velocity.pos.x = odomVel[0];
  pos_iface_->data->velocity.pos.y = odomVel[1];
  pos_iface_->data->velocity.yaw = odomVel[2];

  // TODO
  pos_iface_->data->stall = 0;
}

