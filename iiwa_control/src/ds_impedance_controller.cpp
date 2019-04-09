#include <Eigen/Core>

#include <pluginlib/class_list_macros.hpp>

#include <iiwa_control/ds_impedance_controller.hpp>
#include <iiwa_control/custom_effort_controller.hpp>

namespace iiwa_control {

    void pseudo_inverse(const Eigen::MatrixXd &M, Eigen::MatrixXd &Mpinv, bool damped = true)
    {   
        double lambda_ = damped?0.2:0.0;

        Eigen::JacobiSVD<Eigen::MatrixXd> svd(M, Eigen::ComputeFullU | Eigen::ComputeFullV);
        Eigen::JacobiSVD<Eigen::MatrixXd>::SingularValuesType sing_vals_ = svd.singularValues();
        Eigen::MatrixXd S_ = M; 
        S_.setZero();

        for (int i = 0; i < sing_vals_.size(); i++)
            S_(i,i) = (sing_vals_(i))/(sing_vals_(i)*sing_vals_(i) + lambda_*lambda_);

        Mpinv = Eigen::MatrixXd(svd.matrixV()*S_.transpose()*svd.matrixU().transpose());
    }



    Eigen::Matrix<double,4,1> rotationMatrixToQuaternion(Eigen::Matrix<double,3,3> R)
    {
      Eigen::Matrix<double,4,1> q;

      float r11 = R(0,0);
      float r12 = R(0,1);
      float r13 = R(0,2);
      float r21 = R(1,0);
      float r22 = R(1,1);
      float r23 = R(1,2);
      float r31 = R(2,0);
      float r32 = R(2,1);
      float r33 = R(2,2);

      float tr = r11+r22+r33;
      float tr1 = r11-r22-r33;
      float tr2 = -r11+r22-r33;
      float tr3 = -r11-r22+r33;

      if(tr>0)
      {  
        q(0) = sqrt(1.0f+tr)/2.0f;
        q(1) = (r32-r23)/(4.0f*q(0));
        q(2) = (r13-r31)/(4.0f*q(0));
        q(3) = (r21-r12)/(4.0f*q(0));
      }
      else if((tr1>tr2) && (tr1>tr3))
      {
        q(1) = sqrt(1.0f+tr1)/2.0f;
        q(0) = (r32-r23)/(4.0f*q(1));
        q(2) = (r21+r12)/(4.0f*q(1));
        q(3) = (r31+r13)/(4.0f*q(1));
      }     
      else if((tr2>tr1) && (tr2>tr3))
      {   
        q(2) = sqrt(1.0f+tr2)/2.0f;
        q(0) = (r13-r31)/(4.0f*q(2));
        q(1) = (r21+r12)/(4.0f*q(2));
        q(3) = (r32+r23)/(4.0f*q(2));
      }
      else
      {
        q(3) = sqrt(1.0f+tr3)/2.0f;
        q(0) = (r21-r12)/(4.0f*q(3));
        q(1) = (r31+r13)/(4.0f*q(3));
        q(2) = (r32+r23)/(4.0f*q(3));        
      }

      return q;
    }


    
    Eigen::Matrix<double,3,3> quaternionToRotationMatrix(Eigen::Matrix<double,4,1> q)
    {
      Eigen::Matrix<double,3,3> R;

      double q0 = q(0);
      double q1 = q(1);
      double q2 = q(2);
      double q3 = q(3);

      R(0,0) = q0*q0+q1*q1-q2*q2-q3*q3;
      R(1,0) = 2.0f*(q1*q2+q0*q3);
      R(2,0) = 2.0f*(q1*q3-q0*q2);

      R(0,1) = 2.0f*(q1*q2-q0*q3);
      R(1,1) = q0*q0-q1*q1+q2*q2-q3*q3;
      R(2,1) = 2.0f*(q2*q3+q0*q1);

      R(0,2) = 2.0f*(q1*q3+q0*q2);
      R(1,2) = 2.0f*(q2*q3-q0*q1);
      R(2,2) = q0*q0-q1*q1-q2*q2+q3*q3;  

      return R;
    }


    
    void quaternionToAxisAngle(Eigen::Matrix<double,4,1> q, Eigen::Matrix<double,3,1> &axis, double &angle)
    {
      if((q.segment(1,3)).norm() < 1e-3f)
      {
        axis = q.segment(1,3);
      }
      else
      {
        axis = q.segment(1,3)/(q.segment(1,3)).norm();
        
      }

      angle = 2*std::acos(q(0));
    }

    DSImpedanceController::DSImpedanceController()
    {
        rotationalStiffness_= 0.0f;
        rotationalDamping_= 0.0f;
        jointLimitsGain_= 0.0f;
        desiredJointsGain_ = 0.0f;
        jointVelocitiesGain_ = 0.0f;

        j0_.resize(7);
        j0_ << -0.0f, 0.75f, 0.0f, -1.65f, 0, 0.76f, 0.0f;
    }

    DSImpedanceController::~DSImpedanceController() { sub_command_.shutdown(); }

    bool DSImpedanceController::init(hardware_interface::EffortJointInterface* hw, ros::NodeHandle& n)
    {
        // List of controlled joints
        joint_names_ = hw->getNames();
        n_joints_ = joint_names_.size();

        if (n_joints_ == 0) {
            ROS_ERROR_STREAM("List of joint names is empty.");
            return false;
        }

        std::cerr << n_joints_<< std::endl;
        for (unsigned i=0; i<n_joints_; i++)
            ROS_INFO("Got joint %s", joint_names_[i].c_str());

        // Get URDF
        urdf::Model urdf;
        if (!urdf.initParam("robot_description")) {
            ROS_ERROR("Failed to parse urdf file");
            return false;
        }

        // // Get the URDF XML from the parameter server
        std::string urdf_string, full_param;
        std::string robot_description = "robot_description";
        std::string end_effector;

        // // gets the location of the robot description on the parameter server
        if (!n.searchParam(robot_description, full_param)) {
            ROS_ERROR("Could not find parameter %s on parameter server", robot_description.c_str());
            return false;
        }

        // // search and wait for robot_description on param server
        while (urdf_string.empty()) {
            ROS_INFO_ONCE_NAMED("CustomJointStateController", "CustomJointStateController is waiting for model"
                                                          " URDF in parameter [%s] on the ROS param server.",
                robot_description.c_str());

            n.getParam(full_param, urdf_string);

            usleep(100000);
        }

        ROS_INFO_STREAM_NAMED("CustomJointStateController", "Received urdf from param server, parsing...");

        // Get the end-effector
        n.param<std::string>("end_effector", end_effector, "iiwa_link_ee");

        // Initialize iiwa tools
        tools_.init_rbdyn(urdf_string, end_effector);

        // Get controller's parameters
        std::vector<double> eigvals_vec(3);
        eigvals_vec[0] = 1.0f;
        eigvals_vec[1] = 1.0f;
        eigvals_vec[2] = 1.0f;

        // Init Controller
        std::cerr << eigvals_vec[0] << eigvals_vec[1] << eigvals_vec[2] << std::endl;
        passive_ds_.SetParams(3, eigvals_vec);
        passive_ds_.Init();

        for (unsigned int i = 0; i < n_joints_; i++) {
            try {
                joints_.push_back(hw->getHandle(joint_names_[i]));
            }
            catch (const hardware_interface::HardwareInterfaceException& e) {
                ROS_ERROR_STREAM("Exception thrown: " << e.what());
                return false;
            }

            urdf::JointConstSharedPtr joint_urdf = urdf.getJoint(joint_names_[i]);
            if (!joint_urdf) {
                ROS_ERROR("Could not find joint '%s' in urdf", joint_names_[i].c_str());
                return false;
            }
            joint_urdfs_.push_back(joint_urdf);
        }

        commands_buffer_.writeFromNonRT(std::vector<double>(10, 0.0));

        _dynamicServerParam.reset(new dynamic_reconfigure::Server< iiwa_control::DSImpedance_paramConfig>(ros::NodeHandle(n.getNamespace())));
        _dynamicServerParam->setCallback(boost::bind(&DSImpedanceController::dynamicReconfigureCallback, this, _1, _2));

        sub_command_ = n.subscribe<std_msgs::Float64MultiArray>("command", 1, &DSImpedanceController::commandCB, this);

        return true;
    }

    void DSImpedanceController::update(const ros::Time& time, const ros::Duration& period)
    {
        if(firstCommand_) {

            iiwa_tools::RobotState robot_state;
            robot_state.position.resize(n_joints_);
            robot_state.velocity.resize(n_joints_);

            for (size_t i = 0; i < n_joints_; i++) {
                robot_state.position[i] = joints_[i].getPosition();
                robot_state.velocity[i] = joints_[i].getVelocity();
            }

            auto ee_state = tools_.perform_fk(robot_state);

            x_ = ee_state.translation;
            q_ << ee_state.orientation.w(), ee_state.orientation.x(), ee_state.orientation.y(), ee_state.orientation.z();
            
            // Call jacobian and robot twist
            Eigen::MatrixXd jac(6, n_joints_);
            bool jac_valid = false;

            jac = tools_.jacobian(robot_state);

            Eigen::Vector3d v, omega;
            Eigen::VectorXd twist, j, jv;
            twist.resize(6);
            j.resize(n_joints_);
            jv.resize(n_joints_);

            for (unsigned int i = 0; i < n_joints_; i++) {
                j(i) = joints_[i].getPosition();
                jv(i) = joints_[i].getVelocity();
            }

            twist = jac * jv;
            omega = twist.segment(0,3);
            v = twist.segment(3,3);

            // Get desired linear and angular velocity and quaternion
            std::vector<double>& commands = *commands_buffer_.readFromRT();
            vd_ << commands[0], commands[1], commands[2];
            omegad_ << commands[3], commands[4], commands[5];
            qd_ << commands[6], commands[7], commands[8], commands[9];

            // Update desired velocity in passive DS
            robot_controllers::RobotState state;
            state.velocity_ = vd_;
            passive_ds_.SetInput(state);

            // Update current velocity
            state.velocity_ = v;
            passive_ds_.Update(state);

            // Get linear force
            Eigen::Vector3d Flin = passive_ds_.GetOutput().desired_.force_;

            // Get rotational force
            Eigen::Vector3d axis;
            double angle;

            Eigen::Matrix3d Rd, R, Re;
            R = quaternionToRotationMatrix(q_);
            Rd = quaternionToRotationMatrix(qd_);
            Re = Rd*R.inverse();
            Eigen::Vector4d qe;
            qe = rotationMatrixToQuaternion(Re);
            quaternionToAxisAngle(qe,axis,angle);

            angle = 2*std::acos(qe(0));

            Eigen::Vector3d Frot;

            Frot = rotationalStiffness_*angle*axis+rotationalDamping_*(omegad_-omega);

            // Compute wrench
            Eigen::VectorXd wrench, torques;
            wrench.resize(6);
            wrench.segment(3,3) = Flin;
            wrench.segment(0,3) = Frot;

            torques.resize(n_joints_);

            // Compute torques
            torques = jac.transpose()*wrench;

            // Compute nullspace torques
            Eigen::VectorXd nullspaceTorques;
            nullspaceTorques.resize(n_joints_);
            Eigen::MatrixXd jacPinv, N;
            jacPinv.resize(6,n_joints_);
            pseudo_inverse(jac.transpose(), jacPinv);
            N.resize(n_joints_,n_joints_);
            N = Eigen::MatrixXd::Identity(n_joints_,n_joints_)-jac.transpose()*jacPinv;
            nullspaceTorques = N*(-jointLimitsGain_*j-desiredJointsGain_*(j-j0_)-jointVelocitiesGain_*jv);

            if(useNullSpace_) {
                torques += nullspaceTorques;
            }

            std::vector<double> commanded_effort(n_joints_, 0.);

            Eigen::VectorXd::Map(commanded_effort.data(), commanded_effort.size()) = torques;

            for (unsigned int i = 0; i < n_joints_; i++) {
                enforceJointLimits(commanded_effort[i], i);
                joints_[i].setCommand(commanded_effort[i]);
            }
        }
        else {
            for (unsigned int i = 0; i < n_joints_; i++) {
                joints_[i].setCommand(0.);
            }
        }
    }

    void DSImpedanceController::commandCB(const std_msgs::Float64MultiArrayConstPtr& msg)
    {
        if (msg->data.size() != n_joints_ && msg->data.size() != 10) {
            ROS_ERROR_STREAM("Dimension of command (" << msg->data.size() << ") is not correct! Not executing!");
            return;
        }

        commands_buffer_.writeFromNonRT(msg->data);

        if(!firstCommand_) {
            firstCommand_ = true;
        }
    }

    void DSImpedanceController::enforceJointLimits(double& command, unsigned int index)
    {
        // Check that this joint has applicable limits
        if (command > joint_urdfs_[index]->limits->effort) // above upper limit
        {
            command = joint_urdfs_[index]->limits->effort;
        }
        else if (command < -joint_urdfs_[index]->limits->effort) // below lower limit
        {
            command = -joint_urdfs_[index]->limits->effort;
        }
    }

    void DSImpedanceController::dynamicReconfigureCallback(iiwa_control::DSImpedance_paramConfig& config,uint32_t level)
    {
        std::vector<double> eigvals_vec;
        eigvals_vec.push_back(config.gain0);
        eigvals_vec.push_back(config.gain1);
        eigvals_vec.push_back(config.gain1);
        passive_ds_.SetParams(3,eigvals_vec);
        rotationalStiffness_ = config.rotationalStiffness;
        rotationalDamping_   = config.rotationalDamping;
        useNullSpace_ = config.useNullSpace;
        jointLimitsGain_ = config.jointLimitsGain;
        desiredJointsGain_ = config.desiredJointsGain;
        jointVelocitiesGain_ = config.jointVelocitiesGain;
    }

} // namespace iiwa_control

PLUGINLIB_EXPORT_CLASS(iiwa_control::DSImpedanceController, controller_interface::ControllerBase)
