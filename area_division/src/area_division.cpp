#include "area_division.h"

/**
 * @brief Divide the area of the grid map equally among multiple CPSs.
 */
void divide_area ()
{
    // convert swarm pose to grid
    map<string, vector<int>> swarm_grid;
    for (auto cps : swarm_pose) {
        vector<int> pos(2);
        pos[0] = int(round((cps.second.pose.position.x - gridmap.info.origin.position.x) / gridmap.info.resolution));
        pos[1] = int(round((cps.second.pose.position.y - gridmap.info.origin.position.y) / gridmap.info.resolution));
        swarm_grid.emplace(cps.first, pos);
    }

    // add this robot to swarm grid
    vector<int> pos(2);
    pos[0] = int(round((pose.position.x - gridmap.info.origin.position.x) / gridmap.info.resolution));
    pos[1] = int(round((pose.position.y - gridmap.info.origin.position.y) / gridmap.info.resolution));
    swarm_grid.emplace(uuid, pos);

    // divide area
    ROS_DEBUG("Dividing area...");
    vector<signed char, allocator<signed char>> map = gridmap.data;
    division.initialize_map((int)gridmap.info.width, (int)gridmap.info.height, map);
    division.initialize_cps(swarm_grid);
    division.divide();

    // visualize area
    if (visualize)
        area_publisher.publish(division.get_grid(gridmap, uuid));

    reconfigure = false;
}

/**
 * @brief Callback function to get the area assignment of this CPS.
 * @param req Empty request.
 * @param res The grid map assigned to this CPS.
 * @return Whether the request succeeded.
 */
bool get_area (nav_msgs::GetMap::Request &req, nav_msgs::GetMap::Response &res)
{
    // compute new area division if swarm configuration changed
    if (reconfigure) {
        divide_area();
    }

    // return assigned area
    res.map = division.get_grid(gridmap, uuid);

    return true;
}

/**
 * @brief Callback function to receive the UUID from the communication library.
 * @param msg UUID of this node.
 */
void uuid_callback (const swarmros::String::ConstPtr& msg)
{
    uuid = msg->value;
}

/**
 * @brief Callback function for position updates.
 * @param msg Position received from the CPS.
 */
void pose_callback (const geometry_msgs::PoseStamped::ConstPtr& msg)
{
    // store new position and orientation in class variables
    pose = msg->pose;

    // valid pose received
    if (msg->header.stamp.isValid())
        pose_valid = true;
}

/**
 * @brief Callback function to receive the positions of the other CPSs.
 * @param msg UUIDs and positions of the other CPSs.
 */
void swarm_callback (const cpswarm_msgs::ArrayOfPositions::ConstPtr& msg)
{
    // update cps positions
    for (auto cps : msg->positions) {
        // index of cps in map
        auto idx = swarm_pose.find(cps.swarmio.node);

        // add new cps
        if (idx == swarm_pose.end()) {
            geometry_msgs::PoseStamped pose;
            pose.header.stamp = Time::now();
            pose.pose = cps.pose;
            swarm_pose.emplace(cps.swarmio.node, pose);

            // divide again
            reconfigure = true;
            ROS_DEBUG("New CPS %s", cps.swarmio.node.c_str());
        }

        // update existing cps
        else {
            idx->second.header.stamp = Time::now();
            idx->second.pose = cps.pose;
        }
    }

    // remove old cps
    for (auto cps=swarm_pose.cbegin(); cps!=swarm_pose.cend();) {
        if (cps->second.header.stamp + Duration(swarm_timeout) < Time::now()) {
            ROS_DEBUG("Remove CPS %s", cps->first.c_str());
            swarm_pose.erase(cps++);

            // divide again
            reconfigure = true;
        }
        else {
            ++cps;
        }
    }

    swarm_valid = true;
}

/**
 * @brief Callback function to receive the grid map.
 * @param msg Merged grid map from all CPSs.
 */
void map_callback (const nav_msgs::OccupancyGrid::ConstPtr& msg)
{
    gridmap = *msg;
    map_valid = true;
}

/**
 * @brief A ROS node that divides the available area among a swarm of CPSs.
 * @param argc Number of command line arguments.
 * @param argv Array of command line arguments.
 * @return Success.
 */
int main (int argc, char **argv)
{
    // init ros node
    init(argc, argv, "area_division");
    NodeHandle nh;

    // init global variables
    reconfigure = true;

    // read parameters
    double loop_rate;
    nh.param(this_node::getName() + "/loop_rate", loop_rate, 1.5);
    int queue_size;
    nh.param(this_node::getName() + "/queue_size", queue_size, 1);
    nh.param(this_node::getName() + "/swarm_timeout", swarm_timeout, 5.0);
    nh.param(this_node::getName() + "/visualize", visualize, false);

    // initialize flags
    uuid = "";
    pose_valid = false;
    swarm_valid = false;
    map_valid = false;

    // publishers and subscribers
    Subscriber uuid_sub = nh.subscribe("bridge/uuid", queue_size, uuid_callback);
    Subscriber pose_subscriber = nh.subscribe("pos_provider/pose", queue_size, pose_callback);
    Subscriber swarm_subscriber = nh.subscribe("swarm_position", queue_size, swarm_callback);
    Subscriber map_subscriber = nh.subscribe("map", queue_size, map_callback); // TODO: use explored/merged map
    if (visualize)
        area_publisher = nh.advertise<nav_msgs::OccupancyGrid>("assigned_map", queue_size, true);

    // init loop rate
    Rate rate(loop_rate);

    // init uuid
    while (ok() && uuid == "") {
        ROS_DEBUG_ONCE("Waiting for UUID...");
        rate.sleep();
        spinOnce();
    }

    // init position
    while (ok() && (pose_valid == false || swarm_valid == false)) {
        ROS_DEBUG_ONCE("Waiting for valid position information...");
        rate.sleep();
        spinOnce();
    }

    // init map
    while (ok() && map_valid == false) {
        ROS_DEBUG_ONCE("Waiting for grid map...");
        rate.sleep();
        spinOnce();
    }

    // configure area division optimizer
    division.setup(1, 0.01, 1e-4, 30); // TODO: define parameters

    // provide area service
    ServiceServer area_service = nh.advertiseService("area/assigned", get_area);
    spin();

    return 0;
}