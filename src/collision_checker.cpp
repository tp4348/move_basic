/*
 * Copyright (c) 2018-9, Ubiquity Robotics
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

/*

 The `CollisionChecker` class processes range messages to determine the
 distance to obstacles.  As range messages are received, they are used
 to either create or update a `RangeSensor` object.  `RangeSensor` objects
 are created by looking up the transform from `base_link` to their frame
 and computing a pair of vectors corresponding to the sides of their
 cone.

 `LaserScan` messages from lidar sensors are also processed. If such data is
 received, the transform from `base_link` to the laser's frame is looked up
 to determine the scanner's position.

 The distance to the closest object is calculated based upon the positions
 of the end points of the sensors' cones.  For this purpose, the robot
 footprint is paramatized as having width `robot_width` either side of
 base_link, and length `robot_front_length` forward of base_link and length
 `robot_back_length` behind it.  This could be extended to be an abitrary
 polygon.  When the robot is travelling forward or backwards, the distance
 to the closest point that has an `x` value between `-robot_width` and
 `robot_width` is used as the obstacle distance.

 In the case of rotation in place, the angle that the robot will rotate
 before hitting an obstacle is determined. This is done by converting
 each of the `(x,y)` points into `(r, theta)` and determining how much
 `theta` would change by for the point to intersect with one of the four
 line segments representing the robot footprint.

 Coordinate systems are as specified in http://www.ros.org/reps/rep-0103.html
 x forward, y left

 Jim Vaughan <jimv@mrjim.com> January 2018

*/

#include <tf2/LinearMath/Transform.h>
#include <tf2_ros/buffer.h>
#include <tf2_ros/transform_listener.h>
#include <sensor_msgs/Range.h>
#include <visualization_msgs/Marker.h>
#include "move_smooth/collision_checker.h"


CollisionChecker::CollisionChecker(ros::NodeHandle& nh, tf2_ros::Buffer &tf_buffer, 
		                   ObstaclePoints& op) : tf_buffer(tf_buffer),
	                                                 ob_points(op)
{
    nh.param<std::string>("base_frame", baseFrame, "base_link");

    line_pub = ros::Publisher(
                 nh.advertise<visualization_msgs::Marker>("/obstacle_viz", 10));

    max_age = nh.param<float>("max_age", 1.0);
    no_obstacle_dist = nh.param<float>("no_obstacle_dist", 10.0);

    // Footprint
    robot_width = nh.param<float>("robot_width", 0.08);
    robot_front_length = nh.param<float>("robot_front_length", 0.09);
    robot_back_length = nh.param<float>("robot_back_length", 0.19);

    robot_width_sq = robot_width * robot_width;
    robot_front_length_sq = robot_front_length * robot_front_length;
    robot_back_length_sq = robot_back_length * robot_back_length;

    // To test if obstacles will intersect when rotating
    front_diag = robot_width*robot_width + robot_front_length*robot_front_length;
    back_diag = robot_width*robot_width + robot_back_length*robot_back_length;

}

void CollisionChecker::draw_line(const tf2::Vector3 &p1, const tf2::Vector3 &p2,
                            float r, float g, float b, int id)
{
    visualization_msgs::Marker line;
    line.type = visualization_msgs::Marker::LINE_LIST;
    line.action = visualization_msgs::Marker::MODIFY;
    line.header.frame_id = baseFrame;
    line.color.r = r;
    line.color.g = g;
    line.color.b = b;
    line.color.a = 1.0f;
    line.id = id;
    line.scale.x = line.scale.y = line.scale.z = 0.01;
    line.pose.position.x = 0;
    line.pose.position.y = 0;
    line.pose.orientation.w = 1;
    geometry_msgs::Point gp1, gp2;
    gp1.x = p1.x();
    gp1.y = p1.y();
    gp1.z = p1.z();
    gp2.x = p2.x();
    gp2.y = p2.y();
    gp2.z = p2.z();
    line.points.push_back(gp1);
    line.points.push_back(gp2);
    line_pub.publish(line);
}

void CollisionChecker::clear_line(int id)
{
    visualization_msgs::Marker line;
    line.type = visualization_msgs::Marker::LINE_LIST;
    line.action = visualization_msgs::Marker::DELETE;
    line.id = id;
    line_pub.publish(line);
}

inline void CollisionChecker::check_dist(float x, bool forward, float& min_dist) const
{
    if (forward && x > robot_front_length) {
        if (x < min_dist) {
            min_dist = x;
        }
    }
    if (!forward && -x > robot_back_length ) {
        if (-x < min_dist) {
            min_dist = -x;
        }
    }
}

float CollisionChecker::obstacle_dist(bool forward,
                                      float &min_dist_left,
                                      float &min_dist_right,
                                      tf2::Vector3 &fl,
                                      tf2::Vector3 &fr)
{
    float min_dist = no_obstacle_dist;
    min_dist_left = no_obstacle_dist;
    min_dist_right = no_obstacle_dist;

    auto lines = ob_points.get_lines(ros::Duration(max_age));
    for (const auto& points : lines) {
	float x0 = points.first.x();
	float y0 = points.first.y();
	float x1 = points.second.x();
	float y1 = points.second.y();
	// Forward and rear limits
	if (y0 < -robot_width && robot_width < y1) {
	    // linear interpolate to get closest point inside width
	    float ylen = y1 - y0;
	    float a0 = (y0 - robot_width) / ylen;
	    float a1 = (y1 - robot_width - y0) / ylen;
	    check_dist(a0 * x0 + (1.0 - a0) * x1, forward, min_dist);
	    check_dist(a1 * x1 + (1.0 - a1) * x0, forward, min_dist);
	}
	else if (y1 < -robot_width && robot_width < y0) {
	    // linear interpolate to get closest point inside width
	    float ylen = y0 - y1;
	    float a0 = (y0 - robot_width - y1) / ylen;
	    float a1 = (y1 - robot_width) / ylen;
	    check_dist(a0 * x0 + (1.0 - a0) * x1, forward, min_dist);
	    check_dist(a1 * x1 + (1.0 - a1) * x0, forward, min_dist);
	}
	else {
	    if (-robot_width < y0 && y0 < robot_width) {
		check_dist(x0, forward, min_dist);
	    }
	    if (-robot_width < y1 && y1 < robot_width) {
		check_dist(x1, forward, min_dist);
	    }
	}
	// Sides
	if (x0 < -robot_back_length && x1 > robot_front_length) {
	    // linear interpolate to get closest point in side
	    float xlen = x1 - x0;
	    float ab = (-x0 - robot_back_length) / xlen;
	    float af = (x1 - robot_front_length - x0) / xlen;
	    float yb = ab * y0 + (1.0 - ab) * y1;
	    float yf = af * y1 + (1.0 - af) * y0;
	    if (yb > 0 && yb < min_dist_left) {
		min_dist_left = yb;
	    }
	    if (yb < 0 && -yb < min_dist_right) {
		min_dist_right = -yb;
	    }
	    if (yf> 0 && yf < min_dist_left) {
		min_dist_left = yf;
	    }
	    if (yf < 0 && -yf < min_dist_right) {
		min_dist_right = -yf;
	    }
	}
	else if (x1 < -robot_back_length && x0 > robot_front_length) {
	    // linear interpolate to get closest point in side
	    float xlen = x0 - x1;
	    float ab = (-x1 - robot_back_length) / xlen;
	    float af = (x0 - robot_front_length - x1) / xlen;
	    float yb = ab * y1 + (1.0 - ab) * y0;
	    float yf = af * y0 + (1.0 - af) * y1;
	    if (yb > 0 && yb < min_dist_left) {
		min_dist_left = yb;
	    }
	    if (yb < 0 && -yb < min_dist_right) {
		min_dist_right = -yb;
	    }
	    if (yf> 0 && yf < min_dist_left) {
		min_dist_left = yf;
	    }
	    if (yf < 0 && -yf < min_dist_right) {
		min_dist_right = -yf;
	    }
	}
	else {
	    if (x0 > -robot_back_length && x0 < robot_front_length) {
		if (y0 > 0 && y0 < min_dist_left) {
		    min_dist_left = y0;
		}
		if (y0 < 0 && -y0 < min_dist_right) {
		    min_dist_right = -y0;
		}
	    }
	    if (x1 > -robot_back_length && x1 < robot_front_length) {
		if (y1> 0 && y1 < min_dist_left) {
		    min_dist_left = y1;
		}
		if (y1 < 0 && -y1 < min_dist_right) {
		    min_dist_right = -y1;
		}
	    }
	}
    }

    // Forward side points
    fl.setX(robot_front_length);
    fl.setY(min_dist_left);
    fr.setX(robot_front_length);
    fr.setY(min_dist_right);
    
    auto pts = ob_points.get_points(ros::Duration(max_age));
    for (const auto& p : pts) {
       float y = p.y();
       float x = p.x();
       // Forward and rear
       if (-robot_width < y && y < robot_width) {
          check_dist(x, forward, min_dist);
       }
       // Sides
       if (x > -robot_back_length && x < robot_front_length) {
          if (y > 0 && y < min_dist_left) {
	     min_dist_left = y;
	  }
	  else if (y < 0 && -y < min_dist_right) {
             min_dist_right = -y;
	  }
       }
    }

    // Green lines at sides
    draw_line(tf2::Vector3(robot_front_length, min_dist_left, 0),
              tf2::Vector3(-robot_back_length, min_dist_left, 0), 0, 1, 0, 20000);
    draw_line(tf2::Vector3(robot_front_length, min_dist_left, 0),
              tf2::Vector3(robot_front_length + 2, min_dist_left, 0), 0, 0.5, 0, 20001);

    draw_line(tf2::Vector3(robot_front_length, -min_dist_right, 0),
              tf2::Vector3(-robot_back_length, -min_dist_right, 0), 0, 1, 0, 20002);

    draw_line(tf2::Vector3(robot_front_length, -min_dist_right, 0),
              tf2::Vector3(robot_front_length + 2, -min_dist_right, 0), 0, 0.5, 0, 20003);
 
    // Blue
    draw_line(tf2::Vector3(robot_front_length, min_dist_left, 0),
              tf2::Vector3(fl.x(), fl.y(), 0), 0, 0, 1, 30000);

    draw_line(tf2::Vector3(robot_front_length, -min_dist_right, 0),
              tf2::Vector3(fr.x(), -fr.y(), 0), 0, 0, 1, 30001);

    // Min side dist
    draw_line(tf2::Vector3(robot_front_length, -robot_width -min_side_dist, 0),
              tf2::Vector3(robot_front_length + 2, -robot_width -min_side_dist, 0), 0.5, 0.5, 0, 40001);

    draw_line(tf2::Vector3(robot_front_length, robot_width+min_side_dist, 0),
              tf2::Vector3(robot_front_length + 2, robot_width+min_side_dist, 0), 0.5, 0.5, 0, 40002);

    // Red line at front or back
    if (forward) {
        draw_line(tf2::Vector3(min_dist, -robot_width, 0),
                  tf2::Vector3(min_dist, robot_width, 0), 1, 0, 0, 10000);

        draw_line(tf2::Vector3(min_dist, -robot_width - 2, 0),
                  tf2::Vector3(min_dist, -robot_width, 0), 0.5, 0, 0, 10020);

        draw_line(tf2::Vector3(min_dist, robot_width + 2, 0),
                  tf2::Vector3(min_dist, robot_width, 0), 0.5, 0, 0, 10030);
        min_dist -= robot_front_length;
    }
    else {
        draw_line(tf2::Vector3(-min_dist, -robot_width, 0),
                  tf2::Vector3(-min_dist, robot_width, 0), 1, 0, 0, 10000);
        min_dist -= robot_back_length;
    }

    min_dist_left -= robot_width;
    min_dist_right -= robot_width;
    return min_dist;
}

float CollisionChecker::degrees(float radians) const
{
    return radians * 180.0 / M_PI;
}

/*
 Determine the rotation required to for to move point from its
 initial rotation of theta to (x, y), and store the smallest
 value
*/
inline void CollisionChecker::check_angle(float theta, float x, float y,
                                          bool left, float& min_dist) const
{
    float theta_int = theta - std::atan2(y, x);
    if (theta_int < -M_PI) {
        theta_int += 2.0 * M_PI;
    }
    if (theta_int > M_PI) {
        theta_int -= 2.0 * M_PI;
    }
    if (left && theta_int > 0 && theta_int < min_dist) {
        min_dist = theta_int;
    }
    if (!left && theta_int < 0 && -theta_int < min_dist) {
        min_dist = -theta_int;
    }
}

float CollisionChecker::obstacle_angle(bool left)
{
    float min_angle = M_PI;

    auto points = ob_points.get_points(ros::Duration(max_age));
    // draw footprint
    draw_line(tf2::Vector3(robot_front_length, robot_width, 0),
              tf2::Vector3(-robot_back_length, robot_width, 0),
              0.28, 0.5, 1, 10003);
    draw_line(tf2::Vector3(robot_front_length, -robot_width, 0),
              tf2::Vector3(-robot_back_length, -robot_width, 0),
              0.28, 0.5, 1, 10004);
    draw_line(tf2::Vector3(robot_front_length, robot_width, 0),
              tf2::Vector3(robot_front_length, -robot_width, 0),
              0.28, 0.5, 1, 10005);
    draw_line(tf2::Vector3(-robot_back_length, robot_width, 0),
              tf2::Vector3(-robot_back_length, -robot_width, 0),
              0.28, 0.5, 1, 10006);

    for (const auto& p : points) {
        float x = p.x();
        float y = p.y();
        // initial orientation wrt base_link
        float theta = std::atan2(y, x);
        float r_squared = x*x + y*y;
        if (r_squared <= back_diag) {
           // left line segment:
           //   y = robot_width, -robot_back_length <= x <= robot_front_length
           // right line segment:
           //   y = -robot_width, -robot_back_length <= x <= robot_front_length
           if (robot_width_sq <= r_squared) {
               float xi = std::sqrt(r_squared - robot_width_sq);
               if (-robot_back_length <= xi && xi <= robot_front_length) {
                   check_angle(theta, xi, robot_width, left, min_angle);
                   check_angle(theta, xi, -robot_width, left, min_angle);
               }
               if (-robot_back_length <= -xi && -xi <= robot_front_length) {
                   check_angle(theta, -xi, robot_width, left, min_angle);
                   check_angle(theta, -xi, -robot_width, left, min_angle);
               }
           }

           // back line segment:
           //   x = -robot_back_length, -robot_width <= y <= robot_width
           if (x < 0 && robot_back_length_sq <= r_squared) {
               float yi = std::sqrt(r_squared - robot_back_length_sq);
               if (-robot_width <= yi && yi <= robot_width) {
                   check_angle(theta, -robot_back_length, yi, left, min_angle);
               }
               if (-robot_width <= -yi && -yi <= robot_width) {
                   check_angle(theta, -robot_back_length, -yi, left, min_angle);
               }
           }

           // front line segment:
           //   x = robot_front_length, -robot_width <= y <= robot_width
           if (x > 0 && r_squared <= front_diag && robot_front_length_sq <= r_squared) {
               float yi = std::sqrt(r_squared - robot_front_length_sq);
               if (-robot_width <= yi && yi <= robot_width) {
                   check_angle(theta, robot_front_length, yi, left, min_angle);
               }
               if (-robot_width <= -yi && -yi <= robot_width) {
                   check_angle(theta, robot_front_length, -yi, left, min_angle);
               }
           }
        }
    }

    // Draw rotated footprint to show limit of rotation
    float rotation;
    if (left) {
        rotation = min_angle;
    }
    else {
        rotation = -min_angle;
    }
    float sin_theta = std::sin(rotation);
    float cos_theta = std::cos(rotation);


    if (std::abs(min_angle) < M_PI) {
        float x_fl = robot_front_length * cos_theta - robot_width * sin_theta;
        float y_fl = robot_front_length * sin_theta + robot_width * cos_theta;
        float x_fr = robot_front_length * cos_theta + robot_width * sin_theta;
        float y_fr = robot_front_length * sin_theta - robot_width * cos_theta;
        float x_bl = -robot_back_length * cos_theta - robot_width * sin_theta;
        float y_bl = -robot_back_length * sin_theta + robot_width * cos_theta;
        float x_br = -robot_back_length * cos_theta + robot_width * sin_theta;
        float y_br = -robot_back_length * sin_theta - robot_width * cos_theta;
        draw_line(tf2::Vector3(x_fl, y_fl, 0), tf2::Vector3(x_bl, y_bl, 0),
                  1, 0, 0, 10010);
        draw_line(tf2::Vector3(x_bl, y_bl, 0), tf2::Vector3(x_br, y_br, 0),
                  1, 0, 0, 10011);
        draw_line(tf2::Vector3(x_br, y_br, 0), tf2::Vector3(x_fr, y_fr, 0),
                  1, 0, 0, 10012);
        draw_line(tf2::Vector3(x_fr, y_fr, 0), tf2::Vector3(x_fl, y_fl, 0),
                  1, 0, 0, 10013);
    }
    else {
        clear_line(10010);
        clear_line(10011);
        clear_line(10012);
        clear_line(10013);
    }

    ROS_DEBUG("min angle %f\n", degrees(min_angle));
    return min_angle;
}


float CollisionChecker::obstacle_arc_angle(double linear, double angular) {
    const float radius = (float) std::abs(linear/angular);
    const bool forward = linear >= 0;
    const bool left = angular >= 0;

    // Point of rotation relative to base_link
    const auto point_of_rotation = tf2::Vector3(0, (left) ? radius : -radius, 0);

    // Critical robot corners relative to point of rotation
    const auto outer_point = tf2::Vector3(-robot_back_length, 
            (left) ? -robot_width: robot_width, 0) - point_of_rotation;
    const auto inner_point = tf2::Vector3(robot_front_length, 
            (left) ? robot_width: -robot_width, 0) - point_of_rotation;

    // Critical robot points in polar (r^2, theta) form relative to center 
    // of rotation
    const float outer_radius_sq = outer_point.length2();
    const float outer_theta = std::atan2(outer_point.y(), outer_point.x());
    const float inner_radius_sq = inner_point.length2();
    //Not used const float inner_theta = std::atan2(inner_point.x(), -inner_point.y());

    // Utility function to make sure that the angle relevant and not behind the robot
    const auto angle_relevant = [&outer_theta, &left, &forward](float angle) -> bool {
        if (forward) {
            if (left) {
                return angle > outer_theta;
            } else {
                return angle < outer_theta;
            }
        } else {
            if (angle < 0) {
                angle += 2.0 * M_PI;
            }

            if (left) {
                return angle < outer_theta;
            } else {
                return angle > outer_theta;
            }
        }
    };

    float closest_angle = M_PI;
    const auto points = ob_points.get_points(ros::Duration(max_age));
    for (const auto& p : points) {
        // Trasform the obstacle point into the coordiate system with the 
        // point of rotation at the origin, with the same orientation as base_link
        const tf2::Vector3 p_in_rot = p - point_of_rotation;
        // Radius for polar coordinates around center of rotation 
        const float p_radius_sq = p_in_rot.length2();

        if(p_radius_sq < outer_radius_sq && p_radius_sq > inner_radius_sq) {
            // Angle for polar coordinates around center of rotation
            const float p_theta = std::atan2(p_in_rot.y(), p_in_rot.x());
            if (angle_relevant(p_theta) && p_theta < M_PI) {
                // TODO: This assumes that any collision with the point will be
                // on the leading part of the robot, when in reality we can turn more
                // than this amount if the point only causes a collision with the rear
                // part of the robot as it swings around for a turn
                closest_angle = std::min(closest_angle, p_theta);
            }
        }
    }

    // TODO: Check obstacle lines for intersection with robot arc

    return closest_angle;
}
