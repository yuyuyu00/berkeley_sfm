/*
 * Copyright (c) 2015, The Regents of the University of California (Regents).
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *    1. Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *    2. Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *
 *    3. Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS AS IS
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * Please contact the author(s) of this library if you have any questions.
 * Authors: Erik Nelson            ( eanelson@eecs.berkeley.edu )
 *          David Fridovich-Keil   ( dfk@eecs.berkeley.edu )
 */

#include <camera/camera.h>
#include <camera/camera_extrinsics.h>
#include <camera/camera_intrinsics.h>
#include <geometry/eight_point_algorithm_solver.h>
#include <geometry/essential_matrix_solver.h>
#include <geometry/rotation.h>
#include <matching/distance_metric.h>
#include <matching/feature_matcher_options.h>
#include <matching/pairwise_image_match.h>
#include <matching/naive_feature_matcher.h>
#include <math/random_generator.h>
#include <pose/pose.h>
#include <sfm/view.h>
#include <slam/landmark.h>
#include <slam/observation.h>
#include <util/types.h>

#include <Eigen/Core>
#include <gtest/gtest.h>

namespace bsfm {

using Eigen::Matrix3d;
using Eigen::Vector3d;

class TestSimpleNoiselessSfm : public ::testing::Test {
 public:
  // Clean up before and after running tests.
  TestSimpleNoiselessSfm() {
    Landmark::ResetLandmarks();
    View::ResetViews();
  }

  ~TestSimpleNoiselessSfm() {
    Landmark::ResetLandmarks();
    View::ResetViews();
  }

 protected:

  // Create a bunch of 3D points, and cameras that are able to see at least one of
  // those points. Also generate a random descriptor for each point.
  void GenerateGroundTruthPointsAndCameras() {
    points_.clear();
    descriptors_.clear();
    cameras_.clear();

    // Generate a bunch of 3D points, and then a bunch of cameras that each
    // observe some subset of the points. Run a simple incremental SfM pipeline
    // to estimate the 3D points of all cameras and images, and check that they
    // are all in the right places.
    math::RandomGenerator rng(0);

    // Generate random points in 3D, and give them each a unique descriptor.
    std::vector<Descriptor> descriptors;
    for (int ii = 0; ii < kNumPoints_; ++ii) {
      const double x = rng.DoubleUniform(-2.0, 2.0);
      const double y = rng.DoubleUniform(-2.0, 2.0);
      const double z = rng.DoubleUniform(-2.0, 2.0);
      points_.emplace_back(x, y, z);
      descriptors_.push_back(Descriptor::Random(64));
    }

    // Generate cameras such that each camera sees at least 8 points.
    CameraIntrinsics intrinsics = DefaultIntrinsics();
    while (cameras_.size() < kNumCameras_) {
      Camera camera;
      CameraExtrinsics extrinsics;
      const double x = rng.DoubleUniform(-10.0, 10.0);
      const double y = rng.DoubleUniform(-10.0, 10.0);
      const double z = rng.DoubleUniform(-10.0, 10.0);

      // Don't get too close to the 3D points.
      if ((x > -3.0 && x < 3.0) || (y > -3.0 && y < 3.0) || (z > -3.0 && z < 3.0))
        continue;

      extrinsics.SetTranslation(x, y, z);
      Vector3d euler_angles(Vector3d::Random()*D2R(20.0));
      extrinsics.Rotate(EulerAnglesToMatrix(euler_angles));
      camera.SetExtrinsics(extrinsics);
      camera.SetIntrinsics(intrinsics);

      // Make sure this camera can see at least eight 3D points.
      bool sees_at_least_eight = false;
      unsigned int count = 0;
      for (const auto& point : points_) {
        double u = 0.0, v = 0.0;
        if (camera.WorldToImage(point.X(), point.Y(), point.Z(), &u, &v)) {
          count++;
        }

        if (count == 8) {
          sees_at_least_eight = true;
          break;
        }
      }

      if (sees_at_least_eight)
        cameras_.push_back(camera);
    }
  }

  // Simulate feature extraction from the camera at a specified index.
  void SimulateFeatureExtraction(unsigned int camera_index,
                                 std::vector<Feature>& features,
                                 std::vector<Descriptor>& descriptors) {
    features.clear();
    descriptors.clear();

    // Get this camera.
    Camera camera = cameras_[camera_index];

    // Try to project each point into the camera.
    for (size_t ii = 0; ii < points_.size(); ++ii) {
      Point3D p = points_[ii];
      double u = 0.0, v = 0.0;

      // If any points project successfully, store the projected point and its
      // corresponding descriptor as output.
      if (camera.WorldToImage(p.X(), p.Y(), p.Z(), &u, &v)) {
        features.push_back(Feature(u, v));
        descriptors.push_back(descriptors_[ii]);
      }
    }
  }

  CameraIntrinsics DefaultIntrinsics() const {
    CameraIntrinsics intrinsics;
    intrinsics.SetImageLeft(0);
    intrinsics.SetImageTop(0);
    intrinsics.SetImageWidth(kImageWidth_);
    intrinsics.SetImageHeight(kImageHeight_);
    intrinsics.SetVerticalFOV(kVerticalFov_);
    intrinsics.SetFU(intrinsics.f_v());
    intrinsics.SetCU(0.5 * kImageWidth_);
    intrinsics.SetCV(0.5 * kImageHeight_);
    intrinsics.SetK(0.0, 0.0, 0.0, 0.0, 0.0);
    return intrinsics;
  }

  int kImageWidth_ = 1920;
  int kImageHeight_ = 1080;
  int kVerticalFov_ = D2R(90.0);

  int kNumPoints_ = 100;
  int kNumCameras_ = 20;

  std::vector<Point3D> points_;
  std::vector<Descriptor> descriptors_;
  std::vector<Camera> cameras_;
};

TEST_F(TestSimpleNoiselessSfm, TestNoBundleAdjustment) {

  // Make some fake 3D points, a random descriptor for each point, and cameras
  // that are able to observe at least 1 point.
  GenerateGroundTruthPointsAndCameras();

  // Initialize using the first two cameras.
  std::vector<Feature> features1;
  std::vector<Feature> features2;
  std::vector<Descriptor> descriptors1;
  std::vector<Descriptor> descriptors2;
  SimulateFeatureExtraction(0, features1, descriptors1);
  SimulateFeatureExtraction(1, features2, descriptors2);

  // Match features between first 2 cameras.
  NaiveFeatureMatcher feature_matcher;
  FeatureMatcherOptions matcher_options;
  matcher_options.min_num_feature_matches = 8;
  feature_matcher.AddImageFeatures(features1, descriptors1);
  feature_matcher.AddImageFeatures(features2, descriptors2);
  PairwiseImageMatchList image_matches;
  DistanceMetric::Instance().SetMetric(DistanceMetric::Metric::SCALED_L2);
  ASSERT_TRUE(feature_matcher.MatchImages(matcher_options, image_matches));
  PairwiseImageMatch image_match = image_matches[0];
  FeatureMatchList feature_matches = image_match.feature_matches_;

  // Get fundamental matrix of first 2 cameras.
  Matrix3d E, F;
  EightPointAlgorithmSolver f_solver;
  FundamentalMatrixSolverOptions solver_options;
  f_solver.SetOptions(solver_options);
  ASSERT_TRUE(f_solver.ComputeFundamentalMatrix(feature_matches, F));

  // Get essential matrix of first 2 cameras.
  CameraIntrinsics intrinsics = DefaultIntrinsics();
  EssentialMatrixSolver e_solver;
  E = e_solver.ComputeEssentialMatrix(F, intrinsics, intrinsics);

  // Get extrinsics from essential matrix.
  Pose relative_pose;
  ASSERT_TRUE(e_solver.ComputeExtrinsics(E, feature_matches, intrinsics,
                                         intrinsics, relative_pose));

  // Now we know the transformation between the first two cameras up to scale.
  Camera camera1, camera2;
  CameraExtrinsics extrinsics1, extrinsics2;
  extrinsics1.SetWorldToCamera(Pose());
  extrinsics2.SetWorldToCamera(relative_pose);
  camera1.SetExtrinsics(extrinsics1);
  camera2.SetExtrinsics(extrinsics2);
  camera1.SetIntrinsics(intrinsics);
  camera2.SetIntrinsics(intrinsics);

  // Triangulate features in 3D.
  Point3DList initial_points;
  ASSERT_TRUE(Triangulate(feature_matches, camera1, camera2, initial_points));
  ASSERT_EQ(initial_points.size(), feature_matches.size());

  // Initialize views and landmarks.
  View::Ptr view1 = View::Create(camera1);
  View::Ptr view2 = View::Create(camera2);
  for (size_t ii = 0; ii < initial_points.size(); ++ii) {
    Landmark::Ptr landmark = Landmark::Create();

    Descriptor d1 = descriptors1[image_match.descriptor_indices1_[ii]];
    Descriptor d2 = descriptors2[image_match.descriptor_indices2_[ii]];
    ASSERT_TRUE(d1.isApprox(d2));

    Observation::Ptr observation1 =
        Observation::Create(view1, feature_matches[ii].feature1_, d1);
    Observation::Ptr observation2 =
        Observation::Create(view2, feature_matches[ii].feature2_, d2);

    landmark->IncorporateObservation(observation1);
    landmark->IncorporateObservation(observation2);

    // The landmark's 3D position should automatically be triangulated from the
    // observations.
    EXPECT_NEAR(initial_points[ii].X(), landmark->Position().X(), 1e-8);
    EXPECT_NEAR(initial_points[ii].Y(), landmark->Position().Y(), 1e-8);
    EXPECT_NEAR(initial_points[ii].Z(), landmark->Position().Z(), 1e-8);

    view1->AddObservation(observation1);
    view2->AddObservation(observation2);
  }

#if 0
  // Loop over remaining cameras as if we are receiving their images
  // incrementally.
  for (int ii = 2; ii < kNumCameras_; ++ii) {
    // Get features and descriptors for the new frame.
    std::vector<Feature> features;
    std::vector<Descriptor> descriptors;
    SimulateFeatureExtraction(ii, features, descriptors);

    // Match with existing landmarks.
    std::vector<LandmarkIndex> landmark_indices =
        FindLandmarkMatches(descriptors);

    // 2D<-->3D pose estimation.
    Point3DList points;
    for (const auto& index : landmark_indices)
      points.push_back(Landmark::GetLandmark(index)->Position());

    // Calculate the 3D pose of this camera.
    Pose calculated_pose;
    PoseEstimator2D3D pose_estimator;
    pose_estimator.Initialize(features, points, intrinsics);
    pose_estimator.Solve(calculated_pose);

    // Create a camera and view.
    Camera new_camera;
    CameraExtrinsics new_extrinsics;
    extrinsics.SetWorldToCamera(calculated_pose);
    new_camera.SetExtrinsics(extrinsics);
    new_camera.SetIntrinsics(intrinsics);
    View::Ptr new_view = View::Create(new_camera);

    // Add all observed landmarks to the view.
    for (size_t ii = 0; ii < landmark_indices.size(); ++ii) {
      Observation::Ptr observation =
          Observation::Create(new_view, features[ii], descriptors[ii]);
      Landmark::Ptr landmark = Landmark::GetLandmark(landmark_indices[ii]);
      landmark->IncorporateObservation(observation);
      new_view->AddObservation(observation);
    }

    // Try to find new landmarks by matching descriptors against descriptors
    // from other images that have not been used yet.


  }
#endif
}

}  //\namespace bsfm
