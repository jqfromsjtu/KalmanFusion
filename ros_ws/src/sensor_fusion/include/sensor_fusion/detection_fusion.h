#ifndef DETECTION_FUSION_H
#define DETECTION_FUSION_H
#include "LinkList.hpp"

#include <string>
#include <sstream>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <vector>
#include <algorithm>
#include <unordered_set>

#include <Eigen/Eigen>
#include <pcl/filters/extract_indices.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/kdtree/kdtree.h>
#include <pcl/segmentation/extract_clusters.h>
#include <pcl/common/impl/io.hpp>

#include <cv_bridge/cv_bridge.h>
#include <opencv2/core/core.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/highgui/highgui.hpp>

#include <rclcpp/rclcpp.hpp>
#include <geometry_msgs/msg/point.hpp>
#include <visualization_msgs/msg/marker.hpp>

#include "darknet_ros_msgs/msg/bounding_box.hpp"
#include "darknet_ros_msgs/msg/bounding_boxes.hpp"
#include "darknet_ros_msgs/msg/bounding_box3d.hpp"
#include "darknet_ros_msgs/msg/bounding_boxes3d.hpp"



// Calculation of 2d-box
#define IMG_LENGTH 1242
#define IMG_WIDTH 375



#define S_GROUP_THRESHOLD 10
#define S_GROUP_REFINED_THRESHOLD 5
// L-shape Fitting Proposal Params
#define ANGLE_RESO 0.06
#define POINT_NUM 2
#define MIN_SLOPE 0.0000001
typedef std::string string;
typedef Eigen::Matrix<double, 3, 4> Matrix34d;
typedef Eigen::Matrix<double, 4, 4> Matrix4d;
typedef Eigen::Matrix<float, 4, 4> Matrix4f;
typedef Eigen::Matrix<float, 2, 2> Matrix2f;
typedef Eigen::Matrix<float, 4, 1> Matrix41f;
typedef std::vector<darknet_ros_msgs::msg::BoundingBox> Boxes2d;
typedef std::vector<darknet_ros_msgs::msg::BoundingBox3d> Boxes3d;
typedef darknet_ros_msgs::msg::BoundingBox Box2d;
typedef darknet_ros_msgs::msg::BoundingBox3d Box3d;
struct detection_cam {
    int id = 0;
    int miss = 0;
    bool far = false;
    float distance_far = 0;
    Box2d box;
    Box3d box3d;
    //pcl::PointCloud<pcl::PointXYZI> PointCloud;
    pcl::PointCloud<pcl::PointXYZI> CarCloud;
    
};
struct PointXYZIRT {
    pcl::PointXYZI point;
    float theta;
    float radius;
};
typedef std::vector<PointXYZIRT> PointCloudXYZIRT;
class detection_fusion {
private:
    Boxes2d boxes2d;
    Boxes2d objs2d;
    LinkList<detection_cam>* ptrDetectFrame;
    LinkList<detection_cam>* ptrObjFrame;
    Matrix34d point_projection_matrix;
    std::vector<std::vector<bool>> occlusion_table;
    bool is_initialized;
    pcl::PointCloud<pcl::PointXYZI>::Ptr inCloud;
    
public:
    detection_fusion();
    ~detection_fusion();
    void Initialize(const Matrix34d point_projection_matrix_, 
                    LinkList<detection_cam> &DetectFrame, 
                    const darknet_ros_msgs::msg::BoundingBoxes::ConstPtr& BBoxes_msg,
                    const darknet_ros_msgs::msg::BoundingBoxes::ConstPtr& Objs_msg,
                    pcl::PointCloud<pcl::PointXYZI>::Ptr in_cloud);
    bool Is_initialized();
    void extract_feature();
    void obstacle_extract(const int num);
    void vehicle_extract(const int num);
    bool eu_cluster(pcl::PointCloud<pcl::PointXYZI>::Ptr in_cloud, 
                      pcl::PointCloud<pcl::PointXYZI>::Ptr cloud_cluster);
    
    void clip_frustum(const Box2d box2d, pcl::PointCloud<pcl::PointXYZI>::Ptr &outCloud);
    bool in_frustum(const double u, const double v, const Box2d box);
    bool in_frustum_overlap(const double u, const double v, const int num);
    Box2d overlap_box(const Box2d prev_box, const Box2d curr_box);
    double Lshape(pcl::PointCloud<pcl::PointXYZI>::Ptr &ptrCarCloud,
                  pcl::PointCloud<pcl::PointXYZI>::Ptr &ptrSgroup,
                  Eigen::Matrix<float, 4, 1> &u);
    void Lproposal(const PointCloudXYZIRT Sgroup_, pcl::PointCloud<pcl::PointXYZI>::Ptr &ptrSgroup);
    double Lfit(const pcl::PointCloud<pcl::PointXYZI>::Ptr in_cloud, Matrix41f &u);
    Matrix4f deltaM_compute(const pcl::PointXYZI point);
    void bounding_box_param(const Eigen::Matrix<float, 4, 1> u, pcl::PointCloud<pcl::PointXYZI>::Ptr& ptrSgroup, 
                            pcl::PointCloud<pcl::PointXYZI>::Ptr& carCloud, Box3d& box3d);
    void point_projection_into_line(float &x, float &y, const float k, const float b);
};
bool IoU_bool(const Box2d prev_box, const Box2d curr_box);
void add_to_group(const std::vector<std::vector<bool>> occlusion_table, const int start, std::unordered_set<int>& group_set);
#endif
