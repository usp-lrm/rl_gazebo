// Copyright © 2017 Thomio Watanabe
// Universidade de Sao Paulo
// Laboratorio de robotica movel
// January 2017

#include <gazebo/transport/transport.hh>
#include <gazebo/physics/physics.hh>
#include <gazebo/common/common.hh>

#include "CaffeInference.hpp"
#include "RoverModel.hpp"
#include "QLearner.hpp"


namespace gazebo{
    class DRLPlugin : public ModelPlugin{
        public:
        DRLPlugin();
        ~DRLPlugin();

        private:
        void Load( physics::ModelPtr model, sdf::ElementPtr sdf );
        void onUpdate( const common::UpdateInfo &info );
        void printState( const std::vector<float> &observed_state );
        std::vector<float> getState();
        void trainAlgorithm();
        void testAlgorithm();

        transport::PublisherPtr serverControlPub;
        event::ConnectionPtr updateConnection;
        boost::shared_ptr<RoverModel> roverModel;
        boost::shared_ptr<QLearner> rlAgent;
        boost::shared_ptr<CaffeInference> caffeNet;
        // Counts the time between the action and its result
        common::Time actionInterval, timeMark;
        physics::WorldPtr worldPtr;

        std::vector<math::Pose> initialPos;
        std::vector<math::Vector3> destinationPos;

        unsigned maxSteps, numSteps;
        bool trainNet;
    };
    GZ_REGISTER_MODEL_PLUGIN(DRLPlugin)
}
