/*
 * Copyright (c) 2017-9, Ubiquity Robotics
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation are
 * those of the authors and should not be interpreted as representing official
 * policies, either expressed or implied, of the FreeBSD Project.
 *
 */

#include <ros/ros.h>
#include <tf2/LinearMath/Transform.h>
#include <tf2_ros/buffer.h>
#include <tf2_ros/transform_listener.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>

#include <geometry_msgs/PoseStamped.h>
#include <geometry_msgs/Twist.h>
#include <geometry_msgs/Vector3.h>
#include <nav_msgs/Path.h>
#include <std_msgs/Float32.h>
#include <std_msgs/Bool.h>

#include <dynamic_reconfigure/server.h>
#include <actionlib/server/simple_action_server.h>
#include <move_base_msgs/MoveBaseAction.h>

#include "move_smooth/collision_checker.h"
#include "move_smooth/queued_action_server.h"
#include <move_smooth/MovesmoothConfig.h>
#include <move_smooth/Stop.h>

#include <assert.h>
#include <string>
#include <condition_variable>
#include <mutex>
#include <chrono>

typedef actionlib::QueuedActionServer<move_base_msgs::MoveBaseAction> MoveBaseActionServer;

class MoveBasic {
  private:
    ros::Subscriber goalSub;

    ros::Publisher goalPub;
    ros::Publisher cmdPub;
    ros::Publisher pathPub;
    ros::Publisher obstacle_dist_pub;

    std::unique_ptr<MoveBaseActionServer> actionServer;
    std::unique_ptr<CollisionChecker> collision_checker;
    std::unique_ptr<ObstaclePoints> obstacle_points;

    tf2_ros::Buffer tfBuffer;
    tf2_ros::TransformListener listener;

    std::string preferredDrivingFrame;
    std::string alternateDrivingFrame;
    std::string baseFrame;

    double maxAngularVelocity;
    double maxAngularAcceleration;
    double maxLinearVelocity;
    double maxLinearAcceleration;
    double angleTolerance;

    double maxIncline;
    double gravityConstant;
    double maxLateralDev;

    int goalId;
    bool stop;

    double lateralKp;
    double lateralKi;
    double lateralKd;

    double runawayTimeoutSecs;

    double forwardObstacleThreshold;

    double minSideDist;

    float forwardObstacleDist;
    float leftObstacleDist;
    float rightObstacleDist;
    tf2::Vector3 forwardLeft;
    tf2::Vector3 forwardRight;

    dynamic_reconfigure::Server<move_smooth::MovesmoothConfig> dr_srv;

    void dynamicReconfigCallback(move_smooth::MovesmoothConfig& config, uint32_t level);
    void goalCallback(const geometry_msgs::PoseStamped::ConstPtr& msg);
    void executeAction(const move_base_msgs::MoveBaseGoalConstPtr& goal);
    void sendCmd(double angular, double linear);
    void abortGoal(const std::string msg);

    double limitLinearVelocity(const double& velocity);
    double limitAngularVelocity(const double& velocity);
    bool getTransform(const std::string& from, const std::string& to,
                      tf2::Transform& tf);
    bool transformPose(const std::string& from, const std::string& to,
                       const tf2::Transform& in, tf2::Transform& out);

  public:
    MoveBasic();

    void run();

    bool rotate(double& finalOrientation,
                const std::string& drivingFrame);

    bool smoothFollow(const std::string& drivingFrame,
                      tf2::Transform& goalInDriving);

    bool stopService(move_smooth::Stop::Request &req,
                     move_smooth::Stop::Response &);
};


// Radians to degrees

static double rad2deg(double rad)
{
    return rad * 180.0 / M_PI;
}

// Adjust angle to be between -PI and PI

static void normalizeAngle(double& angle)
{
    if (angle < -M_PI)
        angle += 2 * M_PI;
        assert (angle > -M_PI);

    if (angle > M_PI)
        angle -= 2 * M_PI;
        assert (angle < M_PI);
}

// Get the sign of a number

static int sign(double n)
{
    return (n <0 ? -1 : 1);
}

// retreive the 3 DOF we are interested in from a Transform

static void getPose(const tf2::Transform& tf, double& x, double& y, double& yaw)
{
    tf2::Vector3 trans = tf.getOrigin();
    x = trans.x();
    y = trans.y();

    double roll, pitch;
    tf.getBasis().getRPY(roll, pitch, yaw);
}


// Constructor
MoveBasic::MoveBasic(): tfBuffer(ros::Duration(3.0)),
                        listener(tfBuffer)
{
    ros::NodeHandle nh("~");
    nh.param<double>("max_angular_velocity", maxAngularVelocity, 2.0);
    nh.param<double>("angular_acceleration", maxAngularAcceleration, 5.0);
    nh.param<double>("max_linear_velocity", maxLinearVelocity, 0.5);
    nh.param<double>("linear_acceleration", maxLinearAcceleration, 1.1);
    nh.param<double>("angular_tolerance", angleTolerance, 0.1);

    // Parameters for turn PID
    nh.param<double>("lateral_kp", lateralKp, 0.5);
    nh.param<double>("lateral_ki", lateralKi, 0.0);
    nh.param<double>("lateral_kd", lateralKd, 3.0);

    // To prevent sliping and tipping over when turning
    nh.param<double>("max_incline_without_slipping", maxIncline, 0.01);

    // Maximum lateral deviation from the path
    nh.param<double>("max_lateral_deviation", maxLateralDev, 1.0);

    // Minimum distance to maintain at each side
    nh.param<double>("min_side_dist", minSideDist, 0.3);

    // how long to wait for an obstacle to disappear
    nh.param<double>("forward_obstacle_threshold", forwardObstacleThreshold, 0.5);

    nh.param<double>("runaway_timeout", runawayTimeoutSecs, 1.0);

    nh.param<std::string>("preferred_driving_frame",
                         preferredDrivingFrame, "map");
    nh.param<std::string>("alternate_driving_frame",
                          alternateDrivingFrame, "odom");
    nh.param<std::string>("base_frame", baseFrame, "base_link");

    goalId = 1;
    stop = false;
    gravityConstant = 9.81;

    dynamic_reconfigure::Server<move_smooth::MovesmoothConfig>::CallbackType f;
    f = boost::bind(&MoveBasic::dynamicReconfigCallback, this, _1, _2);
    dr_srv.setCallback(f);

    cmdPub = ros::Publisher(nh.advertise<geometry_msgs::Twist>("/cmd_vel", 1));
    pathPub = ros::Publisher(nh.advertise<nav_msgs::Path>("/plan", 1));

    obstacle_dist_pub =
        ros::Publisher(nh.advertise<geometry_msgs::Vector3>("/obstacle_distance", 1));

    goalSub = nh.subscribe("/move_base_simple/goal", 1,
                            &MoveBasic::goalCallback, this);
    ros::NodeHandle actionNh("");

    actionServer.reset(new MoveBaseActionServer(actionNh, "move_base",
	        boost::bind(&MoveBasic::executeAction, this, _1)));

    actionServer->start();
    goalPub = actionNh.advertise<move_base_msgs::MoveBaseActionGoal>(
      "/move_base/goal", 1);

    obstacle_points.reset(new ObstaclePoints(nh, tfBuffer));
    collision_checker.reset(new CollisionChecker(nh, tfBuffer, *obstacle_points));

    ROS_INFO("Move Smooth ready");
}

// Limit velocities

double MoveBasic::limitLinearVelocity(const double& velocity)
{
    return std::min(maxLinearVelocity, velocity);
}

double MoveBasic::limitAngularVelocity(const double& velocity)
{
    return std::max(-maxAngularVelocity, std::min(maxAngularVelocity, velocity));
}

// Lookup the specified transform, returns true on success

bool MoveBasic::getTransform(const std::string& from, const std::string& to,
                             tf2::Transform& tf)
{
    try {
        geometry_msgs::TransformStamped tfs =
            tfBuffer.lookupTransform(to, from, ros::Time(0));
        tf2::fromMsg(tfs.transform, tf);
        return true;
    }
    catch (tf2::TransformException &ex) {
         return false;
    }
}


// Transform a pose from one frame to another, returns true on success

bool MoveBasic::transformPose(const std::string& from, const std::string& to,
                              const tf2::Transform& in, tf2::Transform& out)
{
    tf2::Transform tf;
    if (!getTransform(from, to, tf))
        return false;

    out = tf * in;
    return true;
}

// Dynamic reconfigure

void MoveBasic::dynamicReconfigCallback(move_smooth::MovesmoothConfig& config, uint32_t){
    maxAngularVelocity = config.max_angular_velocity;
    maxAngularAcceleration = config.max_angular_acceleration;
    maxLinearVelocity = config.max_linear_velocity;
    maxLinearAcceleration = config.max_linear_acceleration;
    lateralKp = config.lateral_kp;
    lateralKi = config.lateral_ki;
    lateralKd = config.lateral_kd;
    minSideDist = config.min_side_dist;
    maxLateralDev = config.max_lateral_dev;
    runawayTimeoutSecs = config.runaway_timeout;
    forwardObstacleThreshold = config.forward_obstacle_threshold;

    ROS_WARN("MoveSmooth: Parameter change detected");
}

// Stop robot in place and save last state

bool MoveBasic::stopService(move_smooth::Stop::Request &req,
                     move_smooth::Stop::Response &)
{
    stop = req.stop;
    if (stop) ROS_WARN("MoveSmooth: Robot is forced to stop!");

    return true;
}

// Called when a simple goal message is received

void MoveBasic::goalCallback(const geometry_msgs::PoseStamped::ConstPtr &msg)
{
    move_base_msgs::MoveBaseActionGoal actionGoal;
    actionGoal.header.stamp = ros::Time::now();
    actionGoal.goal_id.id = std::to_string(goalId);
    goalId = goalId + 1;
    actionGoal.goal.target_pose = *msg;
    goalPub.publish(actionGoal);
}

// Abort goal and print message

void MoveBasic::abortGoal(const std::string msg)
{
    actionServer->setAborted(move_base_msgs::MoveBaseResult(), msg);
    ROS_ERROR("MoveSmooth: %s", msg.c_str());
}


// Called when an action goal is received

void MoveBasic::executeAction(const move_base_msgs::MoveBaseGoalConstPtr& msg)
{
    /*
      It is assumed that we are dealing with imperfect localization data:
         map->base_link is accurate but may be delayed and is at a slow rate
         odom->base_link is frequent, but drifts, particularly after rotating
    */

    tf2::Transform goal;
    tf2::fromMsg(msg->target_pose.pose, goal);
    std::string frameId = msg->target_pose.header.frame_id;

    // Needed for RobotCommander
    if (frameId[0] == '/')
        frameId = frameId.substr(1);

    double x, y, yaw;
    getPose(goal, x, y, yaw);
    ROS_INFO("MoveSmooth: Received goal %f %f %f %s", x, y, rad2deg(yaw), frameId.c_str());
    if (std::isnan(yaw)) {
        abortGoal("MoveSmooth: Aborting goal because an invalid orientation was specified");
        return;
    }

    // Driving frame
    std::string drivingFrame;
    tf2::Transform goalInDriving;
    tf2::Transform currentDrivingBase;
    if (!getTransform(preferredDrivingFrame, baseFrame, currentDrivingBase)) {
         ROS_WARN("MoveSmooth: %s not available, attempting to drive using %s frame",
                  preferredDrivingFrame.c_str(), alternateDrivingFrame.c_str());
         if (!getTransform(alternateDrivingFrame, baseFrame, currentDrivingBase)) {
             abortGoal("MoveSmooth: Cannot determine robot pose in driving frame");
             return;
         }
         else drivingFrame = alternateDrivingFrame;
    }
    else drivingFrame = preferredDrivingFrame;


    // Publish our planned path
    nav_msgs::Path path;
    geometry_msgs::PoseStamped p0, p1;
    path.header.frame_id = frameId;
    p0.pose.position.x = x;
    p0.pose.position.y = y;
    p0.header.frame_id = frameId;
    path.poses.push_back(p0);

    tf2::Transform poseFrameId;
    if (!getTransform(baseFrame, frameId, poseFrameId)) {
         abortGoal("MoveSmooth: Cannot determine robot pose in goal frame");
         return;
    }
    getPose(poseFrameId, x, y, yaw);
    p1.pose.position.x = x;
    p1.pose.position.y = y;
    p1.header.frame_id = frameId;
    path.poses.push_back(p1);

    pathPub.publish(path);

    // Current goal in driving frame
    if (!transformPose(frameId, drivingFrame, goal, goalInDriving)) {
         abortGoal("MoveSmooth: Cannot determine goal pose in driving frame");
         return;
    }

    // Goal orientation in driving frame
    double goalYaw;
    getPose(goalInDriving, x, y, goalYaw);

    tf2::Transform goalInBase = currentDrivingBase * goalInDriving;
    {
       double x, y, yaw;
       getPose(goalInBase, x, y, yaw);
       ROS_INFO("MoveSmooth: Goal in %s  %f %f %f", baseFrame.c_str(),
             x, y, rad2deg(yaw));
    }

    // Driving distance
    tf2::Vector3 linear = goalInBase.getOrigin();
    double requestedDistance = sqrt(linear.x() * linear.x() + linear.y() * linear.y());

    // Send control commands
    double minRequestedDistance = maxLateralDev;
    if (requestedDistance > minRequestedDistance) {
        if (!smoothFollow(drivingFrame, goalInDriving))
            return;
    }
    else {
        abortGoal("MoveSmooth: Aborting due to goal being already close enough.");
        return;
    }

    // Rotate towards final orientation if new goal not available
    if (!actionServer->isNewGoalAvailable()) {
        if (!rotate(goalYaw, drivingFrame)) {
            return;
        }
    }

    actionServer->setSucceeded();
}

// Send a motion command

void MoveBasic::sendCmd(double angular, double linear)
{
   if (stop) { angular = 0; linear = 0; }
   geometry_msgs::Twist msg;
   msg.angular.z = angular;
   msg.linear.x = linear;

   cmdPub.publish(msg);
}


// Main loop

void MoveBasic::run()
{
    ros::Rate r(20);


    while (ros::ok()) {
        ros::spinOnce();
        collision_checker->min_side_dist = minSideDist;
        forwardObstacleDist = collision_checker->obstacle_dist(true,
                                                               leftObstacleDist,
                                                               rightObstacleDist,
                                                               forwardLeft,
                                                               forwardRight);
        geometry_msgs::Vector3 msg;
        msg.x = forwardObstacleDist;
        msg.y = leftObstacleDist;
        msg.z = rightObstacleDist;
        obstacle_dist_pub.publish(msg);

        r.sleep();
    }
}

// On-spot rotation

bool MoveBasic::rotate(double& finalOrientation,
                       const std::string& drivingFrame)
{
    normalizeAngle(finalOrientation);
    double previousAngleRemaining = 0.0;
    double previousAngularVelocity = 0.0;
    int oscillations = 0;

    bool done = false;
    ros::Rate r(50);

    while(!done && ros::ok()){
        ros::spinOnce();
        r.sleep();

        tf2::Transform poseDriving;
        if (!getTransform(drivingFrame, baseFrame, poseDriving)) {
             ROS_WARN("MoveSmooth: Cannot determine robot pose for driving");
             return false;
        }

        double x, y, currentYawInDriving;
        getPose(poseDriving, x, y, currentYawInDriving);
        double angleRemaining = finalOrientation - (-currentYawInDriving);
        normalizeAngle(angleRemaining);

        double obstacle = collision_checker->obstacle_angle(angleRemaining > 0);
        double obstacleAngle = std::min(std::abs(angleRemaining), std::abs(obstacle));

        if (sign(previousAngleRemaining) != sign(angleRemaining))
            oscillations++;

        if (std::abs(angleRemaining) < angleTolerance || oscillations > 2) {
            sendCmd(0, 0);
            ROS_INFO("MoveSmooth: ORIENTATION ERROR ~ yaw: %f degrees", rad2deg(angleRemaining));
            ROS_INFO("MoveSmooth: Goal reached");
            done = true;
        }

        double angularVelocity = limitAngularVelocity(std::sqrt(previousAngularVelocity + 2.0 * maxAngularAcceleration * obstacleAngle));

        if (actionServer->isNewGoalAvailable()) {
            angularVelocity = 0;
            done = true;
        }

        if (actionServer->isPreemptRequested()) {
            ROS_INFO("MoveSmooth: Stopping rotation due to preempt");
            sendCmd(0, 0);
            actionServer->setPreempted();
            return false;
        }

        previousAngleRemaining = angleRemaining;
        previousAngularVelocity = angularVelocity;

        bool counterwise = (angleRemaining < 0.0);
        if (counterwise) {
            angularVelocity = -angularVelocity;
        }

        sendCmd(angularVelocity, 0);
    }

    return done;
}

// Smooth control drive

bool MoveBasic::smoothFollow(const std::string& drivingFrame,
                             tf2::Transform& goalInDriving)
{

    tf2::Transform poseDrivingInitial;
    if (!getTransform(baseFrame, drivingFrame, poseDrivingInitial)) {
         abortGoal("MoveSmooth: Cannot determine robot pose for linear");
         return false;
    }

    tf2::Vector3 linear = (poseDrivingInitial.getOrigin() -
                           goalInDriving.getOrigin());
    linear.setZ(0);
    double requestedDistance = linear.length();

    // Abort check
    ros::Time last = ros::Time::now();
    ros::Duration runawayTimeout(runawayTimeoutSecs);
    double prevDistanceRemaining = requestedDistance;

    // De/Acceleration constraint
    double previousLinearVelocity = 0.0;
    double previousAngularVelocity = 0.0;

    // Lateral control
    double angleRemaining = 0.0;
    double lateralIntegral = 0.0;
    double lateralError = 0.0;
    double prevLateralError = 0.0;
    double lateralDiff = 0.0;

    bool done = false;
    ros::Rate r(50);

    while(!done && ros::ok()){
        ros::spinOnce();
        r.sleep();

        tf2::Transform poseDriving;
        if (!getTransform(drivingFrame, baseFrame, poseDriving)) {
             ROS_WARN("MoveSmooth: Cannot determine robot pose for driving");
             return false;
        }

        // Current goal state in base frame
        tf2::Transform goalInBase = poseDriving * goalInDriving;
        tf2::Vector3 remaining = goalInBase.getOrigin();
        double distRemaining = sqrt(remaining.x() * remaining.x() + remaining.y() * remaining.y());
        angleRemaining = std::atan2(remaining.y(), remaining.x());
        normalizeAngle(angleRemaining);

        // Collision avoidance
        double obstacle = collision_checker->obstacle_angle(angleRemaining > 0);
        double obstacleAngle = std::min(std::abs(angleRemaining), std::abs(obstacle));
        double obstacleDist = forwardObstacleDist;
        if (distRemaining < 0.0) { // Reverse
            obstacleDist = collision_checker->obstacle_dist(false,
                                                    leftObstacleDist,
                                                    rightObstacleDist,
                                                    forwardLeft,
                                                    forwardRight);
        }
        ROS_DEBUG("MoveSmooth: %f L %f, R %f\n",
                forwardObstacleDist, leftObstacleDist, rightObstacleDist);

        bool obstacleDetected = (obstacleDist <= forwardObstacleThreshold);
        if (obstacleDetected) { // Stop if there is an obstacle in the distance we would hit in given time
            sendCmd(0, 0);
            ROS_INFO("MoveSmooth: Waiting for OBSTACLE");
            continue;
        }

        // Preempt check
        if (actionServer->isPreemptRequested()) {
            ROS_INFO("MoveSmooth: Stopping due to preempt request");
            actionServer->setPreempted();
            done = false;
            goto FinishWithStop;
        }

        /* Since we are dealing with imperfect localization we should make
         * sure we are at least runawayTimeout driving away from the goal*/
        double localizationDev = 0.02;
        if (std::cos(angleRemaining) < 0 and (prevDistanceRemaining + localizationDev) < distRemaining) {
            if (ros::Time::now() - last > runawayTimeout) {
                abortGoal("MoveSmooth: Moving away from goal");
                sendCmd(0, 0);
                return false;
            }
        }
        else {
            // Only update time when moving towards the goal
            last = ros::Time::now();
        }
        prevDistanceRemaining = distRemaining;

        /* Finish Check */

        if (distRemaining < maxLateralDev) {
            if (actionServer->isNewGoalAvailable()) { // If next goal available keep up with velocity
                ROS_INFO("MoveSmooth: Intermitent goal reached - ERROR: x: %f meters, y: %f meters",
                        remaining.x(), remaining.y());
                done = true;
                goto FinishWithoutStop;
            }

            ROS_INFO("MoveSmooth: Done linear, error: x: %f meters, y: %f meters", remaining.x(), remaining.y());
            done = true;
            goto FinishWithStop;
        }

        // Linear control
        double maxAngleDev = std::atan2(maxLateralDev, 1.0); // Nominal
        // constrain linear velocity according to maximum angular deviation from path - maxAngleDev[0,PI/2]; angleRemaining[0,PI]
        double angularDevVelocity = std::max((maxAngleDev - (std::abs(angleRemaining) / 2)) / maxAngleDev, 0.0) * maxLinearVelocity;
        double linearAccelerationConstraint = std::sqrt(previousLinearVelocity + 2.0 * maxLinearAcceleration *
                                                                            std::min(obstacleDist, distRemaining));
        double proportionalControl = distRemaining;
        double linearVelocity = limitLinearVelocity(std::min(angularDevVelocity,
                    std::min(proportionalControl, linearAccelerationConstraint)));

        // Lateral control
        lateralError = remaining.y();
        lateralDiff = lateralError - prevLateralError;
        prevLateralError = lateralError;
        lateralIntegral += lateralError;
        double pidAngularVelocity = (lateralKp * lateralError) + (lateralKi * lateralIntegral) + (lateralKd * lateralDiff);
        double angularAccelerationConstraint = std::sqrt(previousAngularVelocity + 2.0 * maxAngularAcceleration * obstacleAngle);
        double angularVelocity = limitAngularVelocity(std::min(pidAngularVelocity, angularAccelerationConstraint));

        // Next goal state
        if (actionServer->isNewGoalAvailable()) {
            move_base_msgs::MoveBaseActionGoal nextGoal;
            nextGoal.goal = *(actionServer->getQueuedGoalState());
            std::string frameIdNext = nextGoal.goal.target_pose.header.frame_id;
            ROS_DEBUG_STREAM(nextGoal);

            // Next goal in driving frame
            tf2::Transform nextGoalInDriving;
            tf2::Transform nextGoalPose;
            tf2::fromMsg(nextGoal.goal.target_pose.pose, nextGoalPose);
            if (!transformPose(frameIdNext, drivingFrame, nextGoalPose, nextGoalInDriving)) {
                 abortGoal("MoveSmooth: Cannot determine next goal pose in driving frame");
                 done = false;
                 goto FinishWithStop;
            }

            // Next goal in base frame
            tf2::Transform nextGoalInBase = poseDriving * nextGoalInDriving;
            tf2::Vector3 nextRemaining = nextGoalInBase.getOrigin();
            double distanceToNextGoal = sqrt(nextRemaining.x() * nextRemaining.x() + nextRemaining.y() * nextRemaining.y());
            double angleToNextGoal = std::atan2(nextRemaining.y(), nextRemaining.x());
            normalizeAngle(angleToNextGoal);

            // Turn algorithm - calculating the maximum allowed speed when cornering in order for the robot not to slip or tip over
            double maxTurnVelocity = sqrt(gravityConstant * maxIncline * maxLateralDev / (1 - cos(angleToNextGoal/2)));
            double nextGoalVelocity = distanceToNextGoal;
            linearVelocity = limitLinearVelocity(std::min(nextGoalVelocity, std::max(linearVelocity, maxTurnVelocity)));
        }

        previousLinearVelocity = linearVelocity;
        previousAngularVelocity = angularVelocity;

        sendCmd(angularVelocity, linearVelocity);
    }
    FinishWithStop:
        sendCmd(0, 0);

    FinishWithoutStop:

    return done;
}


int main(int argc, char ** argv) {
    ros::init(argc, argv, "move_basic");
    MoveBasic mb_node;

    ros::NodeHandle nh;
    ros::ServiceServer stop_service = nh.advertiseService("stop_move", &MoveBasic::stopService, (MoveBasic*) &mb_node);
    mb_node.run();

    return 0;
}
