#include <boost/make_shared.hpp>
#include "NFQPlugin.hpp"

using namespace std;
using namespace gazebo;


NFQPlugin::NFQPlugin() : numSteps(0), maxSteps(5000), train(true)
{
    actionInterval.Set(1,0);

    destinationPos.push_back( math::Vector3(0, 0, 0.1) );

    // Forward and backward
//    initialPos.push_back( math::Pose(-2, 0, .12, 0, 0, 0) );
//    initialPos.push_back( math::Pose(2, 0, .12, 0, 0, 0) );

    // Turn left and right
    initialPos.push_back( math::Pose(-4, -2, .12, 0, 0, 0) );
    initialPos.push_back( math::Pose(-4, 2, .12, 0, 0, 0) );
    initialPos.push_back( math::Pose(4, -2, .12, 0, 0, 0) );
    initialPos.push_back( math::Pose(4, 2, .12, 0, 0, 0) );
}


NFQPlugin::~NFQPlugin() {}


void NFQPlugin::Load( physics::ModelPtr model, sdf::ElementPtr sdfPtr )
{
    transport::NodePtr node( new transport::Node() );
    node->Init();
    serverControlPub = node->Advertise<msgs::ServerControl>("/gazebo/server/control");

    loadParameters( model, sdfPtr );

    // onUpdate is called each simulation step.
    // It will be used to publish simulation data (sensors, pose, etc).
    updateConnection = event::Events::ConnectWorldUpdateBegin(
        boost::bind(&NFQPlugin::onUpdate, this, _1));

    // World simulation time will be used to synchronize the actions
    worldPtr = model->GetWorld();
    timeMark = worldPtr->GetSimTime();

    // Apply first action
    roverModel->resetModel();
    firstAction();
}


void NFQPlugin::loadParameters( const physics::ModelPtr &model, const sdf::ElementPtr &sdfPtr )
{
    if( sdfPtr->HasElement( "mode" ) ){
        string execution_mode = sdfPtr->Get<string>("mode");
        if( execution_mode == "train" ){
            train = true;
        }
        if( execution_mode == "test" ){
            train = false;
        }
    }

    if( sdfPtr->HasElement( "max_steps" ) )
        maxSteps = sdfPtr->Get<unsigned>("max_steps");

    roverModel = boost::make_shared<RoverModel>( model, sdfPtr );
    roverModel->setOriginAndDestination( initialPos, destinationPos );

    rlAgent = boost::make_shared<QLearner>( roverModel->getNumActions() );
    rlAgent->loadPolicy();

    const string model_file = "./caffe/network/nfq_gazebo.prototxt";
    string weights_file = "./caffe/models/nfq_gazebo_iter_1000.caffemodel";
    if( sdfPtr->HasElement( "weights_file" ) ){
        weights_file = sdfPtr->Get<string>("weights_file");
    }

    caffeNet = boost::make_shared<CaffeInference>( model_file, weights_file );
}


void NFQPlugin::onUpdate( const common::UpdateInfo &info )
{
    roverModel->velocityController();
    roverModel->steeringWheelController();
    train ? trainAlgorithm() : testAlgorithm();
}


void NFQPlugin::printState( const vector<float> &observed_state ) const
{
    vector<float>::const_iterator it;
    stringstream stream;
    stream << "State = ( ";
    for(it = observed_state.begin(); it != observed_state.end(); ++it)
        stream << setprecision(2) << *it << " ";
    stream << ")";

    gzmsg << stream.str() << endl;
}


vector<float> NFQPlugin::getState() const
{
    vector<float> observed_state;

    const math::Vector3 distance = roverModel->getDistanceState();
    observed_state.push_back( distance.x );
    observed_state.push_back( distance.y );
    observed_state.push_back( distance.z );

    const math::Vector3 orientation = roverModel->getEulerAnglesState();
    // In this dataset the robot wont change roll and pitch
    // observed_state.push_back( orientation.x );
    // observed_state.push_back( orientation.y );
    observed_state.push_back( orientation.z );

    const int velocity = roverModel->getVelocityState();
    observed_state.push_back( velocity );

    const int steering = roverModel->getSteeringState();
    observed_state.push_back( steering );

    printState( observed_state );

    return observed_state;
}


// This function doesn't call updateQValues because the initial position doesn't
// have a last state to update.
void NFQPlugin::firstAction() const
{
    vector<float> observed_state = getState();
    const unsigned state_index = rlAgent->fetchState( observed_state );
    const unsigned action = rlAgent->chooseAction( state_index );
    roverModel->applyAction( action );
    gzmsg << "Applying action = " << action << endl;
}


void NFQPlugin::trainAlgorithm()
{
    bool collision = roverModel->checkCollision();
    if( collision ){
        gzmsg << "Collision detected !!!" << endl;
        const float bad_reward = -100;
        rlAgent->updateQValues( bad_reward );
        // Reset gazebo model to initial position
        roverModel->resetModel();
        firstAction();
    }

    common::Time elapsed_time = worldPtr->GetSimTime() - timeMark;
    if( elapsed_time >= actionInterval ){
        gzmsg << endl;
        gzmsg << "Step = " << numSteps << endl;

        // Terminal state
        if( roverModel->isTerminalState() ){
            gzmsg << "Model reached terminal state !!!" << endl;
            roverModel->resetModel();
            firstAction();

        }else if( roverModel->isDistancing() ){
            // Reset model if it is going in the opposite direction
            // This helps reduce the number of states decreasing the learning time
            gzmsg << "Wrong direction !" << endl;
            roverModel->resetModel();
            firstAction();

        }else{
            vector<float> observed_state = getState();
            const float *input_state = &observed_state[0];

            vector<float> qvalues = caffeNet->Predict( input_state );

            const float reward = roverModel->getReward();
            const unsigned state_index = rlAgent->fetchState( observed_state, qvalues );
            rlAgent->updateQValues( reward, state_index );

            const unsigned action = rlAgent->chooseAction( state_index );
            roverModel->applyAction( action );
            gzmsg << "Applying action = " << action << endl;
        }

        ++numSteps;
        // Terminate simulation after maxSteps
        if( numSteps == maxSteps ){
            rlAgent->savePolicy();

            gzmsg << endl;
            gzmsg << "Simulation reached max number of steps." << endl;
            gzmsg << "Terminating simulation..." << endl;
            msgs::ServerControl server_msg;
            server_msg.set_stop(true);
            serverControlPub->Publish(server_msg);
        }

        timeMark = worldPtr->GetSimTime();
    }
}

void NFQPlugin::testAlgorithm()
{
    common::Time elapsed_time = worldPtr->GetSimTime() - timeMark;
    if( elapsed_time >= actionInterval ){
        if( roverModel->isTerminalState() ){
            gzmsg << "Model reached terminal state !!!" << endl;
            roverModel->resetModel();
        }
        vector<float> observed_state = getState();
        const float *input_state = &observed_state[0];

        vector<float> qvalues = caffeNet->Predict( input_state );
        vector<float>::iterator qvalues_it = max_element( qvalues.begin(), qvalues.end() );
        const unsigned action = distance( qvalues.begin(), qvalues_it );

        roverModel->applyAction( action );
        gzmsg << "Applying action = " << action << endl;
        gzmsg << endl;

        ++numSteps;
        // Terminate simulation after maxStep
        if( numSteps == maxSteps){
            gzmsg << endl;
            gzmsg << "Simulation reached max number of steps." << endl;
            gzmsg << "Terminating simulation..." << endl;
            msgs::ServerControl server_msg;
            server_msg.set_stop(true);
            serverControlPub->Publish(server_msg);
        }

        timeMark = worldPtr->GetSimTime();
    }
}
