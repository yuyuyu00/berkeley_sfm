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
#include <geometry/point_3d.h>
#include <geometry/pose_estimator_2d_3d.h>
#include <geometry/rotation.h>
#include <matching/feature.h>
#include <math/random_generator.h>

#include <gtest/gtest.h>

namespace bsfm {

namespace {
  const int kNumPoints = 20;
  const int kImageWidth = 1920;
  const int kImageHeight = 1080;
  const double kVerticalFov = 0.5 * M_PI;

  CameraIntrinsics DefaultIntrinsics() {
    CameraIntrinsics intrinsics;
    intrinsics.SetImageLeft(0);
    intrinsics.SetImageTop(0);
    intrinsics.SetImageWidth(kImageWidth);
    intrinsics.SetImageHeight(kImageHeight);
    intrinsics.SetVerticalFOV(kVerticalFov);
    intrinsics.SetFU(intrinsics.f_v());
    intrinsics.SetCU(0.5 * kImageWidth);
    intrinsics.SetCV(0.5 * kImageHeight);

    return intrinsics;
  }
}  //\namespace

// Test if the pose estimator can correctly predict camera position when a set
// of 3D points are perfectly projected into the image.
TEST(PoseEstimator2D3D, TestPoseEstimatorNoiseless) {

  // Create a random number generator.
  math::RandomGenerator rng(0);

  for (int iter = 0; iter < 100; ++iter) {
    // Create a camera and shift it from the origin.
    double cx = rng.DoubleUniform(-2.0, 2.0);
    double cy = rng.DoubleUniform(-2.0, 2.0);
    double cz = rng.DoubleUniform(-2.0, 2.0);

    // Wobble it around a little.
    const double twenty_degrees = 0.11 * M_PI;
    Eigen::Vector3d e_in;
    e_in.setRandom();  // sets all elements to be \in (-1, 1).
    e_in *= twenty_degrees;  // all angles are \in (-20, 20) degrees.
    const Eigen::Matrix3d R_in = EulerAnglesToMatrix(e_in);

    Camera camera;
    CameraExtrinsics extrinsics;
    extrinsics.Rotate(R_in);
    extrinsics.Translate(cx, cy, cz);
    camera.SetExtrinsics(extrinsics);
    camera.SetIntrinsics(DefaultIntrinsics());

    // Randomly create a bunch of 3D points. The camera will be looking down the
    // world's -Y direction, so make the points somewhere out there. Then
    // project
    // all the points into the camera
    Point3DList points_3d;
    FeatureList points_2d;
    while (points_3d.size() < kNumPoints) {
      // Make some points out in front of the camea
      double x = rng.DoubleUniform(-10.0, 10.0);
      double y = rng.DoubleUniform(5.0, 20.0);
      double z = rng.DoubleUniform(-10.0, 10.0);

      // Project the point into the camera. No noise.
      Feature point_2d;
      double u = 0.0, v = 0.0;
      if (camera.WorldToImage(x, y, z, &u, &v)) {
        points_2d.emplace_back(u, v);
        points_3d.emplace_back(x, y, z);
      }
    }

    // Now use the pose estimator to predict the camera pose.
    PoseEstimator2D3D estimator;
    estimator.Initialize(points_2d, points_3d, camera.Intrinsics());

    Pose calculated_pose;
    estimator.Solve(calculated_pose);

    // Make sure we got the right rotation and translation. Extract camera
    // center with c = -R' * t, and extract euler angles from R for comparison.
    const Eigen::Matrix3d R_out = calculated_pose.Rotation();
    const Eigen::Vector3d c =
        -R_out.transpose() * calculated_pose.Translation();

    EXPECT_NEAR(cx, c(0), 1e-4);
    EXPECT_NEAR(cy, c(1), 1e-4);
    EXPECT_NEAR(cz, c(2), 1e-4);
    EXPECT_TRUE(extrinsics.Rt().block(0, 0, 3, 3).isApprox(R_out, 1e-4));
  }
}

}  //\namespace bsfm
