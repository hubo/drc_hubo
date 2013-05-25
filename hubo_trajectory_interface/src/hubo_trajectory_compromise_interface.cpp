/*
 * Copyright (c) 2013, Calder Phillips-Grafflin (WPI) and M.X. Grey (Georgia Tech), Drexel DARPA Robotics Challenge team
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
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
 */
#include <stdlib.h>
#include <vector>
// System includes to handle safe shutdown
#include <signal.h>
// ROS & includes
#include <ros/ros.h>
// Boost includes
#include <boost/thread.hpp>
// Message and action includes for Hubo actions
#include <trajectory_msgs/JointTrajectory.h>
#include <hubo_robot_msgs/JointTrajectoryState.h>
#include <trajectory_msgs/JointTrajectoryPoint.h>
// Includes for ACH and hubo-motion-rt
#include <ach.h>
#include <hubo.h>
#include <motion-trajectory.h>

// ACH channels
ach_channel_t chan_traj_cmd;
ach_channel_t chan_traj_state;
ach_channel_t chan_hubo_state;
ach_channel_t chan_hubo_ref_filter;

/*
 * These are the two values we don't quite know how to set - we actually want these
 * to be "slow" - i.e. small trajectory chunks at a comparatively slow rate, since
 * it allows for a useful "cancelling" of the trajectory to be done by simply not
 * sending any more chunks without needing explicit support for this in hubo motion.
 *
 * These values may need to be experimentally determined, and the SPIN_RATE parameter
 * may need to dynamically change based on the timings on the incoming trajectory.
*/
#define MAX_TRAJ_LENGTH 10 //Number of points in each trajectory chunk
double SPIN_RATE = 5.0; //Rate in hertz at which to send trajectory chunks

// Index->Joint name mapping (the index in this array matches the numerical index of the joint name in Hubo-Ach as defined in hubo.h
char* joint_names[] = {"HPY", "not in urdf1", "HNR", "HNP", "LSP", "LSR", "LSY", "LEP", "LWY", "not in urdf2", "LWP", "RSP", "RSR", "RSY", "REP", "RWY", "not in urdf3", "RWP", "not in ach1", "LHY", "LHR", "LHP", "LKP", "LAP", "LAR_dummy", "not in ach2", "RHY", "RHR", "RHP", "RKP", "RAP", "RAR_dummy", "not in urdf4", "not in urdf5", "not in urdf6", "not in urdf7", "not in urdf8", "not in urdf9", "not in urdf10", "not in urdf11", "not in urdf12", "not in urdf13", "unknown1", "unknown2", "unknown3", "unknown4", "unknown5", "unknown6", "unknown7", "unknown8"};

// Trajectory storage
std::vector<std::string> g_joint_names;
std::vector< std::vector<trajectory_msgs::JointTrajectoryPoint> > g_trajectory_chunks;
int g_tid = 0;

// Publisher and subscriber
ros::Publisher g_state_pub;
ros::Subscriber g_traj_sub;

// Thread for listening to the hubo state and republishing it
boost::thread* pub_thread;

/*
 * Signal handler to safely shutdown the node
*/
void shutdown(int signum)
{
    ROS_INFO("Attempting to shutdown node...");
    if (signum == SIGINT)
    {
        ROS_INFO("Starting safe shutdown...");
        g_trajectory_chunks.clear();
        ros::shutdown();
        pub_thread->join();
        ROS_INFO("All threads done, shutting down!");
    }
}

/*
 * Takes the current "trajectory chunk" and sends it to hubo-motion over
 * hubo ach channels. Because the trajectory chunk has been reprocessed,
 * the joint indices in the trajectory chunk match the indicies used by
 * inside hubo ach, and no further remapping is needed.
*/
void sendTrajectory(std::vector<trajectory_msgs::JointTrajectoryPoint> processed_traj)
{
    if (processed_traj.size() == 0)
    {
        return;
    }
    else
    {
        // Make a new hubo_traj_t, and fill it in with the command data we've processed
        hubo_traj_t ach_traj;
        memset(&ach_traj, 0, sizeof(ach_traj));
        unsigned int points = processed_traj.size();
        for (unsigned int p = 0; p < points; p++)
        {
            for (int i = 0; i < HUBO_JOINT_COUNT; i++)
            {
                ach_traj.joint[i].position[p] = processed_traj[p].positions[i];
                ach_traj.joint[i].velocity[p] = processed_traj[p].velocities[i];
                ach_traj.joint[i].acceleration[p] = processed_traj[p].accelerations[i];
            }
            ach_traj.time[p] = processed_traj[p].time_from_start.toSec();
        }
        ach_traj.endTime = processed_traj.back().time_from_start.toSec();
        ach_traj.trajID = g_tid;
        ach_put(&chan_traj_cmd, &ach_traj, sizeof(ach_traj));
        g_tid++;
    }
}

/*
 * Given a string name of a joint, it looks it up in the list of joint names
 * to determine the joint index used in hubo ach for that joint. If the name
 * can't be found, it returns -1.
*/
int IndexLookup(std::string joint_name)
{
    // Find the Hubo joint name [used in hubo.h, with some additions for joints that don't map directly]
    // and the relevant index to map from the ROS trajectories message to the hubo-motion struct
    bool match = false;
    int best_match = -1;
    //See if we've got a matching joint name, and if so, return the
    //relevant index so we can map it into the hubo struct
    for (int i = 0; i < HUBO_JOINT_COUNT; i++)
    {
        if (strcmp(joint_name.c_str(), joint_names[i]) == 0)
        {
            match = true;
            best_match = i;
            break;
        }
    }
    if (match)
    {
        return best_match;
    }
    else
    {
        return -1;
    }
}

/**************************************************************************************************************************
 * POTENTIALLY NEEDS MAJOR CHANGES TO HUBO-MOTION-RT
 *
 * This function re-orders a given JointTrajectoryPoint in the commanded trajectory to do two things:
 *
 * 1) Provide a command to every joint, including those that may not match the URDF
 *    This is done by ordering the data with the same indexing as that used directly
 *    by the Hubo. This effectively handles the conversion between trajectories in
 *    ROS that use joint names to identify active joints, and trajectories used with
 *    Hubo-Ach that use indexing to match values with joints.
 *
 * 2) Set all "unused" joints, i.e. those not being actively commanded, to their last
 *    setpoint as reported by the trajectory controller & hubo-ach. Once again, this
 *    provides support for trajectories with limited active joints.
 *
 * In order for this to be possible, ideally we would have 6 pieces of information per joint:
 *
 * 1) Current setpoint position
 * 2) Current setpoint velocity
 * 3) Current setpoint acceleration
 * 4) Current actual position
 * 5) Current actual velocity
 * 6) Current actual acceleration
 *
 * Of those, 4 and 5 are provided from the hubo-state channel, and 1 is provided from the
 * hubo-ref and/or hubo-ref-filter channels.
 *
 * There are two courses of action here - either, (a) fields 2,3, and 6 can be added to hubo-ach's
 * reporting of hubo state in hubo motion, or (b) the interface can be changed to use hubo-ach and
 * hubo-motion together with several assumptions to not require the missing information.
 *
 * From our perspective, (a) is a better option, as it maintains functional equivalence with other
 * robots such as the PR2 and doesn't require any major assumptions.
 *
 * However, (b) may be easier to implement, BUT, it requires two major assumptions:
 *
 * 1) All uncommanded joints (i.e. those not in the current trajectory)
 *    have zero desired velocity and zero desirec acceleration.
 *    [This lets us avoid the need for data on vel. and accel. setpoints]
 *
 * 2) All trajectories must end at zero velocity
 *    [This means we can specifically assume that any velocity at the end of a
 *     trajectory is error velocity]
 *
 * In this version of the file, we demonstrate what we would do in case (b) where no changes to hubo-motion
 * necessary and we change the interface slightly as a result. A consequnce of this is than not all data
 * reported from this node is accurate, as it doesn't all exist in the first place!
 *************************************************************************************************************************/
trajectory_msgs::JointTrajectoryPoint processPoint(trajectory_msgs::JointTrajectoryPoint raw, struct hubo_ref * cur_commands)
{
    trajectory_msgs::JointTrajectoryPoint processed;
    processed.positions.resize(HUBO_JOINT_COUNT);
    processed.velocities.resize(HUBO_JOINT_COUNT);
    processed.accelerations.resize(HUBO_JOINT_COUNT);
    if (g_joint_names.size() != raw.positions.size())
    {
        ROS_ERROR("Stored joint names and received joint commands do not match");
        for (int i = 0; i < HUBO_JOINT_COUNT; i++)
        {
            processed.positions[i] = cur_commands->ref[i];
            processed.velocities[i] = 0.0;
            processed.accelerations[i] = 0.0;
        }
        return processed;
    }
    else
    {
        // Remap the provided joint trajectory point to the Hubo's joint indices
        // First, fill in everything with data from the current Hubo state
        for (int i = 0; i < HUBO_JOINT_COUNT; i++)
        {
            processed.positions[i] = cur_commands->ref[i];
            processed.velocities[i] = 0.0;
            processed.accelerations[i] = 0.0;
        }
        // Now, overwrite with the commands in the current trajectory
        for (unsigned int i = 0; i < raw.positions.size(); i++)
        {
            int index = IndexLookup(g_joint_names[i]);
            if (index != -1)
            {
                processed.positions[index] = raw.positions[i];
                processed.velocities[index] = raw.velocities[i];
                processed.accelerations[index] = raw.accelerations[i];
            }
        }
        return processed;
    }
}


/*
 * Reprocesses the current "trajectory chunk" one point at a time to make it safe
 * to send to hubo motion that assumes all joints will be commanded/populated.
 *
 * Once a chunk has been reprocessed, it is removed from the stored list of chunks.
*/
std::vector<trajectory_msgs::JointTrajectoryPoint> processTrajectory(struct hubo_ref* cur_commands)
{
    std::vector<trajectory_msgs::JointTrajectoryPoint> processed;
    if (g_trajectory_chunks.size() == 0)
    {
        return processed;
    }
    else
    {
        // Grab the next chunk to execute
        std::vector<trajectory_msgs::JointTrajectoryPoint> cur_set = g_trajectory_chunks[0];
        for (unsigned int i = 0; i < cur_set.size(); i++)
        {
            trajectory_msgs::JointTrajectoryPoint processed_point = processPoint(cur_set[i], cur_commands);
            processed.push_back(processed_point);
        }
        // Remove the current chunk from storage now that we've used it
        g_trajectory_chunks.erase(g_trajectory_chunks.begin(), g_trajectory_chunks.begin() + 1);
        return processed;
    }
}

/*
 * Callback to chunk apart the trajectory into chunks that will fit over hubo-ach
 *
 * For a trajectory of N points and a chunk size of M points, this will produce
 * ceil(N/M) chunks.
 *
 * An important part of this chunking is that each chunk is retimed from the end time
 * of the previous chunk. Otherwise, as ROS trajectories are timed via absolute time
 * from start, the trajectory chunks would "drift into the future".
 */
void trajectoryCB(const trajectory_msgs::JointTrajectory& traj)
{
    // Callback to chunk and save incoming trajectories
    // Before we do anything, check if the trajectory is empty - this is a special "stop" value that flushes the current stored trajectory
    if (traj.points.size() == 0)
    {
        g_trajectory_chunks.clear();
        ROS_INFO("Flushing current trajectory");
        return;
    }
    // First, chunk the trajectory into parts that can be sent over ACH channels to hubo-motion-rt
    std::vector< std::vector<trajectory_msgs::JointTrajectoryPoint> > new_chunks;
    unsigned int i = 0;
    ros::Duration base_time(0.0);
    while (i < traj.points.size())
    {
        std::vector<trajectory_msgs::JointTrajectoryPoint> new_chunk;
        unsigned int index = 0;
        while (index < traj.points.size() && index < MAX_TRAJ_LENGTH)
        {
            // Make sure the JointTrajectoryPoint gets retimed to match its new trajectory chunk
            trajectory_msgs::JointTrajectoryPoint cur_point = traj.points[i];
            // Retime based on the end time of the previous trajectory chunk
            cur_point.time_from_start = cur_point.time_from_start - base_time;
            // Make sure position, velocity, and acceleration are all the same length
            if (cur_point.accelerations.size() != cur_point.positions.size())
            {
                cur_point.accelerations.resize(cur_point.positions.size());
            }
            if (cur_point.velocities.size() != cur_point.positions.size())
            {
                cur_point.velocities.resize(cur_point.positions.size());
            }
            // Store it
            new_chunk.push_back(cur_point);
            index++;
            i++;
        }
        // Save the ending time to use for the next chunk
        base_time = new_chunk.back().time_from_start;
        // Store it
        new_chunks.push_back(new_chunk);
    }
    // Second, store those chunks - first, we flush the stored trajectory
    g_trajectory_chunks.clear();
    g_joint_names.clear();
    g_trajectory_chunks = new_chunks;
    g_joint_names = traj.joint_names;
}

/*
 * This runs in a second thread in the background.
 *
 * This thread grabs the latest states from the hubo's joints and joint setpoints
 * and repacks them into the ROS messages sent to the trajectory action server.
 *
 * NOTE: This thread uses ACH_0_WAIT as a delay operation to synchronize against
 * hubo ach, and thus it doesn't need a explicity sleep() or wait operation.
 *
 * NOTE: This implements option (b) from above!
 */
void publishLoop()
{
    size_t fs;
    struct hubo_state H_state;
    memset(&H_state, 0, sizeof(H_state));
    struct hubo_ref H_ref_filter;
    memset(&H_ref_filter, 0, sizeof(H_ref_filter));
    // Loop until node shutdown
    while (ros::ok())
    {
        // Get the latest hubo state from HUBO-ACH
        ach_get(&chan_hubo_state, &H_state, sizeof(H_state), &fs, NULL, ACH_O_WAIT);
        if (fs != sizeof(H_state))
        {
            ROS_ERROR("Hubo state size error! [publishing loop]");
            continue;
        }
        // Get latest reference from HUBO-ACH (this is used to populate the desired values)
        ach_get(&chan_hubo_ref_filter, &H_ref_filter, sizeof(H_ref_filter), &fs, NULL, ACH_O_LAST);
        if (fs != sizeof(H_ref_filter))
        {
            ROS_ERROR("Hubo ref size error! [publishing loop]");
            continue;
        }
        // Publish the latest hubo state back out
        hubo_robot_msgs::JointTrajectoryState cur_state;
        cur_state.header.stamp = ros::Time::now();
        // Set the names
        cur_state.joint_names = g_joint_names;
        unsigned int num_joints = cur_state.joint_names.size();
        // Make the empty states
        trajectory_msgs::JointTrajectoryPoint cur_setpoint;
        trajectory_msgs::JointTrajectoryPoint cur_actual;
        trajectory_msgs::JointTrajectoryPoint cur_error;
        // Resize the states
        cur_setpoint.positions.resize(num_joints);
        cur_setpoint.velocities.resize(num_joints);
        cur_setpoint.accelerations.resize(num_joints);
        cur_actual.positions.resize(num_joints);
        cur_actual.velocities.resize(num_joints);
        cur_actual.accelerations.resize(num_joints);
        cur_error.positions.resize(num_joints);
        cur_error.velocities.resize(num_joints);
        cur_error.accelerations.resize(num_joints);
        // Fill in the setpoint and actual & calc the error in the process
        for (unsigned int i = 0; i < num_joints; i++)
        {
            // Fill in the setpoint and actual data
            // Values that we don't have data for are set to NAN
            int hubo_index = IndexLookup(cur_state.joint_names[i]);
            cur_setpoint.positions[i] = H_ref_filter.ref[hubo_index];
            cur_setpoint.velocities[i] = NAN;
            cur_setpoint.accelerations[i] = NAN;
            cur_actual.positions[i] = H_state.joint[hubo_index].pos;
            cur_actual.velocities[i] = H_state.joint[hubo_index].vel;
            cur_actual.accelerations[i] = NAN;
            // Calc the error
            cur_error.positions[i] = cur_setpoint.positions[i] - cur_actual.positions[i];
            cur_error.velocities[i] = NAN;
            cur_error.accelerations[i] = NAN;
        }
        // Pack them together
        cur_state.desired = cur_setpoint;
        cur_state.actual = cur_actual;
        cur_state.error = cur_error;
        // Publish
        g_state_pub.publish(cur_state);
    }
}

int main(int argc, char** argv)
{
    printf("Starting JointTrajectoryAction controller interface node...");
    ros::init(argc, argv, "hubo_joint_trajectory_controller_interface_node", ros::init_options::NoSigintHandler);
    ros::NodeHandle nh;
    ros::NodeHandle nhp("~");
    ROS_INFO("Attempting to start JointTrajectoryAction controller interface...");
    // Get all the active joint names
    XmlRpc::XmlRpcValue joint_names;
    if (!nhp.getParam("joints", joint_names))
    {
        ROS_FATAL("No joints given. (namespace: %s)", nhp.getNamespace().c_str());
        exit(1);
    }
    if (joint_names.getType() != XmlRpc::XmlRpcValue::TypeArray)
    {
        ROS_FATAL("Malformed joint specification.  (namespace: %s)", nhp.getNamespace().c_str());
        exit(1);
    }
    for (int i = 0; i < joint_names.size(); ++i)
    {
        XmlRpc::XmlRpcValue &name_value = joint_names[i];
        if (name_value.getType() != XmlRpc::XmlRpcValue::TypeString)
        {
            ROS_FATAL("Array of joint names should contain all strings.  (namespace: %s)", nhp.getNamespace().c_str());
            exit(1);
        }
        g_joint_names.push_back((std::string)name_value);
    }
    // Register a signal handler to safely shutdown the node
    signal(SIGINT, shutdown);
    // Set up the ACH channels to and from hubo-motion-rt
    char command[100];
    sprintf(command, "ach -1 -C %s -m 10 -n 1000000 -o 666", HUBO_TRAJ_CHAN);
    system(command);
    sprintf(command, "ach -1 -C %s -o 666", HUBO_TRAJ_STATE_CHAN);
    system(command);
    //initialize HUBO-ACH feedback channel
    ach_status_t r = ach_open(&chan_hubo_state, HUBO_CHAN_STATE_NAME , NULL);
    if (r != ACH_OK)
    {
        ROS_FATAL("Could not open ACH channel: HUBO_STATE_CHAN !");
        exit(1);
    }
    //initialize HUBO-ACH reference channel
    r = ach_open(&chan_hubo_ref_filter, HUBO_CHAN_REF_NAME , NULL);
    if (r != ACH_OK)
    {
        ROS_FATAL("Could not open ACH channel: HUBO_REF_CHAN !");
        exit(1);
    }
    // Make sure the ACH channels to hubo motion are opened properly
    r = ach_open(&chan_traj_cmd, HUBO_TRAJ_CHAN, NULL);
    if (r != ACH_OK)
    {
        ROS_FATAL("Could not open ACH channel: HUBO_TRAJ_CHAN !");
        exit(1);
    }
    r = ach_open(&chan_traj_state, HUBO_TRAJ_STATE_CHAN, NULL);
    if (r != ACH_OK)
    {
        ROS_FATAL("Could not open ACH channel: HUBO_TRAJ_STATE_CHAN !");
        exit(1);
    }
    ROS_INFO("Opened ACH channels to hubo-motion-rt");
    // Set up state publisher
    std::string pub_path = nh.getNamespace() + "/state";
    g_state_pub = nh.advertise<hubo_robot_msgs::JointTrajectoryState>(pub_path, 1);
    // Spin up the thread for getting data from hubo and publishing it
    pub_thread = new boost::thread(&publishLoop);
    // Set up the trajectory subscriber
    std::string sub_path = nh.getNamespace() + "/command";
    g_traj_sub = nh.subscribe(sub_path, 1, trajectoryCB);
    ROS_INFO("Loaded trajectory interface to hubo-motion-rt");
    // Spin until killed
    
    size_t fs;
    struct hubo_ref H_ref_filter;
    memset(&H_ref_filter, 0, sizeof(H_ref_filter));
    while (ros::ok())
    {
        //Get latest reference from HUBO-ACH (this is used to populate the uncommanded joints!)
        ach_get(&chan_hubo_ref_filter, &H_ref_filter, sizeof(H_ref_filter), &fs, NULL, ACH_O_LAST);
        if (fs != sizeof(H_ref_filter))
        {
            ROS_ERROR("Hubo ref size error! [sending loop]");
        }
        else
        {
            // Reprocess the current trajectory chunk (this does nothing if we have nothing to send)
            std::vector<trajectory_msgs::JointTrajectoryPoint> cleaned_trajectory = processTrajectory(&H_ref_filter);
            if (cleaned_trajectory.size() > 0)
            {
                SPIN_RATE = 1.0 / (cleaned_trajectory.back().time_from_start.toSecs());
            }
            // Send the latest trajectory chunk (this does nothing if we have nothing to send)
            sendTrajectory(cleaned_trajectory);
        }
        ros::spinOnce();
        // Wait long enough before sending the next one
        ros::Rate looprate(SPIN_RATE);
        looprate.sleep();
    }
    // Make the compiler happy
    return 0;
}
