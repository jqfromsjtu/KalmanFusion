#ifndef DATA_UTILS_H
#define DATA_UTILS_H
///////////////
/// HEADERS ///
///////////////
#include <string>
#include <vector>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp/logger.hpp>

#include <opencv2/core/core.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <cv_bridge/cv_bridge.h>

#include <pcl/io/pcd_io.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/point_types.h>
#include <pcl/PCLPointCloud2.h>
#include <pcl/conversions.h>

#include <sensor_msgs/msg/point_cloud2.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <visualization_msgs/msg/marker.hpp>

#include "darknet_ros_msgs/msg/bounding_box.hpp"
#include "darknet_ros_msgs/msg/bounding_boxes.hpp"
#include "darknet_ros_msgs/msg/bounding_box3d.hpp"
#include "darknet_ros_msgs/msg/bounding_boxes3d.hpp"

#include <message_filters/subscriber.h>
#include <message_filters/synchronizer.h>
#include <message_filters/sync_policies/approximate_time.h>
#include <message_filters/sync_policies/exact_time.h>
using namespace std::chrono_literals;
//static int marker_id = 0;
/////////////
/// TYPES ///
/////////////
typedef std::vector<darknet_ros_msgs::msg::BoundingBox> Boxes2d;
typedef std::vector<darknet_ros_msgs::msg::BoundingBox3d> Boxes3d;
typedef darknet_ros_msgs::msg::BoundingBox Box2d;
typedef darknet_ros_msgs::msg::BoundingBox3d Box3d;
typedef std::string string;
typedef message_filters::sync_policies::ApproximateTime
        <sensor_msgs::msg::PointCloud2,                 
         sensor_msgs::msg::Image,
         //sensor_msgs::msg::Imu,
         //sensor_msgs::msg::NavSatFix,
         darknet_ros_msgs::msg::BoundingBoxes,
         darknet_ros_msgs::msg::BoundingBoxes> my_sync_policy;
typedef message_filters::Synchronizer<my_sync_policy> Sync;
/////////////////
/// FUNCTIONS ///
/////////////////
void draw_box(const sensor_msgs::msg::Image::SharedPtr& img_in, 
              sensor_msgs::msg::Image::SharedPtr& img_out,
              const darknet_ros_msgs::msg::BoundingBoxes::SharedPtr& BBoxes_msg,
              const int thickness);
void rotateZ(geometry_msgs::msg::Point &p, float pos_x, float pos_y, float heading);
void publish_point_cloud(rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr& pcl_pub, 
                         pcl::PointCloud<pcl::PointXYZI>::Ptr cloud, std_msgs::msg::Header header);
void publish_3d_box(rclcpp::Publisher<visualization_msgs::msg::Marker>::SharedPtr& box3d_pub, 
                    const Box3d box3d, const std_msgs::msg::Header header, const int track_id);
/*****************************************************
*??????????????????????????????????????????
*?????????
*cv_ptr: ??????OpenCV?????????????????????
*box?????????????????????
*track_id????????????????????????id??????????????????????????????
*****************************************************/
void draw_box(cv_bridge::CvImageConstPtr& cv_ptr, 
              const Box2d box, int track_id, const int thickness = 4) {
    int r[8] = {255,255,255,0,0,  0,  0  ,255};
    int g[8] = {0,  255,255,0,255,255,0  ,0};
    int b[8] = {0,  0,  255,0,0,  255,255,255};
    cv::rectangle(cv_ptr->image, cv::Point(box.xmin,box.ymin), cv::Point(box.xmax,box.ymax), cv::Scalar(b[track_id%8],g[track_id%8],r[track_id%8]), thickness);
}
/*****************************************************
*?????????PCL?????????????????????ros_msg?????????????????????
*?????????
*pcl_pub: ???????????????
*cloud: ??????pcl?????????????????????
*header: ????????????????????????header????????????????????????header
*****************************************************/
void publish_point_cloud(rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr& pcl_pub, 
                         pcl::PointCloud<pcl::PointXYZI>::Ptr cloud, std_msgs::msg::Header header) {
    sensor_msgs::msg::PointCloud2 cloud_msg;
    pcl::toROSMsg(*cloud, cloud_msg);
    cloud_msg.header = header;
    pcl_pub->publish(cloud_msg);
}
/*****************************************************
*????????????z??????????????????
*?????????
*p: ??????????????????
*pos_x: ????????????x??????
*pox_y: ????????????y??????
*heading??????z???????????????
*****************************************************/
void rotateZ(geometry_msgs::msg::Point &p, float pos_x, float pos_y, float heading) {
    float x = p.x;
    float y = p.y;
    p.x = (x-pos_x)*cos(heading) - (y-pos_y)*sin(heading) + pos_x;
    p.y = (x-pos_x)*sin(heading) + (y-pos_y)*cos(heading) + pos_y;
}
/*****************************************************
*????????????????????????????????????
*?????????
*box3d_pub: ??????????????????????????????publsiher
*box3d: ??????????????????
*headr: maker??????????????????header
*track_id: ??????maker???id?????????marker????????????????????????
*miss: ???????????????????????????????????????
*****************************************************/
void publish_3d_box(rclcpp::Publisher<visualization_msgs::msg::Marker>::SharedPtr& box3d_pub, const Box3d box3d, const std_msgs::msg::Header header, const int track_id, bool miss) {
    visualization_msgs::msg::Marker bbox_marker;
    bbox_marker.id = track_id;
    bbox_marker.header = header;
    bbox_marker.ns = "";
    bbox_marker.frame_locked = true;
    bbox_marker.type = visualization_msgs::msg::Marker::LINE_LIST;
    bbox_marker.action = visualization_msgs::msg::Marker::ADD;

    if (!miss) {
        int r[8] = {255,255,255,0,0,  0,  0  ,255};
        int g[8] = {0,  255,255,0,255,255,0  ,0};
        int b[8] = {0,  0,  255,0,0,  255,255,255};
        if ((box3d.length < 6) && (box3d.length > 0) 
            && (box3d.width < 6) && (box3d.width > 0) 
            && (box3d.height < 3)) {
            bbox_marker.pose.orientation.w = 1.0;
            bbox_marker.color.r = r[track_id%8]/255;
            bbox_marker.color.g = g[track_id%8]/255;
            bbox_marker.color.b = b[track_id%8]/255;
            bbox_marker.color.a = 1.0f;
            bbox_marker.scale.x = 0.2f;
            int line1_x_sig[12] = {-1,-1,1,1,-1,-1,1,1,-1,-1,1,1};
            int line1_y_sig[12] = {-1,1,1,-1,-1,1,1,-1,-1,1,1,-1};
            int line2_x_sig[12] = {-1,1,1,-1,-1,1,1,-1,-1,-1,1,1};
            int line2_y_sig[12] = {1,1,-1,-1,1,1,-1,-1,-1,1,1,-1};
            int line1_z_sig[12] = {-1,-1,-1,-1,1,1,1,1,-1,-1,-1,-1};
            int line2_z_sig[12] = {-1,-1,-1,-1,1,1,1,1,1,1,1,1};
            geometry_msgs::msg::Point p;
            for (int i = 0; i < 12; i++) {
                p.x = box3d.pos.x + box3d.length/2*line1_x_sig[i];
                p.y = box3d.pos.y + box3d.width/2*line1_y_sig[i];
                p.z = box3d.pos.z + box3d.height/2*line1_z_sig[i];
                rotateZ(p, box3d.pos.x, box3d.pos.y, box3d.heading);
                bbox_marker.points.push_back(p);
                p.x = box3d.pos.x + box3d.length/2*line2_x_sig[i];
                p.y = box3d.pos.y + box3d.width/2*line2_y_sig[i];
                p.z = box3d.pos.z + box3d.height/2*line2_z_sig[i];
                rotateZ(p, box3d.pos.x, box3d.pos.y, box3d.heading);
                bbox_marker.points.push_back(p);
            }
            bbox_marker.lifetime = rclcpp::Duration(5000ms);
            box3d_pub->publish(bbox_marker);
        } else {
            bbox_marker.lifetime = rclcpp::Duration(0,0);
            box3d_pub->publish(bbox_marker);
        }
    }
}

#endif



