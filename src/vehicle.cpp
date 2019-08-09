#include "udp_bridge/udp_bridge.h"
#include "std_msgs/String.h"
#include "std_msgs/Bool.h"
#include "std_msgs/Float32.h"
#include "geographic_msgs/GeoPointStamped.h"
#include "marine_msgs/Heartbeat.h"
#include "marine_msgs/Contact.h"
#include "diagnostic_msgs/DiagnosticArray.h"
#include "marine_msgs/NavEulerStamped.h"
#include "marine_msgs/RadarSectorStamped.h"
#include "sensor_msgs/NavSatFix.h"
#include "sensor_msgs/PointCloud.h"
#include "marine_msgs/Helm.h"
#include "geometry_msgs/TwistStamped.h"
#include "geographic_msgs/GeoPath.h"
#include "geographic_visualization_msgs/GeoVizItem.h"
#include <regex>
#include "boost/date_time/posix_time/posix_time.hpp"
#include "darknet_ros_msgs/BoundingBoxes.h"

std::map<udp_bridge::Channel,std::string> udp_bridge::UDPROSNode::topic_map;

int main(int argc, char **argv)
{
    ros::init(argc, argv, "udp_bridge_vehicle");
    
    std::string host = "localhost";
    if (argc > 1)
        host = argv[1];
  
    int send_port = 4200;
    int receive_port = 4201;
    
    udp_bridge::UDPROSNode n(host,send_port,receive_port);

    n.addSender<geographic_msgs::GeoPointStamped, udp_bridge::position>("/position");
    n.addSender<std_msgs::String, udp_bridge::appcast>("/moos/appcast");
    n.addSender<geographic_msgs::GeoPoint,udp_bridge::origin>("/origin");
    n.addSender<marine_msgs::Heartbeat,udp_bridge::heartbeat>("/heartbeat");
    n.addSender<std_msgs::String, udp_bridge::flir_engine>("/flir_engine");
    n.addSender<marine_msgs::NavEulerStamped, udp_bridge::heading>("/heading");
    n.addSender<marine_msgs::Contact, udp_bridge::contact>("/contact");
    n.addSender<diagnostic_msgs::DiagnosticArray, udp_bridge::diagnostics>("/diagnostics");
    n.addSender<marine_msgs::NavEulerStamped, udp_bridge::posmv_orientation>("/posmv/orientation");
    n.addSender<sensor_msgs::NavSatFix, udp_bridge::posmv_position>("/posmv/position");
    n.addSender<geometry_msgs::TwistStamped, udp_bridge::sog>("/sog");
    n.addSender<geographic_msgs::GeoPath, udp_bridge::coverage>("/coverage");
    n.addSender<sensor_msgs::PointCloud, udp_bridge::mbes_ping>("/mbes_ping");
    n.addSender<std_msgs::String, udp_bridge::response>("/project11/response");
    n.addSender<marine_msgs::RadarSectorStamped, udp_bridge::radar>("/radar");
    n.addSender<geographic_visualization_msgs::GeoVizItem, udp_bridge::display>("/project11/display");
    n.addSender<darknet_ros_msgs::BoundingBoxes, udp_bridge::darknet_bounding_boxes>("/darknet_ros/bounding_boxes");
    n.addSender<std_msgs::Float32, udp_bridge::mbr_margin_avg>("/mbr/9372/2510/margin_avg");
    n.addSender<std_msgs::Float32, udp_bridge::mbr_margin_min>("/mbr/9372/2510/margin_min");
    
    n.addReceiver<std_msgs::String,udp_bridge::helm_mode>("/helm_mode");
    n.addReceiver<std_msgs::String,udp_bridge::wpt_updates>("/moos/wpt_updates");
    n.addReceiver<std_msgs::String,udp_bridge::loiter_updates>("/moos/loiter_updates");
    n.addReceiver<std_msgs::String,udp_bridge::mission_plan>("/mission_plan");
    n.addReceiver<std_msgs::String,udp_bridge::command>("/project11/command");
    n.addReceiver<marine_msgs::Helm,udp_bridge::helm>("/helm");

    n.spin();
    
    return 0;
}

