/* Copyright (c) 2017, United States Government, as represented by the
 * Administrator of the National Aeronautics and Space Administration.
 * 
 * All rights reserved.
 * 
 * The Astrobee platform is licensed under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with the
 * License. You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

// Standard ROS includes
#include <ros/ros.h>
#include <nodelet/nodelet.h>
#include <pluginlib/class_list_macros.h>

// Standard sensor messages
#include <sensor_msgs/JointState.h>

// FSW shared libraries
#include <config_reader/config_reader.h>
#include <ff_util/config_server.h>
#include <ff_util/ff_nodelet.h>
#include <ff_util/ff_action.h>
#include <ff_util/ff_fsm.h>

// FSW actions, services, messages
#include <ff_msgs/ArmAction.h>
#include <ff_msgs/ArmStateStamped.h>
#include <ff_msgs/JointSampleStamped.h>
#include <ff_msgs/SetState.h>

/**
 * \ingroup beh
 */
namespace arm {

// Different joint types
enum JointType { PAN, TILT, GRIPPER };

// Joint information, where HUMAN = SCALE * DRIVER + OFFSET
struct JointInfo {
  std::string name;     // Low level joint state name
  std::string generic;  // Generic name for joint state
  double val;           // Current value in HUMAN form
  double goal;          // Current goal in HUMAN form
  double tol;           // Tolerance in HUMAN form
  double offset;        // DRIVER -> HUMAN offset
  double scale;         // FRIVER -> HUMAN SCALE
};

// List of generic joints "pan", "tilt" and "gripper"
typedef std::map<JointType, JointInfo> JointMap;

// Reverse lookup for joint name -> generic joint name
typedef std::map<std::string, JointType> JointDictionary;

// Match the internal states and responses with the message definition
using FSM = ff_util::FSM;
using STATE = ff_msgs::ArmState;
using RESPONSE = ff_msgs::ArmResult;

class ArmNodelet : public ff_util::FreeFlyerNodelet {
 public:
  // Possible events
  enum : FSM::Event {
    READY              = (1<<0),     // We are connected to the arm
    DEPLOYED           = (1<<1),     // Background deploy
    STOWED             = (1<<2),     // Background stow
    GOAL_DEPLOY        = (1<<3),     // Start a new deploy action
    GOAL_STOW          = (1<<4),     // Start a new deploy action
    GOAL_MOVE          = (1<<5),     // Start a new move (pan and tilt)
    GOAL_CALIBRATE     = (1<<6),     // Start a new gripper action
    GOAL_SET           = (1<<7),     // Start a new gripper action
    GOAL_CANCEL        = (1<<8),     // Cancel the current goal
    PAN_COMPLETE       = (1<<9),     // Pan complete
    TILT_COMPLETE      = (1<<10),    // Tilt complete
    GRIPPER_COMPLETE   = (1<<11),    // Gripper action complete
    CALIBRATE_COMPLETE = (1<<12),    // Calibration complete
    TIMEOUT            = (1<<13)     // Current action doesn't complete in time
  };

  // Constructor
  ArmNodelet() : ff_util::FreeFlyerNodelet(NODE_ARM, true),
    fsm_(STATE::INITIALIZING, std::bind(&ArmNodelet::UpdateCallback,
      this, std::placeholders::_1, std::placeholders::_2)) {
    // INITIALIZING -> UNKNOWN
    //   [label="[0]\nREADY", color=blue];
    fsm_.Add(STATE::INITIALIZING,
      READY,
      [this](FSM::Event const& event) -> FSM::State {
        return STATE::UNKNOWN;
      });

    // UNKNOWN -> STOWED
    //   [label="[1]\nARM_STOWED", color=blue];
    fsm_.Add(STATE::UNKNOWN,
      STOWED,
      [this](FSM::Event const& event) -> FSM::State {
        return STATE::STOWED;
      });

    // UNKNOWN -> DEPLOYED
    //   [label="[2]\nARM_DEPLOYED", color=blue];
    fsm_.Add(STATE::UNKNOWN,
      DEPLOYED,
      [this](FSM::Event const& event) -> FSM::State {
        return STATE::DEPLOYED;
      });

    // STOWED -> DEPLOYED
    //   [label="[3]\nARM_DEPLOYED", color=blue];
    fsm_.Add(STATE::STOWED,
      DEPLOYED,
      [this](FSM::Event const& event) -> FSM::State {
        return STATE::DEPLOYED;
      });

    // DEPLOYED -> STOWED
    //   [label="[4]\nARM_STOWED", color=blue];
    fsm_.Add(STATE::DEPLOYED,
      STOWED,
      [this](FSM::Event const& event) -> FSM::State {
        return STATE::STOWED;
      });

    // STOWED -> DEPLOYING_PANNING
    //   [label="[5]\nGOAL_DEPLOY\nPan(DEPLOY)"];
    fsm_.Add(STATE::STOWED,
      GOAL_DEPLOY,
      [this](FSM::Event const& event) -> FSM::State {
        if (!Arm(PAN))
          return Result(RESPONSE::PAN_FAILED);
        return STATE::DEPLOYING_PANNING;
      });

    // DEPLOYING_PANNING -> DEPLOYING_TILTING
    //   [label="[6]\nPAN_COMPLETE\nTilt(DEPLOY)"];
    fsm_.Add(STATE::DEPLOYING_PANNING,
      PAN_COMPLETE,
      [this](FSM::Event const& event) -> FSM::State {
        if (!Arm(TILT))
          return Result(RESPONSE::TILT_FAILED);
        return STATE::DEPLOYING_TILTING;
      });

    // DEPLOYING_TILTING -> DEPLOYED
    //   [label="[7]\nTILT_COMPLETE\nResult(SUCCESS)", color=darkgreen];
    fsm_.Add(STATE::DEPLOYING_TILTING,
      TILT_COMPLETE,
      [this](FSM::Event const& event) -> FSM::State {
        return Result(RESPONSE::SUCCESS);
      });

    // DEPLOYED -> STOWING_SETTING
    //   [label="[8]\nGOAL_STOW\nGripper(CLOSE)"];
    fsm_.Add(STATE::DEPLOYED,
      GOAL_STOW,
      [this](FSM::Event const& event) -> FSM::State {
        // If the gripper is calibrated
        if (RequiresClosing()) {
          if (!Arm(GRIPPER))
            return Result(RESPONSE::GRIPPER_FAILED);
          return STATE::STOWING_SETTING;
        }
        if (!Arm(PAN))
          return Result(RESPONSE::PAN_FAILED);
        return STATE::STOWING_PANNING;
      });

    // STOWING_SETTING -> STOWING_PANNING
    //   [label="[9]\nGRIPPER_COMPLETE\nPan(STOWED)", color=black];
    fsm_.Add(STATE::STOWING_SETTING,
      GRIPPER_COMPLETE,
      [this](FSM::Event const& event) -> FSM::State {
        if (!Arm(PAN))
          return Result(RESPONSE::PAN_FAILED);
        return STATE::STOWING_PANNING;
      });

    // STOWING_SETTING -> DEPLOYED
    //   [label="[10]\nTIMEOUT\nResult(FAILED)", color=red];
    fsm_.Add(STATE::STOWING_SETTING,
      TIMEOUT | GOAL_CANCEL,
      [this](FSM::Event const& event) -> FSM::State {
        return Result(RESPONSE::GRIPPER_FAILED);
      });


    // STOWING_PANNING -> STOWING_TILTING
    //   [label="[11]\nPAN_COMPLETE\nTilt(STOWED)", color=black];
    fsm_.Add(STATE::STOWING_PANNING,
      PAN_COMPLETE,
      [this](FSM::Event const& event) -> FSM::State {
        if (!Arm(TILT))
          return Result(RESPONSE::TILT_FAILED);
        return STATE::STOWING_TILTING;
      });

    // STOWING_PANNING -> DEPLOYED
    //   [label="[12]\nTIMEOUT\nResult(FAILED)", color=red];
    fsm_.Add(STATE::STOWING_PANNING,
      TIMEOUT | GOAL_CANCEL,
      [this](FSM::Event const& event) -> FSM::State {
        return Result(RESPONSE::PAN_FAILED);
      });

    // STOWING_TILTING -> STOWED
    //   [label="[13]\nTILT_COMPLETE\nResult(SUCCESS)", color=darkgreen];
    fsm_.Add(STATE::STOWING_TILTING,
      TILT_COMPLETE,
      [this](FSM::Event const& event) -> FSM::State {
        return Result(RESPONSE::SUCCESS);
      });

    // STOWING_TILTING -> DEPLOYED
    //   [label="[14]\nTIMEOUT\nResult(FAILED)", color=red];
    fsm_.Add(STATE::STOWING_TILTING,
      TIMEOUT | GOAL_CANCEL,
      [this](FSM::Event const& event) -> FSM::State {
        return Result(RESPONSE::TILT_FAILED);
      });

    // {STOWED, DEPLOYED} -> PANNING
    //   [label="[15]\nGOAL_MOVE\nPan(angle)"];
    fsm_.Add(STATE::STOWED, STATE::DEPLOYED,
      GOAL_MOVE,
      [this](FSM::Event const& event) -> FSM::State {
        // Check that we have a valid pan value
        if (!Arm(PAN))
          return Result(RESPONSE::PAN_FAILED);
        return STATE::PANNING;
      });

    // PANNING -> TILTING
    //   [label="[16]\nPAN_COMPLETE\nTilt(angle)"];
    fsm_.Add(STATE::PANNING,
      PAN_COMPLETE,
      [this](FSM::Event const& event) -> FSM::State {
        // Check that we have a valid tilt value
        if (!Arm(TILT))
          return Result(RESPONSE::TILT_FAILED);
        return STATE::TILTING;
      });

    // PANNING -> DEPLOYED
    //   [label="[17]\nTIMEOUT\nResult(PAN_FAILED)", color=red];
    fsm_.Add(STATE::PANNING,
      TIMEOUT | GOAL_CANCEL,
      [this](FSM::Event const& event) -> FSM::State {
        return Result(RESPONSE::PAN_FAILED);
      });

    // TILTING -> DEPLOYED
    //   [label="[18]\nTILT_COMPLETE\nResult(SUCCESS)", color=darkgreen];
    fsm_.Add(STATE::TILTING,
      TILT_COMPLETE,
      [this](FSM::Event const& event) -> FSM::State {
        return Result(RESPONSE::SUCCESS);
      });

    // TILTING -> DEPLOYED
    //   [label="[19]\nTIMEOUT\nResult(TILT_FAILED)", color=red];
    fsm_.Add(STATE::TILTING,
      TIMEOUT | GOAL_CANCEL,
      [this](FSM::Event const& event) -> FSM::State {
        return Result(RESPONSE::TILT_FAILED);
      });

    // DEPLOYED -> SETTING
    //   [label="[20]\nGOAL_SET\nGripper(percent)"];
    fsm_.Add(STATE::DEPLOYED,
      GOAL_SET,
      [this](FSM::Event const& event) -> FSM::State {
        if (!Arm(GRIPPER))
          return Result(RESPONSE::GRIPPER_FAILED);
        return STATE::SETTING;
      });

    // SETTING -> DEPLOYED
    //   [label="[21]\nGRIPPER_COMPLETE\nResult(SUCCESS)", color=darkgreen];
    fsm_.Add(STATE::SETTING,
      GRIPPER_COMPLETE,
      [this](FSM::Event const& event) -> FSM::State {
        return Result(RESPONSE::SUCCESS);
      });

    // SETTING -> DEPLOYED
    //   [label="[22]\nTIMEOUT\nResult(GRIPPER_FAILED)", color=red];
    fsm_.Add(STATE::SETTING,
      TIMEOUT | GOAL_CANCEL,
      [this](FSM::Event const& event) -> FSM::State {
        return Result(RESPONSE::GRIPPER_FAILED);
      });

    // DEPLOYED -> CALIBRATING
    //   [label="[23]\nGOAL_CALIBRATE\nCalibrate()"];
    fsm_.Add(STATE::DEPLOYED,
      GOAL_CALIBRATE,
      [this](FSM::Event const& event) -> FSM::State {
        if (!Arm(GRIPPER))
          return Result(RESPONSE::CALIBRATE_FAILED);
        return STATE::CALIBRATING;
      });

    // CALIBRATING -> DEPLOYED
    //   [label="[24]\nCALIBRATE_COMPLETE\nResult(SUCCESS)", color=darkgreen];
    fsm_.Add(STATE::CALIBRATING,
      CALIBRATE_COMPLETE,
      [this](FSM::Event const& event) -> FSM::State {
        return Result(RESPONSE::SUCCESS);
      });

    // CALIBRATING -> DEPLOYED
    //   [label="[25]\nTIMEOUT\nResult(SET_FAILED)", color=red];
    fsm_.Add(STATE::CALIBRATING,
      TIMEOUT | GOAL_CANCEL,
      [this](FSM::Event const& event) -> FSM::State {
        return Result(RESPONSE::CALIBRATE_FAILED);
      });
  }

  // Destructor
  virtual ~ArmNodelet() {}

 protected:
  // Called to initialize this nodelet
  void Initialize(ros::NodeHandle *nh) {
    // Grab some configuration parameters for this node from the LUA config reader
    cfg_.Initialize(GetPrivateHandle(), "behaviors/arm.config");
    if (!cfg_.Listen(boost::bind(
      &ArmNodelet::ReconfigureCallback, this, _1)))
      return AssertFault("INITIALIZATION_FAULT", "Could not load config");

    // Read the confgiuration for this specific node
    config_reader::ConfigReader *cfg = cfg_.GetConfigReader();
    config_reader::ConfigReader::Table joints;
    if (!cfg->GetTable(GetName().c_str(), &joints))
      return AssertFault("INITIALIZATION_FAULT", "Cannot read LUA file");
    std::string name;
    if (!joints.GetStr("pan", &name))
      return AssertFault("INITIALIZATION_FAULT", "Cannot read PAN joint");
    joints_[PAN].name = name;
    joints_[PAN].generic = "pan";
    joints_[PAN].tol = cfg_.Get<double>("tol_pan");
    joints_[PAN].scale = K_RADS_TO_DEGS;
    joints_[PAN].offset = K_PAN_OFFSET;
    dictionary_[name] = PAN;
    if (!joints.GetStr("tilt", &name))
      return AssertFault("INITIALIZATION_FAULT", "Cannot read TILT joint");
    joints_[TILT].name = name;
    joints_[TILT].generic = "tilt";
    joints_[TILT].tol = cfg_.Get<double>("tol_tilt");
    joints_[TILT].scale = K_RADS_TO_DEGS;
    joints_[TILT].offset = K_TILT_OFFSET;
    dictionary_[name] = TILT;
    if (!joints.GetStr("gripper", &name))
      return AssertFault("INITIALIZATION_FAULT", "Cannot read GRIPPER joint");
    joints_[GRIPPER].name = name;
    joints_[GRIPPER].generic = "gripper";
    joints_[GRIPPER].tol = cfg_.Get<double>("tol_gripper");
    joints_[GRIPPER].scale = (K_GRIPPER_OPEN - K_GRIPPER_CLOSE) / 100.00;
    joints_[GRIPPER].offset = K_GRIPPER_CLOSE;
    dictionary_[name] = GRIPPER;

    // Timer for checking goal is reached
    timer_watchdog_ = nh->createTimer(cfg_.Get<double>("timeout_watchdog"),
        &ArmNodelet::WatchdogCallback, this, true, false);

    // Timer for checking goal is reached
    timer_goal_ = nh->createTimer(cfg_.Get<double>("timeout_goal"),
        &ArmNodelet::TimeoutCallback, this, true, false);

    // Publishers for arm and joint state
    sub_joint_states_ = nh->subscribe(TOPIC_JOINT_STATES, 1,
      &ArmNodelet::JointStateCallback, this);
    pub_joint_goals_ = nh->advertise<sensor_msgs::JointState>(
      TOPIC_JOINT_GOALS, 1, true);

    // Internal state publisher
    pub_state_ = nh->advertise<ff_msgs::ArmState>(
      TOPIC_BEHAVIORS_ARM_STATE, 1, true);

    // Allow the state to be manually set
    srv_set_state_ = nh->advertiseService(SERVICE_BEHAVIORS_ARM_SET_STATE,
      &ArmNodelet::SetStateCallback, this);

    // Executive state publishers
    pub_arm_state_ = nh->advertise<ff_msgs::ArmStateStamped>(
      TOPIC_BEHAVIORS_ARM_ARM_STATE, 1, true);
    pub_joint_sample_ = nh->advertise<ff_msgs::JointSampleStamped>(
      TOPIC_BEHAVIORS_ARM_JOINT_SAMPLE, 1);

    // Setup the ARM  action
    server_.SetGoalCallback(std::bind(
      &ArmNodelet::GoalCallback, this, std::placeholders::_1));
    server_.SetPreemptCallback(std::bind(
      &ArmNodelet::PreemptCallback, this));
    server_.SetCancelCallback(std::bind(
      &ArmNodelet::CancelCallback, this));
    server_.Create(nh, ACTION_BEHAVIORS_ARM);
  }

  // Callback to handle reconfiguration requests
  bool ReconfigureCallback(dynamic_reconfigure::Config & config) {
    bool success = false;
    switch (fsm_.GetState()) {
    case STATE::DEPLOYED:
    case STATE::STOWED:
    case STATE::UNKNOWN:
      success = cfg_.Reconfigure(config);
      joints_[PAN].tol = cfg_.Get<double>("tol_pan");
      joints_[TILT].tol = cfg_.Get<double>("tol_tilt");
      joints_[GRIPPER].tol = cfg_.Get<double>("tol_gripper");
    default:
      break;
    }
    return success;
  }

  // When the FSM state changes we get a callback here, so that we
  // can choose to do various things.
  void UpdateCallback(FSM::State const& state, FSM::Event const& event) {
    // Debug events
    std::string str = "UNKNOWN";
    switch (event) {
    case READY:                    str = "READY";               break;
    case DEPLOYED:                 str = "DEPLOYED";            break;
    case STOWED:                   str = "STOWED";              break;
    case GOAL_DEPLOY:              str = "GOAL_DEPLOY";         break;
    case GOAL_STOW:                str = "GOAL_STOW";           break;
    case GOAL_MOVE:                str = "GOAL_MOVE";           break;
    case GOAL_SET:                 str = "GOAL_SET";            break;
    case GOAL_CALIBRATE:           str = "GOAL_CALIBRATE";      break;
    case GOAL_CANCEL:              str = "GOAL_CANCEL";         break;
    case PAN_COMPLETE:             str = "PAN_COMPLETE";        break;
    case TILT_COMPLETE:            str = "TILT_COMPLETE";       break;
    case GRIPPER_COMPLETE:         str = "GRIPPER_COMPLETE";    break;
    case CALIBRATE_COMPLETE:       str = "CALIBRATE_COMPLETE";  break;
    case TIMEOUT:                  str = "TIMEOUT";             break;
    }
    NODELET_DEBUG_STREAM("Received event " << str);
    // Debug state changes
    switch (state) {
    case STATE::INITIALIZING:      str = "INITIALIZING";        break;
    case STATE::UNKNOWN:           str = "UNKNOWN";             break;
    case STATE::STOWED:            str = "STOWED";              break;
    case STATE::DEPLOYED:          str = "DEPLOYED";            break;
    case STATE::PANNING:           str = "PANNING";             break;
    case STATE::TILTING:           str = "TILTING";             break;
    case STATE::SETTING:           str = "SETTING";             break;
    case STATE::CALIBRATING:       str = "CALIBRATING";         break;
    case STATE::STOWING_SETTING:   str = "STOWING_SETTING";     break;
    case STATE::STOWING_PANNING:   str = "STOWING_PANNING";     break;
    case STATE::STOWING_TILTING:   str = "STOWING_TILTING";     break;
    case STATE::DEPLOYING_PANNING: str = "DEPLOYING_PANNING";   break;
    case STATE::DEPLOYING_TILTING: str = "DEPLOYING_TILTING";   break;
    }
    NODELET_DEBUG_STREAM("State changed to " << str);
    // Send the procedure state out to the word
    static ff_msgs::ArmState state_msg;
    state_msg.state = state;
    pub_state_.publish(state_msg);
    // Convert to an executive-formatted state, which is a reduced-complexity
    // full state, designed for GDS visualization purposes. The gripper state
    // is updated in parallel through gripper feedback
    static ff_msgs::ArmStateStamped msg;
    msg.header.frame_id = GetPlatform();
    msg.header.stamp = ros::Time::now();
    // Convert our internal state to an ArmGripperState
    double tol = joints_[GRIPPER].tol;
    if (joints_[GRIPPER].val  < 0)
      msg.gripper_state.state = ff_msgs::ArmGripperState::UNCALIBRATED;
    else if (fabs(joints_[GRIPPER].val - K_GRIPPER_CLOSE) < tol)
      msg.gripper_state.state = ff_msgs::ArmGripperState::CLOSED;
    else
      msg.gripper_state.state = ff_msgs::ArmGripperState::OPEN;
    // Convert the internal state to an ArmJointState
    switch (state) {
    default:
    case STATE::UNKNOWN:
      msg.joint_state.state = ff_msgs::ArmJointState::UNKNOWN;
      break;
    case STATE::DEPLOYING_PANNING:
    case STATE::DEPLOYING_TILTING:
      msg.joint_state.state = ff_msgs::ArmJointState::DEPLOYING;
      break;
    case STATE::CALIBRATING:
      msg.gripper_state.state = ff_msgs::ArmGripperState::CALIBRATING;
      // Intentional lack of break keyword
    case STATE::DEPLOYED:
    case STATE::SETTING:
      msg.joint_state.state = ff_msgs::ArmJointState::STOPPED;
      break;
    case STATE::PANNING:
    case STATE::TILTING:
      msg.joint_state.state = ff_msgs::ArmJointState::MOVING;
      break;
    case STATE::STOWING_SETTING:
    case STATE::STOWING_PANNING:
    case STATE::STOWING_TILTING:
      msg.joint_state.state = ff_msgs::ArmJointState::STOWING;
      break;
    case STATE::STOWED:
      msg.joint_state.state = ff_msgs::ArmJointState::STOWED;
      break;
    }
    // Publish the state!
    pub_arm_state_.publish(msg);
  }

  // Complete the current action
  int32_t Result(int32_t response, bool send = false) {
    // Write the current values to the joint states to bring the whole system
    // to a halt. We don't want any movement here. This is the only way that
    // I can think of to stop a position-controller based driver. It's OK if
    // there has been a communication error, as this will be ignored.
    for (JointMap::iterator it = joints_.begin(); it != joints_.end(); it++)
      it->second.goal = it->second.val;
    // Stop pan and tilt, but don't touch the gripper
    // Arm(PAN);
    // Arm(TILT);
    // Send a re
    switch (fsm_.GetState()) {
    case STATE::PANNING:
    case STATE::TILTING:
    case STATE::SETTING:
    case STATE::CALIBRATING:
    case STATE::STOWING_SETTING:
    case STATE::STOWING_PANNING:
    case STATE::STOWING_TILTING:
    case STATE::DEPLOYING_PANNING:
    case STATE::DEPLOYING_TILTING:
      send = true;
    default:
      break;
    }
    // If we need to physically send a response (we are tracking a goal)
    if (send) {
      static ff_msgs::ArmResult result;
      result.response = response;
      if (response > 0)
        server_.SendResult(ff_util::FreeFlyerActionState::SUCCESS, result);
      else if (response < 0)
        server_.SendResult(ff_util::FreeFlyerActionState::ABORTED, result);
      else
        server_.SendResult(ff_util::FreeFlyerActionState::PREEMPTED, result);
    }
    // Special case: if we lose communication with the low level arm controller
    // then we need to go back to the initializing state.
    if (response == RESPONSE::COMMUNICATION_ERROR)
      return STATE::INITIALIZING;
    // The new waiting state depends
    if (IsStowed())
      return STATE::STOWED;
    return STATE::DEPLOYED;
  }

  // Called on registration of aplanner
  bool SetStateCallback(ff_msgs::SetState::Request& req,
                        ff_msgs::SetState::Response& res) {
    fsm_.SetState(req.state);
    res.success = true;
    return true;
  }

  // Check if the two angle are sufficiently close, respecting modular math
  bool Equal(JointType t, double v) {
    return ((180. - fabs(fabs(joints_[t].val - v) - 180.)) < joints_[t].tol);
  }

  // Look at the pan and tilt angles to determine if stowed
  bool IsStowed() {
    if (!Equal(PAN, K_PAN_STOW))
      return false;
    if (!Equal(TILT, K_TILT_STOW))
      return false;
    return true;
  }

  // Gripper only requires closing if it is calibrated
  bool RequiresClosing() {
    if (Equal(GRIPPER, K_GRIPPER_CAL))
      return false;
    return !Equal(GRIPPER, K_GRIPPER_STOW);
  }

  // Send a single joint goal to the low-level controller. We could in
  // principle send multiple goals at once, but lets keep this simple.
  bool Arm(JointType type) {
    // Check that we actually have the joint present
    JointMap::const_iterator it = joints_.find(type);
    if (it == joints_.end()) {
      NODELET_WARN_STREAM("Not a valid control goal");
      return false;
    }
    // Package up the joint state goal
    static sensor_msgs::JointState goal;
    goal.header.stamp = ros::Time::now();
    goal.header.frame_id = GetPlatform();
    goal.name.resize(1);
    goal.position.resize(1);
    // Set the name and conver from human to low-level
    goal.name[0] = it->second.name;
    goal.position[0] =
      (it->second.goal - it->second.offset) / it->second.scale;
    // Publish the new goal
    pub_joint_goals_.publish(goal);
    // Start a timer
    timer_goal_.stop();
    timer_goal_.setPeriod(
      ros::Duration(cfg_.Get<double>("timeout_goal")));
    timer_goal_.start();
    // Success!
    return true;
  }

  // Called whenever the low-level driver produces updated joint states.
  void JointStateCallback(sensor_msgs::JointState::ConstPtr const& msg) {
    // Add the joint sample stamped
    static ff_msgs::JointSampleStamped jss;
    jss.header.stamp = ros::Time::now();
    jss.header.frame_id = GetPlatform();
    jss.samples.clear();
    // Iterate over data
    static JointType generic;
    for (size_t i = 0; i < msg->name.size(); i++) {
      if (dictionary_.find(msg->name[i]) == dictionary_.end())
        continue;
      // Get the generic name
      generic = dictionary_[msg->name[i]];
      // In nominal conditions, joints are scaled like this
      joints_[generic].val =
        msg->position[i] * joints_[generic].scale + joints_[generic].offset;
      // Take care of a special case where the gripper is uncalibrated
      if (joints_[generic].generic == "gripper" &&
        msg->position[i] == K_GRIPPER_CAL) joints_[generic].val =  K_GRIPPER_CAL;
      //  Package up a joint sample
      static ff_msgs::JointSample js;
      js.name = joints_[generic].generic;
      js.angle_pos = joints_[generic].val;   // Human-readable
      js.angle_vel = msg->velocity[i];       // SI
      js.current = msg->effort[i];           // SI
      jss.samples.push_back(js);
    }
    // If we dind't receive any valid joint updates, then we are done
    if (jss.samples.empty())
      return;
    // Reset the watchdog timer
    timer_watchdog_.stop();
    timer_watchdog_.setPeriod(
      ros::Duration(cfg_.Get<double>("timeout_watchdog")));
    timer_watchdog_.start();
    // Update the state machine
    switch (fsm_.GetState()) {
    // Background states
    case STATE::UNKNOWN:
      if (IsStowed())
        return fsm_.Update(STOWED);
      return fsm_.Update(DEPLOYED);
    // Catch a manual deploy event
    case STATE::STOWED:
      if (!IsStowed())
        return fsm_.Update(DEPLOYED);
      break;
    // Catch a manual stow event
    case STATE::DEPLOYED:
      if (IsStowed())
        return fsm_.Update(STOWED);
      break;
    // Pan wait states
    case STATE::INITIALIZING:
      fsm_.Update(READY);
      break;
    case STATE::PANNING:
    case STATE::STOWING_PANNING:
    case STATE::DEPLOYING_PANNING:
      if (Equal(PAN, joints_[PAN].goal)) {
        timer_goal_.stop();
        fsm_.Update(PAN_COMPLETE);
      }
      break;
    // Tilt wait state
    case STATE::TILTING:
    case STATE::STOWING_TILTING:
    case STATE::DEPLOYING_TILTING:
      if (Equal(TILT, joints_[TILT].goal)) {
        timer_goal_.stop();
        fsm_.Update(TILT_COMPLETE);
      }
      break;
    // Gripper wait state
    case STATE::SETTING:
    case STATE::STOWING_SETTING:
      if (Equal(GRIPPER, joints_[GRIPPER].goal)) {
        timer_goal_.stop();
        fsm_.Update(GRIPPER_COMPLETE);
      }
      break;
    // Check calibrating
    case STATE::CALIBRATING:
      if (!Equal(GRIPPER, K_GRIPPER_CAL)) {
        timer_goal_.stop();
        fsm_.Update(CALIBRATE_COMPLETE);
      }
      break;
    // Catch-all for other states
    default:
      return;
    }
    // Publish the updated joint samples
    pub_joint_sample_.publish(jss);
    // Send some feedback
    // Send the feedback if in avalid action state
    static ff_msgs::ArmFeedback feedback;
    switch (fsm_.GetState()) {
    case STATE::PANNING:
    case STATE::TILTING:
    case STATE::SETTING:
    case STATE::CALIBRATING:
    case STATE::STOWING_SETTING:
    case STATE::STOWING_PANNING:
    case STATE::STOWING_TILTING:
    case STATE::DEPLOYING_PANNING:
    case STATE::DEPLOYING_TILTING:
      feedback.state.state = fsm_.GetState();
      feedback.pan = joints_[PAN].val;
      feedback.tilt = joints_[TILT].val;
      feedback.gripper = joints_[GRIPPER].val;
      server_.SendFeedback(feedback);
    default:
      break;
    }
  }

  // Called to fake feedback to executive during a prep
  void TimeoutCallback(ros::TimerEvent const& event) {
    fsm_.Update(TIMEOUT);
  }

  // If the watchdog expires, it means that after connecting we went for a
  // specified period without joint state feedback. In this case we need
  // to send a communication error to the callee.
  void WatchdogCallback(ros::TimerEvent const& event) {
    fsm_.SetState(Result(RESPONSE::COMMUNICATION_ERROR));
  }

  // A new arm action has been called
  void GoalCallback(ff_msgs::ArmGoalConstPtr const& goal) {
    // We are connected
    switch (goal->command) {
    // Stop the arm
    case ff_msgs::ArmGoal::ARM_STOP:
      NODELET_DEBUG_STREAM("Received a new ARM_STOP command");
      joints_[PAN].goal = joints_[PAN].val;
      joints_[TILT].goal = joints_[TILT].val;
      joints_[GRIPPER].goal = joints_[GRIPPER].val;
      return fsm_.Update(GOAL_CANCEL);
    // Deploy the arm
    case ff_msgs::ArmGoal::ARM_DEPLOY:
      NODELET_DEBUG_STREAM("Received a new ARM_DEPLOY command");
      if (fsm_.GetState() == STATE::STOWED) {
        joints_[PAN].goal = K_PAN_DEPLOY;
        joints_[TILT].goal = K_TILT_DEPLOY;
        joints_[GRIPPER].goal = K_GRIPPER_DEPLOY;
        return fsm_.Update(GOAL_DEPLOY);
      }
      break;
    // Stow the arm
    case ff_msgs::ArmGoal::ARM_STOW:
      NODELET_DEBUG_STREAM("Received a new ARM_STOW command");
      if (fsm_.GetState() == STATE::DEPLOYED) {
        joints_[PAN].goal = K_PAN_STOW;
        joints_[TILT].goal = K_TILT_STOW;
        joints_[GRIPPER].goal = K_GRIPPER_STOW;
        return fsm_.Update(GOAL_STOW);
      }
      break;
    // Pan the arm
    case ff_msgs::ArmGoal::ARM_PAN:
    case ff_msgs::ArmGoal::ARM_TILT:
    case ff_msgs::ArmGoal::ARM_MOVE:
      NODELET_DEBUG_STREAM("Received a new ARM_{PAN,TILT,MOVE} command");
      if (fsm_.GetState() == STATE::DEPLOYED ||
          fsm_.GetState() == STATE::STOWED) {
        // Get the new, proposed PAN and TILT values
        double new_p = joints_[PAN].goal;
        if (goal->command == ff_msgs::ArmGoal::ARM_MOVE ||
            goal->command == ff_msgs::ArmGoal::ARM_PAN)
          new_p = goal->pan;
        double new_t = joints_[TILT].goal;
        if (goal->command == ff_msgs::ArmGoal::ARM_MOVE ||
            goal->command == ff_msgs::ArmGoal::ARM_TILT)
          new_t = goal->tilt;
        ROS_INFO_STREAM(new_p);
        ROS_INFO_STREAM(new_t);
        // Simple bounds and self-collision checking
        if (new_t < K_TILT_MIN || new_t > K_TILT_MAX) {
          Result(RESPONSE::BAD_TILT_VALUE, true);
          return;
        }
        if (new_p < K_PAN_MIN || new_p > K_PAN_MAX) {
          Result(RESPONSE::BAD_PAN_VALUE, true);
          return;
        }
        if (new_t > K_TILT_SAFE && fabs(new_p - K_PAN_STOW) > 0.1) {
          Result(RESPONSE::COLLISION_AVOIDED, true);
          return;
        }
        // Set the new goals
        joints_[PAN].goal = new_p;
        joints_[TILT].goal = new_t;
        // Start the action
        return fsm_.Update(GOAL_MOVE);
      }
      break;
    // Calibrate the gripper
    case ff_msgs::ArmGoal::GRIPPER_CALIBRATE:
      NODELET_DEBUG_STREAM("Received a new GRIPPER_CALIBRATE command");
      if (fsm_.GetState() == STATE::DEPLOYED) {
        joints_[TILT].goal = K_GRIPPER_CAL;
        return fsm_.Update(GOAL_CALIBRATE);
      }
      break;
    // Set the gripper
    case ff_msgs::ArmGoal::GRIPPER_SET:
      NODELET_DEBUG_STREAM("Received a new GRIPPER_SET command");
      if (joints_[GRIPPER].val < 0) {
        Result(RESPONSE::NEED_TO_CALIBRATE, true);
        return;
      }
      if (fsm_.GetState() == STATE::DEPLOYED) {
        // Check that th gripper value is reasonable
        if (goal->gripper < K_GRIPPER_CLOSE || goal->gripper > K_GRIPPER_OPEN) {
          Result(RESPONSE::BAD_GRIPPER_VALUE, true);
          return;
        }
        // Set the goal
        joints_[GRIPPER].goal = goal->gripper;
        // Star the action
        return fsm_.Update(GOAL_SET);
      }
      break;
    // Open the gripper
    case ff_msgs::ArmGoal::GRIPPER_OPEN:
      NODELET_DEBUG_STREAM("Received a new GRIPPER_OPEN command");
      if (joints_[GRIPPER].val < 0) {
        Result(RESPONSE::NEED_TO_CALIBRATE, true);
        return;
      }
      if (fsm_.GetState() == STATE::DEPLOYED) {
        joints_[GRIPPER].goal = K_GRIPPER_OPEN;
        return fsm_.Update(GOAL_SET);
      }
      break;
    // Close the gripper
    case ff_msgs::ArmGoal::GRIPPER_CLOSE:
      NODELET_DEBUG_STREAM("Received a new GRIPPER_CLOSE command");
      if (joints_[GRIPPER].val < 0) {
        Result(RESPONSE::NEED_TO_CALIBRATE, true);
        return;
      }
      if (fsm_.GetState() == STATE::DEPLOYED) {
        joints_[GRIPPER].goal = K_GRIPPER_CLOSE;
        return fsm_.Update(GOAL_SET);
      }
      break;
    default:
      // Catch-all for impossible state transitions
      Result(RESPONSE::INVALID_COMMAND, true);
      return;
    }
    // Catch-all for impossible state transitions
    Result(RESPONSE::NOT_ALLOWED, true);
  }

  // Preempt the current action with a new action
  void PreemptCallback() {
    return fsm_.Update(GOAL_CANCEL);
  }

  // A Cancellation request arrives
  void CancelCallback() {
    return fsm_.Update(GOAL_CANCEL);
  }

 protected:
  // Finite state machine
  ff_util::FSM fsm_;
  // Config server
  ff_util::ConfigServer cfg_;
  // Action server
  ff_util::FreeFlyerActionServer<ff_msgs::ArmAction> server_;
  // Data structures supporting lookup of joint data
  JointMap joints_;
  JointDictionary dictionary_;
  // Timers, publishers and subscribers
  ros::Timer timer_goal_;              // Timer for goal to complete
  ros::Timer timer_watchdog_;          // Watchdog timer for LL data
  ros::Publisher pub_state_;           // State publisher
  ros::ServiceServer srv_set_state_;   // Set the current state
  ros::Subscriber sub_joint_states_;   // Joint state subscriber
  ros::Publisher pub_joint_goals_;     // Joint goal publisher
  ros::Publisher pub_arm_state_;       // Executive arm state publisher
  ros::Publisher pub_joint_sample_;    // Executive joint state publisher
  // Constant vales
  static constexpr double K_PAN_OFFSET      =    0.0;
  static constexpr double K_PAN_MIN         =  -90.0;
  static constexpr double K_PAN_MAX         =   90.0;
  static constexpr double K_PAN_STOW        =    0.0;
  static constexpr double K_PAN_DEPLOY      =    0.0;
  static constexpr double K_TILT_OFFSET     =   90.0;
  static constexpr double K_TILT_MIN        =  -20.0;
  static constexpr double K_TILT_MAX        =  180.0;
  static constexpr double K_TILT_STOW       =  180.0;
  static constexpr double K_TILT_DEPLOY     =    0.0;
  static constexpr double K_TILT_SAFE       =   90.0;
  static constexpr double K_GRIPPER_STOW    =   20.0;
  static constexpr double K_GRIPPER_DEPLOY  =   20.0;
  static constexpr double K_GRIPPER_OPEN    =   45.0;
  static constexpr double K_GRIPPER_CLOSE   =   20.0;
  static constexpr double K_GRIPPER_CAL     = -100.0;
  static constexpr double K_RADS_TO_DEGS    = 180.0 / M_PI;
  static constexpr double K_DEGS_TO_RADS    = M_PI / 180.0;
};

PLUGINLIB_DECLARE_CLASS(arm, ArmNodelet,
                        arm::ArmNodelet, nodelet::Nodelet);

}  // namespace arm
