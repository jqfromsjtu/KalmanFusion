cd ..
cd ..
catkin_make -DCATKIN_WHITELIST_PACKAGES="kitti_pub"
source devel/setup.bash
roslaunch kitti_pub kitti.launch