#include <ros/ros.h>
#include <ros/master.h>
#include "udp_bridge/ChannelStatisticsArray.h"

void statisticsCallback(udp_bridge::ChannelStatisticsArray const &stats)
{
    std::vector<std::string> headers {"source topic", "remote host", "     messages", "message data", " packet data", "  compressed", "  ratio", "send error"};
    std::vector<int> column_widths;
    for(auto h: headers)
        column_widths.push_back(h.size());
    
    for(auto c: stats.channels)
    {
        column_widths[0] = std::max(column_widths[0], int(c.source_topic.size()));
        column_widths[1] = std::max(column_widths[1], int(c.destination_host.size()));
    }

    std::cout << std::left;
    for(int i = 0; i < headers.size(); i++)
        std::cout << std::setw(column_widths[i]+1) << headers[i];
    std::cout << std::endl;

    std::vector<float> totals {0.0, 0.0, 0.0, 0.0};
    
    for(auto c: stats.channels)
    {
        std::cout << std::left;
        std::cout << std::setw(column_widths[0]+1) << c.source_topic;
        std::cout << std::setw(column_widths[1]+1) << c.destination_host;
        
        std::cout << std::fixed;
        std::cout << std::setprecision(1);
        std::cout << std::right;
        
        std::cout << std::setw(5) << c.messages_per_second << " msg/sec ";
        // bytes to kilobits, *8/100 -> /125
        std::cout << std::setw(7) << c.message_bytes_per_second/125.0 << " kbps ";
        std::cout << std::setw(7) << c.packet_bytes_per_second/125.0 << " kbps ";
        std::cout << std::setw(7) << c.compressed_bytes_per_second/125.0 << " kbps";
        
        if(c.message_bytes_per_second > 0)
            std::cout << std::setw(7) << 100*c.compressed_bytes_per_second/c.message_bytes_per_second << "%";
        else
            std::cout << std::setw(7) << 0 << "%";

        std::cout << std::setw(7) << 100*(1.0-c.send_success_rate) << "%";
        
        std::cout << std::endl;
        
        totals[0] += c.messages_per_second;
        totals[1] += c.message_bytes_per_second;
        totals[2] += c.packet_bytes_per_second;
        totals[3] += c.compressed_bytes_per_second;
    }
    std::cout << std::left;
    std::cout << std::setw(column_widths[0]+column_widths[1]+2) << "totals:";
    std::cout << std::right;
    std::cout << std::setw(5) << totals[0] << " msg/sec ";
    std::cout << std::setw(7) << totals[1]/125.0 << " kbps ";
    std::cout << std::setw(7) << totals[2]/125.0 << " kbps ";
    std::cout << std::setw(7) << totals[3]/125.0 << " kbps";
    if(totals[1]>0)
        std::cout << std::setw(7) << 100*totals[3]/totals[1] << "%";
    else
        std::cout << std::setw(7) << 0 << "%";
    std::cout << std::endl;
    
    std::cout << std::endl;
        
}

int main(int argc, char **argv)
{
    ros::init(argc, argv, "udp_bridge_ui");

    ros::NodeHandle nh;
    
    std::vector<ros::Subscriber> statsSubs;
    
    ros::master::V_TopicInfo topic_infos;
    ros::master::getTopics(topic_infos);
    
    for(auto ti:topic_infos)
        if(ti.datatype == "udp_bridge/ChannelStatisticsArray")
            statsSubs.push_back(nh.subscribe(ti.name, 10, &statisticsCallback));
    
    ros::spin();
    
    return 0;
}

