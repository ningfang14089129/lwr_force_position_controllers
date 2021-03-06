#include <pluginlib/class_list_macros.h>

// KDL
#include <kdl/chainfksolverpos_recursive.hpp>
#include <kdl/chainiksolverpos_lma.hpp>
#include <kdl_conversions/kdl_msg.h>
#include <kdl/rigidbodyinertia.hpp>
#include <kdl/rotationalinertia.hpp>

#include <angles/angles.h>
#include <eigen_conversions/eigen_kdl.h>
#include <math.h>
#include <ros/package.h>
#include <yaml-cpp/yaml.h>

#include <lwr_force_position_controllers/cartesian_position_controller.h>

#include <lwr_force_position_controllers/CartesianPositionJointsMsg.h>

//-------------------------------------
// GAIN CONSTANTS
//-------------------------------------
#define DEFAULT_KP 800
#define DEFAULT_KP_A4 1000
#define DEFAULT_KP_A5 1000
#define DEFAULT_KP_A6 18000
#define DEFAULT_KD 30
#define DEFAULT_KD_A4 30
#define DEFAULT_KD_A5 30
#define DEFAULT_KD_A6 30

//------------------------------------------------------------------------------
//TRAJECTORY GENERATION CONSTANTS
//------------------------------------------------------------------------------
#define FINAL_TIME 5.0 
#define TRAJ_5 6.0   // 5-th coeff. of trajectory polynomial
#define TRAJ_4 -15.0 // 4-th coeff. of trajectory polynomial
#define TRAJ_3 10.0  // 3-th coeff. of trajectory polynomial

namespace lwr_controllers {

  CartesianPositionController::CartesianPositionController() {}

  CartesianPositionController::~CartesianPositionController() {}

  bool CartesianPositionController::init(hardware_interface::EffortJointInterface *robot, ros::NodeHandle &n)
  {
    KinematicChainControllerBase<hardware_interface::EffortJointInterface>::init(robot, n);

    // get use_simulation parameter from rosparam server
    ros::NodeHandle nh;
    nh.getParam("/use_simulation", use_simulation_);

    // get publish rate from rosparam
    n.getParam("publish_rate", publish_rate_);

    // add end-effector in kdl chain
    extend_chain(n);

    // instantiate solvers
    dyn_param_solver_.reset(new KDL::ChainDynParam(extended_chain_, gravity_));
    ik_solver_.reset(new KDL::ChainIkSolverPos_LMA(extended_chain_));
    ee_fk_solver_.reset(new KDL::ChainFkSolverPos_recursive(extended_chain_));
    if (use_simulation_)
      {
    	jacobian_solver_.reset(new KDL::ChainJntToJacSolver(kdl_chain_));
    	fk_solver_.reset(new KDL::ChainFkSolverPos_recursive(kdl_chain_));
      }
    
    // set the default gains
    kp_ = DEFAULT_KP;
    kp_a4_ = DEFAULT_KP_A4;
    kp_a5_ = DEFAULT_KP_A5;
    kp_a6_ = DEFAULT_KP_A6;
    kd_ = DEFAULT_KD;
    kd_a4_ = DEFAULT_KD_A4;
    kd_a5_ = DEFAULT_KD_A5;
    kd_a6_ = DEFAULT_KD_A6;

    // set default trajectory duration
    p2p_traj_duration_ = FINAL_TIME;

    // resize desired trajectory
    traj_des_.resize(kdl_chain_.getNrOfJoints());
    traj_a0_.resize(kdl_chain_.getNrOfJoints());
    traj_a3_.resize(kdl_chain_.getNrOfJoints());
    traj_a4_.resize(kdl_chain_.getNrOfJoints());
    traj_a5_.resize(kdl_chain_.getNrOfJoints());
    prev_q_setpoint_.resize(kdl_chain_.getNrOfJoints());

    // advertise CartesianPositionCommand services
    set_cmd_gains_service_ = n.advertiseService("set_cartpos_gains_cmd", \
						&CartesianPositionController::set_cmd_gains, this); 
    set_cmd_traj_service_ = n.advertiseService("set_cartpos_traj_cmd", \
					       &CartesianPositionController::set_cmd_traj, this); 
    get_cmd_gains_service_ = n.advertiseService("get_cartpos_gains_cmd", \
						&CartesianPositionController::get_cmd_gains, this); 
    get_cmd_traj_service_ = n.advertiseService("get_cartpos_traj_cmd", \
					       &CartesianPositionController::get_cmd_traj, this); 

    // advertise topics
    pub_error_ = n.advertise<lwr_force_position_controllers::CartesianPositionJointsMsg>("error", 1000);
    pub_q_des_ = n.advertise<lwr_force_position_controllers::CartesianPositionJointsMsg>("q_des", 1000);

    if(use_simulation_)
      {
    	// subscribe to force/torque sensor topic
    	// (simulation only since it is required to compensate for the mass tool,
    	// the real kuka compensate for mass tool internally)
    	sub_force_ = n.subscribe("/lwr/ft_sensor", 1,\
    				 &CartesianPositionController::ft_sensor_callback, this);
      }

    return true;
  }

  void CartesianPositionController::load_calib_data(double& total_mass, KDL::Vector& p_sensor_tool_com)
  {
    std::string file_name = ros::package::getPath("lwr_force_position_controllers") +\
      "/config/ft_calib_data.yaml";
    YAML::Node ft_data_yaml = YAML::LoadFile(file_name);

    std::vector<double> p_sensor_tool_com_vec(6);
    // get data from the yaml file
    double tool_mass = ft_data_yaml["gripper_mass"].as<double>();
    p_sensor_tool_com_vec = ft_data_yaml["gripper_com_pose"].as<std::vector<double>>();

    // transform to KDL
    for (int i=0; i<3; i++)
	p_sensor_tool_com.data[i] = p_sensor_tool_com_vec[i];

    // compensate for additional items attached
    double mass_sensor_support = 0.194;
    double mass_sensor = 0.099;
    total_mass = tool_mass + mass_sensor_support + mass_sensor;
    
    p_sensor_tool_com.x(tool_mass * p_sensor_tool_com.x() / total_mass);
    p_sensor_tool_com.y(tool_mass * p_sensor_tool_com.y() / total_mass);
    p_sensor_tool_com.z((mass_sensor_support * 0.013 + mass_sensor * 0.0335 + \
			 tool_mass * (0.041 + p_sensor_tool_com.z())) / total_mass);

  }

  void CartesianPositionController::starting(const ros::Time& time)
  {

    // set desired quantities
    time_ = p2p_traj_duration_;
    for(size_t i=0; i<kdl_chain_.getNrOfJoints(); i++)
      {
	traj_a0_(i) = joint_handles_[i].getPosition();
	traj_a3_(i) = 0;
	traj_a4_(i) = 0;
	traj_a5_(i) = 0;
	prev_q_setpoint_(i) = joint_handles_[i].getPosition();
      }
    // initialize publish time
    last_publish_time_ = time;
  }

  void CartesianPositionController::update_fri_inertia_matrix(Eigen::MatrixXd& fri_B)
  {
    int n_joints = kdl_chain_.getNrOfJoints();
    for(int i = 0; i < n_joints; i++)
      for(int j = 0; j < n_joints; j++)
	fri_B(i,j) = inertia_matrix_handles_[i * n_joints + j].getPosition();
  }

  void CartesianPositionController::update(const ros::Time& time, const ros::Duration& period)
  {
    // get current configuration (position and velocity)
    for(size_t i=0; i<joint_handles_.size(); i++)
      {
	joint_msr_states_.q(i) = joint_handles_[i].getPosition();
	joint_msr_states_.qdot(i) = joint_handles_[i].getVelocity();
      }

    Eigen::MatrixXd fri_B (joint_handles_.size(), joint_handles_.size());
    update_fri_inertia_matrix(fri_B);
      
    // compute control law
    KDL::JntArray tau_cmd;
    KDL::JntArray q_error, qdot_error;
    tau_cmd.resize(kdl_chain_.getNrOfJoints());
    q_error.resize(kdl_chain_.getNrOfJoints());
    qdot_error.resize(kdl_chain_.getNrOfJoints());

    evaluate_traj_des(period);
    for(size_t i=0; i<joint_handles_.size() ; i++)
      {
	q_error(i) = traj_des_.q(i) - joint_msr_states_.q(i);
	q_error(i) = angles::normalize_angle(q_error(i));
	qdot_error(i) = traj_des_.qdot(i) - joint_msr_states_.qdot(i);
	if(i == 4)
	  tau_cmd(i) = kp_a4_ * q_error(i) +  kd_a4_ * qdot_error(i) + traj_des_.qdotdot(i);
	else if(i == 5)
	  tau_cmd(i) = kp_a5_ * q_error(i) +  kd_a5_ * qdot_error(i) + traj_des_.qdotdot(i);
	else if (i == 6)
	  tau_cmd(i) = kp_a6_ * q_error(i) +  kd_a6_ * qdot_error(i) + traj_des_.qdotdot(i);
	else
	  tau_cmd(i) = kp_ * q_error(i) +  kd_ * qdot_error(i) + traj_des_.qdotdot(i);
      }
    
    KDL::JntSpaceInertiaMatrix B;
    KDL::JntArray C;
    B.resize(kdl_chain_.getNrOfJoints());
    C.resize(kdl_chain_.getNrOfJoints());
    // evaluate inertia matrix and coriolis effort required to invert the dynamics
    dyn_param_solver_->JntToMass(joint_msr_states_.q, B);
    dyn_param_solver_->JntToCoriolis(joint_msr_states_.q, joint_msr_states_.qdot, C);
        
    KDL::Jacobian J_;
    Eigen::Matrix<double, 6,1> base_F_wrist_;
    if (use_simulation_)
      {
    	////////////////////////////////////////////////////////////////////////////////
    	// evaluate the wrench applied to the wrist from the force/torque sensor
    	// and project it in the world frame (base)
    	// this is required to compensate for additional masses attached past the wrist
    	// (simulation only, the real kuka compensate for mass tool internally)
    	////////////////////////////////////////////////////////////////////////////////
    	//
	
    	// evaluate the current geometric jacobian J(q)
    	J_.resize(kdl_chain_.getNrOfJoints());
    	jacobian_solver_->JntToJac(joint_msr_states_.q, J_);
	
    	// evaluate the forward kinematics required to project the wrench
    	KDL::Frame fk_frame;
    	KDL::Wrench base_wrench_wrist;
    	fk_solver_->JntToCart(joint_msr_states_.q, fk_frame);
    	base_wrench_wrist = fk_frame.M * wrench_wrist_;
	
    	// write down onto an eigen vector
    	tf::wrenchKDLToEigen(base_wrench_wrist, base_F_wrist_);      
    	//
    	///////////////////////////////////////////////////////////////////////////////
      }

    // evaluate B * tau_cmd
    KDL::JntArray B_tau_cmd;
    B_tau_cmd.resize(kdl_chain_.getNrOfJoints());
    if (use_simulation_)
      // use J * base_F_wrist as a way to compensate for the mass of the tool (simulation only)
      B_tau_cmd.data = B.data * tau_cmd.data + C.data;
    else
      // use kdl model in real scenario
      B_tau_cmd.data = B.data * tau_cmd.data + C.data;
    
    // set joint efforts
    for(size_t i=0; i<kdl_chain_.getNrOfJoints(); i++)
      {
	
	joint_handles_[i].setCommand(B_tau_cmd(i));
	
	// required to exploit the JOINT IMPEDANCE MODE of the kuka manipulator
	joint_stiffness_handles_[i].setCommand(0);
	joint_damping_handles_[i].setCommand(0);
	joint_set_point_handles_[i].setCommand(joint_msr_states_.q(i));
      }

    if(time > last_publish_time_ + ros::Duration(1.0 / publish_rate_))
      {
	//update next tick
	last_publish_time_ += ros::Duration(1.0 / publish_rate_);
	
	// publish data
	publish_data(pub_error_, q_error);
	publish_data(pub_q_des_, traj_des_.q);
      }
  }

  void CartesianPositionController::publish_data(ros::Publisher& pub, KDL::JntArray& array)
  {
    lwr_force_position_controllers::CartesianPositionJointsMsg msg;
    msg.header.stamp = ros::Time::now();
    msg.a1 = array(0);
    msg.a2 = array(1);
    msg.e1 = array(2);
    msg.a3 = array(3);
    msg.a4 = array(4);
    msg.a5 = array(5);
    msg.a6 = array(6);
    pub.publish(msg);
  }
  
  void CartesianPositionController::ft_sensor_callback(const geometry_msgs::WrenchStamped::ConstPtr& msg)
  {    
    KDL::Wrench wrench_wrist_topic;
    tf::wrenchMsgToKDL(msg->wrench, wrench_wrist_topic);
    
    // reverse the measured force so that wrench_wrist represents 
    // the force applied on the environment by the end-effector
    wrench_wrist_ = - wrench_wrist_topic;
  }
  
  bool CartesianPositionController::set_cmd_traj(lwr_force_position_controllers::CartesianPositionCommandTraj::Request &req,\
						 lwr_force_position_controllers::CartesianPositionCommandTraj::Response &res)
  {
    if (time_ < p2p_traj_duration_)
      {
	res.command.elapsed_time = time_;
	res.command.accepted = false;
	res.command.p2p_traj_duration = p2p_traj_duration_;

	return true;
      }
    res.command.accepted = true;
    
    KDL::Vector des_pose = KDL::Vector::Zero();
    KDL::Rotation des_attitude = KDL::Rotation::Identity();

    // set the desired position requested by the user
    des_pose.x(req.command.x);
    des_pose.y(req.command.y);
    des_pose.z(req.command.z);

    // set the desired attitude requested by the user
    des_attitude = KDL::Rotation::RPY(req.command.roll,\
  				      req.command.pitch,\
  				      req.command.yaw);

    // set p2p_traj_duration
    p2p_traj_duration_ = req.command.p2p_traj_duration;

    // evaluate new trajectory
    evaluate_traj_constants(des_pose, des_attitude);

    return true;

  }

  bool CartesianPositionController::set_cmd_gains(lwr_force_position_controllers::CartesianPositionCommandGains::Request &req, \
						  lwr_force_position_controllers::CartesianPositionCommandGains::Response &res)
  {
    kp_ = req.command.kp;
    kp_a4_ = req.command.kp_a4;
    kp_a5_ = req.command.kp_a5;
    kp_a6_ = req.command.kp_a6;
    kd_ = req.command.kd;
    kd_a4_ = req.command.kd_a4;
    kd_a5_ = req.command.kd_a5;
    kd_a6_ = req.command.kd_a6;
    return true;
  }

  bool CartesianPositionController::get_cmd_traj(lwr_force_position_controllers::CartesianPositionCommandTraj::Request &req,\
						 lwr_force_position_controllers::CartesianPositionCommandTraj::Response &res)
  {
    KDL::Frame ee_fk_frame;
    double yaw, pitch, roll;
    KDL::JntArray q;

    q.resize(kdl_chain_.getNrOfJoints());
    for(size_t i=0; i<kdl_chain_.getNrOfJoints(); i++)
	q(i) = joint_handles_[i].getPosition();

    ee_fk_solver_->JntToCart(q, ee_fk_frame);
    ee_fk_frame.M.GetRPY(roll, pitch, yaw);
    
    // get desired position 
    res.command.x = ee_fk_frame.p.x();
    res.command.y = ee_fk_frame.p.y();
    res.command.z = ee_fk_frame.p.z();

    // get desired attitude
    res.command.yaw = yaw;
    res.command.pitch = pitch;
    res.command.roll = roll;

    // get p2p_traj_duration
    res.command.p2p_traj_duration = p2p_traj_duration_;

    // get remaining time
    res.command.elapsed_time = time_;

    return true;
  }

  bool CartesianPositionController::get_cmd_gains(lwr_force_position_controllers::CartesianPositionCommandGains::Request &req,\
						  lwr_force_position_controllers::CartesianPositionCommandGains::Response &res)
  {
    // get desired gain
    res.command.kp = kp_;
    res.command.kp_a4 = kp_a4_;
    res.command.kp_a5 = kp_a5_;
    res.command.kp_a6 = kp_a6_;

    res.command.kd = kd_;
    res.command.kd_a4 = kd_a4_;
    res.command.kd_a5 = kd_a5_;
    res.command.kd_a6 = kd_a6_;

    return true;
  }

  void CartesianPositionController::evaluate_traj_constants(KDL::Vector& des_pose,\
							    KDL::Rotation& des_attitude)
  {    
    // evaluate the new desired configuration q_des

    KDL::Frame x_des;
    KDL::JntArray q_des;
    q_des.resize(kdl_chain_.getNrOfJoints());

    // get the current robot configuration
    for(size_t i=0; i<joint_handles_.size(); i++)
      joint_msr_states_.q(i) = joint_handles_[i].getPosition();

    // compose the desired frame
    x_des = KDL::Frame(des_attitude, des_pose);

    // evaluate q_des = ik(x_des)
    ik_solver_->CartToJnt(joint_msr_states_.q, x_des, q_des);

    // normalize q_des between - M_PI and M_PI
    for(int i=0; i<joint_handles_.size(); i++) 
      q_des(i) =  angles::normalize_angle(q_des(i));

    // verify that q_des respect joint position limits 
    print_joint_array(q_des);
    for(int i=0; i<joint_handles_.size(); i++) 
      if((q_des(i) <=  joint_limits_.min(i)) || (q_des(i) >= joint_limits_.max(i)))
	{
	  std::cout<< "[cart_pos]Sorry the configuration found exceeds joint limits! Try again." <<std::endl;
	  return;
	}

    // evaluate trajectory constants and update prev_q_setpoint
    for(int i=0; i<joint_handles_.size(); i++)
      {
	// evaluate trajectory constant
	traj_a0_(i) = prev_q_setpoint_(i);
	traj_a3_(i) = TRAJ_3 / pow(p2p_traj_duration_, 3) * (q_des(i) - prev_q_setpoint_(i));
	traj_a4_(i) = TRAJ_4 / pow(p2p_traj_duration_, 4) * (q_des(i) - prev_q_setpoint_(i));
	traj_a5_(i) = TRAJ_5 / pow(p2p_traj_duration_, 5) * (q_des(i) - prev_q_setpoint_(i));
	
	// update prev_q_setpoint
	prev_q_setpoint_(i) = q_des(i);
      }
    time_ = 0;
    
    return;
  }

  void CartesianPositionController::evaluate_traj_des(const ros::Duration& period)
  {
    time_ += period.toSec();
    if(time_ >= p2p_traj_duration_)
      time_ = p2p_traj_duration_;
    
    for(int i=0; i<joint_handles_.size(); i++)
      {
	// q_des
	traj_des_.q(i) = traj_a5_(i) * pow(time_, 5) + traj_a4_(i) * pow(time_, 4) + \
	  traj_a3_(i) * pow(time_, 3) + traj_a0_(i);

	// qdot_des
	traj_des_.qdot(i) = 5 * traj_a5_(i) * pow(time_, 4) + 4 * traj_a4_(i) * pow(time_, 3) + \
	  3 * traj_a3_(i) * pow(time_, 2);

	// qdotdot_des
	traj_des_.qdotdot(i) = 4 * 5 * traj_a5_(i) * pow(time_, 3) + 3 * 4 * traj_a4_(i) * pow(time_, 2) + \
	  2 * 3 * traj_a3_(i) * time_;

      }
  }

  void CartesianPositionController::extend_chain(ros::NodeHandle &n)
  {
    // get the p_wrist_ee arm from the rosparam server
    std::vector<double> p_wrist_ee;
    n.getParam("p_wrist_ee", p_wrist_ee);
    p_wrist_ee_ = KDL::Vector(p_wrist_ee.at(0), p_wrist_ee.at(1), p_wrist_ee.at(2));

    // Extend the default chain with a fake segment in order to evaluate
    // Jacobians and forward kinematics with respect to a given reference point
    // (typicallly the tool tip)
    // also takes into account end effector mass and inertia
    KDL::Joint fake_joint = KDL::Joint();
    KDL::Frame frame(KDL::Rotation::Identity(), p_wrist_ee_);

    double total_mass;
    double fake_cylinder_radius  = 0.0475;
    double fake_cylinder_height = 0.31;
    KDL::Vector p_sensor_tool_com;
    load_calib_data(total_mass, p_sensor_tool_com);
        
    double fake_cyl_i_xx = 1.0 / 12.0 * total_mass * (3 * pow(fake_cylinder_radius, 2) + \
    						     pow(fake_cylinder_height, 2));
    double fake_cyl_i_zz = 1.0 / 2.0 * total_mass * pow(fake_cylinder_radius, 2);

    KDL::RotationalInertia rot_inertia(fake_cyl_i_xx, fake_cyl_i_xx, fake_cyl_i_zz);
    KDL::RigidBodyInertia inertia(total_mass, p_sensor_tool_com, rot_inertia);

    KDL::Segment fake_segment(fake_joint, frame, inertia);
    extended_chain_ = kdl_chain_;
    extended_chain_.addSegment(fake_segment);
  }

  void CartesianPositionController::print_joint_array(KDL::JntArray& array)
  {
    std::cout << std::fixed;
    std::cout << std::setprecision(3);
	
    std::cout<<"joint \t q_des \t q_min\t q_max \t"<<std::endl;
    std::cout<<"*******************************"<<std::endl;

    for(int i=0; i<joint_handles_.size(); i++)
      {
	std::cout << std::setprecision(2);
	std::cout << i << "\t" << 180.0 / M_PI * array(i);
	std::cout << std::setprecision(0);
	std::cout << "\t" << 180.0 / M_PI * joint_limits_.min(i);
	std::cout << "\t" << 180.0 / M_PI * joint_limits_.max(i) << std::endl;
      }
    
    std::cout<<"*******************************"<<std::endl;
  }

}// namespace

PLUGINLIB_EXPORT_CLASS(lwr_controllers::CartesianPositionController, controller_interface::ControllerBase)
