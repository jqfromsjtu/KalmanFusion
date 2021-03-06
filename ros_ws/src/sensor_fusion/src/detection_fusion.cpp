#include "sensor_fusion/detection_fusion.h"
/*****************************************************
*功能：初始化标志置零
*****************************************************/
detection_fusion::detection_fusion() : ptrObjFrame(new LinkList<detection_obj>(20)) {is_initialized = false;}
/*****************************************************
*功能：释放内存
*****************************************************/
detection_fusion::~detection_fusion(){}
/*****************************************************
*功能：返回初始化标志位
*****************************************************/
bool detection_fusion::Is_initialized() {return is_initialized;}
/*****************************************************
*功能：传入初始化数据
*输入：
*point_projection_matrix_: 激光雷达到相机的外部参数
*DetectFrame: 用于储存一帧检测结果，用于后续追踪匹配
*BBoxes_msg: 图像二维检测单帧检测框结果
*in_cloud_: 对应帧原始点云
*****************************************************/
void detection_fusion::Initialize(LinkList<detection_cam> &DetectFrame, 
                                  const darknet_ros_msgs::msg::BoundingBoxes::ConstPtr& BBoxes_msg,
                                  const darknet_ros_msgs::msg::BoundingBoxes::ConstPtr& Objs_msg,
                                  const pcl::PointCloud<pcl::PointXYZI>::Ptr in_cloud_,
                                  const Matrix34d P, const Matrix3d R, const Matrix31d T) {
    // Initialize calibration parameters
    Matrix4d spatial_trans = Matrix4d::Zero();
    for(size_t a = 0; a < 3; a++)
        for(size_t b = 0; b < 3; b++)
            spatial_trans(a,b) = R(a,b);
    spatial_trans(0,3) = T(0,0);
    spatial_trans(1,3) = T(1,0);
    spatial_trans(2,3) = T(2,0);
    spatial_trans(3,3) = 1;
    point_projection_matrix = P*spatial_trans;
    back_projection_R = R.transpose()*P.block(0,0,3,3).inverse();
    back_projection_T = R.transpose()*T;

    // Initialize detetction list
    boxes2d = BBoxes_msg->bounding_boxes;
    objs2d = Objs_msg->bounding_boxes;
    inCloud = in_cloud_->makeShared();
    ptrDetectFrame = &DetectFrame;
    initialize_list();

    is_initialized = true;
}
/*****************************************************
*功能：将2D检测结果分组分层，依次处理前景障碍物与车辆点云
*****************************************************/
void detection_fusion::extract_feature() {
    occlusion_table_calc();
    seperate_into_group();
    
    // Process obstacles occluding vehicles first
    for(size_t i = boxes2d.size(); i < occlusion_table.size(); i++)
        for(size_t j = 0; j < boxes2d.size(); j++) 
            if(occlusion_table[i][j]) {obstacle_extract(i-boxes2d.size()); break;}

    // Process vehicles according to groups and distance
    for(size_t i = 0; i < group_sorted.size(); i++)
        for(size_t j =0; j < group_sorted[i].size(); j++) vehicle_extract(group_sorted[i][j]);
}
/*****************************************************
*功能：初始化储存检测的两个list
*****************************************************/
void detection_fusion::initialize_list() {
    std::sort(boxes2d.begin(), boxes2d.end(), [](const Box2d& box_1, const Box2d& box_2) {return box_1.ymax > box_2.ymax;});
    for(auto it = boxes2d.begin(); it != boxes2d.end(); it++) {
        detection_cam* ptr_det (new detection_cam);
        ptr_det->box = *it;
        ptrDetectFrame->addItem(*ptr_det);
    }
    for(auto it = objs2d.begin(); it != objs2d.end(); it++) {
        detection_obj* ptr_det (new detection_obj);
        ptr_det->box = *it;
        ptrObjFrame->addItem(*ptr_det);
    }
}
/*****************************************************
*功能：计算表示遮挡关系的表格用于后续查询
*****************************************************/
void detection_fusion::occlusion_table_calc() {
    // occlusion between vehicles and vehicles
    for(size_t i = 0; i < boxes2d.size(); i++) {
        occlusion_table.push_back(std::vector<bool> {});
        for(size_t j = i+1; j < boxes2d.size(); j++)
            occlusion_table[i].push_back(IoU_bool(boxes2d[i], boxes2d[j]));
    }

    // occlusion between objects and vehicles
    for(size_t i = 0; i < objs2d.size(); i++) {
        occlusion_table.push_back(std::vector<bool> {});
        for(size_t j = 0; j < boxes2d.size(); j++)
            occlusion_table[boxes2d.size()+i].push_back(IoU_bool(objs2d[i], boxes2d[j]));
    }
}
/*****************************************************
*功能：对车辆检测结果进行分组分层
*****************************************************/
void detection_fusion::seperate_into_group() {
    // seperate into groups by occlusion
    std::vector<std::unordered_set<size_t>> group_all;
    size_t all_size = 0;
    size_t start = 0;
    while(all_size < boxes2d.size()) {
        std::unordered_set<size_t> group_set;
        add_to_group(occlusion_table, start, group_set);
        group_all.push_back(group_set);
        for(size_t i = start; i < boxes2d.size(); i++) {
            auto it = group_all.begin();
            while(it != group_all.end()) {
                if(it->find(i) != it->end()) break;
                it++;
            }
            if(it == group_all.end()) {start  = i; break;}
        }
        all_size += group_set.size();
    }

    // sort by distance
    for(size_t i = 0; i < group_all.size(); i++) {
        group_sorted.push_back(std::vector<size_t> {});
        for(auto it = group_all[i].begin(); it != group_all[i].end(); it++)
            group_sorted[i].push_back(*it);
        sort(group_sorted[i].begin(), group_sorted[i].end());
    }
}
/*****************************************************
*功能：根据二维结果剪切点云
*输入：
*box2d: 二维检测框
*outCloud: 剪切后的点云
*fruIndices: 剪切后的点云的检索序号
*****************************************************/
void detection_fusion::clip_frustum(const Box2d box2d, pcl::PointCloud<pcl::PointXYZI>::Ptr &outCloud, pcl::PointIndices& fruIndices) {
    for (size_t i = 0; i < inCloud->points.size(); i++) {
        // Project point in lidar coordinate into image in specific camera
        double x = inCloud->points[i].x;
        if(x > 3) {
            double y = inCloud->points[i].y;
            double z = inCloud->points[i].z;
            Eigen::Matrix<double, 4, 1> point3D;
            point3D << x,y,z,1;
            Eigen::Matrix<double, 3, 1> pointPic = point_projection_matrix * point3D;
            // check whether the point is in the detection
            if(in_frustum(pointPic(0,0)/pointPic(2,0), pointPic(1,0)/pointPic(2,0), box2d))
                fruIndices.indices.push_back(i);
        }
    }
    pcl::ExtractIndices<pcl::PointXYZI> cliper;
    cliper.setInputCloud(inCloud);
    cliper.setIndices(boost::make_shared<pcl::PointIndices>(fruIndices));
    cliper.setNegative(false);
    cliper.filter(*outCloud);
}
/*****************************************************
*功能：根据二维结果剪切点云
*输入：
*box2d: 二维检测框
*outCloud: 剪切后的点云
*fruIndices: 剪切后的点云的检索序号
*****************************************************/
void detection_fusion::clip_frustum_with_overlap(const size_t num, pcl::PointCloud<pcl::PointXYZI>::Ptr &outCloud, pcl::PointIndices& fruIndices) {
    for (size_t i = 0; i < inCloud->points.size(); i++) {
        // Project point in lidar coordinate into image in specific camera
        if(inCloud->points[i].x > 5) {
            // check whether the point is in the detection
            if(in_frustum_overlap(i, num))
                fruIndices.indices.push_back(i);
        }
    }
    pcl::ExtractIndices<pcl::PointXYZI> cliper;
    cliper.setInputCloud(inCloud);
    cliper.setIndices(boost::make_shared<pcl::PointIndices>(fruIndices));
    cliper.setNegative(false);
    cliper.filter(*outCloud);
}
/*****************************************************
*功能：判断单个点是否在检测框内
*输入：
*u: 投影后在图像上的u坐标
*u: 投影后在图像上的v坐标
*box2d: 二维检测框
*****************************************************/
bool detection_fusion::in_frustum(const double u, const double v, const Box2d box) {
    if (u >= box.xmin && u <= box.xmax && v >= box.ymin && v <= box.ymax)
        return true;
    else
        return false;
}
/*****************************************************
*功能：判断单个点是否在检测框内，考虑重合部分
*输入：
*cloud_indices: 点云的检索序号
*num: 二维检测框的序号
*****************************************************/
bool detection_fusion::in_frustum_overlap(const size_t cloud_indice, const size_t num) {
    Eigen::Matrix<double, 4, 1> point3D;
    double x = inCloud->points[cloud_indice].x;
    double y = inCloud->points[cloud_indice].y;
    double z = inCloud->points[cloud_indice].z;
    point3D << x,y,z,1;
    Eigen::Matrix<double, 3, 1> pointPic = point_projection_matrix * point3D;
    double u = pointPic(0,0)/pointPic(2,0);
    double v = pointPic(1,0)/pointPic(2,0);
    std::vector<Box2d>::iterator it = boxes2d.begin() + num;
    if(in_frustum(u, v, *it)) {
        auto it_overlap = overlap_area.begin();
        for(; it_overlap != overlap_area.end(); it_overlap++) {
            if(in_frustum(u, v, *it_overlap)) {
                std::vector<int> indices;
                if(it_overlap->id >= boxes2d.size()) {
                    detection_obj* ptr_det = ptrObjFrame->getPtrItem(it_overlap->id - boxes2d.size());
                    indices = ptr_det->indices.indices;
                } else {
                    detection_cam* ptr_det = ptrDetectFrame->getPtrItem(it_overlap->id);
                    indices = ptr_det->indices.indices;
                }
                size_t search_goal[] = {cloud_indice};
                auto it = std::search(indices.begin(), indices.end(), search_goal, search_goal+1);
                if(it != indices.end())
                    return false;
            } else continue;
        }
        if(it_overlap == overlap_area.end()) return true;
    }
    return false;
}
/*****************************************************
*功能：计算具有遮挡关系的2D检测框重叠部分
*输入：
*prev_box: 检测框
*curr_box: 另一个检测框
*输出：
*overlap: 重合部分检测框
*****************************************************/
Box2d detection_fusion::overlap_box(const Box2d prev_box, const Box2d curr_box) {
    Box2d overlap;
    double prev_center_x = (prev_box.xmax +  prev_box.xmin) / 2;
    double prev_center_y = (prev_box.ymax +  prev_box.ymin) / 2;
    double prev_length = prev_box.xmax -  prev_box.xmin;
    double prev_width = prev_box.ymax -  prev_box.ymin;

    double curr_center_x = (curr_box.xmax +  curr_box.xmin) / 2;
    double curr_center_y = (curr_box.ymax +  curr_box.ymin) / 2;
    double curr_length = curr_box.xmax -  curr_box.xmin;
    double curr_width = curr_box.ymax -  curr_box.ymin;

    double delta_x = std::abs(prev_center_x - curr_center_x);
    double delta_y = std::abs(prev_center_y - curr_center_y);
    double x_threshold = std::abs(prev_length - curr_length)/2;
    double y_threshold = std::abs(prev_width - curr_width)/2;
    // completely covered
    if(delta_x < x_threshold && delta_y < y_threshold) {
        //std::cout << "completed covered." << std::endl;
        overlap.xmin = std::max(prev_box.xmin, curr_box.xmin);
        overlap.ymin = std::max(prev_box.ymin, curr_box.ymin);
        overlap.xmax = std::min(prev_box.xmax, curr_box.xmax);
        overlap.ymax = std::min(prev_box.ymax, curr_box.ymax);
    } else if(delta_y < y_threshold) {
        //std::cout << "y too close." << std::endl;
        Box2d small = prev_width < curr_width ? prev_box : curr_box;
        Box2d large = prev_width < curr_width ? curr_box : prev_box;
        double small_center_x = (small.xmax +  small.xmin) / 2;
        double large_center_x = (large.xmax +  large.xmin) / 2;
        if (small_center_x < large_center_x) {
            overlap.xmax = small.xmax;
            overlap.ymax = small.ymax;
            overlap.xmin = large.xmin;
            overlap.ymin = small.ymin;
        } else{
            overlap.xmax = large.xmax;
            overlap.ymax = small.ymax;
            overlap.xmin = small.xmin;
            overlap.ymin = small.ymin;
        }
    } else if(delta_x < x_threshold) {
        //std::cout << "x too close." << std::endl;
        Box2d small = prev_length < curr_length ? prev_box : curr_box;
        Box2d large = prev_length < curr_length ? curr_box : prev_box;
        double small_center_y = (small.ymax +  small.ymin) / 2;
        double large_center_y = (large.ymax +  large.ymin) / 2;
        if (small_center_y < large_center_y) {
            overlap.xmax = small.xmax;
            overlap.ymax = small.ymax;
            overlap.xmin = small.xmin;
            overlap.ymin = large.ymin;
        } else{
            overlap.xmax = small.xmax;
            overlap.ymax = large.ymax;
            overlap.xmin = small.xmin;
            overlap.ymin = small.ymin;
        }
    } else {
        //std::cout << "normal overlap." << std::endl;
        double delta_x_signed = prev_center_x - curr_center_x;
        double delta_y_signed = prev_center_y - curr_center_y;
        if(delta_x_signed * delta_y_signed > 0) {
            Box2d top_left;
            Box2d down_right;
            top_left = prev_center_x < curr_center_x ? prev_box : curr_box;
            down_right = prev_center_x < curr_center_x ? curr_box : prev_box;
            overlap.xmax = top_left.xmax;
            overlap.ymax = top_left.ymax;
            overlap.xmin = down_right.xmin;
            overlap.ymin = down_right.ymin;
        } else {
            Box2d down_left;
            Box2d top_right;
            top_right = prev_center_x < curr_center_x ? curr_box : prev_box;
            down_left = prev_center_x < curr_center_x ? prev_box : curr_box;
            overlap.xmax = down_left.xmax;
            overlap.ymax = top_right.ymax;
            overlap.xmin = top_right.xmin;
            overlap.ymin = down_left.ymin;
        }
    }
    return overlap;
}
/*****************************************************
*功能：欧几里得聚类，输出最大的点云聚类结果
*输入: 
*in_cloud: 经过视锥剪裁的点云结果
*输出：
*cloud_cluster: 聚类后的最大点云
*****************************************************/
bool detection_fusion::eu_cluster(const pcl::PointCloud<pcl::PointXYZI>::Ptr in_cloud, 
                                  const pcl::PointIndices fruIndices,
                                  pcl::PointCloud<pcl::PointXYZI>::Ptr cloud_cluster,
                                  pcl::PointIndices& objIndices) {
    // Data containers used
    pcl::PointCloud<pcl::PointXYZINormal>::Ptr cloud_with_normals(new pcl::PointCloud<pcl::PointXYZINormal>);
    //pcl::PointCloud<pcl::PointXYZI>::Ptr cloud_cluster(new pcl::PointCloud<pcl::PointXYZI>);
    pcl::search::KdTree<pcl::PointXYZI>::Ptr kd_tree (new pcl::search::KdTree<pcl::PointXYZI>);
    
    // Set up a Normal Estimation class and merge data in cloud_with_normals
    pcl::NormalEstimation<pcl::PointXYZI, pcl::PointXYZINormal> ne;
    pcl::copyPointCloud (*in_cloud, *cloud_with_normals);
    ne.setInputCloud (in_cloud);
    ne.setSearchMethod (kd_tree);
    ne.setRadiusSearch (4);
    ne.compute (*cloud_with_normals);
    
    // Set up a Conditional Euclidean Clustering class
    pcl::ConditionalEuclideanClustering<pcl::PointXYZINormal> cec (false);
    pcl::IndicesClustersPtr clusters_indices (new pcl::IndicesClusters);
    cec.setInputCloud(cloud_with_normals);
    cec.setConditionFunction(&customRegionGrowing);
    cec.setClusterTolerance(0.7);
    cec.setMinClusterSize(0.2*in_cloud->size());
    cec.setMaxClusterSize(in_cloud->size());
    cec.segment(*clusters_indices);

    // The largest cluster will be output
    if(clusters_indices->size() == 0) return false;
    std::vector<pcl::PointIndices>::const_iterator max_size_indices = clusters_indices->begin();
    for(std::vector<pcl::PointIndices>::const_iterator it = clusters_indices->begin(); it != clusters_indices->end(); ++it)
        max_size_indices = (it->indices.size() > max_size_indices->indices.size()) ? it : max_size_indices;
    for(std::vector<int>::const_iterator pit = max_size_indices->indices.begin(); pit != max_size_indices->indices.end(); ++pit) {
        cloud_cluster->push_back((*in_cloud)[*pit]);
        objIndices.indices.push_back(fruIndices.indices[*pit]);
    }

    cloud_cluster->width = cloud_cluster->size();
    cloud_cluster->height = 1;
    cloud_cluster->is_dense = true;
    return true;
}
/*****************************************************
*功能：区域增长算法
*输入：
*point_a: 一个点云
*point_b: 另一个点云
*squared_distance: 欧式距离
*****************************************************/
bool customRegionGrowing(const pcl::PointXYZINormal& point_a, const pcl::PointXYZINormal& point_b, float squared_distance) {
    Eigen::Map<const Eigen::Vector3f> point_a_normal = point_a.getNormalVector3fMap(), point_b_normal = point_b.getNormalVector3fMap();
    if(squared_distance < 4){
        if(std::abs (point_a.intensity - point_b.intensity) < 8.0f) return (true);
        if(std::abs (point_a_normal.dot (point_b_normal)) < 0.06) return (true);
    }else
        if(std::abs (point_a.intensity - point_b.intensity) < 3.0f) return (true);
    return false;
}
/*****************************************************
*功能：提取车辆边框点云，进行L型拟合，根据拟合直线计算三维检测框
*输入：
*ptrCarCloud: 语义分割后的车辆点云
*ptrSgroup: 用于储存提取的边框点云
*u: 用于储存拟合结果
*输出:
返回拟合误差，若不满足拟合条件返回0
*****************************************************/
double detection_fusion::Lshape(pcl::PointCloud<pcl::PointXYZI>::Ptr &ptrCarCloud,
                                pcl::PointCloud<pcl::PointXYZI>::Ptr &ptrSgroup,
                                Matrix51f &u) {
    PointCloudXYZIRT Sgroup_;
     // add attributions of theta and radius
    for (size_t i = 0; i < ptrCarCloud->points.size(); i++) {
        PointXYZIRT Spoint;
        Spoint.point = ptrCarCloud->points[i];
        auto theta = (float)atan2(ptrCarCloud->points[i].y, ptrCarCloud->points[i].x) * 180 / M_PI;
        //if (theta < 0)
        //    theta += 360;
        auto radius = sqrt(ptrCarCloud->points[i].x*ptrCarCloud->points[i].x 
                           + ptrCarCloud->points[i].y*ptrCarCloud->points[i].y);
        Spoint.theta = theta;
        Spoint.radius = radius;
        Sgroup_.push_back(Spoint);
    }
    // sorting by ascending theta
    std::sort(Sgroup_.begin(), Sgroup_.end(), 
              [](const PointXYZIRT &a, const PointXYZIRT &b) { return a.theta < b.theta; });
    if (Sgroup_.size() > S_GROUP_THRESHOLD) {
        Lproposal(Sgroup_, ptrSgroup);

        // Create the filtering object
        /*
        pcl::StatisticalOutlierRemoval<pcl::PointXYZI> sor;
        sor.setInputCloud (ptrSgroup);
        sor.setMeanK(10);
        sor.setStddevMulThresh(1);
        sor.filter (*ptrSgroup);*/

        if (ptrSgroup->size() > S_GROUP_THRESHOLD) {
            //for(size_t i = 0; i < 0; i++) {
            //    ptrSgroup->erase(ptrSgroup->end());
            //    ptrSgroup->erase(ptrSgroup->begin());
            //}
            //Eigen::Matrix<float,3,1> p;
            if (ptrSgroup->size() > S_GROUP_REFINED_THRESHOLD) {
                return Lfit(ptrSgroup, u);
            } else return 0;
        }
    }else return 0;
}
/*****************************************************
*功能：通过拟合的L型直线计算三维检测框参数
*输入：
*u: 拟合的L型直线参数
*ptrSgroup：提取的边框点云
*carCloud: 语义分割后的车辆点云
*box3d：用于存储三维检测结果
*****************************************************/
void detection_fusion::bounding_box_param(const Matrix51f u, const pcl::PointCloud<pcl::PointXYZI>::Ptr ptrSgroup, 
                                          const pcl::PointCloud<pcl::PointXYZI>::Ptr carCloud, Box3d& box3d, const Box2d box) {
    Point2D corner_point;
    Point2D point_1;
    Point2D point_2;
    Point2D point_3;

    float c1 = u(0,0);
    float c2 = u(1,0);
    float n1 = u(2,0);
    float n2 = u(3,0);
    float z_min = 0;
    float z_max = -2;
    
    if(box_corner_estimation(u, corner_point)) {
        if(box_point_estimation(u, corner_point, box, point_1, point_3)){
            box3d.length = sqrt(pow(corner_point.x-point_1.x,2) + pow(corner_point.y-point_1.y,2));
            box3d.width = sqrt(pow(corner_point.x-point_3.x,2) + pow(corner_point.y-point_3.y,2));
        } else {
            box3d.length = box_point_estimation(-n1/n2, -c1/n2, 0, u(4,0), ptrSgroup, corner_point, point_1);
            box3d.width = box_point_estimation(n2/n1, -c2/n1, u(4,0), ptrSgroup->points.size(), ptrSgroup, corner_point, point_3);
        }
        
        point_2.x = point_3.x + (point_1.x - corner_point.x);
        point_2.y = point_3.y + (point_1.y - corner_point.y);

        for (size_t i = 0; i < carCloud->points.size(); i++) {
            z_min = std::min(z_min, carCloud->points[i].z);
            if (carCloud->points[i].z < 1) z_max = std::max(z_max, carCloud->points[i].z);
        }

        box3d.height = z_max - z_min;
        box3d.pos.x = (corner_point.x + point_2.x)/2;
        box3d.pos.y = (corner_point.y + point_2.y)/2;
        box3d.pos.z = (z_max - z_min)/2 + z_min;
        box3d.corner_x = corner_point.x;
        box3d.corner_y = corner_point.y;
        box3d.heading = atan((corner_point.y - point_1.y)/(corner_point.x - point_1.x));
    }
}
/*****************************************************
*功能：L-shape拐点计算
*输入：
*u: 拟合的L型直线参数
*corner_point：用于存储拐点坐标信息
*输出：
true/false: 是否计算成功
*****************************************************/
bool detection_fusion::box_corner_estimation(const Matrix51f u, Point2D &corner_point) {
    float c1 = u(0,0);
    float c2 = u(1,0);
    float n1 = u(2,0);
    float n2 = u(3,0);
    // corner point
    if(n2 != 0 && std::abs(n1/n2) < MIN_SLOPE) {corner_point.y = -c1/n2; corner_point.x = c2/n2;}
    else if (n1 != 0 && std::abs(n2/n1) < MIN_SLOPE) {corner_point.y = -c2/n1; corner_point.x = -c1/n1;}
    else if (n1 != 0 && n1 != 0) {
        corner_point.x = (n2*c2-n1*c1)/(n2*n2 + n1*n1);
        corner_point.y = -n1/n2*corner_point.x-c1/n2;
    }else return false;
    return true;
}
/*****************************************************
*功能：将点投影到直线上求取最大长度作为检测框参数
*输入：
*k: 直线的斜率
*b: 直线的截距
*cloud_start: 用于投影计算的点云起始点
*cloud_end: 用于投影计算的点云最末点
*cloud_in: 用于拟合的点云
*corner_point：拐点坐标信息
*point：用于储存端点坐标信息
*输出：
length_max: 投影后的最大尺寸
*****************************************************/
float detection_fusion::box_point_estimation(const float k, const float b, const size_t cloud_start, const size_t cloud_end,
                                             const pcl::PointCloud<pcl::PointXYZI>::Ptr cloud_in, const Point2D corner_point, Point2D& point) {
    float length_max = 0;
    for (size_t i = cloud_start; i < cloud_end; i++) {
        float x = cloud_in->points[i].x;
        float y = cloud_in->points[i].y;
        point_projection_into_line(x, y, k, b);
        float length = sqrt(pow(x-corner_point.x,2) + pow(y-corner_point.y,2));
        if (length > length_max) {
            length_max = length;
            point.x = x;
            point.y = y;
        }
    }
    return length_max;
}
/*****************************************************
*功能：计算两条直线的交点
*输入：
*u: 拟合的L型直线参数
*corner_point：用于存储拐点坐标信息
*box: 二维检测结果
*point_1: 一个端点
*point_3: 另一个端点
*输出：
true/false: 是否计算成功
*****************************************************/
bool detection_fusion::box_point_estimation(const Matrix51f u, const Point2D corner_point, const Box2d box, 
                                            Point2D &point_1, Point2D &point_3) {
    float c1 = u(0,0);
    float c2 = u(1,0);
    float n1 = u(2,0);
    float n2 = u(3,0);
    float slope_min, slope_max, b_min, b_max;
    uv_projection_into_xy(box.xmin, box.ymin, slope_min, b_min);
    uv_projection_into_xy(box.xmax, box.ymax, slope_max, b_max);
    if(slope_min == -n1/n2 || slope_min == n2/n1 || slope_max == -n1/n2 || slope_max == n2/n1) return false;
    if(line_intersection(slope_min, b_min, -n1/n2, -c1/n2, point_1) && point_1.x >= corner_point.x) {
        line_intersection(slope_max, b_max, n2/n1, -c2/n1, point_1);
    }
    else {
        line_intersection(slope_max, b_max, -n1/n2, -c1/n2, point_1);
        line_intersection(slope_min, b_min, n2/n1, -c2/n1, point_3);
    }
    return true;
}
/*****************************************************
*功能：计算两条直线的交点
*输入：
*k1: 直线L1的斜率
*b1: 直线L1的截距
*k2: 直线L2的斜率
*b2: 直线L2的截距
*point: 交点
*输出：
true/false: 是否计算成功
*****************************************************/
bool detection_fusion::line_intersection(const float k1, const float b1, const float k2, const float b2, Point2D &point) {
    if(k1 == k2) return false;
    point.x = (b2-b1)/(k1-k2);
    point.y = k1*point.x+b1;
    return true;
}
/*****************************************************
*功能：将图像中的像素点投影到雷达坐标系下的鸟瞰图直线
*输入：
*u: 像素点坐标u
*v: 像素点坐标v
*slope: 用于储存直线的斜率
*b: 用于储存直线的截距
*****************************************************/
void detection_fusion::uv_projection_into_xy(const float u, const float v, float &slope, float &b) {
    Eigen::Matrix<double, 3, 1> point2D;
    Eigen::Matrix<double, 3, 1> k;
    Eigen::Matrix<double, 3, 1> k_max;
    point2D << u, v, 1;
    k = back_projection_R*point2D;
    slope = k(1,0)/k(0,0);
    b = k(1,0)/k(0,0)*back_projection_T(0,0)- back_projection_T(1,0);
}
/*****************************************************
*功能：提取车辆边框点云，同一角度选择距离最短的一定数量的点云
*输入：
*Sgroup_: 根据角度排序的点云
*ptrSgroup：筛选出的点云
*****************************************************/
void detection_fusion::Lproposal(const PointCloudXYZIRT Sgroup_, pcl::PointCloud<pcl::PointXYZI>::Ptr &ptrSgroup){
    float theta = Sgroup_[0].theta;
    float theta_sum = 0;
    int num = 0;
    PointCloudXYZIRT tmp;
    // Picking L-shape fitting points according to theta and radius
    for (size_t i = 0; i < Sgroup_.size(); i++) {
        if (abs(Sgroup_[i].theta - theta) < ANGLE_RESO) {
            theta_sum += Sgroup_[i].theta;
            num++;
            theta = theta_sum/num;
            tmp.push_back(Sgroup_[i]);
            if (i == Sgroup_.size() - 1) {
                std::sort(tmp.begin(), tmp.end(), [](const PointXYZIRT &a, const PointXYZIRT &b){return a.radius < b.radius;});
                int j = 0;
                while (tmp.size() > POINT_NUM && j < POINT_NUM){
                    ptrSgroup->points.push_back(tmp[j].point);
                    j++;
                } 
            }
        } else {
            std::sort(tmp.begin(), tmp.end(), [](const PointXYZIRT &a, const PointXYZIRT &b){return a.radius < b.radius;});
            int j = 0;
            while (tmp.size() > POINT_NUM && j < POINT_NUM){
                ptrSgroup->points.push_back(tmp[j].point);
                j++;
            } 
            tmp.clear();
            theta = Sgroup_[i].theta;
            theta_sum = theta;
            num = 1;
            tmp.push_back(Sgroup_[i]);
            if (i == Sgroup_.size() - 1) {
                std::sort(tmp.begin(), tmp.end(), [](const PointXYZIRT &a, const PointXYZIRT &b){return a.radius < b.radius;});
                size_t j = 0;
                while (j < tmp.size() && j < POINT_NUM){
                    ptrSgroup->points.push_back(tmp[j].point);
                    j++;
                }
            }
        }
    }
    //for(size_t i = 0; i < ptrSgroup->points.size(); i++)
        //std::cout << ptrSgroup->points[i].x << '\t' << ptrSgroup->points[i].y << std::endl;
}
/*****************************************************
*功能：增量法拟合L-shape点云
*输入：
*in_cloud: 用于拟合的点云
*u：用于储存拟合后的直线参数
*****************************************************/
double detection_fusion::Lfit(const pcl::PointCloud<pcl::PointXYZI>::Ptr in_cloud, Matrix51f &u) {
    float eigenVal = 10000;
    Eigen::Matrix<float, 2, 1> n;
    Eigen::Matrix<float, 2, 1> c;

    Matrix4f M = Matrix4f::Zero();
    for (size_t i = 0; i < in_cloud->points.size(); i++) {
        float x = in_cloud->points[i].x;
        float y = in_cloud->points[i].y;
        M(1,2) += y;
        M(1,3) -= x;
        M(2,2) += y*y;
        M(2,3) -= x*y;
        M(3,3) += x*x;
    }
    M(0,0) = 0;
    M(1,1) = in_cloud->points.size();
    M(2,1) = M(1, 2);
    M(3,1) = M(1, 3);
    M(3,2) = M(2, 3);

    for (size_t i = 0; i < in_cloud->size()-1; i++) {
        M += deltaM_compute(in_cloud->points[i]);
        Matrix2f M11 = M.block<2,2>(0,0);
        Matrix2f M12 = M.block<2,2>(0,2);
        Matrix2f M22 = M.block<2,2>(2,2);
        Matrix2f tmp = M22 - M12.transpose()*M11.inverse()*M12;
        // get eigenvalues and eigenvectors
        Eigen::EigenSolver<Matrix2f> eigen_solver (tmp);
        for (int j = 0; j < tmp.rows(); j++) {
            if (eigen_solver.eigenvalues()(j).real() < eigenVal) {
                eigenVal = eigen_solver.eigenvalues()(j).real();
                n = eigen_solver.eigenvectors().col(j).real();
                c = -M11.inverse()*M12*n;
                u << c[0],c[1],n[0],n[1], i+1;
            }
        }
    }
    return eigenVal;
}
/*****************************************************
*功能：计算增量的deltaM
*输入:
*point: 从Q集合到P集合中的一个点
输出:
*deltaM: 矩阵增量值
*****************************************************/
Matrix4f detection_fusion::deltaM_compute(const pcl::PointXYZI point) {
    float x = point.x;
    float y = point.y;
    Matrix4f deltaM;
    deltaM << 1, 0,  x      , y,
              0,-1, -y      , x,
              x,-y,  x*x-y*y, 2*x*y,
              y, x,  2*x*y  , y*y-x*x;
    return deltaM;
}
/*****************************************************
*功能：点云投影到直线上的坐标
*输入：
*x: 点云横坐标
*y: 点云纵坐标
*k：直线斜率
*b：直线横截率
*****************************************************/
void detection_fusion::point_projection_into_line(float &x, float &y, const float k, const float b) {
    x = (k*(y-b)+x)/(k*k+1);
    y = k*x+b;
}
/*****************************************************
*功能：提取前景障碍物点云聚类结果
*输入：
*num: 前景障碍物序号
*****************************************************/
void detection_fusion::obstacle_extract(const size_t num) {
    std::vector<Box2d>::iterator it = objs2d.begin() + num;
    detection_obj* ptr_det = ptrObjFrame->getPtrItem(num);
    pcl::PointCloud<pcl::PointXYZI>::Ptr fruCloud (new pcl::PointCloud<pcl::PointXYZI>);
    pcl::PointCloud<pcl::PointXYZI>::Ptr objCloud (new pcl::PointCloud<pcl::PointXYZI>);
    pcl::PointIndices fruIndices;
    clip_frustum(*it, fruCloud, fruIndices);
    if (fruCloud->points.size()) {
        pcl::PointIndices objIndices;
        if(eu_cluster(fruCloud, fruIndices, objCloud, objIndices)) {
            std::sort(objIndices.indices.begin(), objIndices.indices.end());
            ptr_det->indices = objIndices;
        } else {
            // add far-away objects flag
            ptr_det->far = true;
            float dis = 0;
            for(size_t i = 0; i < fruCloud->points.size(); i++) {
                dis = (dis*i+fruCloud->points[i].x)/(i+1);
            }
            ptr_det->distance_far = dis;
        }
    }
}
/*****************************************************
*功能：提取车辆点云聚类结果
*输入：
*num: 前景障碍物序号
*****************************************************/
void detection_fusion::vehicle_extract(const size_t num) {
    std::vector<Box2d>::iterator it = boxes2d.begin() + num;
    //if (it->xmin > 0 && it->ymin > 0 && it->xmax < IMG_LENGTH && it->ymax < IMG_WIDTH) {
    //detection_cam* ptr_det (new detection_cam);
    detection_cam* ptr_det = ptrDetectFrame->getPtrItem(num);
    pcl::PointCloud<pcl::PointXYZI>::Ptr fruCloud (new pcl::PointCloud<pcl::PointXYZI>);
    pcl::PointCloud<pcl::PointXYZI>::Ptr carCloud (new pcl::PointCloud<pcl::PointXYZI>);
    pcl::PointCloud<pcl::PointXYZI>::Ptr ptrSgroup (new pcl::PointCloud<pcl::PointXYZI>);
    // finding overlap areas
    for(size_t i = boxes2d.size(); i < occlusion_table.size(); i++) {
        if(occlusion_table[i][num]) {
            Box2d overlap = overlap_box(*it, objs2d[i-boxes2d.size()]);
            overlap.id = i;
            overlap_area.push_back(overlap);
            //std::cout << "overlapped by: " << i << std::endl;
        }
    }
    for(size_t i = 0; i < num; i++) {
        if(occlusion_table[i][num-i-1]) {
            Box2d overlap = overlap_box(*it, boxes2d[i]);
            overlap.id = i;
            overlap_area.push_back(overlap);
            //std::cout << "overlapped by: " << i << std::endl;
        }
    }
    pcl::PointIndices fruIndices;
    clip_frustum_with_overlap(num, fruCloud, fruIndices);
    Matrix51f u = Matrix51f::Zero();
    if (fruCloud->points.size()) {
        pcl::PointIndices carIndices;
        if(eu_cluster(fruCloud, fruIndices, carCloud, carIndices)) {
            double error = Lshape(carCloud, ptrSgroup, u);
            ptr_det->CarCloud = *carCloud;
            ptr_det->fruCloud = *fruCloud;
            ptr_det->surCloud = *ptrSgroup;
            ptr_det->indices = carIndices;
            if (error > 0) bounding_box_param(u, ptrSgroup, carCloud, ptr_det->box3d, ptr_det->box);
        } else {
            // add far-away objects flag
            ptr_det->far = true;
            float dis = 0;
            for(size_t i = 0; i < fruCloud->points.size(); i++) {
                dis = (dis*i+fruCloud->points[i].x)/(i+1);
            }
            ptr_det->distance_far = dis;
        }
    }
    //ptr_det->box = *it;
    //ptrDetectFrame->addItem(*ptr_det);
    //}
}
/*
void detection_fusion::vehicle_extract(const int num) {
    std::vector<Box2d>::iterator it = boxes2d.begin() + num;
    //if (it->xmin > 0 && it->ymin > 0 && it->xmax < IMG_LENGTH && it->ymax < IMG_WIDTH) {
    detection_cam* ptr_det (new detection_cam);
    pcl::PointCloud<pcl::PointXYZI>::Ptr fruCloud (new pcl::PointCloud<pcl::PointXYZI>);
    pcl::PointCloud<pcl::PointXYZI>::Ptr carCloud (new pcl::PointCloud<pcl::PointXYZI>);
    pcl::PointCloud<pcl::PointXYZI>::Ptr ptrSgroup (new pcl::PointCloud<pcl::PointXYZI>);
    clip_frustum(*it, fruCloud);
    Eigen::Matrix<float, 4, 1> u = Matrix41f::Zero();
    if (fruCloud->points.size()) {
        //ptr_det->PointCloud = *fruCloud;
        if(eu_cluster(fruCloud, carCloud)) {
            double error = Lshape(carCloud, ptrSgroup, u);
            ptr_det->CarCloud = *ptrSgroup;
            if (error > 0) bounding_box_param(u, ptrSgroup, carCloud, ptr_det->box3d);
        } else {
            // add far-away objects flag
            ptr_det->far = true;
            float dis = 0;
            for(size_t i = 0; i < fruCloud->points.size(); i++) {
                dis = (dis*i+fruCloud->points[i].x)/(i+1);
            }
            ptr_det->distance_far = dis;
        }
    }
    ptr_det->box = *it;
    ptrDetectFrame->addItem(*ptr_det);
    //}
}
*/
/*****************************************************
*功能：求取两个图像二维检测结果IoU数值
*输入：
*prev_box: 遮挡物的检测框
*curr_box: 被遮挡的检测框
*输出：
*IoU数值
*****************************************************/
bool IoU_bool(const Box2d prev_box, const Box2d curr_box) {
    double prev_center_x = (prev_box.xmax +  prev_box.xmin) / 2;
    double prev_center_y = (prev_box.ymax +  prev_box.ymin) / 2;
    double prev_length = prev_box.xmax -  prev_box.xmin;
    double prev_width = prev_box.ymax -  prev_box.ymin;

    double curr_center_x = (curr_box.xmax +  curr_box.xmin) / 2;
    double curr_center_y = (curr_box.ymax +  curr_box.ymin) / 2;
    double curr_length = curr_box.xmax -  curr_box.xmin;
    double curr_width = curr_box.ymax -  curr_box.ymin;

    double len = (prev_length + curr_length)/2 -  std::abs(prev_center_x - curr_center_x);
    double wid = (prev_width + curr_width)/2 -  std::abs(prev_center_y - curr_center_y);

    if(len > IOU_THRESHOLD*curr_length && wid > IOU_THRESHOLD*curr_width) return true;
    else return false;
}
void detection_fusion::add_to_group(const std::vector<std::vector<bool>> occlusion_table, const int start, std::unordered_set<size_t>& group_set) {
    group_set.insert(start);
    //std::cout << "group_member: " << start << std::endl;
    for(size_t i = 0; i < occlusion_table[start].size(); i++) {
        if(occlusion_table[start][i]) {
            int group_member = start+1+i;
            if(group_set.find(group_member) == group_set.end()) {
                add_to_group(occlusion_table, group_member, group_set);
            }
        }
    }
}
Boxes2d detection_fusion::get_boxes(){
    return overlap_area;
}