#include "tf/LinearMath/Quaternion.h"
#include "tf/LinearMath/Transform.h"
#include "tf/transform_datatypes.h"
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>

#include <tf2/LinearMath/Matrix3x3.h>
#include <tf2_ros/transform_broadcaster.h>
#include <tf2_ros/transform_listener.h>
#include <geometry_msgs/TransformStamped.h>
#include <ros/ros.h>
#include <std_msgs/String.h>
#include <std_msgs/Float32.h>
#include <std_msgs/Bool.h>
#include <std_srvs/Trigger.h>
#include <sensor_msgs/Joy.h>
#include <geometry_msgs/Twist.h>
#include <naoqi_bridge_msgs/JointAnglesWithSpeed.h>
#include <pepper_surrogate/ButtonToggle.h>

#include <tf_conversions/tf_eigen.h>
#include <openhmd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

void ohmd_sleep(double);

// applies a deadzone filter to a joystick value [0,1]
// the value is rescaled so that a threshold it is 0, and 1 at 1.
float deadzone(float value, float threshold) {
  float result = 0;
  if (abs(value) > threshold) {
    if (value > 0) {
        result = value - threshold / (1.f - threshold);
    } else {
        result = value + threshold / (1.f - threshold);
    }
  }
  return result;
}

// constrains angle to [-pi, pi]
double constrainAngle(double x){
    x = fmod(x + M_PI,2.*M_PI);
    if (x < 0)
        x += 2.*M_PI;
    return x - M_PI;
}

bool quaternionIsUnit(tf::Quaternion q) {
  return std::abs(
      (q.x() * q.x() +
       q.y() * q.y() +
       q.z() * q.z() +
       q.w() * q.w()) - 1.0f
      ) < 10e-6;
}


double clip(double n, double lower, double upper) {
  return std::max(lower, std::min(n, upper));
}

// gets float values from the device and prints them
void print_infof(ohmd_device* hmd, const char* name, int len, ohmd_float_value val)
{
  float f[16];
  assert(len <= 16);
  ohmd_device_getf(hmd, val, f);
  printf("%-25s", name);
  for(int i = 0; i < len; i++)
    printf("%f ", f[i]);
  printf("\n");
}

// gets int values from the device and prints them
void print_infoi(ohmd_device* hmd, const char* name, int len, ohmd_int_value val)
{
  int iv[16];
  assert(len <= 16);
  ohmd_device_geti(hmd, val, iv);
  printf("%-25s", name);
  for(int i = 0; i < len; i++)
    printf("%d ", iv[i]);
  printf("\n");
}

namespace head_control_node {

/// \brief A circular array based on std::vector
template <class T>
class CircularArray {
  public:
    CircularArray(std::initializer_list<T> l) : data_(l) {};
    T getNextItem() {
      T result = data_.at(current_pos_);
      if ( ++current_pos_ >= data_.size() ) {
        current_pos_ = 0;
      }
      return result;
    }

  private:
    std::size_t current_pos_ = 0;
    const std::vector<T> data_;
}; // class CircularArray

class OculusHeadController {

  public:
    explicit OculusHeadController(ros::NodeHandle& n) : nh_(n), tfListener_(tfBuffer_) {
      if (init_hmd() != 0) {
        ROS_ERROR_STREAM("FAILED TO INITIALIZE HMD DEVICE");
        return;
      }

      // Topic names.
      const std::string kPepperPoseTopic = "/pepper_robot/pose/joint_angles";
      const std::string kButtonATopic = "/oculus/button_a_toggle";
      const std::string kButtonBTopic = "/oculus/button_b_toggle";
      const std::string kLeftGripperTopic = "/oculus/left_gripper";
      const std::string kRightGripperTopic = "/oculus/right_gripper";
      const std::string kCmdVelTopic = "/cmd_vel";
      const std::string kCmdVelEnabledTopic = "/oculus/cmd_vel_enabled";
      kVRRoomFrame = "vrroom";
      // the real world frame that is fixed.
      // if set to odom, body rotation is independent of head rotation
      // if set to base_footprint, body and head rotation are dependent
      kOdomFrame = "base_footprint";
      kHMDFrame = "oculus";
      kLCtrFrame = "oculus_left_controller";
      kRCtrFrame = "oculus_right_controller";
      kBaseLinkFrame = "base_footprint";
      kPosVrInOdom = tf::Vector3(0., 0., 0.1); // slight difference to distinguish them in rviz

      // Variables
      rot_vr_in_odom_ = tf::Quaternion(0., 0., 0., 1.);
      known_pepper_in_vrroom_ = false;
      cmd_vel_enabled_ = true;

      // Parameters
      nh_.param<int>("verbosity", verbosity_, 0);

      // Publishers and subscribers.
      pose_pub_ = nh_.advertise<naoqi_bridge_msgs::JointAnglesWithSpeed>(kPepperPoseTopic, 1);
      button_a_pub_ = nh_.advertise<pepper_surrogate::ButtonToggle>(kButtonATopic, 1);
      button_b_pub_ = nh_.advertise<pepper_surrogate::ButtonToggle>(kButtonBTopic, 1);
      left_gripper_pub_ = nh_.advertise<std_msgs::Float32>(kLeftGripperTopic, 1);
      right_gripper_pub_ = nh_.advertise<std_msgs::Float32>(kRightGripperTopic, 1);
      cmd_vel_pub_ = nh_.advertise<geometry_msgs::Twist>(kCmdVelTopic, 1);
      cmd_vel_enabled_pub_ = nh_.advertise<std_msgs::Bool>(kCmdVelEnabledTopic, 1);

      // Initialize times.
      pose_pub_last_publish_time_ = ros::Time::now();

      // Timer callbacks
      pose_timer_ = nh_.createTimer(ros::Duration(0.01), &OculusHeadController::timerCallback, this);
      get_tf_timer_ = nh_.createTimer(ros::Duration(0.01), &OculusHeadController::gettfCallback, this);

    }
    ~OculusHeadController() {
      geometry_msgs::Twist cmd_vel_msg;
      cmd_vel_msg.linear.x = 0.;
      cmd_vel_msg.angular.z = 0.;
      cmd_vel_msg.linear.y = 0.;
      cmd_vel_pub_.publish(cmd_vel_msg);
    }

  protected:
    int init_hmd() {
      controllers_disabled_ = false;
      ohmd_require_version(0, 3, 0);

      int major, minor, patch;
      ohmd_get_version(&major, &minor, &patch);

      printf("OpenHMD version: %d.%d.%d\n", major, minor, patch);

      ctx_ = ohmd_ctx_create();

      // Probe for devices
      int num_devices = ohmd_ctx_probe(ctx_);
      if(num_devices < 0){
        printf("failed to probe devices: %s\n", ohmd_ctx_get_error(ctx_));
        return -1;
      }

      printf("num devices: %d\n\n", num_devices);

      // Print device information
      // & find index of headset and controllers
      int hmd_index = -1;
      int lctr_index = -1;
      int rctr_index = -1;
      for(int i = 0; i < num_devices; i++){
        int device_class = 0, device_flags = 0;
        const char* device_class_s[] = {"HMD", "Controller", "Generic Tracker", "Unknown"};

        ohmd_list_geti(ctx_, i, OHMD_DEVICE_CLASS, &device_class);
        ohmd_list_geti(ctx_, i, OHMD_DEVICE_FLAGS, &device_flags);

        std::string product_info = ohmd_list_gets(ctx_, i, OHMD_PRODUCT);
        printf("device %d\n", i);
        printf("  vendor:  %s\n", ohmd_list_gets(ctx_, i, OHMD_VENDOR));
        printf("  product: %s\n", ohmd_list_gets(ctx_, i, OHMD_PRODUCT));
        printf("  path:    %s\n", ohmd_list_gets(ctx_, i, OHMD_PATH));
        printf("  class:   %s\n", device_class_s[device_class > OHMD_DEVICE_CLASS_GENERIC_TRACKER ? 4 : device_class]);
        printf("  flags:   %02x\n",  device_flags);
        printf("    null device:         %s\n", device_flags & OHMD_DEVICE_FLAGS_NULL_DEVICE ? "yes" : "no");
        printf("    rotational tracking: %s\n", device_flags & OHMD_DEVICE_FLAGS_ROTATIONAL_TRACKING ? "yes" : "no");
        printf("    positional tracking: %s\n", device_flags & OHMD_DEVICE_FLAGS_POSITIONAL_TRACKING ? "yes" : "no");
        printf("    left controller:     %s\n", device_flags & OHMD_DEVICE_FLAGS_LEFT_CONTROLLER ? "yes" : "no");
        printf("    right controller:    %s\n\n", device_flags & OHMD_DEVICE_FLAGS_RIGHT_CONTROLLER ? "yes" : "no");

        if (product_info == "Rift (CV1)") {
          printf("  * Selected as headset device\n");
          hmd_index = i;
        } else if (product_info == "Rift (CV1): Right Controller") {
          printf("  * Selected as right controller device\n");
          rctr_index = i;
        } else if (product_info == "Rift (CV1): Left Controller") {
          printf("  * Selected as left controller device\n");
          lctr_index = i;
        }
      }

      // Open headset device
      if (hmd_index == -1) {
          ROS_ERROR_STREAM("FATAL: headset device not found. exiting.");
          return -1;
      }
      printf("opening headset device: %d\n", hmd_index);
      hmd_ = ohmd_list_open_device(ctx_, hmd_index);
      if(!hmd_){
        printf("failed to open headset device: %s\n", ohmd_ctx_get_error(ctx_));
        return -1;
      }

      // Open controller devices
      if (lctr_index >= 0) {
        lctr_ = ohmd_list_open_device(ctx_, lctr_index);
        if(!lctr_){
          ROS_ERROR_STREAM("failed to open left controller device (" << lctr_index << "), disabling controllers.");
          controllers_disabled_ = true;
        }
      } else {
        ROS_ERROR_STREAM("no device found for left controller, disabling controllers.");
        controllers_disabled_ = true;
      }
      if (rctr_index >= 0) {
        rctr_ = ohmd_list_open_device(ctx_, rctr_index);
        if(!rctr_){
          ROS_ERROR_STREAM("failed to open right controller device (" << rctr_index << "), disabling controllers.");
          controllers_disabled_ = true;
        }
      } else {
        ROS_ERROR_STREAM("no device found for right controller, disabling controllers.");
        controllers_disabled_ = true;
      }

      // Print hardware information for the opened device
      int ivals[2];
      ohmd_device_geti(hmd_, OHMD_SCREEN_HORIZONTAL_RESOLUTION, ivals);
      ohmd_device_geti(hmd_, OHMD_SCREEN_VERTICAL_RESOLUTION, ivals + 1);
      printf("resolution:              %i x %i\n", ivals[0], ivals[1]);

      print_infof(hmd_, "hsize:",            1, OHMD_SCREEN_HORIZONTAL_SIZE);
      print_infof(hmd_, "vsize:",            1, OHMD_SCREEN_VERTICAL_SIZE);
      print_infof(hmd_, "lens separation:",  1, OHMD_LENS_HORIZONTAL_SEPARATION);
      print_infof(hmd_, "lens vcenter:",     1, OHMD_LENS_VERTICAL_POSITION);
      print_infof(hmd_, "left eye fov:",     1, OHMD_LEFT_EYE_FOV);
      print_infof(hmd_, "right eye fov:",    1, OHMD_RIGHT_EYE_FOV);
      print_infof(hmd_, "left eye aspect:",  1, OHMD_LEFT_EYE_ASPECT_RATIO);
      print_infof(hmd_, "right eye aspect:", 1, OHMD_RIGHT_EYE_ASPECT_RATIO);
      print_infof(hmd_, "distortion k:",     6, OHMD_DISTORTION_K);

      print_infoi(hmd_, "control count:   ", 1, OHMD_CONTROL_COUNT);

      return 0;
    }

    // Gets the transform between pepper and vrroom
    void gettfCallback(const ros::TimerEvent& e) {
      try{
        pepper_in_vrroom_ = tfBuffer_.lookupTransform(kVRRoomFrame, kBaseLinkFrame, ros::Time(0));
        known_pepper_in_vrroom_ = true;
      }
      catch (tf2::TransformException &ex) {
        ROS_WARN_ONCE("%s",ex.what());
      }
    }

    void timerCallback(const ros::TimerEvent& e) {

      ohmd_ctx_update(ctx_);

      // get rotation and position
      if (verbosity_ >= 1) {
        print_infof(hmd_, "rotation quat:", 4, OHMD_ROTATION_QUAT);
        print_infof(hmd_, "position vec: ", 3, OHMD_POSITION_VECTOR);
      }

      float f[16];
      ohmd_device_getf(hmd_, OHMD_ROTATION_QUAT, f);
      tf::Quaternion q(f[0], f[1], f[2], f[3]);
      ohmd_device_getf(hmd_, OHMD_POSITION_VECTOR, f);
      tf::Vector3 v(f[0], f[1], f[2]);
      tf::Transform new_t = oculusToROSFrameRotation(tf::Transform(q, v));
      tf::Quaternion q_HMD_in_vrroom = new_t.getRotation();
      tf::Vector3 pos_HMD_in_vrroom = new_t.getOrigin();
      sendTransform(ros::Time::now(), kOdomFrame, kVRRoomFrame, kPosVrInOdom, rot_vr_in_odom_);
      sendTransform(ros::Time::now(), kVRRoomFrame, kHMDFrame, pos_HMD_in_vrroom, q_HMD_in_vrroom);
      double roll, pitch, yaw;
      getOculusInPepperRPY(q_HMD_in_vrroom, roll, pitch, yaw);
      sendHeadTrackingJointAngles(pitch, yaw);

      // if we don't know base_footprint's tf, this is 0
      double yaw_oculus_in_base_footprint = yaw;

      // rotate base if yaw is high (natural max human yaw is a bit less than 90deg)
      geometry_msgs::Twist cmd_vel_msg;
      cmd_vel_msg.linear.x = 0.;
      cmd_vel_msg.linear.y = 0.;
      cmd_vel_msg.angular.z = 0.;
      float kMaxHumanHeadYawRad = 1.6;
      if (abs(yaw_oculus_in_base_footprint) > kMaxHumanHeadYawRad) {
        cmd_vel_msg.angular.z = yaw_oculus_in_base_footprint - (yaw_oculus_in_base_footprint > 0 ? kMaxHumanHeadYawRad : -kMaxHumanHeadYawRad);
      }

      // ---------------------
      // Controllers
      if (!controllers_disabled_) {

        int n_controllers = 2;
        for (int k = 0; k < n_controllers; k++) {

          // select left or right controller
          ohmd_device* ctr;
          std::string ctr_frame;
          float* prev_control_state;
          bool is_left_controller = (k == 0);
          if (is_left_controller) {
            ctr = lctr_;
            ctr_frame = kLCtrFrame;
            prev_control_state = lctr_prev_state_;
          } else {
            ctr = rctr_;
            ctr_frame = kRCtrFrame;
            prev_control_state = rctr_prev_state_;
          }

          float f[16];
          ohmd_device_getf(ctr, OHMD_ROTATION_QUAT, f);
          tf::Quaternion q(f[0], f[1], f[2], f[3]);

          ohmd_device_getf(ctr, OHMD_POSITION_VECTOR, f);
          tf::Vector3 v(f[0], f[1], f[2]);

          if (!quaternionIsUnit(q)) {
            ROS_WARN_ONCE("Invalid quaternion for right controller. Waiting for correct value.");
          } else {

            tf::Transform t(q, v);
            tf::Transform new_t = oculusToROSFrameRotation(t);
            q = new_t.getRotation();
            v = new_t.getOrigin();
            sendTransform(ros::Time::now(), kVRRoomFrame, ctr_frame, v, q);
          }

          // buttons state
          const char* controls_fn_str[] = { "generic", "trigger", "trigger_click", "squeeze", "menu", "home",
            "analog-x", "analog-y", "anlog_press", "button-a", "button-b", "button-x", "button-y",
            "volume-up", "volume-down", "mic-mute"};
          const char* controls_type_str[] = {"digital", "analog"};

          int control_count;
          ohmd_device_geti(ctr, OHMD_CONTROL_COUNT, &control_count);

          int controls_fn[64];
          int controls_types[64];
          ohmd_device_geti(ctr, OHMD_CONTROLS_HINTS, controls_fn);
          ohmd_device_geti(ctr, OHMD_CONTROLS_TYPES, controls_types);

          if (control_count) {
            float control_state[256];
            ohmd_device_getf(ctr, OHMD_CONTROLS_STATE, control_state);

            // DEBUG
            if (verbosity_ >= 1) {
              printf("%-25s", "controls:");
              for(int i = 0; i < control_count; i++){
                printf("%s (%s)%s", controls_fn_str[controls_fn[i]], controls_type_str[controls_types[i]], i == hmd_control_count_ - 1 ? "" : ", ");
              }
              printf("\n");
              printf("%-25s", "controls state:");
              for(int i = 0; i < control_count; i++)
              {
                printf("%f ", control_state[i]);
              }
              printf("\n");
              printf("%-25s", "prev controls state:");
              for(int i = 0; i < control_count; i++)
              {
                printf("%f ", prev_control_state[i]);
              }
              printf("\n");
              printf("\n\n");
            }

//             const static float kMaxBaseVelMPerS = 0.55; // Using moveToward, vels are normalized
            const static float kMaxBaseVelMPerS = 0.2; // DEBUG SAFETY SPEED CAP
//             const static float kMaxBaseRotRadPerS = 2.; // Using moveToward, rotation is normalized
            const static float kMaxBaseRotRadPerS = 0.5; // Using moveToward, rotation is normalized
            const static float kLAnalogDeadzone = 0.1; // small movements of the joystick do nothing
            for(int i = 0; i < control_count; i++){
              bool is_toggled = false;
              if (control_state[i] != prev_control_state[i]) {
                is_toggled = true;
              }
              prev_control_state[i] = control_state[i];
              if (strcmp(controls_fn_str[controls_fn[i]], "button-a") == 0 && is_toggled) {
                pepper_surrogate::ButtonToggle msg;
                msg.event = (control_state[i] ?  msg.PRESSED : msg.RELEASED);
                button_a_pub_.publish(msg);
              } else if (strcmp(controls_fn_str[controls_fn[i]], "button-b") == 0 && is_toggled) {
                pepper_surrogate::ButtonToggle msg;
                msg.event = (control_state[i] ?  msg.PRESSED : msg.RELEASED);
                button_b_pub_.publish(msg);
              } else if (strcmp(controls_fn_str[controls_fn[i]], "button-x") == 0 && is_toggled) {
                if (control_state[i]) {
                  resetHeadYaw(yaw_oculus_in_base_footprint);
                }
              } else if (strcmp(controls_fn_str[controls_fn[i]], "menu") == 0 && is_toggled) {
                if (control_state[i]) {
                  if (cmd_vel_enabled_) {
                    cmd_vel_enabled_ = false;
                    geometry_msgs::Twist cmd_vel_msg;
                    cmd_vel_msg.linear.x = 0.;
                    cmd_vel_msg.angular.z = 0.;
                    cmd_vel_msg.linear.y = 0.;
                    cmd_vel_pub_.publish(cmd_vel_msg);
                  } else {
                    cmd_vel_enabled_ = true;
                  }
                  std_msgs::Bool msg;
                  msg.data = cmd_vel_enabled_;
                  cmd_vel_enabled_pub_.publish(msg);
                }
              } else if (strcmp(controls_fn_str[controls_fn[i]], "analog-x") == 0 && is_left_controller) {
                  cmd_vel_msg.linear.y = -deadzone(control_state[i], kLAnalogDeadzone) * kMaxBaseVelMPerS;
              } else if (strcmp(controls_fn_str[controls_fn[i]], "analog-y") == 0 && is_left_controller) {
                  cmd_vel_msg.linear.x = deadzone(control_state[i], kLAnalogDeadzone) * kMaxBaseVelMPerS;
              } else if (strcmp(controls_fn_str[controls_fn[i]], "analog-x") == 0 && !is_left_controller) {
                  cmd_vel_msg.angular.z = -deadzone(control_state[i], kLAnalogDeadzone) * kMaxBaseRotRadPerS;
              } else if (strcmp(controls_fn_str[controls_fn[i]], "squeeze") == 0 && is_left_controller) {
                std_msgs::Float32 msg;
                msg.data = control_state[i];
                left_gripper_pub_.publish(msg);
              } else if (strcmp(controls_fn_str[controls_fn[i]], "squeeze") == 0 && !is_left_controller) {
                std_msgs::Float32 msg;
                msg.data = control_state[i];
                right_gripper_pub_.publish(msg);
              }
            }

          } // if control count

        } // for each controller
      } // if controllers enabled

      // publish cmd_vel
      if (cmd_vel_enabled_) {
        cmd_vel_pub_.publish(cmd_vel_msg);
      }

    }

    // returns the roll, pitch, yaw of oculus in base_footprint frame
    // q: oculus 3dof pose in vrroom frame
    void getOculusInPepperRPY(tf::Quaternion q, double& roll, double& pitch, double& yaw) {
      // oculus pose as euler angles in vrroom frame
      double oculus_roll, oculus_pitch, oculus_yaw;
      tf::Matrix3x3(q).getRPY(oculus_roll, oculus_pitch, oculus_yaw);

      roll = oculus_roll;
      pitch = oculus_pitch;

      if (known_pepper_in_vrroom_) {
        // pepper yaw in vroom frame
        double pepper_roll, pepper_pitch, pepper_yaw;
        tf::Quaternion qpepper_in_vrroom(
            pepper_in_vrroom_.transform.rotation.x,
            pepper_in_vrroom_.transform.rotation.y,
            pepper_in_vrroom_.transform.rotation.z,
            pepper_in_vrroom_.transform.rotation.w);
        tf::Matrix3x3(qpepper_in_vrroom).getRPY(pepper_roll, pepper_pitch, pepper_yaw);
        // yaw of oculus in base_footprint frame
        // TODO: check that base_footprint and vrroom (odom) z axis are aligned?
        double relative_yaw = constrainAngle(oculus_yaw - pepper_yaw);
        yaw = relative_yaw;
      } else {
        yaw = 0.;
      }

    }

    // tracks oculus pose with pepper head, by sending the appropriate joint angles
    void sendHeadTrackingJointAngles(double pitch, double yaw) {

      // Control pepper head to oculus pose
      // Pepper head has no roll axis, use hip?
      // how do we track pepper's head direction?
      const static float kMaxHeadMoveAngleRad = 1.;
      const static float kMaxJointSpeedRadPerS = 0.2;
      const static double kMinHeadYawRad = -2.0857;
      const static double kMaxHeadYawRad =  2.0857;
      const static double kMinHeadPitchRad = -0.7068;
      const static double kMaxHeadPitchRad =  0.6371;
      const static double kMinHipPitchRad = -1.0385;
      const static double kMaxHipPitchRad =  1.0385;
      const static std::string kHeadYawJointName = "HeadYaw";
      const static std::string kHeadPitchJointName = "HeadPitch";
      const static std::string kHipPitchJointName = "HipPitch";
      const static ros::Duration kMaxPosePubRate(0.1);
      // Limit the maximum rate publishing rate for pose control
      if ( ( ros::Time::now() - pose_pub_last_publish_time_ ) > kMaxPosePubRate )
      {
        naoqi_bridge_msgs::JointAnglesWithSpeed joint_angles_msg;
        joint_angles_msg.speed = kMaxJointSpeedRadPerS;
        joint_angles_msg.joint_names.push_back(kHeadYawJointName);
        joint_angles_msg.joint_angles.push_back(clip(yaw, kMinHeadYawRad, kMaxHeadYawRad));
        joint_angles_msg.joint_names.push_back(kHeadPitchJointName);
        joint_angles_msg.joint_angles.push_back(clip(pitch, kMinHeadPitchRad, kMaxHeadPitchRad));
        double kRestHipPitchRad = -0.1;
        double hip_pitch = kRestHipPitchRad;
        // Lean down if head is pointing down hard (to see feet)
        if (pitch > kMaxHeadPitchRad) {
          hip_pitch = -(pitch - kMaxHeadPitchRad) + kRestHipPitchRad;
        // lean back if looking up (disabled, unstable)
        } else if (pitch < kMinHeadPitchRad) {
          if (false) {
            hip_pitch = -(pitch - kMinHeadPitchRad) + kRestHipPitchRad;
          }
        }
        joint_angles_msg.joint_names.push_back(kHipPitchJointName);
        joint_angles_msg.joint_angles.push_back(clip(hip_pitch, kMinHipPitchRad, kMaxHipPitchRad));
        pose_pub_.publish(joint_angles_msg);
        pose_pub_last_publish_time_ = ros::Time::now();
      }
    }

    void sendTransform(ros::Time time, std::string parent, std::string child, tf::Vector3 v, tf::Quaternion q) {
        geometry_msgs::TransformStamped transformStamped;
        transformStamped.header.stamp = time;
        transformStamped.header.frame_id = parent;
        transformStamped.child_frame_id = child;
        transformStamped.transform.translation.x = v.x();
        transformStamped.transform.translation.y = v.y();
        transformStamped.transform.translation.z = v.z();
        transformStamped.transform.rotation.x = q.x();
        transformStamped.transform.rotation.y = q.y();
        transformStamped.transform.rotation.z = q.z();
        transformStamped.transform.rotation.w = q.w();
        br_.sendTransform(transformStamped);
    }

    // Avoids creating extra static tf frames between vrroom and oculus by applying tf directly
    tf::Transform oculusToROSFrameRotation(tf::Transform t) {
        // qrotx and qrotz make the z axis point up in the oculus frame
        // qtilt makes the headset point up in the oculus frame
        tf::Quaternion qtilt, qrotx, qrotz;
        qtilt.setRPY(3.14159/2., 0.0, 0.0);
        qrotx.setRPY(-3.14159/2., 0.0, 0.0);
        qrotz.setRPY(0.0, 0.0, 3.14159/2.);
        tf::Transform ttilt(qtilt);
        tf::Transform trotx(qrotx);
        tf::Transform trotz(qrotz);
        // This is the same as creating a world to qtilt tf with qtilt as rot, (rotates the actual oculus frame)
        // then a qtilt to oculus tf with q * qrotx * qrotz as tf (rotates the axes around the oculus)
        return ttilt * t * trotx * trotz;
    }

    // yaw: yaw of oculus in base_footprint
    void resetHeadYaw(double yaw) {
      // get current yaw
      double vrroll, vrpitch, vryaw;
      tf::Matrix3x3(rot_vr_in_odom_).getRPY(vrroll, vrpitch, vryaw);
      // set relative yaw to 0 by rotating vrroom in odom
      rot_vr_in_odom_.setRPY(0., 0., vryaw-yaw);
    }

  private:
    ros::NodeHandle& nh_;
    ros::Timer pose_timer_;
    ros::Timer get_tf_timer_;
    ros::Publisher pose_pub_;
    ros::Publisher button_a_pub_;
    ros::Publisher button_b_pub_;
    ros::Publisher left_gripper_pub_;
    ros::Publisher right_gripper_pub_;
    ros::Publisher cmd_vel_pub_;
    ros::Publisher cmd_vel_enabled_pub_;
    ros::Time pose_pub_last_publish_time_;
    tf2_ros::TransformBroadcaster br_;
    tf2_ros::Buffer tfBuffer_;
    tf2_ros::TransformListener tfListener_;
    geometry_msgs::TransformStamped pepper_in_vrroom_;
    bool known_pepper_in_vrroom_;
    bool cmd_vel_enabled_;
    std::string kBaseLinkFrame;
    std::string kVRRoomFrame;
    std::string kOdomFrame;
    std::string kHMDFrame;
    std::string kLCtrFrame;
    std::string kRCtrFrame;
    int verbosity_;
    tf::Quaternion rot_vr_in_odom_;
    tf::Vector3 kPosVrInOdom;

    bool controllers_disabled_;
    ohmd_context* ctx_;
    ohmd_device* hmd_;  // headset
    ohmd_device* lctr_; // left controller
    ohmd_device* rctr_; // right controller
    int hmd_control_count_;
    int lctr_control_count_;
    int rctr_control_count_;
    float lctr_prev_state_[256];
    float rctr_prev_state_[256];

}; // class OculusHeadController

} // namespace head_control_node

using namespace head_control_node;

int main(int argc, char **argv) {

//   oldmain(argc, argv);

  ros::init(argc, argv, "head_control_node");
  ros::NodeHandle n("~");
  OculusHeadController head_controller(n);

  try {
    ros::spin();
  }
  catch (const std::exception& e) {
    ROS_ERROR_STREAM("Exception: " << e.what());
    return 1;
  }
  catch (...) {
    ROS_ERROR_STREAM("Unknown Exception.");
    return 1;
  }

  return 0;
}
