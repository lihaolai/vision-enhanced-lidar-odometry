#include <iostream>
#include <fstream>
#include <cstdlib>
#include <vector>
#include <deque>
#include <string>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <list>
#include <unordered_map>
#include <random>

#include <Eigen/StdVector>
#include <Eigen/Dense>

#include <opencv2/opencv.hpp>
#include <opencv2/core.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/xfeatures2d.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/highgui.hpp>

#include <opencv2/core/cuda.hpp>
#include <opencv2/cudaimgproc.hpp>
#include <opencv2/cudafeatures2d.hpp>

#include <pcl/io/pcd_io.h>
#include <pcl/point_types.h>

#include <pcl/registration/ndt.h>
#include <pcl/filters/approximate_voxel_grid.h>

#include <ceres/ceres.h>
#include <ceres/rotation.h>
#include <glog/logging.h>
#include <gflags/gflags.h>

#include <isam/isam.h>
#include <isam/robust.h>
#include <isam/slam_monocular.h>

#define VISUALIZE
#define ENABLE_ICP
#define ENABLE_2D2D
#define ENABLE_3D2D

#include "utility.h"
#include "kitti.h"
#include "costfunctions.h"
#include "velo.h"
#include "lru.h"


int main(int argc, char** argv) {
    cv::setUseOptimized(true);

    cv::cuda::setDevice(1);
    //cv::cuda::printCudaDeviceInfo(cv::cuda::getDevice());
    if(argc < 2) {
        std::cout << "Usage: velo kittidatasetnumber. e.g. velo 00" << std::endl;
        return 1;
    }
    std::string dataset = argv[1];
    loadImage(dataset, 0, 0); // to set width and height
    loadCalibration(dataset);
    loadTimes(dataset);

    int num_frames = times.size();

    // FREAK feature descriptor
    cv::Ptr<cv::xfeatures2d::FREAK> freak = cv::xfeatures2d::FREAK::create(
            false, // orientation normalization
            false // scale normalization
            );
    // good features to detect
    cv::Ptr<cv::GFTTDetector> gftt = cv::GFTTDetector::create(
            corner_count,
            quality_level,
            min_distance);

    // tracked keypoints, camera canonical coordinates
    std::vector<std::vector<std::vector<cv::Point2f>>> keypoints(num_cams,
            std::vector<std::vector<cv::Point2f>>(num_frames));
    // tracked keypoints, pixel coordinates
    std::vector<std::vector<std::vector<cv::Point2f>>> keypoints_p(num_cams,
            std::vector<std::vector<cv::Point2f>>(num_frames));
    // IDs of keypoints
    std::vector<std::vector<std::vector<int>>> keypoint_ids(num_cams,
            std::vector<std::vector<int>>(num_frames));
    // FREAK descriptors of each keypoint
    std::vector<std::vector<cv::Mat>> descriptors(num_cams,
            std::vector<cv::Mat>(num_frames));
    // -1 if no depth, index of kp_with_depth otherwise
    std::vector<std::vector<std::vector<int>>> has_depth(num_cams,
            std::vector<std::vector<int>>(num_frames));
    // interpolated lidar point, physical coordinates
    std::vector<std::vector<pcl::PointCloud<pcl::PointXYZ>::Ptr>> kp_with_depth(
            num_cams,
            std::vector<pcl::PointCloud<pcl::PointXYZ>::Ptr>(num_frames));
    // images of current and frame, used for optical flow tracking
    std::vector<cv::Mat> imgs(num_cams);
    std::vector<cv::Mat> img_prevs(num_cams);

    // vector which maps from keypoint id to observed position
    // [keypoint_id][cam][frame] = observation
    std::vector<std::vector<std::map<int, cv::Point2f>>> keypoint_obs2;
    // [keypoint_id][cam][frame] = observation3
    std::vector<std::vector<std::map<int, pcl::PointXYZ>>> keypoint_obs3;
    // number of observations for each keypoint_id
    std::vector<int> keypoint_obs_count;


    // counters for keypoint id, one for each camera
    int id_counter = 0;
    // histogram to keep track of how long keypoints last
    // no keypoint can possibly be seen more than 1000 times;
    std::vector<int> keypoint_obs_count_hist(1000, 0);
    std::vector<bool> keypoint_added;

    std::vector<Eigen::Matrix4d,
        Eigen::aligned_allocator<Eigen::Matrix4d>> dead_reckoning(num_frames);
    dead_reckoning[0] = Eigen::Matrix4d::Identity();

#ifdef VISUALIZE
    char video[] = "video";
    cvNamedWindow(video);
#endif

    Eigen::Matrix4d pose = Eigen::Matrix4d::Identity();
    double transform[6] = {0, 0, 0, 0, 0, 1};
    ScansLRU lru;

    // preliminaries for bundle adjustment
    isam::Slam slam;
    isam::Properties prop = slam.properties();
    prop.max_iterations = 50;
    slam.set_properties(prop);
    std::vector<std::vector<isam::Pose3d_Node*>> cam_nodes(num_cams);
    std::vector<isam::Point3d_Node*> point_nodes;
    std::vector<isam::MonocularCamera> monoculars(num_cams);
    for(int cam=0; cam<num_cams; cam++) {
        monoculars[cam] = isam::MonocularCamera(1, Eigen::Vector2d(0, 0));
        for(int frame = 0; frame < num_frames; frame++) {
            isam::Pose3d_Node* initial_node = new isam::Pose3d_Node();
            cam_nodes[cam].push_back(initial_node);
        }
        slam.add_node(cam_nodes[cam][0]);
    }
    isam::Noise noiseless6 = isam::Information(1000. * isam::eye(6));
    isam::Noise noisy6 = isam::Information(1 * isam::eye(6));
    isam::Pose3d origin;
    isam::Pose3d_Factor* prior = new isam::Pose3d_Factor(
            cam_nodes[0][0], origin, noiseless6);
    slam.add_factor(prior);
    for(int cam = 1; cam<num_cams; cam++) {
        isam::Pose3d_Pose3d_Factor* cam_factor = new isam::Pose3d_Pose3d_Factor(
                cam_nodes[0][0],
                cam_nodes[cam][0],
                isam::Pose3d(cam_pose[cam].cast<double>()),
                noiseless6
                );
        slam.add_factor(cam_factor);
    }

    for(int frame = 0; frame < num_frames; frame++) {
        if(frame > 0) {
            for(int cam = 0; cam<num_cams; cam++) {
                slam.add_node(cam_nodes[cam][frame]);
            }
        }

        ScanData *sd = lru.get(dataset, frame);
        const auto &scans = sd->scans;
        for(int cam = 0; cam<num_cams; cam++) {
            imgs[cam] = loadImage(dataset, cam, frame);
        }
        if(frame > 0) {
            for(int cam = 0; cam<num_cams; cam++) {
                for(int prev_cam = 0; prev_cam < num_cams; prev_cam++) {
                    trackFeatures(
                            keypoints,
                            keypoints_p,
                            keypoint_ids,
                            descriptors,
                            img_prevs[prev_cam],
                            imgs[cam],
                            prev_cam,
                            cam,
                            frame-1,
                            frame);
                }
            }
            for(int cam = 0; cam<num_cams; cam++) {
                consolidateFeatures(
                        keypoints[cam][frame],
                        keypoints_p[cam][frame],
                        keypoint_ids[cam][frame],
                        descriptors[cam][frame],
                        cam);

                removeTerribleFeatures(
                        keypoints[cam][frame],
                        keypoints_p[cam][frame],
                        keypoint_ids[cam][frame],
                        descriptors[cam][frame],
                        freak,
                        imgs[cam],
                        cam);

                std::vector<std::vector<cv::Point2f>> projection;
                std::vector<pcl::PointCloud<pcl::PointXYZ>::Ptr> scans_valid;
                projectLidarToCamera(scans, projection, scans_valid, cam);

                kp_with_depth[cam][frame] =
                    pcl::PointCloud<pcl::PointXYZ>::Ptr(
                            new pcl::PointCloud<pcl::PointXYZ>);
                featureDepthAssociation(scans_valid,
                    projection,
                    keypoints[cam][frame],
                    kp_with_depth[cam][frame],
                    has_depth[cam][frame]);
            }
        }
        std::cerr << "Feature tracking done" << std::endl;

        for(int dframe = 1; dframe <= 10; dframe++) {
            if(frame-dframe < 0) break;
            // matches are what's fed into frameToFrame,
            // good matches have outliers removed during optimization
            // TODO: compute transform guess
            Eigen::Matrix4d dT;
            if(dframe == 1) {
                if(frame > 1) {
                    //Eigen::Matrix4d T1 = cam_nodes[0][frame-2]->value().wTo();
                    //Eigen::Matrix4d T2 = cam_nodes[0][frame-1]->value().wTo();
                    auto T1 = dead_reckoning[frame-2];
                    auto T2 = dead_reckoning[frame-1];
                    dT = T2 * T1.inverse();
                } else {
                    dT = util::pose_mat2vec(transform);
                }
            } else {
                //Eigen::Matrix4d T1 = cam_nodes[0][frame-dframe]->value().wTo();
                //Eigen::Matrix4d T2 = cam_nodes[0][frame]->value().wTo();
                auto T1 = dead_reckoning[frame-dframe];
                auto T2 = dead_reckoning[frame];
                dT = T2 * T1.inverse();
            }
            util::pose_vec2mat(dT, transform);
            std::cerr << "Computing f2f pose: " << 
                " " << frame-dframe << "-" << frame << std::endl;
            std::vector<std::vector<std::pair<int, int>>> matches(num_cams);
            std::vector<std::vector<std::pair<int, int>>> good_matches(num_cams);
            std::vector<std::vector<ResidualType>> residual_type(num_cams);
            sd = lru.get(dataset, frame);
            ScanData *sd_prev = lru.get(dataset, frame - dframe);
            matchUsingId(keypoint_ids, frame, frame-dframe, matches);
            std::cerr << "Matches using id: " << matches[0].size() << std::endl;
            if(dframe > 1 && matches[0].size() < min_matches) break;
            std::cerr << "Predicted: ";
            for(int i=0; i<6; i++) std::cerr << transform[i] << " ";
            std::cerr << std::endl;
            auto start = clock() / double(CLOCKS_PER_SEC);
            Eigen::Matrix4d dpose = frameToFrame(
                    matches,
                    keypoints,
                    kp_with_depth,
                    has_depth,
                    sd->scans,
                    sd_prev->scans,
                    sd_prev->trees,
                    frame,
                    frame-dframe,
                    transform,
                    good_matches,
                    residual_type);
            auto end = clock() / double(CLOCKS_PER_SEC);
            if(dframe == 1) {
                dead_reckoning[frame] = dpose * dead_reckoning[frame-1];
            }
            std::cerr << "Optimized (t=" << end - start << "): ";
            for(int i=0; i<6; i++) std::cerr << transform[i] << " ";
            std::cerr << std::endl;

            Eigen::Matrix4d dT2 = util::pose_mat2vec(transform);
            double agreement[6];
            util::pose_vec2mat(dT2 * dT.inverse(), agreement);
            std::cerr << "Agreement: ";
            for(int i=0; i<6; i++) std::cerr << agreement[i] << " ";
            std::cerr << std::endl;
            Eigen::Matrix3d agreement_t;
            agreement_t << agreement[3], agreement[4], agreement[5];
            Eigen::Matrix3d agreement_r;
            agreement_r << agreement[0], agreement[1], agreement[2];
            if(agreement_t.norm() > agreement_t_thresh * dframe && dframe > 1) {
                std::cerr << "Poor t agreement" << std::endl;
                break;
            }
            if(agreement_r.norm() > agreement_r_thresh * dframe && dframe > 1) {
                std::cerr << "Poor r agreement" << std::endl;
                break;
            }

#ifdef VISUALIZE
            if(dframe == 1) {
                std::vector<cv::Mat> draws(num_cams);
                for(int cam=0; cam<num_cams; cam++) {
                    cv::Mat draw;
                    cvtColor(imgs[cam], draw, cv::COLOR_GRAY2BGR);
                    auto &K = cam_intrinsic[cam];
                    //cv::drawKeypoints(img, keypoints[frame], draw);
                    for(int k=0; k<keypoints[cam][frame].size(); k++) {
                        auto p = keypoints_p[cam][frame][k];
                        if(has_depth[cam][frame][k] != -1) {
                            cv::circle(draw, p, 2, cv::Scalar(0, 0, 255), -1, 8, 0);
                        } else {
                            cv::circle(draw, p, 2, cv::Scalar(255, 200, 0), -1, 8, 0);
                        }
                    }
                    /*
                    for(int s=0, _s = projection.size(); s<_s; s++) {
                        auto P = projection[s];
                        for(auto p : P) {
                            auto pp = canonical2pixel(p, K);
                            cv::circle(draw, pp, 1,
                                    cv::Scalar(0, 128, 0), -1, 8, 0);
                        }
                    }
                    */

                    for(auto m : matches[cam]) {
                        auto p1 = keypoints_p[cam][frame][m.first];
                        auto p2 = keypoints_p[cam][frame-dframe][m.second];
                        cv::arrowedLine(draw, p1, p2,
                                cv::Scalar(0, 255, 255), 1, CV_AA);
                    }

                    for(int i=0; i<good_matches[cam].size(); i++) {
                        auto m = good_matches[cam][i];
                        auto p1 = keypoints_p[cam][frame][m.first];
                        auto p2 = keypoints_p[cam][frame-dframe][m.second];
                        cv::Scalar color = cv::Scalar(0, 0, 0);
                        switch(residual_type[cam][i]) {
                            case RESIDUAL_3D3D:
                                color = cv::Scalar(0, 0, 255);
                                break;
                            case RESIDUAL_3D2D:
                            case RESIDUAL_2D3D:
                                color = cv::Scalar(0, 250, 0);
                                break;
                            case RESIDUAL_2D2D:
                                color = cv::Scalar(255, 200, 0);
                                break;
                        }
                        cv::arrowedLine(draw, p1, p2,
                                color, 2, CV_AA);
                    }

                    // Draw stereo matches
                    std::vector<std::pair<int, int>> intercamera_matches;
                    matchUsingId(keypoint_ids, 0, 1, frame, frame,
                            intercamera_matches);
                    for(auto m : intercamera_matches) {
                        auto p1 = keypoints_p[0][frame][m.first];
                        auto p2 = keypoints_p[1][frame][m.second];
                        cv::line(draw, p1, p2, cv::Scalar(255, 0, 255), 1, CV_AA);
                    }
                    draw.copyTo(draws[cam]);
                }
                cv::Mat D;
                vconcat(draws[0], draws[1], D);
                cv::imshow(video, D);
                cvWaitKey(1);
            }
#endif
            if(dframe == 1) {
                /*
                removeSlightlyLessTerribleFeatures(
                        keypoints,
                        keypoints_p,
                        kp_with_depth,
                        keypoint_ids,
                        descriptors,
                        has_depth,
                        frame,
                        good_matches);
                        */
            }

            // iSAM time!
            isam::Pose3d_Pose3d_Factor* odom_factor =
                new isam::Pose3d_Pose3d_Factor(
                    cam_nodes[0][frame-dframe],
                    cam_nodes[0][frame],
                    isam::Pose3d(dpose),
                    noisy6
                    );
            slam.add_factor(odom_factor);
            for(int cam = 1; cam<num_cams; cam++) {
                isam::Pose3d_Pose3d_Factor* cam_factor =
                    new isam::Pose3d_Pose3d_Factor(
                        cam_nodes[0][frame],
                        cam_nodes[cam][frame],
                        isam::Pose3d(cam_pose[cam].cast<double>()),
                        noiseless6
                        );
                slam.add_factor(cam_factor);
            }
            //slam.batch_optimization();
        }
        sd = lru.get(dataset, frame);
        for(int cam = 0; cam<num_cams; cam++) {
            if(frame % detect_every == 0) {
                detectFeatures(
                        keypoints[cam],
                        keypoints_p[cam],
                        keypoint_ids[cam],
                        descriptors[cam],
                        gftt,
                        freak,
                        imgs[cam],
                        id_counter,
                        cam,
                        frame);
                if(cam == 0) {
                    for(int other_cam = 0; other_cam < num_cams; other_cam++) {
                        if(other_cam == cam) continue;
                        trackFeatures(
                                keypoints,
                                keypoints_p,
                                keypoint_ids,
                                descriptors,
                                imgs[cam],
                                imgs[other_cam],
                                cam,
                                other_cam,
                                frame,
                                frame);
                    }
                }
            }
        }
        for(int cam = 0; cam<num_cams; cam++) {
            //std::cerr << "inter frame tracked" << std::endl;
            // TODO: don't do this twice
            consolidateFeatures(
                    keypoints[cam][frame],
                    keypoints_p[cam][frame],
                    keypoint_ids[cam][frame],
                    descriptors[cam][frame],
                    cam);

            removeTerribleFeatures(
                    keypoints[cam][frame],
                    keypoints_p[cam][frame],
                    keypoint_ids[cam][frame],
                    descriptors[cam][frame],
                    freak,
                    imgs[cam],
                    cam);
            std::vector<std::vector<cv::Point2f>> projection;
            std::vector<pcl::PointCloud<pcl::PointXYZ>::Ptr> scans_valid;
            projectLidarToCamera(sd->scans, projection, scans_valid, cam);

            kp_with_depth[cam][frame].reset();
            kp_with_depth[cam][frame] = pcl::PointCloud<pcl::PointXYZ>::Ptr(new pcl::PointCloud<pcl::PointXYZ>);
            featureDepthAssociation(scans_valid,
                projection,
                keypoints[cam][frame],
                kp_with_depth[cam][frame],
                has_depth[cam][frame]);
            img_prevs[cam] = imgs[cam];
        }

        while(point_nodes.size() <= id_counter) {
            isam::Point3d_Node* point_node = new isam::Point3d_Node();
            point_nodes.push_back(point_node);
        }
        keypoint_added.resize(id_counter+1, false);
        keypoint_obs_count_hist[0] += id_counter+1 - keypoint_obs_count.size();
        keypoint_obs_count.resize(id_counter+1, 0);
        keypoint_obs2.resize(id_counter+1,
                std::vector<std::map<int, cv::Point2f>>(num_cams));
        keypoint_obs3.resize(id_counter+1,
                std::vector<std::map<int, pcl::PointXYZ>>(num_cams));
        for(int cam=0; cam<num_cams; cam++) {
            for(int i=0; i<keypoints[cam][frame].size(); i++) {
                int id = keypoint_ids[cam][frame][i];
                keypoint_obs_count[id]++;
                int koc = keypoint_obs_count[id];
                keypoint_obs_count_hist[koc-1]--;
                keypoint_obs_count_hist[koc]++;
                /*
                std::cerr << i << "," << has_depth[cam][frame][i] 
                    << "/" << keypoints[cam][frame].size()
                    << " " << std::endl;
                    */
                if(has_depth[cam][frame][i] == -1) {
                    keypoint_obs2[id][cam][frame] = keypoints[cam][frame][i];
                } else {
                    keypoint_obs3[id][cam][frame] = kp_with_depth[cam][frame]->at(
                            has_depth[cam][frame][i]);
                }
            }
            //std::cerr << std::endl;
        }
        std::cerr << "Keypoint observation stats: ";
        for(int i=0; i<10; i++) {
            std::cerr << keypoint_obs_count_hist[i] << " ";
        }
        int zxcv = 0; for(auto h : keypoint_obs_count_hist) zxcv += h;
        std::cerr << zxcv << std::endl;
        std::cerr << "Features: " << id_counter+1 << std::endl;

        std::set<int> ids_seen;
        for(int cam=0; cam<num_cams; cam++) {
            for(int i=0; i<keypoints[cam][frame].size(); i++) {
                int id = keypoint_ids[cam][frame][i];
                ids_seen.insert(id);
            }
        }
        /*
        for(auto id : ids_seen) {
            if(keypoint_obs_count[id] <= 3) {
                continue;
            }
            if(!keypoint_added[id]) {
                slam.add_node(point_nodes[id]);
                keypoint_added[id] = true;
            }
            for(int cam=0; cam<num_cams; cam++) {
                for(auto obs3 : keypoint_obs3[id][cam]) {
                    isam::Noise noise3 = isam::Information(1 * isam::eye(3));
                    auto p3 = obs3.second;
                    Eigen::Vector3d point3d = p3.getVector3fMap().cast<double>();
                    isam::Pose3d_Point3d_Factor* factor = new isam::Pose3d_Point3d_Factor(
                            cam_nodes[cam][obs3.first],
                            point_nodes[id],
                            isam::Point3d(point3d),
                            noise3
                            );
                    slam.add_factor(factor);
                }
                keypoint_obs3[id][cam].clear();
            }
            for(int cam=0; cam<num_cams; cam++) {
                for(auto obs2 : keypoint_obs2[id][cam]) {
                    isam::MonocularMeasurement measurement(
                            obs2.second.x,
                            obs2.second.y
                            );
                    isam::Noise noise2 = isam::Information(1 * isam::eye(2));
                    isam::Monocular_Factor* factor;
                    factor = new isam::Monocular_Factor(
                            cam_nodes[cam][obs2.first],
                            point_nodes[id],
                            &(monoculars[cam]),
                            measurement,
                            noise2
                            );
                    slam.add_factor(factor);
                }
                keypoint_obs2[id][cam].clear();
            }
        }
        */
        if(frame > 0 && frame % ba_every == 0) {
            slam.batch_optimization();

            std::ofstream output;
            output.open(("results/" + std::string(argv[1]) + ".txt").c_str());
            for(int i=0; i<=frame; i++) {
                auto node = cam_nodes[0][i];
                Eigen::Matrix4d result = node->value().wTo();
                output_line(result, output);
            }
            output.close();
        }
        std::cerr << "Frame complete: " << frame << std::endl;
    }
    return 0;
}
